#ifndef POSITION_MANAGER_HPP
#define POSITION_MANAGER_HPP

#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <sqlite3.h>
#include <stdexcept>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

#include "PortfolioRiskManager.hpp"

// Forward declarations
namespace nox::options_router { class OptionsOrderRouter; }
namespace nox { class TelegramNotifier; }

struct OptionPosition {
    long id;
    std::string ticker;
    std::string option_type; // "call" or "put"
    double strike;
    int quantity;
    double entry_price;
    std::string entry_date;
    std::string profile_type; // "long" or "short_premium"
    std::string expiration_date;
};

// A multi-leg spread/straddle/strangle/reverse-iron-condor position. Every
// strategy OptionsOrderRouter can open is a net debit (buy the more expensive
// leg(s), sell the cheaper leg(s)) — see OptionsSignalGenerator's entry_price
// computation for each strategy — so exit rules only need one profile: apply
// the same 50%-profit / 50%-stop / 21-DTE thresholds the single-leg "long"
// profile already uses, against the NET value of all legs combined.
struct SpreadLeg {
    std::string option_type; // "call" or "put"
    double strike = 0.0;
    std::string side;        // "buy" or "sell" — the ENTRY side; closing reverses it
};

struct SpreadPosition {
    long id;
    std::string underlying;
    std::string strategy;
    int quantity;
    double entry_debit;      // net debit paid, per contract
    std::string entry_date;
    std::string expiration_date;
    std::vector<SpreadLeg> legs; // 2 legs (spreads/straddle/strangle) or 4 (reverse iron condor)
};

class PositionManager {
public:
    PositionManager(const std::string& db_path, nox::options_router::OptionsOrderRouter& order_router)
        : db_path_(db_path), order_router_(order_router), db_(nullptr) {
        if (sqlite3_open(db_path.c_str(), &db_)) {
            throw std::runtime_error("Can't open database: " + std::string(sqlite3_errmsg(db_)));
        }
        // The heartbeat monitor (Python) shares this DB file. WAL lets a reader and
        // a writer coexist without "database is locked"; busy_timeout makes the rare
        // writer-writer collision block-and-retry for up to 5s instead of failing.
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", 0, 0, nullptr);
        initialize_database();
        risk_targets_ = nox::risk::RiskTargets::fromEnv();
    }

    ~PositionManager() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    void start_monitoring() {
        monitoring_thread_ = std::thread(&PositionManager::monitor_positions, this);
    }

    void stop_monitoring() {
        {
            std::lock_guard<std::mutex> lock(monitor_lock_);
            run_monitoring_ = false;
        }
        // Wake the monitoring thread immediately so shutdown doesn't block for up
        // to a full 30-minute sleep cycle before join() can return.
        monitor_cv_.notify_all();
        if (monitoring_thread_.joinable()) {
            monitoring_thread_.join();
        }
    }

    // Record a new open option position in the database
    void add_position(const std::string& ticker,
                      const std::string& option_type,
                      double strike,
                      int quantity,
                      double entry_price,
                      const std::string& entry_date,
                      const std::string& profile_type,
                      const std::string& expiration_date)
    {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql = "INSERT INTO open_positions "
                          "(ticker, option_type, strike, quantity, entry_price, entry_date, profile_type, expiration_date) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, option_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, strike);
            sqlite3_bind_int(stmt, 4, quantity);
            sqlite3_bind_double(stmt, 5, entry_price);
            sqlite3_bind_text(stmt, 6, entry_date.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, profile_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 8, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    // Record a new open multi-leg spread position. `legs` must have 2 or 4
    // entries, in the same order OptionsOrderRouter submitted them.
    void add_spread_position(const std::string& underlying,
                             const std::string& strategy,
                             int quantity,
                             double entry_debit,
                             const std::string& entry_date,
                             const std::string& expiration_date,
                             const std::vector<SpreadLeg>& legs)
    {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql = "INSERT INTO open_spread_positions "
                          "(underlying, strategy, quantity, entry_debit, entry_date, expiration_date, "
                          " leg1_type, leg1_strike, leg1_side, leg2_type, leg2_strike, leg2_side, "
                          " leg3_type, leg3_strike, leg3_side, leg4_type, leg4_strike, leg4_side) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, underlying.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, strategy.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, quantity);
            sqlite3_bind_double(stmt, 4, entry_debit);
            sqlite3_bind_text(stmt, 5, entry_date.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
            for (int i = 0; i < 4; ++i) {
                int base = 7 + i * 3;
                if (static_cast<size_t>(i) < legs.size()) {
                    sqlite3_bind_text(stmt, base,     legs[i].option_type.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_double(stmt, base+1, legs[i].strike);
                    sqlite3_bind_text(stmt, base+2,   legs[i].side.c_str(), -1, SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_null(stmt, base);
                    sqlite3_bind_null(stmt, base+1);
                    sqlite3_bind_null(stmt, base+2);
                }
            }
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

private:
    std::string db_path_;
    sqlite3* db_;
    std::mutex db_lock_;
    nox::options_router::OptionsOrderRouter& order_router_;
    std::thread monitoring_thread_;
    // Written by stop_monitoring() (caller thread), read by monitor_positions()
    // (monitoring thread) — must be atomic to avoid a data race / hoisted read.
    std::atomic<bool> run_monitoring_{true};
    // Guards the interruptible inter-cycle wait so stop_monitoring() can wake the
    // monitoring thread out of its sleep instead of waiting up to 30 minutes.
    std::mutex monitor_lock_;
    std::condition_variable monitor_cv_;

    // Phase 4, item 2: loaded once at construction from env (fromEnv()'s
    // fake-safe defaults apply if unset); last_risk_breach_ is overwritten at
    // the end of every monitor_positions() cycle and read by main.cpp's
    // pre-order gate to decide whether to block new orders.
    nox::risk::RiskTargets risk_targets_;
    std::mutex risk_snapshot_lock_;
    nox::risk::RiskBreach last_risk_breach_;

    void set_last_risk_breach(const nox::risk::RiskBreach& breach) {
        std::lock_guard<std::mutex> lock(risk_snapshot_lock_);
        last_risk_breach_ = breach;
    }

    void initialize_database() {
        std::lock_guard<std::mutex> lock(db_lock_);
        // open_positions: options the exit monitor manages (50%/stop/21-DTE rules).
        // trade_history: the canonical, cross-service trade ledger. The execution
        //   engine is the single writer (every equity + option entry/exit lands here);
        //   the heartbeat monitor reads it for the EOD/EOW/monthly reports. Schema is
        //   kept in sync with heartbeat/monitor.py init_db(); both create-if-not-exists
        //   and additively migrate, so either service may create the file first.
        const char* sql =
            "CREATE TABLE IF NOT EXISTS open_positions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "ticker TEXT NOT NULL, "
            "option_type TEXT NOT NULL, "
            "strike REAL NOT NULL, "
            "quantity INTEGER NOT NULL, "
            "entry_price REAL NOT NULL, "
            "entry_date TEXT NOT NULL, "
            "profile_type TEXT NOT NULL, "
            "expiration_date TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS trade_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "ticker TEXT, "
            "action TEXT, "
            "price REAL, "
            "rsi_value REAL, "
            "sizing_kelly_ratio REAL, "
            "pnl REAL, "
            "asset_class TEXT DEFAULT 'equity', "
            "quantity REAL DEFAULT 0, "
            "detail TEXT DEFAULT '');"
            // Phase 2, item B: live P&L per position. trade_history already records
            // realized P&L at the moment of close, but nothing tracks intraday
            // unrealized P&L for OPEN positions — this table is refreshed every
            // monitor cycle (5 min) so `mark_price`/`unrealized_pnl` are a live,
            // queryable snapshot rather than something only visible in stdout logs.
            // `detail` disambiguates multiple option contracts on the same ticker/day
            // (e.g. "call 100.00 2026-08-21"); equities use "".
            "CREATE TABLE IF NOT EXISTS daily_ledger ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "date TEXT NOT NULL, "
            "ticker TEXT NOT NULL, "
            "asset_class TEXT NOT NULL, "
            "detail TEXT NOT NULL DEFAULT '', "
            "quantity REAL, "
            "entry_price REAL, "
            "mark_price REAL, "
            "unrealized_pnl REAL DEFAULT 0, "
            "realized_pnl REAL DEFAULT 0, "
            "updated_at INTEGER NOT NULL, "
            "UNIQUE(date, ticker, asset_class, detail));"
            "CREATE INDEX IF NOT EXISTS idx_daily_ledger_date ON daily_ledger(date);"
            // Phase 4, item 1: live Greeks. Solved every monitor cycle from the
            // CURRENT underlying price + current option mark (not the entry-time
            // snapshot OptionsSignalGenerator computed once at signal time) — this
            // is the one place a position's Greeks are ever refreshed intraday.
            // One row per open position, overwritten each cycle (last-known-value,
            // same semantics as daily_ledger's mark/unrealized columns).
            "CREATE TABLE IF NOT EXISTS live_greeks ("
            "position_id INTEGER PRIMARY KEY, "
            "ticker TEXT NOT NULL, "
            "underlying_price REAL NOT NULL, "
            "delta REAL NOT NULL, "
            "gamma REAL NOT NULL, "
            "theta REAL NOT NULL, "
            "vega REAL NOT NULL, "
            "implied_volatility REAL NOT NULL, "
            "updated_at INTEGER NOT NULL);"
            // Multi-leg spreads/straddles/strangles/reverse-iron-condors — never
            // populated into open_positions (single-leg only, see has_open_position's
            // own comment). Up to 4 legs; leg3/leg4 are NULL for 2-leg strategies.
            "CREATE TABLE IF NOT EXISTS open_spread_positions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "underlying TEXT NOT NULL, "
            "strategy TEXT NOT NULL, "
            "quantity INTEGER NOT NULL, "
            "entry_debit REAL NOT NULL, "
            "entry_date TEXT NOT NULL, "
            "expiration_date TEXT NOT NULL, "
            "leg1_type TEXT NOT NULL, leg1_strike REAL NOT NULL, leg1_side TEXT NOT NULL, "
            "leg2_type TEXT NOT NULL, leg2_strike REAL NOT NULL, leg2_side TEXT NOT NULL, "
            "leg3_type TEXT, leg3_strike REAL, leg3_side TEXT, "
            "leg4_type TEXT, leg4_strike REAL, leg4_side TEXT);";
        char* err_msg = nullptr;
        if (sqlite3_exec(db_, sql, 0, 0, &err_msg) != SQLITE_OK) {
            std::string err = "SQL error: " + std::string(err_msg);
            sqlite3_free(err_msg);
            throw std::runtime_error(err);
        }
        // Additive migration for a trade_history table created by an older build
        // (or by the monitor's original schema) that lacks the newer columns.
        ensure_column_locked("trade_history", "asset_class", "TEXT DEFAULT 'equity'");
        ensure_column_locked("trade_history", "quantity",    "REAL DEFAULT 0");
        ensure_column_locked("trade_history", "detail",       "TEXT DEFAULT ''");
    }

    // Adds `column` to `table` if it does not already exist. Idempotent; safe to
    // call on every startup. Must be called with db_lock_ held.
    void ensure_column_locked(const std::string& table,
                              const std::string& column,
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

public:
    // Append a trade to the canonical ledger. Called by the execution engine on
    // every equity/option entry and exit. `action` is BUY/SELL (equity) or
    // OPEN/CLOSE (option); `asset_class` is "equity" or "option". Best-effort:
    // a ledger write must never abort or delay an order path, so failures are
    // swallowed (the mutex + SQLite busy_timeout handle contention).
    void record_trade(const std::string& ticker,
                      const std::string& action,
                      const std::string& asset_class,
                      double quantity,
                      double price,
                      double rsi_value,
                      double sizing_kelly_ratio,
                      double pnl,
                      const std::string& detail)
    {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql =
            "INSERT INTO trade_history "
            "(ticker, action, price, rsi_value, sizing_kelly_ratio, pnl, asset_class, quantity, detail) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_text  (stmt, 1, ticker.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, action.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, price);
        sqlite3_bind_double(stmt, 4, rsi_value);
        sqlite3_bind_double(stmt, 5, sizing_kelly_ratio);
        sqlite3_bind_double(stmt, 6, pnl);
        sqlite3_bind_text  (stmt, 7, asset_class.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 8, quantity);
        sqlite3_bind_text  (stmt, 9, detail.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Phase 2, item B: refresh the live unrealized-P&L snapshot for one still-open
    // position. Called every monitor cycle — overwrites quantity/mark/unrealized
    // (last-known-value-for-the-day semantics) but leaves realized_pnl untouched,
    // since a still-open position hasn't realized anything yet.
    void upsert_unrealized(const std::string& date, const std::string& ticker,
                           const std::string& asset_class, const std::string& detail,
                           double quantity, double entry_price, double mark_price,
                           double unrealized_pnl) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql =
            "INSERT INTO daily_ledger "
            "(date, ticker, asset_class, detail, quantity, entry_price, mark_price, "
            " unrealized_pnl, realized_pnl, updated_at) "
            "VALUES (?,?,?,?,?,?,?,?,0,?) "
            "ON CONFLICT(date, ticker, asset_class, detail) DO UPDATE SET "
            "quantity=excluded.quantity, entry_price=excluded.entry_price, "
            "mark_price=excluded.mark_price, unrealized_pnl=excluded.unrealized_pnl, "
            "updated_at=excluded.updated_at;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_text  (stmt, 1, date.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, ticker.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 3, asset_class.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 4, detail.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 5, quantity);
        sqlite3_bind_double(stmt, 6, entry_price);
        sqlite3_bind_double(stmt, 7, mark_price);
        sqlite3_bind_double(stmt, 8, unrealized_pnl);
        sqlite3_bind_int64 (stmt, 9, static_cast<long>(std::time(nullptr)));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Phase 2, item B: record realized P&L against the same day's row when a
    // position closes. Additive (not overwrite) so a partial-close-then-full-close
    // on the same day accumulates correctly rather than clobbering.
    void add_realized(const std::string& date, const std::string& ticker,
                      const std::string& asset_class, const std::string& detail,
                      double realized_pnl_delta) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql =
            "INSERT INTO daily_ledger "
            "(date, ticker, asset_class, detail, quantity, entry_price, mark_price, "
            " unrealized_pnl, realized_pnl, updated_at) "
            "VALUES (?,?,?,?,0,0,0,0,?,?) "
            "ON CONFLICT(date, ticker, asset_class, detail) DO UPDATE SET "
            "realized_pnl=realized_pnl + excluded.realized_pnl, updated_at=excluded.updated_at;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        long now = static_cast<long>(std::time(nullptr));
        sqlite3_bind_text  (stmt, 1, date.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, ticker.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 3, asset_class.c_str(),-1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 4, detail.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 5, realized_pnl_delta);
        sqlite3_bind_int64 (stmt, 6, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Phase 2, item B: read back one day's ledger — used by the test suite and
    // will back the EOD/EOW report's realized+unrealized P&L section.
    struct DailyLedgerRow {
        std::string date, ticker, asset_class, detail;
        double quantity = 0.0, entry_price = 0.0, mark_price = 0.0;
        double unrealized_pnl = 0.0, realized_pnl = 0.0;
        long   updated_at = 0;
    };
    std::vector<DailyLedgerRow> get_daily_ledger(const std::string& date) {
        std::lock_guard<std::mutex> lock(db_lock_);
        std::vector<DailyLedgerRow> out;
        const char* sql =
            "SELECT date, ticker, asset_class, detail, quantity, entry_price, "
            "mark_price, unrealized_pnl, realized_pnl, updated_at "
            "FROM daily_ledger WHERE date = ? ORDER BY ticker ASC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return out;
        }
        sqlite3_bind_text(stmt, 1, date.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DailyLedgerRow r;
            auto text_col = [&](int i) {
                const unsigned char* t = sqlite3_column_text(stmt, i);
                return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
            };
            r.date           = text_col(0);
            r.ticker         = text_col(1);
            r.asset_class    = text_col(2);
            r.detail         = text_col(3);
            r.quantity       = sqlite3_column_double(stmt, 4);
            r.entry_price    = sqlite3_column_double(stmt, 5);
            r.mark_price     = sqlite3_column_double(stmt, 6);
            r.unrealized_pnl = sqlite3_column_double(stmt, 7);
            r.realized_pnl   = sqlite3_column_double(stmt, 8);
            r.updated_at     = static_cast<long>(sqlite3_column_int64(stmt, 9));
            out.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Sum of realized+unrealized P&L across every ticker/asset-class row for
    // one day — the input a daily-loss-limit kill switch needs. Convenience
    // wrapper over get_daily_ledger() rather than a second query, since the
    // row count per day is small and this isn't called on a hot path.
    double get_total_daily_pnl(const std::string& date) {
        double total = 0.0;
        for (const auto& row : get_daily_ledger(date))
            total += row.realized_pnl + row.unrealized_pnl;
        return total;
    }

    // Phase 4, item 1: overwrite one position's live Greeks snapshot. Called
    // every monitor cycle for every position whose live-price fetch + IV solve
    // succeeded; skipped (not zeroed) on failure so a stale-but-real snapshot
    // is never clobbered with garbage.
    void upsert_live_greeks(long position_id, const std::string& ticker,
                            double underlying_price, double delta, double gamma,
                            double theta, double vega, double implied_volatility) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql =
            "INSERT INTO live_greeks "
            "(position_id, ticker, underlying_price, delta, gamma, theta, vega, "
            " implied_volatility, updated_at) VALUES (?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(position_id) DO UPDATE SET "
            "underlying_price=excluded.underlying_price, delta=excluded.delta, "
            "gamma=excluded.gamma, theta=excluded.theta, vega=excluded.vega, "
            "implied_volatility=excluded.implied_volatility, updated_at=excluded.updated_at;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_int64 (stmt, 1, position_id);
        sqlite3_bind_text  (stmt, 2, ticker.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, underlying_price);
        sqlite3_bind_double(stmt, 4, delta);
        sqlite3_bind_double(stmt, 5, gamma);
        sqlite3_bind_double(stmt, 6, theta);
        sqlite3_bind_double(stmt, 7, vega);
        sqlite3_bind_double(stmt, 8, implied_volatility);
        sqlite3_bind_int64 (stmt, 9, static_cast<long>(std::time(nullptr)));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Phase 4, item 2: the options pre-order gate (main.cpp) consults this
    // before allowing a new order — reflects the breach found at the END of
    // the most recently completed monitor cycle (up to 5 min stale, same
    // staleness window as the mark-price refresh itself). Never blocks on a
    // query failure; a portfolio risk manager that occasionally can't fetch
    // its own snapshot should not itself become an outage.
    nox::risk::RiskBreach get_last_risk_breach() {
        std::lock_guard<std::mutex> lock(risk_snapshot_lock_);
        return last_risk_breach_;
    }

    nox::risk::RiskTargets get_risk_targets() const { return risk_targets_; }

    // Phase 1, item 4: trustworthy pre-order position-exists check. Backed by
    // open_positions, which the engine's reconciliation poll keeps in sync with
    // broker truth — so a `true` here reliably means "already holding this
    // contract" and a new order should be blocked (prevents double-entry).
    // Single-leg contracts only; multi-leg open-state is tracked via the ledger
    // signature (open_positions holds single-leg rows only).
    bool has_open_position(const std::string& ticker,
                           const std::string& option_type,
                           double strike,
                           const std::string& expiration_date) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql =
            "SELECT COUNT(*) FROM open_positions "
            "WHERE ticker = ? AND option_type = ? "
            "AND ABS(strike - ?) < 0.001 AND expiration_date = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return false; // fail-open: never block trading on a query error
        }
        sqlite3_bind_text  (stmt, 1, ticker.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, option_type.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, strike);
        sqlite3_bind_text  (stmt, 4, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count > 0;
    }

    // Multi-leg analogue of has_open_position (audit §4 C4). open_spread_positions
    // holds one row per open spread; a spread is dedup-identified by
    // underlying + strategy + expiration_date. Without this guard the reconciler
    // re-inserts a spread the recorder already booked ≤30s earlier (double-entry:
    // double exits, double-counted P&L). Fail-open on query error, same as the
    // single-leg check — never block on a DB hiccup.
    bool has_open_spread_position(const std::string& underlying,
                                  const std::string& strategy,
                                  const std::string& expiration_date) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql =
            "SELECT COUNT(*) FROM open_spread_positions "
            "WHERE underlying = ? AND strategy = ? AND expiration_date = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return false; // fail-open: never block trading on a query error
        }
        sqlite3_bind_text(stmt, 1, underlying.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, strategy.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count > 0;
    }

    // Reverse an optimistically-booked position when reconciliation proves the
    // originating order never actually filled (broker rejected/expired, or
    // canceled with filled_qty == 0). The execution recorder books a position
    // on the broker's ACCEPT ack (a 200/201), but an async rejection afterward
    // would otherwise leave a phantom the exit monitor trades against forever
    // AND the position-exists gate would keep blocking the signal from
    // regenerating. The pre-order gate guarantees at most one open position per
    // contract tuple, so keying removal on that tuple is unambiguous; callers
    // MUST gate on filled_qty == 0 so a partial fill is never reversed.
    // Returns the number of position rows removed (0 = nothing to reverse).
    int remove_phantom_single_leg(const std::string& ticker,
                                  const std::string& option_type,
                                  double strike,
                                  const std::string& expiration_date) {
        std::lock_guard<std::mutex> lock(db_lock_);
        // Clear live_greeks for the matching position(s) before deleting them.
        const char* gsql =
            "DELETE FROM live_greeks WHERE position_id IN "
            "(SELECT id FROM open_positions WHERE ticker=? AND option_type=? "
            " AND ABS(strike-?)<0.001 AND expiration_date=?);";
        sqlite3_stmt* gstmt = nullptr;
        if (sqlite3_prepare_v2(db_, gsql, -1, &gstmt, 0) == SQLITE_OK) {
            sqlite3_bind_text  (gstmt, 1, ticker.c_str(),          -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (gstmt, 2, option_type.c_str(),     -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(gstmt, 3, strike);
            sqlite3_bind_text  (gstmt, 4, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(gstmt);
        }
        sqlite3_finalize(gstmt);

        const char* sql =
            "DELETE FROM open_positions WHERE ticker=? AND option_type=? "
            "AND ABS(strike-?)<0.001 AND expiration_date=?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return 0;
        }
        sqlite3_bind_text  (stmt, 1, ticker.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, option_type.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, strike);
        sqlite3_bind_text  (stmt, 4, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        int removed = sqlite3_changes(db_);
        sqlite3_finalize(stmt);
        return removed;
    }

    // Multi-leg analogue. Spread live_greeks rows are keyed by the NEGATED
    // spread id (open_positions and open_spread_positions share the same
    // autoincrement space, so single-leg ids are positive and spread ids
    // negative in live_greeks — see monitor_positions()).
    int remove_phantom_spread(const std::string& underlying,
                              const std::string& strategy,
                              const std::string& expiration_date) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* gsql =
            "DELETE FROM live_greeks WHERE position_id IN "
            "(SELECT -id FROM open_spread_positions WHERE underlying=? AND strategy=? "
            " AND expiration_date=?);";
        sqlite3_stmt* gstmt = nullptr;
        if (sqlite3_prepare_v2(db_, gsql, -1, &gstmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(gstmt, 1, underlying.c_str(),      -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(gstmt, 2, strategy.c_str(),        -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(gstmt, 3, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(gstmt);
        }
        sqlite3_finalize(gstmt);

        const char* sql =
            "DELETE FROM open_spread_positions WHERE underlying=? AND strategy=? "
            "AND expiration_date=?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return 0;
        }
        sqlite3_bind_text(stmt, 1, underlying.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, strategy.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        int removed = sqlite3_changes(db_);
        sqlite3_finalize(stmt);
        return removed;
    }

private:

    void monitor_positions(); // Implementation will be in a .cpp file

    std::vector<OptionPosition> get_open_positions() {
        std::lock_guard<std::mutex> lock(db_lock_);
        std::vector<OptionPosition> positions;
        const char* sql = "SELECT id, ticker, option_type, strike, quantity, entry_price, entry_date, profile_type, expiration_date FROM open_positions;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                OptionPosition pos;
                pos.id = sqlite3_column_int(stmt, 0);
                pos.ticker = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                pos.option_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                pos.strike = sqlite3_column_double(stmt, 3);
                pos.quantity = sqlite3_column_int(stmt, 4);
                pos.entry_price = sqlite3_column_double(stmt, 5);
                pos.entry_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                pos.profile_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                pos.expiration_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
                positions.push_back(pos);
            }
        }
        sqlite3_finalize(stmt);
        return positions;
    }
    
    void remove_position(long position_id) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql = "DELETE FROM open_positions WHERE id = ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, position_id);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);

        const char* sql_greeks = "DELETE FROM live_greeks WHERE position_id = ?;";
        sqlite3_stmt* stmt2;
        if (sqlite3_prepare_v2(db_, sql_greeks, -1, &stmt2, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt2, 1, position_id);
            sqlite3_step(stmt2);
        }
        sqlite3_finalize(stmt2);
    }

    std::vector<SpreadPosition> get_open_spread_positions() {
        std::lock_guard<std::mutex> lock(db_lock_);
        std::vector<SpreadPosition> out;
        const char* sql =
            "SELECT id, underlying, strategy, quantity, entry_debit, entry_date, expiration_date, "
            "leg1_type, leg1_strike, leg1_side, leg2_type, leg2_strike, leg2_side, "
            "leg3_type, leg3_strike, leg3_side, leg4_type, leg4_strike, leg4_side "
            "FROM open_spread_positions;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                SpreadPosition p;
                p.id              = sqlite3_column_int(stmt, 0);
                p.underlying      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                p.strategy        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                p.quantity        = sqlite3_column_int(stmt, 3);
                p.entry_debit     = sqlite3_column_double(stmt, 4);
                p.entry_date      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                p.expiration_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                for (int i = 0; i < 4; ++i) {
                    int base = 7 + i * 3;
                    if (sqlite3_column_type(stmt, base) == SQLITE_NULL) continue;
                    SpreadLeg leg;
                    leg.option_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, base));
                    leg.strike       = sqlite3_column_double(stmt, base + 1);
                    leg.side         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, base + 2));
                    p.legs.push_back(leg);
                }
                out.push_back(std::move(p));
            }
        }
        sqlite3_finalize(stmt);
        return out;
    }

    void remove_spread_position(long position_id) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql = "DELETE FROM open_spread_positions WHERE id = ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, position_id);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);

        // Spread live_greeks rows are keyed by the NEGATED spread id (single-leg
        // and spread ids share one autoincrement space; negating keeps them from
        // colliding in live_greeks — see monitor_positions()).
        const char* sql_greeks = "DELETE FROM live_greeks WHERE position_id = ?;";
        sqlite3_stmt* stmt2;
        if (sqlite3_prepare_v2(db_, sql_greeks, -1, &stmt2, 0) == SQLITE_OK) {
            sqlite3_bind_int64(stmt2, 1, -static_cast<long long>(position_id));
            sqlite3_step(stmt2);
        }
        sqlite3_finalize(stmt2);
    }
};

#endif // POSITION_MANAGER_HPP