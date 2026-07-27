#ifndef ORDER_LEDGER_HPP
#define ORDER_LEDGER_HPP

// OrderLedger — the client-order-ID ledger (CLAUDE.md Phase 1, item 1).
//
// Purpose: keep local truth in sync with broker truth so the bot is never WRONG
// about whether an order actually filled — the "ghost fill" problem. Every order
// gets a row written *before* the HTTP call fires, so a crash mid-request still
// leaves a record the reconciliation poll can resolve against the broker.
//
// Lifecycle of a row's status:
//   pending  — written before the POST; POST returned 2xx (accepted/new) OR is
//              still in flight. Reconciliation upgrades this to filled/failed.
//   unknown  — the POST timed out or returned an unparseable 2xx body. We DON'T
//              know the broker state; reconciliation (not a guess) decides.
//   filled   — broker confirmed a fill (or the position is confirmed open).
//   failed   — broker explicitly rejected, or the order never reached the broker
//              (404 after the grace period).
//
// This class opens its OWN sqlite3 handle to the SAME db file PositionManager
// uses. That multi-handle-to-one-file pattern is already in production (the Python
// heartbeat monitor opens the same file); WAL + busy_timeout make it safe. The
// ledger owns only the `order_ledger` table and never touches `open_positions` —
// reconciliation orchestration (in the engine) bridges the two.

#include <sqlite3.h>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace nox::execution {

class OrderLedger {
public:
    // One order's worth of ledger state. The columns beyond CLAUDE.md's required
    // minimum (client_oid/ticker/strategy/status/sent_at) exist so reconciliation
    // can reconstruct an open_positions row for a ghost fill it discovers.
    struct Order {
        std::string client_oid;       // == broker client_order_id
        std::string ticker;
        std::string strategy;
        std::string signature;        // ticker|strategy|strike|strike2|expiry|side (dedup key)
        std::string side;             // buy/sell
        std::string option_type;      // call/put ("" for multi-leg)
        std::string profile_type;     // long/short_premium
        std::string expiration_date;  // YYYY-MM-DD
        std::string asset_class = "option";
        std::string status;           // pending/unknown/filled/failed
        std::string broker_order_id;
        double      strike      = 0.0;
        double      strike2     = 0.0;
        double      qty         = 0.0;
        double      entry_price = 0.0;
        long        sent_at     = 0;  // unix epoch seconds
    };

    explicit OrderLedger(const std::string& db_path) {
        if (sqlite3_open(db_path.c_str(), &db_)) {
            throw std::runtime_error("OrderLedger: can't open database: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        // Match PositionManager's PRAGMAs — same file, shared with the Python
        // heartbeat. WAL + busy_timeout let multiple handles coexist safely.
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", 0, 0, nullptr);
        initialize();
    }

    ~OrderLedger() {
        if (db_) sqlite3_close(db_);
    }

    OrderLedger(const OrderLedger&) = delete;
    OrderLedger& operator=(const OrderLedger&) = delete;

    // item 1: write the pending/unknown row BEFORE the HTTP call. Idempotent on
    // client_oid (INSERT OR IGNORE) so an accidental double-insert can't error.
    bool insertPending(const Order& o) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "INSERT OR IGNORE INTO order_ledger "
            "(client_oid, ticker, strategy, signature, side, option_type, "
            " profile_type, expiration_date, asset_class, status, broker_order_id, "
            " strike, strike2, qty, entry_price, sent_at, updated_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return false;
        }
        long now = o.sent_at > 0 ? o.sent_at : now_epoch();
        sqlite3_bind_text  (stmt, 1,  o.client_oid.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2,  o.ticker.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 3,  o.strategy.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 4,  o.signature.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 5,  o.side.c_str(),            -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 6,  o.option_type.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 7,  o.profile_type.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 8,  o.expiration_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 9,  o.asset_class.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 10, o.status.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 11, o.broker_order_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 12, o.strike);
        sqlite3_bind_double(stmt, 13, o.strike2);
        sqlite3_bind_double(stmt, 14, o.qty);
        sqlite3_bind_double(stmt, 15, o.entry_price);
        sqlite3_bind_int64 (stmt, 16, now);
        sqlite3_bind_int64 (stmt, 17, now);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }

    // item 3: the 60-second network-retry blocker. Keyed on signature (not oid) so
    // a retry that straddles a minute bucket is still caught. Returns true if the
    // SAME order (same signature) was already ATTEMPTED within the window —
    // regardless of outcome. Any status counts: a 429/500 that left a 'failed' row
    // must still block an immediate re-fire (that's an accidental retry). The
    // signal-driven philosophy handles legitimate re-entry via the NEXT scan
    // (minutes later, > window), which lands outside this window and is allowed.
    bool hasRecentActive(const std::string& signature, int window_seconds) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "SELECT COUNT(*) FROM order_ledger "
            "WHERE signature = ? AND sent_at > ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_bind_text (stmt, 1, signature.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, now_epoch() - window_seconds);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count > 0;
    }

    // item 2: update a row's status after route() returns or reconciliation resolves.
    void setStatus(const std::string& client_oid,
                   const std::string& status,
                   const std::string& broker_order_id = "") {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "UPDATE order_ledger SET status = ?, "
            "broker_order_id = CASE WHEN ?='' THEN broker_order_id ELSE ? END, "
            "updated_at = ? WHERE client_oid = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_text (stmt, 1, status.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 2, broker_order_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 3, broker_order_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, now_epoch());
        sqlite3_bind_text (stmt, 5, client_oid.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // item 2 reconciliation input: all rows still needing broker confirmation.
    std::vector<Order> getUnresolved() {
        std::lock_guard<std::mutex> lock(lock_);
        std::vector<Order> out;
        const char* sql =
            "SELECT client_oid, ticker, strategy, signature, side, option_type, "
            "       profile_type, expiration_date, asset_class, status, broker_order_id, "
            "       strike, strike2, qty, entry_price, sent_at "
            "FROM order_ledger WHERE status IN ('pending','unknown') ORDER BY sent_at ASC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return out;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Order o;
            o.client_oid      = col_text(stmt, 0);
            o.ticker          = col_text(stmt, 1);
            o.strategy        = col_text(stmt, 2);
            o.signature       = col_text(stmt, 3);
            o.side            = col_text(stmt, 4);
            o.option_type     = col_text(stmt, 5);
            o.profile_type    = col_text(stmt, 6);
            o.expiration_date = col_text(stmt, 7);
            o.asset_class     = col_text(stmt, 8);
            o.status          = col_text(stmt, 9);
            o.broker_order_id = col_text(stmt, 10);
            o.strike          = sqlite3_column_double(stmt, 11);
            o.strike2         = sqlite3_column_double(stmt, 12);
            o.qty             = sqlite3_column_double(stmt, 13);
            o.entry_price     = sqlite3_column_double(stmt, 14);
            o.sent_at         = static_cast<long>(sqlite3_column_int64(stmt, 15));
            out.push_back(std::move(o));
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Reconciliation throttle (design decision D): the two options threads share
    // one ledger, so only one should actually poll per window. Returns true at most
    // once per min_interval_seconds; the losing/too-soon caller cheaply skips.
    // Reconciliation is idempotent anyway, so this is an efficiency guard, not a
    // correctness dependency.
    bool shouldReconcileNow(int min_interval_seconds) {
        std::lock_guard<std::mutex> lock(recon_lock_);
        long now = now_epoch();
        if (last_reconcile_ != 0 && (now - last_reconcile_) < min_interval_seconds)
            return false;
        last_reconcile_ = now;
        return true;
    }

    static long now_epoch() {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    // Phase 2, item A: the signal-regeneration audit trail. order_ledger only ever
    // sees signals that reached executeSignal() — it can't tell you WHY a signal
    // was silent (suppressed by a gate/cap) vs simply not produced this cycle. This
    // table records every decision point on the executeSignal path (gates in
    // run_scan + the pre-order gate) so a query by `signature` across scan cycles
    // answers "did it regenerate because conditions still hold, or stay silent
    // because the position/duplicate guard correctly blocked it?"
    struct SignalEvent {
        std::string ticker;
        std::string strategy;
        std::string signature;
        double      quality_score = 0.0;
        std::string outcome;   // suppressed_vix_term_gate/suppressed_liquidity_gate/
                                // suppressed_cap/gate_blocked_duplicate/
                                // gate_blocked_position_exists/gate_blocked_error/submitted
        std::string reason;
        long        scan_at = 0;
    };

    void logSignalEvent(const std::string& ticker, const std::string& strategy,
                        const std::string& signature, double quality_score,
                        const std::string& outcome, const std::string& reason) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "INSERT INTO signal_events "
            "(ticker, strategy, signature, quality_score, outcome, reason, scan_at) "
            "VALUES (?,?,?,?,?,?,?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_text  (stmt, 1, ticker.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, strategy.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 3, signature.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, quality_score);
        sqlite3_bind_text  (stmt, 5, outcome.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 6, reason.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 7, now_epoch());
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Engine-wide prediction-quality logging (additive only — never changes
    // sizing/gating behavior). `source_type` is one of: options_signal,
    // ws1_contradiction, skeptic_insider, skeptic_altmacro, china_lag_ws8,
    // fundamentals_beneish, fundamentals_fcf. `confidence` is 0..1 and may be
    // left at 0 (source has no confidence notion) — the Python-side rollup
    // treats 0 as "no confidence data" via NULL-friendly aggregation, so
    // callers without a real confidence number should still pass 0.0 rather
    // than omitting the call.
    void logPrediction(const std::string& source_type, long source_ref_id,
                        const std::string& ticker, const std::string& direction,
                        double confidence, const std::string& detail) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "INSERT INTO predictions_log "
            "(source_type, source_ref_id, ticker, direction, confidence, logged_at, detail) "
            "VALUES (?,?,?,?,?,?,?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_text  (stmt, 1, source_type.c_str(), -1, SQLITE_TRANSIENT);
        if (source_ref_id > 0) sqlite3_bind_int64(stmt, 2, source_ref_id);
        else                   sqlite3_bind_null (stmt, 2);
        sqlite3_bind_text  (stmt, 3, ticker.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 4, direction.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 5, confidence);
        sqlite3_bind_int64 (stmt, 6, now_epoch());
        sqlite3_bind_text  (stmt, 7, detail.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // For the audit test and any future reporting: the full decision history for
    // one signature, oldest first, so consecutive rows show whether a signal
    // regenerated (new 'submitted' after a 'failed'-equivalent outcome) or stayed
    // silent for a documented reason.
    std::vector<SignalEvent> getEventsBySignature(const std::string& signature) {
        std::lock_guard<std::mutex> lock(lock_);
        std::vector<SignalEvent> out;
        const char* sql =
            "SELECT ticker, strategy, signature, quality_score, outcome, reason, scan_at "
            "FROM signal_events WHERE signature = ? ORDER BY scan_at ASC, id ASC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return out;
        }
        sqlite3_bind_text(stmt, 1, signature.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SignalEvent e;
            e.ticker        = col_text(stmt, 0);
            e.strategy      = col_text(stmt, 1);
            e.signature     = col_text(stmt, 2);
            e.quality_score = sqlite3_column_double(stmt, 3);
            e.outcome       = col_text(stmt, 4);
            e.reason        = col_text(stmt, 5);
            e.scan_at       = static_cast<long>(sqlite3_column_int64(stmt, 6));
            out.push_back(std::move(e));
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Full-detail signal store: every generated candidate (submitted, gate-
    // suppressed, earnings-skipped, DTE-floor-skipped — not just suppressions
    // like signal_events) gets one row here with its contract/regime context,
    // so past signals can be analyzed instead of only what actually got sent.
    struct GeneratedSignal {
        std::string ticker;
        std::string strategy;
        std::string signature;
        std::string direction;          // bullish/bearish/neutral
        double      strike           = 0.0;
        double      strike2          = 0.0;
        double      strike3          = 0.0; // 4-leg strategies only (e.g. REVERSE_IRON_CONDOR)
        double      strike4          = 0.0;
        std::string expiration_date;
        int         dte              = 0;
        bool        macro_override_used = false;
        double      iv_rank          = 0.0;
        double      hrv30            = 0.0;
        double      quality_score    = 0.0;
        std::string regime;             // RISK_ON/TRANSITION/RISK_OFF
        std::string vix_term_label;     // CONTANGO/BACKWARDATION/NEUTRAL
        bool        earnings_checked  = true;
        std::string outcome;            // submitted/suppressed_*/skipped_earnings_*/skipped_dte_floor
        std::string reason;
        long        scan_at           = 0;
    };

    void logGeneratedSignal(const GeneratedSignal& s) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "INSERT INTO options_signals "
            "(ticker, strategy, signature, direction, strike, strike2, strike3, strike4, expiration_date, "
            " dte, macro_override_used, iv_rank, hrv30, quality_score, regime, "
            " vix_term_label, earnings_checked, outcome, reason, scan_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        long now = s.scan_at > 0 ? s.scan_at : now_epoch();
        sqlite3_bind_text  (stmt, 1,  s.ticker.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2,  s.strategy.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 3,  s.signature.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 4,  s.direction.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 5,  s.strike);
        sqlite3_bind_double(stmt, 6,  s.strike2);
        sqlite3_bind_double(stmt, 7,  s.strike3);
        sqlite3_bind_double(stmt, 8,  s.strike4);
        sqlite3_bind_text  (stmt, 9,  s.expiration_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 10, s.dte);
        sqlite3_bind_int   (stmt, 11, s.macro_override_used ? 1 : 0);
        sqlite3_bind_double(stmt, 12, s.iv_rank);
        sqlite3_bind_double(stmt, 13, s.hrv30);
        sqlite3_bind_double(stmt, 14, s.quality_score);
        sqlite3_bind_text  (stmt, 15, s.regime.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 16, s.vix_term_label.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 17, s.earnings_checked ? 1 : 0);
        sqlite3_bind_text  (stmt, 18, s.outcome.c_str(),         -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 19, s.reason.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 20, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Read path for analysis/reporting (and tests): the full generated-signal
    // history for one ticker, most recent first.
    std::vector<GeneratedSignal> getGeneratedSignalsByTicker(const std::string& ticker,
                                                               int limit = 100) {
        std::lock_guard<std::mutex> lock(lock_);
        std::vector<GeneratedSignal> out;
        const char* sql =
            "SELECT ticker, strategy, signature, direction, strike, strike2, strike3, strike4, expiration_date, "
            "       dte, macro_override_used, iv_rank, hrv30, quality_score, regime, "
            "       vix_term_label, earnings_checked, outcome, reason, scan_at "
            "FROM options_signals WHERE ticker = ? ORDER BY scan_at DESC, id DESC LIMIT ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return out;
        }
        sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 2, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            GeneratedSignal s;
            s.ticker              = col_text(stmt, 0);
            s.strategy            = col_text(stmt, 1);
            s.signature           = col_text(stmt, 2);
            s.direction           = col_text(stmt, 3);
            s.strike              = sqlite3_column_double(stmt, 4);
            s.strike2             = sqlite3_column_double(stmt, 5);
            s.strike3             = sqlite3_column_double(stmt, 6);
            s.strike4             = sqlite3_column_double(stmt, 7);
            s.expiration_date     = col_text(stmt, 8);
            s.dte                 = sqlite3_column_int(stmt, 9);
            s.macro_override_used = sqlite3_column_int(stmt, 10) != 0;
            s.iv_rank             = sqlite3_column_double(stmt, 11);
            s.hrv30               = sqlite3_column_double(stmt, 12);
            s.quality_score       = sqlite3_column_double(stmt, 13);
            s.regime              = col_text(stmt, 14);
            s.vix_term_label      = col_text(stmt, 15);
            s.earnings_checked    = sqlite3_column_int(stmt, 16) != 0;
            s.outcome             = col_text(stmt, 17);
            s.reason              = col_text(stmt, 18);
            s.scan_at             = static_cast<long>(sqlite3_column_int64(stmt, 19));
            out.push_back(std::move(s));
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // ── Post-earnings drift research (passive) ────────────────────────────────
    struct EarningsDriftObservation {
        std::string ticker;
        std::string earnings_date; // YYYY-MM-DD
        double      pre_price = 0.0;
        double      pre_rsi   = 0.0;
        double      pre_sma20 = 0.0;
        double      pre_sma50 = 0.0;
        double      pre_atr   = 0.0;
        std::string direction;
    };

    // INSERT OR IGNORE — the UNIQUE(ticker, earnings_date) constraint makes
    // this idempotent across restarts (the generator's in-memory dedup set
    // handles the common case; this is the backstop).
    void recordEarningsDriftObservation(const EarningsDriftObservation& o) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "INSERT OR IGNORE INTO earnings_drift_observations "
            "(ticker, earnings_date, observed_at, pre_price, pre_rsi, pre_sma20, pre_sma50, pre_atr, direction) "
            "VALUES (?,?,?,?,?,?,?,?,?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_text  (stmt, 1, o.ticker.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, o.earnings_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 3, now_epoch());
        sqlite3_bind_double(stmt, 4, o.pre_price);
        sqlite3_bind_double(stmt, 5, o.pre_rsi);
        sqlite3_bind_double(stmt, 6, o.pre_sma20);
        sqlite3_bind_double(stmt, 7, o.pre_sma50);
        sqlite3_bind_double(stmt, 8, o.pre_atr);
        sqlite3_bind_text  (stmt, 9, o.direction.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // One row awaiting its T+1 or T+5 realized-move fill (day_offset 1 or 5).
    struct PendingEarningsDrift {
        long        id         = 0;
        std::string ticker;
        double      pre_price  = 0.0;
        int         day_offset = 0;
    };

    // Calendar-day math (same Howard Hinnant algorithm OptionsSignalGenerator
    // uses) — kept local rather than shared, matching this codebase's
    // own-handle-own-helpers pattern (see IvRankStore/AlphaDecayStore).
    static long earningsDaysFromCivil(int y, unsigned m, unsigned d) {
        y -= (m <= 2);
        const long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<long>(doe) - 719468;
    }
    static long earningsParseDateToEpochDays(const std::string& ymd) {
        int y = 0; unsigned m = 0, d = 0;
        std::sscanf(ymd.c_str(), "%d-%u-%u", &y, &m, &d);
        return earningsDaysFromCivil(y, m, d);
    }

    // Uses calendar days (not trading days) — deliberately simple for a
    // research trail that only needs to be read a few months from now, not
    // precise to the trading session.
    std::vector<PendingEarningsDrift> getPendingEarningsDrift() {
        std::lock_guard<std::mutex> lock(lock_);
        std::vector<PendingEarningsDrift> out;
        const char* sql =
            "SELECT id, ticker, earnings_date, pre_price, resolved_t1, resolved_t5 "
            "FROM earnings_drift_observations WHERE resolved_t1 = 0 OR resolved_t5 = 0;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return out;
        }
        auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm today_buf{};
        gmtime_r(&now_t, &today_buf);
        long today_days = earningsDaysFromCivil(today_buf.tm_year + 1900,
                                                 today_buf.tm_mon + 1, today_buf.tm_mday);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            long id                    = sqlite3_column_int64(stmt, 0);
            std::string ticker         = col_text(stmt, 1);
            std::string earnings_date  = col_text(stmt, 2);
            double pre_price           = sqlite3_column_double(stmt, 3);
            bool resolved_t1           = sqlite3_column_int(stmt, 4) != 0;
            bool resolved_t5           = sqlite3_column_int(stmt, 5) != 0;
            long earnings_days         = earningsParseDateToEpochDays(earnings_date);
            long days_since            = today_days - earnings_days;
            if (!resolved_t1 && days_since >= 1) {
                out.push_back({id, ticker, pre_price, 1});
            }
            if (!resolved_t5 && days_since >= 5) {
                out.push_back({id, ticker, pre_price, 5});
            }
        }
        sqlite3_finalize(stmt);
        return out;
    }

    void resolveEarningsDriftT1(long id, double price, double move_pct) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql = "UPDATE earnings_drift_observations "
                          "SET price_t1 = ?, move_pct_t1 = ?, resolved_t1 = 1 WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_double(stmt, 1, price);
        sqlite3_bind_double(stmt, 2, move_pct);
        sqlite3_bind_int64 (stmt, 3, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void resolveEarningsDriftT5(long id, double price, double move_pct) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql = "UPDATE earnings_drift_observations "
                          "SET price_t5 = ?, move_pct_t5 = ?, resolved_t5 = 1 WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_double(stmt, 1, price);
        sqlite3_bind_double(stmt, 2, move_pct);
        sqlite3_bind_int64 (stmt, 3, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Fallback-only position-exists check for multi-leg spreads: they never
    // populate open_positions (single-leg only — see main.cpp's execution
    // recorder), so there is no sqlite table that directly says "this spread
    // is still open." This is a proxy: a filled multi-leg order for this
    // ticker whose expiration hasn't passed. It is NOT authoritative — it
    // can't see a manual close — so callers should prefer
    // OptionsOrderRouter::hasOpenOptionPosition (the broker) when reachable
    // and only fall back to this when the broker call fails.
    bool hasOpenMultiLegPosition(const std::string& ticker, const std::string& today_yyyy_mm_dd) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "SELECT COUNT(*) FROM order_ledger "
            "WHERE ticker = ? AND asset_class = 'option' AND status = 'filled' "
            "AND option_type = '' AND expiration_date >= ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_bind_text(stmt, 1, ticker.c_str(),           -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, today_yyyy_mm_dd.c_str(), -1, SQLITE_TRANSIENT);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count > 0;
    }

private:
    void initialize() {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "CREATE TABLE IF NOT EXISTS order_ledger ("
            "id              INTEGER PRIMARY KEY AUTOINCREMENT, "
            "client_oid      TEXT NOT NULL UNIQUE, "
            "ticker          TEXT NOT NULL, "
            "strategy        TEXT NOT NULL, "
            "signature       TEXT NOT NULL, "
            "side            TEXT, "
            "option_type     TEXT, "
            "profile_type    TEXT, "
            "expiration_date TEXT, "
            "asset_class     TEXT DEFAULT 'option', "
            "status          TEXT NOT NULL, "
            "broker_order_id TEXT, "
            "strike          REAL, "
            "strike2         REAL, "
            "qty             REAL, "
            "entry_price     REAL, "
            "sent_at         INTEGER NOT NULL, "
            "updated_at      INTEGER);"
            "CREATE INDEX IF NOT EXISTS idx_ledger_sig    ON order_ledger(signature, sent_at);"
            "CREATE INDEX IF NOT EXISTS idx_ledger_status ON order_ledger(status);"
            "CREATE TABLE IF NOT EXISTS signal_events ("
            "id              INTEGER PRIMARY KEY AUTOINCREMENT, "
            "ticker          TEXT NOT NULL, "
            "strategy        TEXT NOT NULL, "
            "signature       TEXT NOT NULL, "
            "quality_score   REAL, "
            "outcome         TEXT NOT NULL, "
            "reason          TEXT, "
            "scan_at         INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS idx_events_sig ON signal_events(signature, scan_at);"
            "CREATE TABLE IF NOT EXISTS options_signals ("
            "id              INTEGER PRIMARY KEY AUTOINCREMENT, "
            "ticker          TEXT NOT NULL, "
            "strategy        TEXT NOT NULL, "
            "signature       TEXT NOT NULL, "
            "direction       TEXT, "
            "strike          REAL, "
            "strike2         REAL, "
            "strike3         REAL, "
            "strike4         REAL, "
            "expiration_date TEXT, "
            "dte             INTEGER, "
            "macro_override_used INTEGER DEFAULT 0, "
            "iv_rank         REAL, "
            "hrv30           REAL, "
            "quality_score   REAL, "
            "regime          TEXT, "
            "vix_term_label  TEXT, "
            "earnings_checked INTEGER DEFAULT 1, "
            "outcome         TEXT NOT NULL, "
            "reason          TEXT, "
            "scan_at         INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS idx_options_signals_ticker_time ON options_signals(ticker, scan_at);"
            // Passive post-earnings-drift research trail (CLAUDE.md-style: track,
            // don't trade on it yet). One row per (ticker, earnings_date); T+1/T+5
            // realized move filled in later once enough calendar time has passed.
            "CREATE TABLE IF NOT EXISTS earnings_drift_observations ("
            "id              INTEGER PRIMARY KEY AUTOINCREMENT, "
            "ticker          TEXT NOT NULL, "
            "earnings_date   TEXT NOT NULL, "
            "observed_at     INTEGER NOT NULL, "
            "pre_price       REAL, "
            "pre_rsi         REAL, "
            "pre_sma20       REAL, "
            "pre_sma50       REAL, "
            "pre_atr         REAL, "
            "direction       TEXT, "
            "price_t1        REAL, "
            "move_pct_t1     REAL, "
            "resolved_t1     INTEGER DEFAULT 0, "
            "price_t5        REAL, "
            "move_pct_t5     REAL, "
            "resolved_t5     INTEGER DEFAULT 0, "
            "UNIQUE(ticker, earnings_date));"
            "CREATE INDEX IF NOT EXISTS idx_earnings_drift_ticker ON earnings_drift_observations(ticker);"
            // Engine-wide prediction-quality scoring: a generic directional-call
            // log, closing the gap where WS1/Skeptic (WS2/WS3/WS8) compute a
            // direction+confidence but never persist it anywhere (options_signals
            // already carries direction/quality_score, so those get a thin
            // logPrediction() call alongside their existing insert rather than a
            // schema change). heartbeat/prediction_outcome_resolver.py reads this
            // table, resolves it against real price action, and rolls it up into
            // weekly/monthly per-source quality scores — surface-only, no sizing/
            // gating impact (see CLAUDE.md Phase 5+ "Engine-Wide Prediction
            // Quality Scoring").
            "CREATE TABLE IF NOT EXISTS predictions_log ("
            "id              INTEGER PRIMARY KEY AUTOINCREMENT, "
            "source_type     TEXT NOT NULL, "
            "source_ref_id   INTEGER, "
            "ticker          TEXT NOT NULL, "
            "direction       TEXT NOT NULL, "
            "confidence      REAL, "
            "logged_at       INTEGER NOT NULL, "
            "detail          TEXT);"
            "CREATE INDEX IF NOT EXISTS idx_predictions_log_source ON predictions_log(source_type, ticker, logged_at);";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, 0, 0, &err) != SQLITE_OK) {
            std::string msg = "OrderLedger init: " + std::string(err ? err : "unknown");
            sqlite3_free(err);
            throw std::runtime_error(msg);
        }
        // Migration: strike3/strike4 (4-leg strategies, e.g. REVERSE_IRON_CONDOR)
        // added after options_signals already shipped — `CREATE TABLE IF NOT
        // EXISTS` above is a no-op on a pre-existing DB, so add the columns
        // explicitly if missing. Idempotent; safe on every startup.
        ensure_column("options_signals", "strike3", "REAL");
        ensure_column("options_signals", "strike4", "REAL");
    }

    // Adds `column` to `table` if it does not already exist. Idempotent; safe
    // to call on every startup. Same pattern as PositionManager::ensure_column_locked.
    void ensure_column(const std::string& table, const std::string& column,
                        const std::string& decl) {
        std::string pragma = "PRAGMA table_info(" + table + ");";
        sqlite3_stmt* stmt = nullptr;
        bool exists = false;
        if (sqlite3_prepare_v2(db_, pragma.c_str(), -1, &stmt, 0) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* name = sqlite3_column_text(stmt, 1); // col 1 = name
                if (name && column == reinterpret_cast<const char*>(name)) {
                    exists = true;
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);
        if (!exists) {
            std::string alter = "ALTER TABLE " + table + " ADD COLUMN " + column + " " + decl + ";";
            sqlite3_exec(db_, alter.c_str(), 0, 0, nullptr); // best-effort
        }
    }

    static std::string col_text(sqlite3_stmt* stmt, int i) {
        const unsigned char* t = sqlite3_column_text(stmt, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }

    sqlite3*   db_ = nullptr;
    std::mutex lock_;         // guards all statements on db_ (one handle, two threads)
    std::mutex recon_lock_;   // guards the reconcile throttle timestamp
    long       last_reconcile_ = 0;
};

} // namespace nox::execution

#endif // ORDER_LEDGER_HPP
