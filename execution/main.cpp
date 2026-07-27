#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "../shared/RegimeStateMachine.hpp"
#include "../shared/TelegramNotifier.hpp"
#include "../shared/TradingDayUtils.hpp"
#include "OptionEngine.hpp"
#include "EquitySignalGenerator.hpp"
#include "OptionsSignalGenerator.hpp"
#include "PositionManager.hpp"
#include "OrderLedger.hpp"
#include "IvRankStore.hpp"
#include "AlphaDecayStore.hpp"
#include "KillSwitchStore.hpp"
#include "MassiveFuturesClient.hpp"
#include "FuturesSignalStore.hpp"
#include "FuturesSignalGenerator.hpp"
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <string>
#include <cmath>
#include <thread>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <fstream>
#include <deque>

// ── IBKR execution path (compile with -DIBKR_ENABLED and TWS API on include path) ──
// Vendor TWS API: run execution/setup_ibkr_vendor.sh to download and unpack
// the IBKR C++ source under third_party/twsapi/source/cppclient/client/.
#ifdef IBKR_ENABLED
#include "IBKROrderRouter.hpp"
#endif

using json = nlohmann::json;
using TelegramNotifier = nox::TelegramNotifier;

// --- 2. UTILITIES & DATA ---
class Logger {
public:
    static void log(std::string level, std::string msg) {
        std::cout << "[" << level << "] " << msg << std::endl;
    }
};

struct TradeSignal {
    std::string ticker = "UNKNOWN";
    std::string action = "HOLD";
    double price = 0.0;
    double rsi = 50.0;
    long long vol = 0;
    double atr = 0.0;
    double stop_loss_atr_multiplier = 2.0; // Default, will be overridden by payload
    int risk_tier = 0; // Default 0 falls back to Kelly sizing
    double vix         = 20.0;  // Default neutral — cautious but not blocking
    double spy_price   = 0.0;
    double spy_200_sma = 0.0;
    // CN-RULE-001: Trade date used for T+1 enforcement.
    // Backtester feeds this field; live execution defaults to today's system date.
    std::string trade_date = "";
    // Human-readable origin of an internally-generated SELL (e.g. "rule:Take-profit
    // (+15.2%)", "trailing_stop_close"). Empty means an externally-supplied webhook
    // signal. Surfaced in the close reason, the T+1-block Telegram message, and
    // /recent-signals so an operator can distinguish sources at a glance.
    std::string source = "";
};

// Tracks the purchase date of an open A-share position.
// Used exclusively by the T+1 settlement gate (CN-RULE-002).
struct ChinaPositionRecord {
    std::string entry_date; // "YYYY-MM-DD" format
};

// --- 3. KELLY CALCULATOR ---
// Returns -1 to signal that sizing is not possible (negative Kelly = no edge).
// RULE-005: Negative Kelly must halt the trade, not be silently promoted to 1%.
// RULE-009: Kelly parameters are validated at startup (see NoxEngine ctor).
//
// kelly_fraction: the scaling multiplier applied to raw Kelly before the hard cap.
// This is injected at startup from KELLY_FRACTION env var (default 0.15).
//
// Why 0.15 and not 0.5 (half-Kelly)?
// With the OOS winner parameters (W=0.6842, R=2.316), raw Kelly = 54.8%.
// Half-Kelly (×0.5) = 27.4%, which always exceeds the 10% hard cap, effectively
// reducing Kelly to a flat 10%-of-equity rule on every single trade — the dynamic
// risk scaling that makes Kelly valuable is completely lost.
// At ×0.15, adjusted Kelly = 8.2%, which is below the 10% cap and scales properly
// with win rate and win/loss ratio as market conditions change.
int calculate_kelly_size(double equity, double current_price,
                         double win_rate, double win_loss_ratio,
                         double kelly_fraction) {
    if (current_price <= 0) {
        Logger::log("ERROR", "[KELLY] current_price is zero or negative — cannot size position.");
        return -1;
    }

    // The Kelly Formula: K% = W - ((1 - W) / R)
    double kelly_pct = win_rate - ((1.0 - win_rate) / win_loss_ratio);

    // RULE-005: A negative Kelly means the strategy has no mathematical edge.
    // Promoting it to 1% masks a broken strategy and risks real capital.
    if (kelly_pct <= 0.0) {
        Logger::log("CRITICAL", "[KELLY] Raw Kelly output is non-positive (" +
                    std::to_string(kelly_pct) + ") — no statistical edge. Trade halted.");
        return -1;
    }

    // Apply the configurable Kelly fraction to raw Kelly output.
    // KELLY_FRACTION is set in .env — default 0.15 gives adjusted Kelly = 8.2%
    // for current OOS parameters, safely below the 10% hard cap so Kelly
    // actually drives position sizing rather than being overridden every trade.
    double adjusted_risk = kelly_pct * kelly_fraction;
    Logger::log("INFO", "[KELLY] Raw Kelly: " + std::to_string(kelly_pct * 100.0) +
                "% | Fraction: " + std::to_string(kelly_fraction) +
                " | Adjusted: " + std::to_string(adjusted_risk * 100.0) + "%");

    // Hard cap: 10% max risk per trade (RULE-005).
    // This is a physical last-resort ceiling — under normal operation with
    // KELLY_FRACTION=0.15, adjusted_risk will be below this cap and this
    // branch will not fire.
    if (adjusted_risk > 0.10) {
        Logger::log("WARN", "[KELLY] Adjusted Kelly (" + std::to_string(adjusted_risk * 100.0) +
                    "%) exceeds 10% hard cap. Clamping to 10%. "
                    "Consider lowering KELLY_FRACTION.");
        adjusted_risk = 0.10;
    }

    double dollar_amount = equity * adjusted_risk;
    int shares = static_cast<int>(std::floor(dollar_amount / current_price));

    // Patch B — RULE-005 / RULE-018: If the Kelly dollar allocation is smaller
    // than the price of one share (shares == 0), the old code silently promoted
    // the result to 1 share. On a small account (e.g. $1 000) trading a high-
    // priced stock (e.g. $350), that forced purchase would represent 35 %+ of
    // total equity, completely bypassing the 10 % hard cap enforced above.
    //
    // Correct behaviour: return -1 ("no valid sizing") so the caller aborts the
    // trade — identical to the negative-Kelly path — and logs a CRITICAL alert.
    if (shares <= 0) {
        Logger::log("CRITICAL",
            "[KELLY] Calculated share allocation is 0. "
            "Forcing 1-share purchase would violate the 10 % portfolio risk cap. "
            "Aborting trade.");
        return -1;
    }

    return shares;
}

// --- 4. THE ENGINE ---
class NoxEngine {
private:
    std::string secret;
    std::string apiKey;
    std::string apiSec;
    std::string alpacaBaseUrl;
    double      kellyWinRate;
    double      kellyWinLossRatio;
    double      kellyFraction;
    int         cnBoardLotSize;   // CN-RULE-001: configurable via CN_BOARD_LOT_SIZE (default 1)
    std::string cnPositionsPath;  // path for T+1 persistence file
    std::string equityPositionsPath_; // path for equity trailing-stop tracking persistence
    std::string memory_bank_path;     // path for options position tracking and trade ledger
    RegimeStateMachine regimeMachine;
    std::string last_analyst_report_time;

    // WS5 — pre-execution microstructure gate (rolling per-symbol spread baseline)
    nox::liquidity::LiquidityGate liquidity_gate_;

    // Position Manager (for options)
    std::unique_ptr<PositionManager> positionManager_;

    // Phase 1 ghost-fill infrastructure: the client-order-ID ledger (shared by
    // both options threads and the equity path) + the reconciliation throttle.
    std::unique_ptr<nox::execution::OrderLedger> orderLedger_;
    std::unique_ptr<nox::execution::IvRankStore> ivRankStore_;
    std::unique_ptr<nox::execution::AlphaDecayStore> alphaDecayStore_;
    std::shared_ptr<nox::execution::FuturesSignalStore> futuresSignalStore_;
    std::shared_ptr<nox::options_router::OptionsOrderRouter> optionsOrderRouter_; // for reconciliation lookups
    static constexpr int RECON_COOLDOWN_SECONDS = 30; // min gap between reconcile polls
    static constexpr int RECON_GRACE_SECONDS    = 75; // wait before declaring a 404 order 'failed'

    // Global kill switch: persisted halt on ALL new-entry order submission
    // (equity + options), triggered by /pause (operator, via Telegram ->
    // monitor.py -> POST /pause) or automatically by check_daily_loss_limit().
    // Existing positions are never touched — only new entries are blocked.
    std::unique_ptr<nox::execution::KillSwitchStore> killSwitch_;
    std::atomic<long> lastKillSwitchCheck_{0};
    static constexpr int KILL_SWITCH_CHECK_COOLDOWN_SECONDS = 60;
    // Fake-safe default: -$1500/day. Real value should be tuned to the
    // account's actual risk tolerance via MAX_DAILY_LOSS_DOLLARS.
    double maxDailyLossDollars_ = -1500.0;

    // Options signal scanner profiles (configured from env vars in the constructor)
    nox::options_signal::RiskProfile optionsBotProfile_;
    nox::options_signal::RiskProfile optionsPersonalProfile_;
    nox::options_signal::RiskProfile optionsBreakoutProfile_;

    // Equity signal scanner config (independent of Skeptic)
    std::vector<std::string> equityWatchlist_;
    int    equityScanIntervalMinutes_ = 30;
    int    equityMaxSignals_          = 2;
    bool   equityScanEnabled_         = true;
    bool   equityBypassHours_         = false;

    // Futures signal scanner config — CLAUDE.md futures phase 1: signals only,
    // no order routing. Default OFF (unlike equity's default-on) since it
    // needs a paid Massive Futures API key the user opts into separately.
    std::vector<std::string> futuresWatchlist_;
    int    futuresScanIntervalMinutes_ = 60;
    double futuresAlertThreshold_      = 0.5;
    bool   futuresScanEnabled_         = false;
    std::string massiveApiKey_;

    // ── Rule-based equity exit config ─────────────────────────────────────────
    // Evaluated by the trailing-stop monitor thread on each 5-min cycle for every
    // open equity position. These are the strategy-consistent inverse of the entry
    // rules (trend-following momentum): take profit, hard-stop backup, RSI
    // exhaustion, and trend break below SMA20. Each rule is individually
    // configurable; set a threshold to 0 (or the toggle to false) to disable it.
    bool   equityRuleExitsEnabled_  = true;
    double equityExitTakeProfitPct_ = 0.15; // +15% unrealized → take profit
    double equityExitStopLossPct_   = 0.10; // -10% unrealized → hard-stop backup
    double equityExitRsiCeiling_    = 78.0; // RSI ≥ ceiling → momentum exhausted
    bool   equityExitSmaBreak_      = true; // close < SMA20 → uptrend broken
    int    equityExitMaxHoldDays_   = 0;    // 0 = disabled
    std::string equitySellQtyMode_  = "full"; // "full" | "kelly" | "prorated"

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    // SIGTERM sets running_ = false and calls shutdown(), which stops the HTTP
    // server and wakes the options scanner threads so they can exit their
    // wait_for() sleep rather than blocking until the next scan interval.
    std::atomic<bool>       running_{true};
    std::mutex              stop_mutex_;
    std::condition_variable stop_cv_;
    std::vector<std::thread> option_threads_;
    httplib::Server*        svr_ptr_{nullptr}; // set in run() before listen()

    static NoxEngine* s_instance_;

    static void handle_signal(int) {
        if (s_instance_) s_instance_->shutdown();
    }

    void shutdown() {
        if (running_.exchange(false)) {
            if (svr_ptr_) svr_ptr_->stop();
            stop_cv_.notify_all();
        }
    }

    // ── Inbound signal log (last 50 authenticated signals, newest at back) ─────
    struct SignalLogEntry {
        std::string received_at;
        std::string ticker;
        std::string action;
        double price = 0.0;
        double rsi   = 0.0;
        double vix   = 0.0;
        std::string source; // origin tag for internally-generated SELLs; empty = webhook
    };
    std::deque<SignalLogEntry> signal_log_;
    std::mutex                 signal_log_mutex_;
    static constexpr std::size_t SIGNAL_LOG_MAX = 50;

    // ── Order idempotency cache (prevent duplicate orders from retried signals) ──
    // Maps idempotency key (hash of ticker+action+price) to timestamp of last order
    // placed. If same signal received within IDEMPOTENCY_WINDOW_SECONDS, reject it.
    struct IdempotencyCacheEntry {
        std::chrono::system_clock::time_point last_seen;
        std::string order_id;
    };
    std::unordered_map<std::string, IdempotencyCacheEntry> idempotency_cache_;
    std::mutex idempotency_cache_mutex_;
    static constexpr int IDEMPOTENCY_WINDOW_SECONDS = 300; // 5 minute window

    void record_signal(const TradeSignal& s) {
        auto now    = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        std::ostringstream ts;
        ts << std::put_time(::gmtime_r(&time_t, &tm_buf), "%Y-%m-%dT%H:%M:%SZ");
        std::lock_guard<std::mutex> lock(signal_log_mutex_);
        signal_log_.push_back({ts.str(), s.ticker, s.action, s.price, s.rsi, s.vix, s.source});
        if (signal_log_.size() > SIGNAL_LOG_MAX)
            signal_log_.pop_front();
    }

    // Generate idempotency key from signal (ticker+action+price)
    // Hash prevents duplicate orders within IDEMPOTENCY_WINDOW_SECONDS
    std::string generate_idempotency_key(const TradeSignal& sig) {
        std::ostringstream oss;
        oss << sig.ticker << "|" << sig.action << "|" << std::fixed << std::setprecision(2) << sig.price;
        std::hash<std::string> hasher;
        return std::to_string(hasher(oss.str()));
    }

    // Check if this signal is a duplicate within the idempotency window
    // Returns pair: (is_duplicate, cached_order_id_if_any)
    std::pair<bool, std::string> check_idempotency(const TradeSignal& sig) {
        std::string key = generate_idempotency_key(sig);
        auto now = std::chrono::system_clock::now();

        std::lock_guard<std::mutex> lock(idempotency_cache_mutex_);

        // Clean old entries
        auto it = idempotency_cache_.begin();
        while (it != idempotency_cache_.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.last_seen).count();
            if (age > IDEMPOTENCY_WINDOW_SECONDS) {
                it = idempotency_cache_.erase(it);
            } else {
                ++it;
            }
        }

        auto cache_it = idempotency_cache_.find(key);
        if (cache_it != idempotency_cache_.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - cache_it->second.last_seen).count();
            if (age < IDEMPOTENCY_WINDOW_SECONDS) {
                return {true, cache_it->second.order_id}; // DUPLICATE!
            }
        }
        return {false, ""};
    }

    // Record this order in the idempotency cache
    void record_idempotency(const TradeSignal& sig, const std::string& order_id) {
        std::string key = generate_idempotency_key(sig);
        std::lock_guard<std::mutex> lock(idempotency_cache_mutex_);
        idempotency_cache_[key] = {std::chrono::system_clock::now(), order_id};
    }

    // ── IBKR execution venue (compiled in only when IBKR_ENABLED is defined) ───
    // Set EXECUTION_VENUE=ibkr in the environment to activate. Defaults to alpaca.
    // Requires IB Gateway (paper port 4002 / live port 4001) reachable on the
    // Docker network.
#ifdef IBKR_ENABLED
    std::string execution_venue_;
    std::unique_ptr<nox::ibkr::IBKRWrapper>     ibkr_wrapper_;
    std::unique_ptr<nox::ibkr::IBKRConnection>  ibkr_conn_;
    std::unique_ptr<nox::ibkr::IBKROrderRouter> ibkr_router_;
#endif

    // CN-RULE-002: T+1 position state — maps ticker → entry date.
    // Written on confirmed BUY, read & evicted on confirmed SELL.
    // Persisted to cnPositionsPath so state survives engine restarts.
    // Guarded by mutex because httplib dispatches concurrent handler threads.
    std::map<std::string, ChinaPositionRecord> china_positions_;
    std::mutex china_positions_mutex_;

    // ── Trailing Stop Monitoring ─────────────────────────────────────────────
    // Tracks open equity positions so we can detect when trailing stops execute.
    // When a position disappears from Alpaca, we know the stop was hit.
    struct OpenEquityPosition {
        std::string ticker;
        int quantity;
        double entry_price;
        std::chrono::system_clock::time_point entry_time;
        // Last values seen from Alpaca while the position was still open. When the
        // position later disappears (trailing stop hit), last_pnl is the best
        // available estimate of realized P&L for the trade ledger.
        double last_price = 0.0;
        double last_pnl   = 0.0;
    };

    std::map<std::string, OpenEquityPosition> equity_positions_;
    std::mutex equity_positions_mutex_;

    // Returns today's date as "YYYY-MM-DD" anchored to US Eastern (the market this
    // engine actually trades on via Alpaca) — NOT the container's local timezone.
    // Two bugs this fixes:
    //   1. std::localtime() returns a pointer into a single process-wide static
    //      buffer — not thread-safe. This is called from both the webhook handler
    //      thread and the monitor thread; a race could corrupt the date the
    //      CN-RULE-002 T+1 gate compares against. gmtime_r (used here, and already
    //      used elsewhere in this file, e.g. record_signal()) is reentrant.
    //   2. The execution-engine container sets no TZ (defaults to UTC on Ubuntu),
    //      unlike analyst-brain which explicitly sets TZ=America/New_York. UTC's
    //      calendar date flips 4-5 hours before US Eastern's does, so a SELL
    //      arriving in the US evening could compute "tomorrow" in UTC while it's
    //      still "today" by US trading-day convention — letting a same-day
    //      round-trip silently slip past the T+1 gate. Anchoring to ET (via the
    //      same approximate DST offset already used by is_us_market_hours() and
    //      EquitySignalGenerator::isMarketHours()) fixes this for the market this
    //      engine actually trades on today. A real Beijing-time A-share venue
    //      would need a per-venue offset — out of scope until that venue exists.
    static std::string get_today_date_string() {
        auto now    = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&time_t, &utc);
        int offset_h = (utc.tm_mon >= 3 && utc.tm_mon <= 9) ? 4 : 5; // EDT vs EST
        std::time_t et_time = time_t - offset_h * 3600;
        std::tm et{};
        gmtime_r(&et_time, &et);
        std::ostringstream oss;
        oss << std::put_time(&et, "%Y-%m-%d");
        return oss.str();
    }

    // The call/put + buy/sell layout each routeXXX() in OptionsOrderRouter
    // submits — kept in one place so the execution recorder (fresh signal) and
    // the ghost-fill reconciliation path (recovered from the ledger) build the
    // exact same leg structure a closing order will need to reverse later.
    // REVERSE_IRON_CONDOR needs strike3/strike4, which the ledger's Order
    // struct doesn't carry — callers reconstructing from ledger rows only
    // (not a live OptionsSignal) will get an empty result for it.
    static std::vector<SpreadLeg> spread_legs_for(const std::string& strategy,
                                                  double strike, double strike2,
                                                  double strike3, double strike4) {
        if (strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD") {
            std::string t = (strategy == "BULL_CALL_SPREAD") ? "call" : "put";
            return {{t, strike, "buy"}, {t, strike2, "sell"}};
        }
        if (strategy == "STRADDLE") {
            return {{"call", strike, "buy"}, {"put", strike, "buy"}};
        }
        if (strategy == "STRANGLE") {
            // Short strangle (income thesis, see selectStrategy's prefer_sell
            // branch) — both legs sold for a credit, mirroring routeStrangle().
            return {{"call", strike, "sell"}, {"put", strike2, "sell"}};
        }
        if (strategy == "REVERSE_IRON_CONDOR" && strike3 != 0.0 && strike4 != 0.0) {
            return {{"call", strike, "buy"}, {"call", strike2, "sell"},
                    {"put", strike3, "buy"}, {"put", strike4, "sell"}};
        }
        return {};
    }

    // SpreadPosition::entry_debit uses the same buy(+)/sell(-) sign convention
    // PositionManager's exit monitor uses for net_value, so pnl = net_value -
    // entry_debit is correct for both net-debit (spreads/straddle) and
    // net-credit (short strangle) structures without a separate code path.
    // `magnitude` is the always-positive premium OptionsSignal reports.
    static double signed_entry_debit_for(const std::vector<SpreadLeg>& legs, double magnitude) {
        bool all_sell = !legs.empty() &&
            std::all_of(legs.begin(), legs.end(),
                        [](const SpreadLeg& l) { return l.side == "sell"; });
        return all_sell ? -magnitude : magnitude;
    }

    // Phase 1 (equity path): a deterministic client_order_id for broker-side
    // idempotency. Minute-bucketed so a network retry of the same order reproduces
    // the same id (Alpaca rejects the duplicate) while a legitimate later order
    // gets a fresh one. `tag` distinguishes entry vs stop vs exit on the same name.
    static std::string makeEquityClientOid(const std::string& ticker,
                                           const std::string& tag) {
        long epoch = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        std::ostringstream oss;
        oss << "nox-e-" << ticker << "-" << tag << "-" << (epoch / 60);
        return oss.str();
    }

    // Writes the current china_positions_ map to disk as JSON.
    // Called under china_positions_mutex_ — callers must already hold the lock.
    void persist_china_positions_locked() {
        try {
            json j = json::object();
            for (const auto& kv : china_positions_) {
                j[kv.first] = kv.second.entry_date;
            }
            std::ofstream f(cnPositionsPath, std::ios::trunc);
            if (f.is_open()) {
                f << j.dump(2);
            } else {
                Logger::log("WARN", "[CN-RULE-002] Could not open " + cnPositionsPath + " for writing.");
            }
        } catch (const std::exception& e) {
            Logger::log("WARN", "[CN-RULE-002] Failed to persist positions: " + std::string(e.what()));
        }
    }

    // Loads china_positions_ from disk at startup.
    // Removes entries whose entry_date is strictly before today — they are at
    // minimum T+1 old and the T+1 sell gate no longer applies to them.
    void load_china_positions() {
        std::lock_guard<std::mutex> lock(china_positions_mutex_);
        std::ifstream f(cnPositionsPath);
        if (!f.is_open()) {
            Logger::log("INFO", "[CN-RULE-002] No persisted positions file found at " +
                        cnPositionsPath + ". Starting with empty T+1 map.");
            return;
        }
        try {
            json j = json::parse(f);
            std::string today = get_today_date_string();
            int loaded = 0, pruned = 0;
            for (auto it = j.begin(); it != j.end(); ++it) {
                std::string ticker     = it.key();
                std::string entry_date = it.value().get<std::string>();
                if (entry_date < today) {
                    // Entry is from a previous day — T+1 restriction has lifted.
                    pruned++;
                } else {
                    china_positions_[ticker] = ChinaPositionRecord{entry_date};
                    loaded++;
                }
            }
            Logger::log("INFO", "[CN-RULE-002] Loaded " + std::to_string(loaded) +
                        " active T+1 position(s) from disk; pruned " +
                        std::to_string(pruned) + " stale record(s).");
        } catch (const std::exception& e) {
            Logger::log("WARN", "[CN-RULE-002] Failed to parse positions file: " +
                        std::string(e.what()) + ". Starting with empty T+1 map.");
        }
    }

    // Writes equity_positions_ to disk as JSON. Caller must hold equity_positions_mutex_.
    // Persisting this map lets the trailing-stop monitor survive restarts instead of
    // forgetting every open position (which orphaned trailing-stop fills before).
    void persist_equity_positions_locked() {
        try {
            json j = json::object();
            for (const auto& kv : equity_positions_) {
                j[kv.first] = {
                    {"quantity",    kv.second.quantity},
                    {"entry_price", kv.second.entry_price}
                };
            }
            std::ofstream f(equityPositionsPath_, std::ios::trunc);
            if (f.is_open()) f << j.dump(2);
        } catch (const std::exception& e) {
            Logger::log("WARN", "[TRAILING_STOP_MONITOR] Failed to persist equity positions: " +
                        std::string(e.what()));
        }
    }

    // Loads equity_positions_ from disk, then RECONCILES against Alpaca so the map
    // always reflects broker reality after a restart:
    //   • Alpaca position not in the map  → adopt it (recover entry from disk if known,
    //     else use Alpaca's average entry price) so its trailing stop is monitored.
    //   • Map entry no longer open at Alpaca → drop it (already closed while we were down).
    void load_and_reconcile_equity_positions() {
        std::map<std::string, OpenEquityPosition> from_disk;
        std::ifstream f(equityPositionsPath_);
        if (f.is_open()) {
            try {
                json j = json::parse(f);
                for (auto it = j.begin(); it != j.end(); ++it) {
                    OpenEquityPosition p;
                    p.ticker      = it.key();
                    p.quantity    = it.value().value("quantity", 0);
                    p.entry_price = it.value().value("entry_price", 0.0);
                    p.entry_time  = std::chrono::system_clock::now();
                    from_disk[it.key()] = p;
                }
            } catch (const std::exception& e) {
                Logger::log("WARN", "[TRAILING_STOP_MONITOR] Could not parse equity positions file: " +
                            std::string(e.what()));
            }
        }

        bool fetch_ok = false;
        auto live = fetch_open_positions_map(fetch_ok);
        std::lock_guard<std::mutex> lock(equity_positions_mutex_);
        if (!fetch_ok) {
            // Alpaca unreachable at startup — keep whatever we loaded from disk rather
            // than wiping tracking. The monitor will reconcile naturally once online.
            equity_positions_ = from_disk;
            Logger::log("WARN", "[TRAILING_STOP_MONITOR] Alpaca unreachable at startup — "
                        "using " + std::to_string(from_disk.size()) +
                        " position(s) from disk pending live reconcile.");
            return;
        }
        equity_positions_.clear();
        int adopted = 0;
        for (const auto& kv : live) {
            const std::string& ticker = kv.first;
            OpenEquityPosition p;
            p.ticker     = ticker;
            p.quantity   = static_cast<int>(kv.second.qty);
            auto d = from_disk.find(ticker);
            p.entry_price = (d != from_disk.end() && d->second.entry_price > 0.0)
                                ? d->second.entry_price
                                : kv.second.avg_entry;
            p.entry_time  = std::chrono::system_clock::now();
            p.last_price  = kv.second.current_price;
            p.last_pnl    = kv.second.unrealized_pl;
            equity_positions_[ticker] = p;
            adopted++;
        }
        persist_equity_positions_locked();
        Logger::log("INFO", "[TRAILING_STOP_MONITOR] Reconciled equity positions with Alpaca — " +
                    std::to_string(adopted) + " open position(s) now tracked.");
    }

    // ── Phase 1, item 2: options order reconciliation ─────────────────────────
    //
    // Called at scan-cycle start (and once at startup). For every ledger row still
    // 'pending'/'unknown', ask the broker "what actually happened to this order?"
    // by its client_order_id, and bring local truth back in sync — the core
    // ghost-fill fix. Idempotent (broker-truth-derived, add_position guarded by
    // has_open_position), so it is safe even if it races or repeats.
    //
    // The two options threads share one ledger; the throttle keeps only one poll
    // running per RECON_COOLDOWN_SECONDS window (efficiency, not correctness).
    void reconcile_options_orders() {
        if (!orderLedger_ || !optionsOrderRouter_ || !positionManager_) return;
        if (!orderLedger_->shouldReconcileNow(RECON_COOLDOWN_SECONDS)) return;

        auto pending = orderLedger_->getUnresolved();
        if (pending.empty()) return;

        long now = nox::execution::OrderLedger::now_epoch();
        int resolved_filled = 0, resolved_failed = 0, still_open = 0;

        for (const auto& o : pending) {
            auto st = optionsOrderRouter_->getOrderByClientId(o.client_oid);
            long age = now - o.sent_at;

            if (!st.reachable) {
                // Network/parse error — never self-inflict 'failed'; retry next cycle.
                still_open++;
                continue;
            }

            if (!st.found) {
                // Broker reached but has no such order.
                if (age >= RECON_GRACE_SECONDS) {
                    // The order never landed → free to regenerate (item 6 logging).
                    orderLedger_->setStatus(o.client_oid, "failed");
                    resolved_failed++;
                    Logger::log("WARN", "[RECONCILE] " + o.ticker + " / " + o.strategy +
                        " (" + o.client_oid + ") never reached broker after " +
                        std::to_string(age) + "s — marked failed; signal free to regenerate.");
                    TelegramNotifier::sendMessage(
                        "🔁 *ORDER NEVER LANDED — " + o.ticker + "*\n"
                        "────────────────────────\n"
                        "• *Strategy:* " + o.strategy + "\n"
                        "• *Client OID:* `" + o.client_oid + "`\n"
                        "No broker record after " + std::to_string(age) +
                        "s. Marked failed — the next scan may regenerate it.");
                } else {
                    still_open++; // within grace — recheck next cycle
                }
                continue;
            }

            // Broker returned a record — act on its status.
            const std::string& s = st.status;
            if (s == "filled") {
                // Genuinely TERMINAL fill at the broker. Upgrade the ledger and
                // make sure the position is tracked for exits (double-entry guarded
                // by has_open_position / has_open_spread_position). Ghost-fill catch.
                // NOTE (audit §4 H1): only "filled" is terminal here. accepted/new/
                // pending_new/partially_filled are WORKING states handled below —
                // treating them as fills booked phantom positions for async-rejected
                // orders and booked full qty on partials.
                orderLedger_->setStatus(o.client_oid, "filled", st.broker_order_id);
                resolved_filled++;
                bool single_leg = (o.strategy == "LONG_CALL" || o.strategy == "LONG_PUT" ||
                                   o.strategy == "CSP"       || o.strategy == "CC");
                if (single_leg && !o.option_type.empty() &&
                    !positionManager_->has_open_position(o.ticker, o.option_type,
                                                         o.strike, o.expiration_date)) {
                    std::string entry_date = get_today_date_string();
                    positionManager_->add_position(
                        o.ticker, o.option_type, o.strike,
                        static_cast<int>(o.qty), o.entry_price, entry_date,
                        o.profile_type.empty() ? "long" : o.profile_type,
                        o.expiration_date);
                    Logger::log("INFO", "[RECONCILE] Recovered ghost fill for " + o.ticker +
                        " / " + o.strategy + " (" + o.client_oid + ") — position now tracked.");
                    TelegramNotifier::sendMessage(
                        "👻 *GHOST FILL RECOVERED — " + o.ticker + "*\n"
                        "────────────────────────\n"
                        "• *Strategy:* " + o.strategy + "\n"
                        "• *Broker Order:* `" + st.broker_order_id + "`\n"
                        "Order filled at the broker but our confirmation was lost. "
                        "Now tracked for exit — double-entry prevented.");
                } else if (!single_leg) {
                    // Ledger's Order struct carries only strike/strike2, so
                    // REVERSE_IRON_CONDOR (needs strike3/strike4) can't be fully
                    // reconstructed here — spread_legs_for() returns empty for it
                    // and this ghost-fill stays untracked, same as before.
                    auto legs = spread_legs_for(o.strategy, o.strike, o.strike2, 0.0, 0.0);
                    if (!legs.empty() &&
                        !positionManager_->has_open_spread_position(
                            o.ticker, o.strategy, o.expiration_date)) {
                        std::string entry_date = get_today_date_string();
                        positionManager_->add_spread_position(
                            o.ticker, o.strategy, static_cast<int>(o.qty),
                            signed_entry_debit_for(legs, o.entry_price),
                            entry_date, o.expiration_date, legs);
                        Logger::log("INFO", "[RECONCILE] Recovered multi-leg ghost fill for " +
                            o.ticker + " / " + o.strategy + " (" + o.client_oid + ") — now tracked.");
                        TelegramNotifier::sendMessage(
                            "👻 *GHOST FILL RECOVERED — " + o.ticker + "*\n"
                            "────────────────────────\n"
                            "• *Strategy:* " + o.strategy + " (multi-leg)\n"
                            "• *Broker Order:* `" + st.broker_order_id + "`\n"
                            "Order filled at the broker but our confirmation was lost. "
                            "Now tracked for exit.");
                    }
                }
            } else if (s == "partially_filled" || s == "accepted" || s == "new" ||
                       s == "pending_new"     || s == "pending_replace") {
                // Working / not-yet-terminal at the broker (audit §4 H1). Not a
                // fill — leave the ledger 'pending' and recheck next cycle rather
                // than booking a phantom position. (A partial that never completes
                // stays untracked here; booking full qty on first partial was the
                // worse bug this replaces.)
                still_open++;
            } else if (s == "canceled" || s == "rejected" || s == "expired") {
                orderLedger_->setStatus(o.client_oid, "failed", st.broker_order_id);
                resolved_failed++;
                Logger::log("INFO", "[RECONCILE] " + o.ticker + " / " + o.strategy +
                    " (" + o.client_oid + ") broker status=" + s + " — marked failed.");
                // The execution recorder books a position on the broker's ACCEPT
                // ack; if that order is later rejected/expired/canceled without
                // filling, that booked position is a phantom — reverse it so the
                // exit monitor doesn't trade a position the broker never opened
                // and the position-exists gate stops blocking regeneration.
                // Only reverse on a CONFIRMED zero fill; a partial fill is real.
                if (positionManager_) {
                    if (st.filled_qty > 0.0) {
                        Logger::log("WARN", "[RECONCILE] " + o.ticker + " / " + o.strategy +
                            " status=" + s + " but filled_qty=" + std::to_string(st.filled_qty) +
                            " — NOT reversing (partial fill is a real position).");
                        TelegramNotifier::sendMessage(
                            "⚠️ *PARTIAL FILL THEN " + s + " — MANUAL CHECK — " + o.ticker + "*\n"
                            "────────────────────────\n"
                            "• *Strategy:* " + o.strategy + "\n"
                            "• *Filled qty:* " + std::to_string(st.filled_qty) + "\n"
                            "The order " + s + " after a partial fill. The booked position was "
                            "left in place — verify the real fill quantity at the broker.");
                    } else {
                        bool single_leg = (o.strategy == "LONG_CALL" || o.strategy == "LONG_PUT" ||
                                           o.strategy == "CSP"       || o.strategy == "CC");
                        int removed = single_leg
                            ? positionManager_->remove_phantom_single_leg(
                                  o.ticker, o.option_type, o.strike, o.expiration_date)
                            : positionManager_->remove_phantom_spread(
                                  o.ticker, o.strategy, o.expiration_date);
                        if (removed > 0) {
                            Logger::log("WARN", "[RECONCILE] Reversed " + std::to_string(removed) +
                                " phantom position(s) for " + o.ticker + " / " + o.strategy +
                                " (order " + s + " with zero fill).");
                            TelegramNotifier::sendMessage(
                                "🧹 *PHANTOM POSITION REVERSED — " + o.ticker + "*\n"
                                "────────────────────────\n"
                                "• *Strategy:* " + o.strategy + "\n"
                                "• *Broker status:* " + s + " (0 filled)\n"
                                "The order was booked on broker ACCEPT but never filled. "
                                "The optimistic position has been removed — no double-entry, "
                                "signal free to regenerate.");
                        }
                    }
                }
            } else {
                still_open++; // unknown/transient broker status — recheck next cycle
            }
        }

        Logger::log("INFO", "[RECONCILE] Options order reconciliation: " +
            std::to_string(resolved_filled) + " filled, " +
            std::to_string(resolved_failed) + " failed, " +
            std::to_string(still_open) + " still pending.");
    }

    // Automatic half of the global kill switch: reads today's realized+
    // unrealized P&L from daily_ledger (Phase 2) and pauses new entries if it
    // breaches MAX_DAILY_LOSS_DOLLARS (fake-safe default -$1500). Throttled
    // like reconcile_options_orders() so both options threads sharing this
    // call site don't hammer sqlite every scan. Never un-pauses on its own —
    // only /resume (operator judgment) clears a loss-limit halt, since the
    // fact that P&L ticked back above the threshold intraday doesn't mean the
    // day's risk event is over.
    void check_daily_loss_limit() {
        if (!killSwitch_ || !positionManager_) return;
        long now = nox::execution::OrderLedger::now_epoch();
        long last = lastKillSwitchCheck_.load();
        if (now - last < KILL_SWITCH_CHECK_COOLDOWN_SECONDS) return;
        lastKillSwitchCheck_.store(now);

        if (killSwitch_->isPaused()) return; // already halted — nothing to do

        double pnl_today = positionManager_->get_total_daily_pnl(get_today_date_string());
        if (pnl_today > maxDailyLossDollars_) return; // within limit (limit is negative)

        std::string reason = "Daily P&L $" + std::to_string(pnl_today) +
            " breached limit $" + std::to_string(maxDailyLossDollars_);
        killSwitch_->pause(reason, "daily_loss_limit");
        Logger::log("CRITICAL", "[KILL_SWITCH] " + reason + " — all new entries halted.");
        TelegramNotifier::sendMessage(
            "🛑 *KILL SWITCH TRIGGERED — DAILY LOSS LIMIT*\n"
            "────────────────────────\n"
            "• *Today's P&L:* $" + std::to_string(pnl_today) + "\n"
            "• *Limit:* $" + std::to_string(maxDailyLossDollars_) + "\n"
            "⛔ All NEW entries halted (equity + options). Existing positions "
            "are untouched. Send /resume once you've reviewed what happened."
        );
    }

    // A live Alpaca position snapshot used for reconciliation and P&L tracking.
    struct AlpacaPositionSnapshot {
        double qty           = 0.0;
        double avg_entry     = 0.0;
        double current_price = 0.0;
        double unrealized_pl = 0.0;
    };

    // Fetch all open positions from Alpaca, keyed by symbol. Sets ok=false on any
    // fetch/parse failure so callers can distinguish "no open positions" (ok=true,
    // empty) from "couldn't reach Alpaca" (ok=false) — critical so a transient
    // outage is never misread as every position having closed.
    std::map<std::string, AlpacaPositionSnapshot> fetch_open_positions_map(bool& ok) {
        std::map<std::string, AlpacaPositionSnapshot> out;
        ok = false;
        try {
            httplib::Client alpaca_cli(alpacaBaseUrl);
            alpaca_cli.set_connection_timeout(std::chrono::seconds(5));
            alpaca_cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey},
                {"APCA-API-SECRET-KEY", apiSec}
            };

            auto res = alpaca_cli.Get("/v2/positions", headers);
            if (!res || res->status != 200) {
                Logger::log("WARN", "[TRAILING_STOP_MONITOR] Failed to fetch positions (HTTP " +
                            std::to_string(res ? res->status : 0) + ")");
                return out;
            }

            json positions = json::parse(res->body);
            if (positions.is_array()) {
                for (const auto& pos : positions) {
                    // /v2/positions returns BOTH equities and options in one array.
                    // Options are tracked/closed exclusively through PositionManager +
                    // OptionsOrderRouter — letting one leak in here means this map's
                    // consumers (trailing-stop monitor, equity exit rules, the equity
                    // close path) apply equity semantics (market sell qty, stop-loss %)
                    // to an option contract, which the broker rejects every cycle.
                    if (pos.value("asset_class", "") != "us_equity") continue;
                    std::string ticker = pos.value("symbol", "");
                    if (ticker.empty()) continue;
                    AlpacaPositionSnapshot s;
                    // Alpaca returns these numeric fields as JSON strings.
                    try { s.qty           = std::stod(pos.value("qty", "0")); }           catch (...) {}
                    try { s.avg_entry     = std::stod(pos.value("avg_entry_price", "0")); } catch (...) {}
                    try { s.current_price = std::stod(pos.value("current_price", "0")); }  catch (...) {}
                    try { s.unrealized_pl = std::stod(pos.value("unrealized_pl", "0")); }  catch (...) {}
                    out[ticker] = s;
                }
            }
            ok = true; // reached Alpaca and parsed the array (possibly empty)
        } catch (const std::exception& e) {
            Logger::log("WARN", "[TRAILING_STOP_MONITOR] Exception fetching positions: " +
                        std::string(e.what()));
        }
        return out;
    }

    // Monitor equity positions and detect trailing stop executions
    // Approximate US market-hours check (Mon–Fri 09:30–16:00 ET, DST-approx).
    // Used to gate rule-based liquidations so we never fire a market order after
    // hours (where it would reject or fill at a bad open).
    static bool is_us_market_hours() {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&tt, &utc);
        if (utc.tm_wday == 0 || utc.tm_wday == 6) return false;
        int offset_h = (utc.tm_mon >= 3 && utc.tm_mon <= 9) ? 4 : 5; // EDT vs EST
        int et_mins  = ((utc.tm_hour - offset_h + 24) % 24) * 60 + utc.tm_min;
        return et_mins >= 9 * 60 + 30 && et_mins < 16 * 60;
    }

    // Rule-based equity exit evaluation. For each open tracked position, apply the
    // configured exit rules and liquidate the first one that fires. Price rules
    // (take-profit / stop-loss / time) are checked first with no network cost;
    // indicator rules (RSI exhaustion, SMA20 trend break) fetch the same bars the
    // entry used and are only consulted if no price rule triggered.
    void evaluate_equity_exit_rules(const std::map<std::string, AlpacaPositionSnapshot>& current) {
        if (!equityRuleExitsEnabled_) return;
        if (!equityBypassHours_ && !is_us_market_hours()) return; // don't liquidate after hours

        struct Candidate {
            std::string ticker;
            double entry;
            double price;
            std::chrono::system_clock::time_point entry_time;
        };
        std::vector<Candidate> cands;
        {
            std::lock_guard<std::mutex> lock(equity_positions_mutex_);
            for (const auto& kv : equity_positions_) {
                auto it = current.find(kv.first);
                if (it == current.end()) continue; // not currently open at Alpaca
                cands.push_back({kv.first, kv.second.entry_price,
                                 it->second.current_price, kv.second.entry_time});
            }
        }

        auto pct = [](double v) {
            std::ostringstream o; o << std::showpos << std::fixed << std::setprecision(1) << v * 100.0 << "%";
            return o.str();
        };

        for (const auto& c : cands) {
            if (!running_.load()) break;
            std::string reason;
            double ret = (c.entry > 0.0 && c.price > 0.0) ? (c.price - c.entry) / c.entry : 0.0;

            if (equityExitTakeProfitPct_ > 0.0 && ret >= equityExitTakeProfitPct_) {
                reason = "Take-profit (" + pct(ret) + ")";
            } else if (equityExitStopLossPct_ > 0.0 && ret <= -equityExitStopLossPct_) {
                reason = "Stop-loss (" + pct(ret) + ")";
            } else if (equityExitMaxHoldDays_ > 0) {
                long held_days = std::chrono::duration_cast<std::chrono::hours>(
                    std::chrono::system_clock::now() - c.entry_time).count() / 24;
                if (held_days >= equityExitMaxHoldDays_)
                    reason = "Time stop (" + std::to_string(held_days) + "d held)";
            }

            double exit_rsi = 50.0;
            if (reason.empty() && (equityExitRsiCeiling_ > 0.0 || equityExitSmaBreak_)) {
                auto d = nox::equity_signal::EquitySignalGenerator::fetchBars(c.ticker);
                if (d.valid) {
                    exit_rsi = d.rsi14;
                    std::ostringstream r;
                    if (equityExitRsiCeiling_ > 0.0 && d.rsi14 >= equityExitRsiCeiling_) {
                        r << std::fixed << std::setprecision(1) << d.rsi14;
                        reason = "RSI exhaustion (" + r.str() + " ≥ " +
                                 std::to_string(static_cast<int>(equityExitRsiCeiling_)) + ")";
                    } else if (equityExitSmaBreak_ && d.price < d.sma20) {
                        reason = "Trend break (close below SMA20)";
                    }
                }
                // Gentle pacing for the Yahoo bar endpoint across multiple positions.
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }

            if (!reason.empty()) {
                Logger::log("INFO", "[EQUITY_EXIT] " + c.ticker + " → " + reason + " — liquidating.");
                // Route through the same pipeline a webhook SELL takes (record_signal +
                // process()) instead of closing directly: this gets the CN-RULE-002 T+1
                // gate applied to rule-triggered exits (previously bypassed — a real
                // correctness gap, not just an observability one) and makes the exit
                // visible in /signals and /details via the tagged `source`.
                TradeSignal sell_sig;
                sell_sig.ticker = c.ticker;
                sell_sig.action = "SELL";
                sell_sig.price  = c.price;
                sell_sig.rsi    = exit_rsi;
                sell_sig.source = "rule:" + reason;
                record_signal(sell_sig);
                process(sell_sig);
            }
        }
    }

    void monitor_trailing_stops() {
        Logger::log("INFO", "[TRAILING_STOP_MONITOR] Thread started. Checking for trailing stop fills every 5 minutes.");

        while (running_.load()) {
            // Sleep with early wakeup on shutdown
            {
                std::unique_lock<std::mutex> lk(stop_mutex_);
                if (stop_cv_.wait_for(lk, std::chrono::minutes(5),
                                      [this] { return !running_.load(); })) {
                    break;
                }
            }

            if (!running_.load()) break;

            try {
                bool fetch_ok = false;
                auto current = fetch_open_positions_map(fetch_ok);
                if (!fetch_ok) {
                    // Couldn't reach Alpaca — do NOT treat tracked positions as closed,
                    // or we'd record phantom exits. Skip this cycle and retry later.
                    Logger::log("WARN", "[TRAILING_STOP_MONITOR] Position fetch failed — "
                                "skipping close-detection this cycle.");
                    continue;
                }

                // Snapshot the closed tickers (tracked, but no longer open at Alpaca)
                // and refresh last-seen P&L for those still open — all under one lock
                // so a concurrent BUY/SELL can't race the detect-then-erase window.
                struct ClosedInfo { int quantity; double last_price; double last_pnl; };
                std::map<std::string, ClosedInfo> closed;
                {
                    std::lock_guard<std::mutex> lock(equity_positions_mutex_);
                    for (auto& kv : equity_positions_) {
                        auto it = current.find(kv.first);
                        if (it == current.end()) {
                            // Tracked but no longer open — closed (likely trailing stop).
                            closed[kv.first] = ClosedInfo{
                                kv.second.quantity, kv.second.last_price, kv.second.last_pnl};
                        } else {
                            // Still open — refresh last-seen values for future P&L estimate.
                            kv.second.last_price = it->second.current_price;
                            kv.second.last_pnl   = it->second.unrealized_pl;
                            // Phase 2, item B: persist the live unrealized snapshot —
                            // this loop is the only place equity marks are already
                            // fetched every 5 minutes, so it's the natural point to
                            // make that P&L queryable instead of log-only.
                            if (positionManager_) {
                                positionManager_->upsert_unrealized(
                                    get_today_date_string(), kv.first, "equity", "",
                                    static_cast<double>(kv.second.quantity),
                                    kv.second.entry_price, it->second.current_price,
                                    it->second.unrealized_pl);
                            }
                        }
                    }
                    // Erase closed entries now, while we still hold the lock.
                    for (const auto& kv : closed) equity_positions_.erase(kv.first);
                    if (!closed.empty()) persist_equity_positions_locked();
                }

                // For each closed position, record the exit to the ledger + notify.
                for (const auto& kv : closed) {
                    const std::string& ticker = kv.first;
                    const ClosedInfo&  info   = kv.second;
                    Logger::log("INFO", "[TRAILING_STOP_MONITOR] Position " + ticker +
                                " detected as closed — likely hit trailing stop.");

                    TradeSignal sell_signal;
                    sell_signal.ticker = ticker;
                    sell_signal.action = "SELL";
                    sell_signal.price  = info.last_price;
                    sell_signal.rsi    = 50.0;
                    sell_signal.vol    = 0;
                    sell_signal.atr    = 0.0;
                    sell_signal.source = "trailing_stop_close";
                    record_signal(sell_signal);

                    // Ledger: record the exit with the last-seen unrealized P&L as the
                    // best available realized figure (the position is already gone).
                    if (positionManager_) {
                        positionManager_->record_trade(
                            ticker, "SELL", "equity",
                            static_cast<double>(info.quantity), info.last_price,
                            50.0, 0.0, info.last_pnl, "trailing_stop_close");
                        positionManager_->add_realized(
                            get_today_date_string(), ticker, "equity", "", info.last_pnl);
                    }

                    // CN-RULE-002: a trailing-stop close also lifts the T+1 record.
                    {
                        std::lock_guard<std::mutex> lock(china_positions_mutex_);
                        if (china_positions_.erase(ticker) > 0)
                            persist_china_positions_locked();
                    }

                    std::ostringstream pnl_ss;
                    pnl_ss << std::showpos << std::fixed << std::setprecision(2) << info.last_pnl;
                    TelegramNotifier::sendMessage(
                        "🔴 *TRAILING STOP DETECTED*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + ticker + "\n"
                        "• *Est. P&L:* $" + pnl_ss.str() + "\n"
                        "• *Trigger:* Position closed (likely trailing stop hit)\n"
                        "✅ Exit recorded to trade ledger"
                    );
                }
                std::vector<std::string> closed_tickers;
                for (const auto& kv : closed) closed_tickers.push_back(kv.first);

                if (!closed_tickers.empty()) {
                    Logger::log("INFO", "[TRAILING_STOP_MONITOR] Detected " +
                                std::to_string(closed_tickers.size()) + " closed position(s).");
                }

                // Rule-based exits: liquidate any still-open position that trips a
                // take-profit / stop-loss / RSI-exhaustion / trend-break / time rule.
                evaluate_equity_exit_rules(current);

            } catch (const std::exception& e) {
                Logger::log("WARN", "[TRAILING_STOP_MONITOR] Exception during monitoring: " +
                            std::string(e.what()));
            }
        }
        Logger::log("INFO", "[TRAILING_STOP_MONITOR] Thread shutting down.");
    }

    // RULE-005: Fetch live equity with exponential-backoff retry (2s -> 4s -> 8s).
    // Returns -1.0 on total failure — callers MUST NOT proceed with any fallback.
    double fetch_account_equity() {
        const int    MAX_RETRIES   = 3;
        unsigned int delay_seconds = 2;

        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
            try {
                httplib::Client alpaca_cli(alpacaBaseUrl);
                alpaca_cli.set_connection_timeout(std::chrono::seconds(5));
                alpaca_cli.set_read_timeout(std::chrono::seconds(10));

                httplib::Headers headers = {
                    {"APCA-API-KEY-ID",     apiKey},
                    {"APCA-API-SECRET-KEY", apiSec}
                };

                auto res = alpaca_cli.Get("/v2/account", headers);
                if (res && res->status == 200) {
                    json account_data = json::parse(res->body);
                    double equity = std::stod(account_data.value("portfolio_value", "-1.0"));
                    if (equity > 0.0) return equity;
                    Logger::log("WARN", "[EXECUTION] Alpaca returned non-positive portfolio_value.");
                } else {
                    Logger::log("WARN", "[EXECUTION] Equity fetch attempt " + std::to_string(attempt) +
                                "/" + std::to_string(MAX_RETRIES) + " failed. Status: " +
                                (res ? std::to_string(res->status) : "TIMEOUT"));
                }
            } catch (const std::exception& e) {
                Logger::log("WARN", "[EXECUTION] Equity fetch exception on attempt " +
                            std::to_string(attempt) + ": " + std::string(e.what()));
            }

            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
                delay_seconds *= 2; // exponential backoff: 2s -> 4s -> 8s
            }
        }

        // All retries exhausted — RULE-005: halt and alert, never fall back.
        Logger::log("CRITICAL", "[EXECUTION] All equity fetch retries exhausted. "
                    "Halting order entry for this cycle.");
        TelegramNotifier::sendMessage(
            "🚨 *CRITICAL: Equity Fetch Failed*\n"
            "────────────────────────\n"
            "All 3 Alpaca equity fetch attempts failed.\n"
            "⛔ New order entries halted for this cycle.\n"
            "Manual review required."
        );
        return -1.0; // sentinel: caller must abort the trade
    }

    // WS5 — read the live RELATIVE bid-ask spread (ask-bid)/mid for an equity
    // from Alpaca's latest quote. Returns -1.0 on failure; the gate fails open
    // on -1.0 so a transient quote-feed hiccup never halts all trading.
    double fetch_equity_spread(const std::string& symbol) const {
        try {
            httplib::Client cli("https://data.alpaca.markets");
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));
            httplib::Headers headers = {
                {"APCA-API-KEY-ID", apiKey},
                {"APCA-API-SECRET-KEY", apiSec}
            };
            std::string path = "/v2/stocks/" + symbol + "/quotes/latest?feed=iex";
            auto res = cli.Get(path.c_str(), headers);
            if (!res || res->status != 200) return -1.0;

            auto body = json::parse(res->body);
            const auto& q = body.at("quote");
            double bid = q.value("bp", 0.0);
            double ask = q.value("ap", 0.0);
            if (bid <= 0.0 || ask <= 0.0 || ask < bid) return -1.0;
            double mid = (ask + bid) / 2.0;
            return (mid > 0.0) ? (ask - bid) / mid : -1.0;
        } catch (...) {
            return -1.0;
        }
    }

    // Broker-truth check: does the account already hold shares of this ticker?
    // Same pattern as OptionsOrderRouter::hasOpenOptionPosition — the idempotency
    // cache (main.cpp check_idempotency) is keyed on price-to-2-decimals with a
    // 5-minute window, so a webhook signal that re-fires ~30 min apart at a
    // slightly different price sails right through it. This is the real gate;
    // `reachable=false` tells the caller to fall back to the ledger instead of
    // assuming "no position."
    bool hasOpenEquityPosition(const std::string& ticker, bool& reachable) const {
        reachable = false;
        try {
            httplib::Client cli(alpacaBaseUrl);
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));
            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey},
                {"APCA-API-SECRET-KEY", apiSec}
            };
            auto res = cli.Get(("/v2/positions/" + ticker).c_str(), headers);
            if (!res) return false;
            if (res->status == 404) { reachable = true; return false; } // confirmed flat
            if (res->status != 200) return false;                       // other error → unreachable
            reachable = true;
            json body = json::parse(res->body);
            return body.value("qty", 0.0) != 0.0;
        } catch (...) {
            reachable = false;
            return false;
        }
    }

    // Closes an open equity position at Alpaca: cancels resting orders (including
    // the trailing stop), liquidates, records the exit to the trade ledger with the
    // supplied reason, notifies, and clears local tracking (T+1 + equity maps).
    // Shared by webhook SELL signals and the rule-based exit monitor so both close
    // Helper: calculate sell quantity based on EQUITY_SELL_QTY_MODE.
    // If not using "full" mode, returns the qty to sell; -1 means "sell all".
    int calculate_sell_qty(const std::string& ticker, double current_price, double entry_price,
                           int open_qty, double equity) {
        if (equitySellQtyMode_ == "full" || equitySellQtyMode_.empty()) {
            return -1; // -1 = liquidate all (original behavior)
        }

        if (equitySellQtyMode_ == "kelly") {
            // Recalculate qty using same Kelly sizing — treats exit with same discipline as entry
            int qty = calculate_kelly_size(equity, current_price, kellyWinRate, kellyWinLossRatio, kellyFraction);
            if (qty <= 0) qty = 1; // minimum 1 share
            return std::min(qty, open_qty); // can't sell more than we hold
        }

        if (equitySellQtyMode_ == "prorated") {
            // Scale by notional value: qty = (entry_notional / current_price)
            // This exits the same notional amt as was originally entered
            double entry_notional = (entry_price > 0) ? static_cast<double>(open_qty) * entry_price : 0.0;
            if (entry_notional > 0 && current_price > 0) {
                int prorated_qty = static_cast<int>(entry_notional / current_price);
                return std::min(std::max(1, prorated_qty), open_qty);
            }
            return open_qty; // fallback: sell all
        }

        return -1; // unknown mode → sell all
    }

    // through one tested path. `reason` is surfaced in the ledger and Telegram.
    // Optional qty_override: if -1 (default), liquidates entire position; otherwise sells that qty.
    void close_equity_position_alpaca(const std::string& ticker,
                                      const std::string& reason,
                                      double rsi,
                                      int qty_override = -1) {
        try {
            httplib::Client alpaca_cli(alpacaBaseUrl);
            // RULE-008: Strict timeout handling
            alpaca_cli.set_connection_timeout(std::chrono::seconds(5));
            alpaca_cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey},
                {"APCA-API-SECRET-KEY", apiSec}
            };

            // --- 0. Snapshot the position first so we can record realized P&L ---
            // Alpaca's liquidate response doesn't include realized P&L, so read the
            // open position's unrealized_pl (which becomes realized on close) and qty.
            double closed_qty = 0.0, realized_pnl = 0.0, exit_price = 0.0;
            int open_qty = 0, entry_price = 0;
            {
                auto snap = alpaca_cli.Get(("/v2/positions/" + ticker).c_str(), headers);
                if (snap && snap->status == 200) {
                    try {
                        json p = json::parse(snap->body);
                        closed_qty   = std::stod(p.value("qty", "0"));
                        open_qty     = static_cast<int>(closed_qty);
                        realized_pnl = std::stod(p.value("unrealized_pl", "0"));
                        exit_price   = std::stod(p.value("current_price", "0"));
                        entry_price  = std::stod(p.value("avg_fill_price", "0"));
                    } catch (...) { /* best-effort */ }
                }
            }

            // Determine actual qty to sell (respects EQUITY_SELL_QTY_MODE)
            int sell_qty = qty_override;
            if (sell_qty < 0) {
                // qty_override not specified → calculate based on mode
                double live_eq = fetch_account_equity();
                sell_qty = calculate_sell_qty(ticker, exit_price, entry_price, open_qty, live_eq);
                if (sell_qty < 0) sell_qty = open_qty; // fallback to full liquidation
            }
            sell_qty = std::min(sell_qty, open_qty); // can't sell more than held

            // Partial realized P&L if selling partial qty
            double partial_pnl = realized_pnl;
            if (sell_qty < open_qty && open_qty > 0) {
                partial_pnl = realized_pnl * (static_cast<double>(sell_qty) / static_cast<double>(open_qty));
            }

            // --- 1. Cancel all open orders for the symbol to avoid interference ---
            std::string cancel_path = "/v2/orders?symbol=" + ticker;
            std::cout << "[EXECUTION] Canceling any existing orders for " << ticker << " before closing position..." << std::endl;
            auto cancel_res = alpaca_cli.Delete(cancel_path.c_str(), headers);
            if (cancel_res && (cancel_res->status == 200 || cancel_res->status == 207)) {
                std::cout << "[EXECUTION] Existing orders for " << ticker << " canceled (or none existed)." << std::endl;
            } else {
                std::string cancel_status = cancel_res ? std::to_string(cancel_res->status) : "TIMEOUT";
                std::cerr << "⚠️ [EXECUTION] Could not verify order cancellation for " << ticker
                          << ". Status: " << cancel_status << ". Proceeding to close position." << std::endl;
            }

            // --- 2. Route sell order (full liquidate or partial based on mode) ---
            json response_data;
            bool sell_success = false;

            if (sell_qty >= open_qty) {
                // Sell all → use DELETE /v2/positions/{ticker}
                std::string path = "/v2/positions/" + ticker;
                std::cout << "[EXECUTION] Sending liquidate position request to Alpaca for " << ticker
                          << " (qty=" << sell_qty << ", reason: " << reason << ")..." << std::endl;
                auto res = alpaca_cli.Delete(path.c_str(), headers);
                if (res && res->status == 200) {
                    response_data = json::parse(res->body);
                    sell_success = true;
                }
            } else {
                // Sell partial qty → use POST /v2/orders with qty
                std::cout << "[EXECUTION] Sending SELL market order to Alpaca: " << sell_qty
                          << " shares of " << ticker << " (partial, reason: " << reason << ")..." << std::endl;
                json order_payload = {
                    {"symbol", ticker},
                    {"qty", sell_qty},
                    {"side", "sell"},
                    {"type", "market"},
                    {"time_in_force", "day"},
                    {"client_order_id", makeEquityClientOid(ticker, "sell")}
                };
                auto res = alpaca_cli.Post("/v2/orders", headers, order_payload.dump(), "application/json");
                if (res && res->status == 200) {
                    response_data = json::parse(res->body);
                    sell_success = true;
                }
            }

            if (sell_success) {
                std::cout << " [SELL ORDER EXECUTED] Alpaca response: " << response_data.dump(2) << std::endl;
                // Ledger: record the equity exit with best-effort realized P&L.
                if (positionManager_) {
                    positionManager_->record_trade(
                        ticker, "SELL", "equity",
                        static_cast<double>(sell_qty), exit_price, rsi, 0.0, partial_pnl,
                        reason + " mode=" + equitySellQtyMode_ + " order_id=" + response_data.value("id", "N/A"));
                    positionManager_->add_realized(
                        get_today_date_string(), ticker, "equity", "", partial_pnl);
                }
                std::ostringstream pnl_ss;
                pnl_ss << std::showpos << std::fixed << std::setprecision(2) << partial_pnl;

                std::string status_str = (sell_qty >= open_qty) ? "FULLY" : "PARTIALLY";
                TelegramNotifier::sendMessage(
                    "⚪ *POSITION " + status_str + " CLOSED*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + ticker + "\n"
                    "• *Qty:* " + std::to_string(sell_qty) + " / " + std::to_string(open_qty) + " shares\n"
                    "• *Mode:* " + equitySellQtyMode_ + "\n"
                    "• *Reason:* " + reason + "\n"
                    "• *Est. P&L:* $" + pnl_ss.str() + "\n"
                    "• *Alpaca Order ID:* `" + response_data.value("id", "N/A") + "`"
                );

                // Only remove position if fully closed
                if (sell_qty >= open_qty) {
                    // CN-RULE-002: Position is closed — remove from T+1 tracking map.
                    {
                        std::lock_guard<std::mutex> lock(china_positions_mutex_);
                        china_positions_.erase(ticker);
                        persist_china_positions_locked();
                    }
                    // Also remove from equity position tracking (trailing stop monitor)
                    {
                        std::lock_guard<std::mutex> lock(equity_positions_mutex_);
                        equity_positions_.erase(ticker);
                        persist_equity_positions_locked();
                    }
                }
            } else {
                // Sell failed — handle error
                std::cerr << "⚠️ [CLOSE REJECTED] Failed to sell " << sell_qty << " shares of " << ticker
                          << " (reason: " << reason << ")" << std::endl;
                TelegramNotifier::sendMessage(
                    "🚨 *SELL ORDER FAILED*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + ticker + "\n"
                    "• *Qty:* " + std::to_string(sell_qty) + " shares\n"
                    "• *Reason:* " + reason + "\n"
                    "⛔ Position remains open. Manual review required."
                );
            }
        } catch (const std::exception& e) {
            std::cerr << "💥 Runtime Exception closing position for " << ticker << ": " << e.what() << std::endl;
        }
    }

    void process(TradeSignal sig) {
        // Skip processing if the action is HOLD or a REPORT audit payload
        if (sig.action == "HOLD" || sig.action == "REPORT") {
            std::cout << "[EXECUTION] Strategy indicates HOLD/REPORT. No orders routed." << std::endl;
            return;
        }

        // --- SELL ROUTING: Close open position (enhanced) ---
        if (sig.action == "SELL") {
            // Origin tag for this sell — empty (webhook) falls back to the historic
            // wording; internally-generated sells (rule-based exits, trailing-stop
            // closes) carry a human-readable reason via sig.source.
            std::string sell_reason = sig.source.empty() ? "Webhook SELL Signal" : sig.source;

            // CN-RULE-002: T+1 Settlement Gate — only applies to Chinese A-shares.
            // US equities (cnBoardLotSize == 1) are exempt; only enforce when
            // routing to a Chinese exchange (cnBoardLotSize == 100).
            if (cnBoardLotSize > 1) {
                std::lock_guard<std::mutex> lock(china_positions_mutex_);
                auto it = china_positions_.find(sig.ticker);
                if (it != china_positions_.end()) {
                    std::string effective_sell_date = sig.trade_date.empty()
                        ? get_today_date_string()
                        : sig.trade_date;
                    if (effective_sell_date == it->second.entry_date) {
                        Logger::log("CRITICAL",
                            "[CN-RULE-002] T+1 gate blocked SELL for " + sig.ticker +
                            " — entry_date=" + it->second.entry_date +
                            " equals sell_date=" + effective_sell_date +
                            ". Same-day round-trips are prohibited on Chinese A-shares. "
                            "Trigger: " + sell_reason);
                        TelegramNotifier::sendMessage(
                            "🚫 *CN T+1 GATE BLOCKED*\n"
                            "────────────────────────\n"
                            "• *Ticker:* " + sig.ticker + "\n"
                            "• *Trigger:* " + sell_reason + "\n"
                            "• *Entry Date:* " + it->second.entry_date + "\n"
                            "• *Sell Date:* " + effective_sell_date + "\n"
                            "⛔ Same-day sell prohibited (T+1 rule). Signal discarded."
                        );
                        return;
                    }
                } else {
                    // No record found — position may have been entered before this engine
                    // instance started (e.g., restart mid-day) or the persistence file was
                    // lost. Log a warning so the operator can verify manually; do NOT block
                    // the sell, as holding a position indefinitely would be worse.
                    Logger::log("WARN",
                        "[CN-RULE-002] SELL received for " + sig.ticker +
                        " but no T+1 entry record found. "
                        "Position may pre-date this engine instance. Proceeding with SELL.");
                }
            }

            Logger::log("INFO", "[EXECUTION] SELL signal for " + sig.ticker + ". Closing position.");
#ifdef IBKR_ENABLED
            if (execution_venue_ == "ibkr") {
                Contract stock;
                stock.symbol   = sig.ticker;
                stock.secType  = "STK";
                stock.exchange = "SMART";
                stock.currency = "USD";

                Order mkt_order;
                mkt_order.action        = "SELL";
                mkt_order.orderType     = "MKT";
                mkt_order.totalQuantity = 0;  // 0 = liquidate full position via IBKR
                // IBKR does not have a "close all" API like Alpaca's DELETE /positions.
                // totalQuantity=0 is invalid — the operator must track qty or query positions.
                // For now we log a warning and Telegram-alert for manual action.
                Logger::log("WARN",
                    "[IBKR] SELL routed to IBKR: qty unknown without position query. "
                    "Manual review required to confirm close.");
                TelegramNotifier::sendMessage(
                    "⚠️ *IBKR SELL — Manual Action Required*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + sig.ticker + "\n"
                    "• IBKR lacks a liquidate-all REST API. Log into TWS/Gateway and\n"
                    "  close the position manually, or implement a position query to get qty."
                );

                // Evict T+1 record regardless.
                {
                    std::lock_guard<std::mutex> lock(china_positions_mutex_);
                    china_positions_.erase(sig.ticker);
                    persist_china_positions_locked();
                }
                return;
            }
#endif
            close_equity_position_alpaca(sig.ticker, sell_reason, sig.rsi);
            return;
        }

        // --- BUY ROUTING: Open new position with trailing stop ---
        if (sig.action == "BUY") {
            // Global kill switch — checked before any other gate. Blocks NEW
            // entries only; an operator /pause or an automatic daily-loss-limit
            // breach must never leave an order in flight, but existing
            // positions are left alone (see check_daily_loss_limit()).
            if (killSwitch_ && killSwitch_->isPaused()) {
                auto ks = killSwitch_->get();
                Logger::log("WARN", "[KILL_SWITCH] Equity BUY blocked for " + sig.ticker +
                            " — trading paused (" + ks.triggered_by + "): " + ks.reason);
                if (orderLedger_) orderLedger_->logSignalEvent(sig.ticker, "EQUITY_BUY",
                    sig.ticker + "|EQUITY_BUY|" + get_today_date_string(), -1.0,
                    "suppressed_kill_switch", ks.reason);
                return;
            }

            // RSI floor/ceiling gate — block trades that violate backtest rules
            if (sig.rsi < 30.0) {
                Logger::log("WARN", "RSI gate blocked BUY for " + sig.ticker + " — RSI below floor at " + std::to_string(sig.rsi));
                TelegramNotifier::sendMessage(
                    "🚧 *RSI GATE BLOCK*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + sig.ticker + "\n"
                    "• *Action:* BUY\n"
                    "• *RSI:* " + std::to_string(sig.rsi) + " (Below Floor < 30)\n"
                    "────────────────────────\n"
                    "⚠️ _Order canceled to protect buying power._"
                );
                return;
            }

            // ── Regime Gate ──────────────────────────────────────────────────────────
            AllocationStrategy regime = regimeMachine.evaluate(
                sig.vix, sig.spy_price, sig.spy_200_sma
            );
            Logger::log("INFO", "[REGIME] " + regime.log_message);
            if (regime.capital_multiplier == 0.0) {
                Logger::log("WARN", "[REGIME] RISK-OFF — new entries halted for " + sig.ticker);
                TelegramNotifier::sendMessage(
                    "🛑 *REGIME BLOCK: RISK-OFF*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + sig.ticker + "\n"
                    "⛔ VIX ≥ 30 or SPY below 200 SMA. No new entries."
                );
                return;
            }
            // ─────────────────────────────────────────────────────────────────────────

            // RULE-005: Fetch live equity — abort if unavailable (no silent fallback).
            double live_equity = fetch_account_equity();
            if (live_equity < 0.0) {
                Logger::log("CRITICAL", "[EXECUTION] Aborting order for " + sig.ticker +
                            " — could not obtain live equity.");
                return;
            }

            // --- Position Sizing (Issue #9) ---
            int qty = 0;
            double stop_multiplier = sig.stop_loss_atr_multiplier; // Default from payload

            // Dynamic sizing based on risk_tier
            if (sig.risk_tier == 3) {
                Logger::log("INFO", "[RISK] Tier 3: 'Let the knife cut' — Risking 5% of capital with 3.5x ATR stop.");
                stop_multiplier = 3.5;
                double dollar_amount = (live_equity * regime.capital_multiplier) * 0.05;
                if (sig.price > 0) qty = static_cast<int>(std::floor(dollar_amount / sig.price));
            } else if (sig.risk_tier == 1) {
                Logger::log("INFO", "[RISK] Tier 1: 'Standard' — Risking 1% of capital with 2.0x ATR stop.");
                stop_multiplier = 2.0;
                double dollar_amount = (live_equity * regime.capital_multiplier) * 0.01;
                if (sig.price > 0) qty = static_cast<int>(std::floor(dollar_amount / sig.price));
            } else {
                // Default to Kelly Criterion if risk_tier is not 1 or 3
                Logger::log("INFO", "[RISK] Using Kelly Criterion sizing for " + sig.ticker);
                double regime_adjusted_equity = live_equity * regime.capital_multiplier;
                qty = calculate_kelly_size(regime_adjusted_equity, sig.price, kellyWinRate, kellyWinLossRatio, kellyFraction);
            }

            if (qty <= 0) {
                Logger::log("CRITICAL", "[EXECUTION] Aborting order for " + sig.ticker +
                            " — position sizing resulted in zero or negative shares (" + std::to_string(qty) + ").");

                // Only send the specific "no-edge" alert if Kelly was the method that failed.
                if (sig.risk_tier != 1 && sig.risk_tier != 3) {
                    TelegramNotifier::sendMessage(
                        "🚨 *CRITICAL: Kelly No-Edge or Insufficient Capital*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + sig.ticker + "\n"
                        "⛔ Raw Kelly ≤ 0 or insufficient capital for 1 share.\n"
                        "Order halted. Review Kelly params or account equity."
                    );
                }
                return;
            }

            // CN-RULE-001: Board-Lot Truncation.
            // Chinese A-share exchanges require orders in multiples of cnBoardLotSize
            // shares (one 手, shǒu; standard = 100). Any fractional lot is truncated,
            // not rounded, to avoid accidentally exceeding the Kelly allocation.
            // e.g. Kelly = 345 shares → submitted qty = 300 shares.
            {
                int lot_qty = (qty / cnBoardLotSize) * cnBoardLotSize;
                if (lot_qty <= 0) {
                    Logger::log("CRITICAL",
                        "[CN-RULE-001] Board-lot truncation dropped qty to 0 for " + sig.ticker +
                        " (raw qty=" + std::to_string(qty) +
                        " < lot size " + std::to_string(cnBoardLotSize) + "). "
                        "Trade aborted. Increase account equity or lower stock price.");
                    TelegramNotifier::sendMessage(
                        "🚨 *CRITICAL: CN Board-Lot Gate*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + sig.ticker + "\n"
                        "• *Raw Kelly Qty:* " + std::to_string(qty) + " shares\n"
                        "• *Lot Size:* " + std::to_string(cnBoardLotSize) + "\n"
                        "⛔ Cannot form one full lot. Order aborted.\n"
                        "Increase account equity or reduce stock price."
                    );
                    return;
                }
                if (lot_qty != qty) {
                    Logger::log("INFO",
                        "[CN-RULE-001] Board-lot truncation: raw=" + std::to_string(qty) +
                        " → submitted=" + std::to_string(lot_qty) + " shares for " + sig.ticker);
                }
                qty = lot_qty;
            }

            // RULE-018 Condition 2 — Notional Value Ceiling (Physical Hard Gate).
            double notional_value    = static_cast<double>(qty) * sig.price;
            double max_notional      = live_equity * 0.10;
            if (notional_value > max_notional) {
                Logger::log("CRITICAL",
                    "[RULE-018] Notional ceiling breached for " + sig.ticker +
                    " — Notional: $" + std::to_string(notional_value) +
                    " vs Max Allowed: $" + std::to_string(max_notional) +
                    ". Order blocked.");
                TelegramNotifier::sendMessage(
                    "🚨 *CRITICAL: RULE-018 Notional Ceiling Breached*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + sig.ticker + "\n"
                    "• *Notional:* $" + std::to_string(notional_value) + "\n"
                    "⛔ Price spike detected between sizing and submission."
                );
                return;
            }

            // WS5 — Liquidity Vacuum / Microstructure Gate (Physical Hard Gate).
            // The final check before any order leaves the building: read the live
            // bid-ask spread and abort if it is N standard deviations above the
            // rolling baseline — a market order into a vacuum fills at a punitive
            // price regardless of signal strength. Bypassable only via .env.
            {
                double rel_spread = fetch_equity_spread(sig.ticker);
                auto gate = liquidity_gate_.evaluate(sig.ticker, rel_spread);
                if (!gate.allow) {
                    Logger::log("CRITICAL",
                        "[WS5][LIQUIDITY_GATE] " + sig.ticker +
                        " order aborted — " + gate.reason);
                    TelegramNotifier::sendMessage(
                        "🛑 *LIQUIDITY GATE — " + sig.ticker + "*\n"
                        "────────────────────────\n"
                        "• *Spread z-score:* " + std::to_string(gate.zscore) + "σ\n"
                        "• *Threshold:* " + std::to_string(liquidity_gate_.threshold()) + "σ\n"
                        "⛔ Liquidity vacuum detected between sizing and submission.\n"
                        "Order aborted to avoid punitive fill."
                    );
                    return;
                }
            }

#ifdef IBKR_ENABLED
            // ── IBKR equity BUY path ────────────────────────────────────────────
            if (execution_venue_ == "ibkr") {
                // Route a plain equity (stock) market buy via IBKR.
                // Options routing goes through IBKROrderRouter::route(OptionsSignal).
                // Webhook BUY signals are stock orders — construct a stock Contract.
                Contract stock;
                stock.symbol   = sig.ticker;
                stock.secType  = "STK";
                stock.exchange = "SMART";
                stock.currency = "USD";

                Order mkt_order;
                mkt_order.action        = "BUY";
                mkt_order.orderType     = "MKT";
                mkt_order.totalQuantity = static_cast<double>(qty);
                mkt_order.tif           = "DAY";

                OrderId oid = ibkr_wrapper_->reserveOrderId();
                ibkr_conn_->placeOrder(oid, stock, mkt_order);

                Logger::log("INFO", "[IBKR] BUY order placed: " + sig.ticker +
                            " qty=" + std::to_string(qty) + " orderId=" + std::to_string(oid));
                TelegramNotifier::sendMessage(
                    "🟢 *" + sig.ticker + "* — " + std::to_string(qty) + " shares → IBKR"
                );

                // Record T+1 entry date for CN-RULE-002 enforcement.
                // Only for Chinese A-shares (cnBoardLotSize > 1); US equities are exempt.
                // (Matches the guard on the Alpaca BUY path below — this branch was
                // previously unconditional, an asymmetry that would spuriously start a
                // T+1 clock on every US-equity IBKR buy.)
                if (cnBoardLotSize > 1) {
                    std::string entry_date = sig.trade_date.empty()
                        ? get_today_date_string() : sig.trade_date;
                    std::lock_guard<std::mutex> lock(china_positions_mutex_);
                    china_positions_[sig.ticker] = ChinaPositionRecord{entry_date};
                    persist_china_positions_locked();
                }
                return;
            }
#endif
            // Position-exists gate: ask the broker first (authoritative — reflects
            // manual closes too), falling back to the day-scoped ledger signature
            // only if the broker is unreachable. Fails closed, not open — the
            // price-keyed idempotency cache above this block does NOT catch a
            // signal re-firing 30 minutes apart at a different price, which is
            // exactly how AAPL got repeatedly bought into every scan cycle.
            {
                bool eq_reachable = false;
                bool eq_has_position = hasOpenEquityPosition(sig.ticker, eq_reachable);
                std::string eq_signature = sig.ticker + "|EQUITY_BUY|" + get_today_date_string();
                if (eq_reachable && eq_has_position) {
                    Logger::log("WARN", "[EXECUTION] Equity BUY blocked for " + sig.ticker +
                                " — broker already holds a position.");
                    if (orderLedger_) orderLedger_->logSignalEvent(sig.ticker, "EQUITY_BUY",
                        eq_signature, -1.0, "gate_blocked_position_exists",
                        "broker already holds shares of this ticker");
                    return;
                }
                if (!eq_reachable && orderLedger_ && orderLedger_->hasRecentActive(eq_signature, 86400)) {
                    Logger::log("WARN", "[EXECUTION] Equity BUY blocked for " + sig.ticker +
                                " — broker unreachable, ledger shows a same-day buy already on file.");
                    orderLedger_->logSignalEvent(sig.ticker, "EQUITY_BUY",
                        eq_signature, -1.0, "gate_blocked_position_exists",
                        "same-day buy already on file (ledger fallback, broker unreachable)");
                    return;
                }
            }

            // Phase 4, item 2: portfolio circuit breaker — equity notional cap.
            // Alert + block new buys only; unlike the options side, an existing
            // equity position is never auto-cut here (equity delta is trivially
            // 1/share — there's no Greeks-breach concept to justify a forced
            // exit, only a notional one, and this project treats forced exits as
            // an options-specific, Greeks-driven exception to the signal-driven
            // philosophy, not a general one).
            if (positionManager_) {
                std::string eq_signature = sig.ticker + "|EQUITY_BUY|" + get_today_date_string();
                double existing_equity_notional = 0.0;
                {
                    std::lock_guard<std::mutex> lock(equity_positions_mutex_);
                    for (const auto& [ticker, p] : equity_positions_) {
                        double mark = (p.last_price > 0.0) ? p.last_price : p.entry_price;
                        existing_equity_notional += std::abs(mark * p.quantity);
                    }
                }
                double max_equity_notional = positionManager_->get_risk_targets().max_equity_notional;
                if (existing_equity_notional + notional_value > max_equity_notional) {
                    Logger::log("WARN", "[EXECUTION] Equity BUY blocked for " + sig.ticker +
                                " — portfolio equity notional cap: existing $" +
                                std::to_string(existing_equity_notional) + " + new $" +
                                std::to_string(notional_value) + " > target $" +
                                std::to_string(max_equity_notional));
                    if (orderLedger_) orderLedger_->logSignalEvent(sig.ticker, "EQUITY_BUY",
                        eq_signature, -1.0, "suppressed_risk_cap",
                        "equity notional cap breached ($" + std::to_string(existing_equity_notional) +
                        " existing + $" + std::to_string(notional_value) + " new > $" +
                        std::to_string(max_equity_notional) + " target)");
                    TelegramNotifier::sendMessage(
                        "🛑 *PORTFOLIO RISK — EQUITY NOTIONAL CAP*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + sig.ticker + "\n"
                        "• *Existing Equity Notional:* $" + std::to_string(existing_equity_notional) + "\n"
                        "• *New Order Notional:* $" + std::to_string(notional_value) + "\n"
                        "• *Target:* $" + std::to_string(max_equity_notional) + "\n"
                        "⛔ New equity entry paused — existing positions left untouched."
                    );
                    return;
                }
            }

            try {
                httplib::Client alpaca_cli(alpacaBaseUrl);
                // RULE-008: Strict timeout handling
                alpaca_cli.set_connection_timeout(std::chrono::seconds(5));
                alpaca_cli.set_read_timeout(std::chrono::seconds(10));

                httplib::Headers headers = {
                    {"APCA-API-KEY-ID",     apiKey},
                    {"APCA-API-SECRET-KEY", apiSec},
                    {"Content-Type",        "application/json"}
                };

                std::string eq_client_oid = makeEquityClientOid(sig.ticker, "buy");
                json order_payload = {
                    {"symbol", sig.ticker},
                    {"qty", qty},
                    {"side", "buy"},
                    {"type", "market"},
                    {"time_in_force", "day"},
                    {"client_order_id", eq_client_oid}
                };

                // Ledger the equity entry (pending) before the POST fires so a
                // crash/timeout leaves a record the reconciler could resolve.
                if (orderLedger_) {
                    nox::execution::OrderLedger::Order o;
                    o.client_oid  = eq_client_oid;
                    o.ticker      = sig.ticker;
                    o.strategy    = "EQUITY_BUY";
                    o.signature   = sig.ticker + "|EQUITY_BUY|" + get_today_date_string();
                    o.side        = "buy";
                    o.asset_class = "equity";
                    o.status      = "pending";
                    o.qty         = static_cast<double>(qty);
                    o.entry_price = sig.price;
                    orderLedger_->insertPending(o);
                }

                std::cout << "[EXECUTION] Routing BUY order to Alpaca: " << qty << " shares of " << sig.ticker << "..." << std::endl;
                auto res = alpaca_cli.Post("/v2/orders", headers, order_payload.dump(), "application/json");

                if (res && res->status == 200) {
                    json response_data = json::parse(res->body);
                    std::string order_id = response_data.value("id", "UNKNOWN");
                    std::cout << " [BUY ORDER EXECUTED] Alpaca Order ID: " << order_id << std::endl;

                    if (orderLedger_)
                        orderLedger_->setStatus(eq_client_oid, "filled", order_id);

                    // CRITICAL: Record this order in idempotency cache to prevent duplicates
                    record_idempotency(sig, order_id);

                    // Ledger: record the equity entry (single source of truth for reports).
                    if (positionManager_) {
                        double kelly_ratio = (live_equity > 0.0) ? (notional_value / live_equity) : 0.0;
                        positionManager_->record_trade(
                            sig.ticker, "BUY", "equity",
                            static_cast<double>(qty), sig.price, sig.rsi,
                            kelly_ratio, 0.0, "order_id=" + order_id);
                    }

                    // CN-RULE-002: Record entry date for T+1 enforcement.
                    // Only for Chinese A-shares (cnBoardLotSize > 1); US equities are exempt.
                    if (cnBoardLotSize > 1) {
                        std::string entry_date = sig.trade_date.empty()
                            ? get_today_date_string()
                            : sig.trade_date;
                        std::lock_guard<std::mutex> lock(china_positions_mutex_);
                        // Intentionally overwrites any existing entry with the newest
                        // entry_date. Repeat-buying a ticker resets the T+1 clock for
                        // the whole position — this looks wrong at a glance (lot-level
                        // tracking would let already-settled shares sell), but this
                        // system only ever fully liquidates a position (no partial sell
                        // exists anywhere), so blocking the entire position until the
                        // *most recent* buy clears T+1 is the only correct behavior
                        // without real per-lot dates. Do not "fix" this into a min().
                        china_positions_[sig.ticker] = ChinaPositionRecord{entry_date};
                        persist_china_positions_locked();
                        Logger::log("INFO",
                            "[CN-RULE-002] Recorded T+1 entry for " + sig.ticker +
                            " on " + entry_date + ". Sell gate active until T+1.");
                    }

                    // --- Place Trailing Stop Order ---
                    std::string stop_line;
                    if (sig.atr > 0 && stop_multiplier > 0) {
                        double trail_offset = sig.atr * stop_multiplier;

                        std::stringstream stream;
                        stream << std::fixed << std::setprecision(2) << trail_offset;
                        std::string trail_offset_str = stream.str();

                        json sl_payload = {
                            {"symbol", sig.ticker},
                            {"qty", qty},
                            {"side", "sell"},
                            {"type", "trailing_stop"},
                            {"time_in_force", "gtc"},
                            {"trail_price", trail_offset_str},
                            {"client_order_id", makeEquityClientOid(sig.ticker, "stop")}
                        };

                        std::cout << "[EXECUTION] Placing trailing stop for " << sig.ticker
                                  << " with trail offset $" << trail_offset_str << std::endl;
                        auto sl_res = alpaca_cli.Post("/v2/orders", headers, sl_payload.dump(), "application/json");

                        if (sl_res && sl_res->status == 200) {
                            json sl_data = json::parse(sl_res->body);
                            std::cout << " [TRAILING STOP PLACED] Order ID: " << sl_data.value("id", "N/A") << std::endl;
                            stop_line = " | 🛡️ trail $" + trail_offset_str;
                        } else {
                            std::string sl_status = sl_res ? std::to_string(sl_res->status) : "TIMEOUT";
                            std::string sl_details = sl_res ? sl_res->body : "No response.";
                            std::cerr << "⚠️ [STOP-LOSS FAILED] Status: " << sl_status
                                      << ", Details: " << sl_details << std::endl;
                            stop_line = " | ⚠️ stop failed";
                        }
                    } else {
                        Logger::log("WARN", "[EXECUTION] ATR or multiplier invalid, skipping trailing stop.");
                        stop_line = " | ⚠️ no stop";
                    }

                    // Track this position for trailing stop monitoring
                    {
                        std::lock_guard<std::mutex> lock(equity_positions_mutex_);
                        equity_positions_[sig.ticker] = OpenEquityPosition{
                            sig.ticker,
                            qty,
                            sig.price,
                            std::chrono::system_clock::now()
                        };
                        persist_equity_positions_locked();
                    }
                    Logger::log("INFO", "[TRAILING_STOP_MONITOR] Tracking position: " +
                                sig.ticker + " x" + std::to_string(qty));

                    // Compact confirmation — one line per trade, details via /details
                    TelegramNotifier::sendMessage(
                        "🟢 *" + sig.ticker + "* — " + std::to_string(qty) + " shares filled" +
                        stop_line
                    );
                } else {
                    std::string status_code = res ? std::to_string(res->status) : "TIMEOUT";
                    std::string details     = res ? res->body : "No response received.";
                    std::cerr << "⚠️ [ALPACA REJECTION] BUY order failed. Status: "
                              << status_code << ", Details: " << details << std::endl;
                    TelegramNotifier::sendMessage(
                        "🚨 *ALPACA REJECTION*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + sig.ticker + "\n"
                        "• *Side:* BUY\n"
                        "• *Status Code:* " + status_code + "\n"
                        "• *Details:* `" + details + "`"
                    );
                }
            } catch (const std::exception& e) {
                std::cerr << "💥 Runtime Exception routing BUY order to Alpaca: " << e.what() << std::endl;
            }
        }
    }

public:
    NoxEngine() {
        // RULE-009 / RULE-014: All credentials and config values come exclusively
        // from env vars. Any missing value is a hard-abort — no silent defaults.
        auto require_env = [](const char* name) -> std::string {
            const char* val = std::getenv(name);
            if (!val || std::string(val).empty()) {
                std::cerr << "[FATAL] [EXECUTION] Required env var '" << name
                          << "' is not set. Refusing to start." << std::endl;
                std::exit(1);
            }
            return std::string(val);
        };

        secret       = require_env("WEBHOOK_SECRET_TOKEN");
        apiKey       = require_env("ALPACA_API_KEY");
        apiSec       = require_env("ALPACA_SECRET_KEY");
        alpacaBaseUrl = require_env("ALPACA_BASE_URL");  // RULE-014: never hardcode live/paper URL

        // RULE-009: Kelly parameters must be present at startup — no silent defaults.
        std::string wr_str  = require_env("KELLY_WIN_RATE");
        std::string wlr_str = require_env("KELLY_WIN_LOSS_RATIO");
        std::string kf_str  = require_env("KELLY_FRACTION");
        try {
            kellyWinRate      = std::stod(wr_str);
            kellyWinLossRatio = std::stod(wlr_str);
            kellyFraction     = std::stod(kf_str);
        } catch (const std::exception& e) {
            std::cerr << "[FATAL] [EXECUTION] Invalid Kelly parameter value: " << e.what()
                      << ". Refusing to start." << std::endl;
            std::exit(1);
        }

        // Validate KELLY_FRACTION is in a sane range (0.0, 1.0].
        // A fraction of 0 would produce zero-share trades on every signal.
        // A fraction above 1.0 would exceed raw Kelly — mathematically reckless.
        if (kellyFraction <= 0.0 || kellyFraction > 1.0) {
            std::cerr << "[FATAL] [EXECUTION] KELLY_FRACTION (" << kellyFraction
                      << ") must be in range (0.0, 1.0]. Refusing to start." << std::endl;
            std::exit(1);
        }

        // Warn loudly if the fraction will cause adjusted Kelly to exceed the
        // 10% hard cap on every trade — that was the original problem.
        double raw_kelly_est = kellyWinRate - ((1.0 - kellyWinRate) / kellyWinLossRatio);
        if (raw_kelly_est > 0.0 && (raw_kelly_est * kellyFraction) > 0.10) {
            std::cerr << "[WARN] [EXECUTION] KELLY_FRACTION=" << kellyFraction
                      << " produces adjusted Kelly (" << (raw_kelly_est * kellyFraction * 100.0)
                      << "%) that exceeds the 10% hard cap. Kelly will be clamped on every trade."
                      << " This defeats dynamic sizing — consider lowering KELLY_FRACTION." << std::endl;
        }

        Logger::log("INFO", "[KELLY] Configured: Win Rate=" + std::to_string(kellyWinRate)
                    + " | Win/Loss Ratio=" + std::to_string(kellyWinLossRatio)
                    + " | Fraction=" + std::to_string(kellyFraction)
                    + " | Est. Adjusted Kelly=" + std::to_string(raw_kelly_est * kellyFraction * 100.0) + "%");

        // RULE-007 / RULE-013: Telegram vars are required for dual-channel
        // observability. Missing credentials are a fatal startup error, not a
        // silent no-op — a system that can act without alerting violates RULE-013.
        require_env("TELEGRAM_BOT_TOKEN");
        require_env("TELEGRAM_CHAT_ID");

        // CN-RULE-001: Board-lot size. Default is 1 (standard for all US-listed equities
        // and US-listed Chinese ADRs on Alpaca). Set CN_BOARD_LOT_SIZE=100 only when
        // routing orders to an actual Chinese A-share exchange (where 1手 = 100 shares).
        // The old default of 100 silently killed every US stock order because
        // 1% of $100k / typical US price < 100 shares → lot_qty = 0 → abort.
        cnBoardLotSize = 1;
        const char* lot_env = std::getenv("CN_BOARD_LOT_SIZE");
        if (lot_env && std::string(lot_env) != "") {
            try {
                int parsed = std::stoi(std::string(lot_env));
                if (parsed > 0) {
                    cnBoardLotSize = parsed;
                } else {
                    std::cerr << "[WARN] [EXECUTION] CN_BOARD_LOT_SIZE must be positive. "
                              << "Using default of 1." << std::endl;
                }
            } catch (...) {
                std::cerr << "[WARN] [EXECUTION] CN_BOARD_LOT_SIZE is not a valid integer. "
                          << "Using default of 1." << std::endl;
            }
        }
        Logger::log("INFO", "[CN-RULE-001] Board-lot size: " + std::to_string(cnBoardLotSize) + " shares.");

        // CN-RULE-002: Path for T+1 position persistence file.
        // Override with CN_POSITIONS_PATH env var, default to /app/data (volume-mounted).
        // /tmp is ephemeral in Docker; losing state mid-day would clear the T+1 sell gate.
        const char* pos_path_env = std::getenv("CN_POSITIONS_PATH");
        cnPositionsPath = (pos_path_env && std::string(pos_path_env) != "")
            ? std::string(pos_path_env)
            : "/app/data/china_positions.json";
        Logger::log("INFO", "[CN-RULE-002] T+1 positions persistence path: " + cnPositionsPath);
        load_china_positions();

        // Equity trailing-stop tracking persistence (survives restarts + reconciles
        // against Alpaca so no open position is ever orphaned by a restart).
        const char* eq_path_env = std::getenv("EQUITY_POSITIONS_PATH");
        equityPositionsPath_ = (eq_path_env && std::string(eq_path_env) != "")
            ? std::string(eq_path_env)
            : "/app/data/equity_positions.json";
        Logger::log("INFO", "[TRAILING_STOP_MONITOR] Equity positions persistence path: " +
                    equityPositionsPath_);
        load_and_reconcile_equity_positions();

        // ── Options signal generator profiles (all env vars optional) ──────────
        {
            // Helper: parse a comma-separated watchlist string into a vector
            auto parseWatchlist = [](const std::string& s) {
                std::vector<std::string> v;
                std::istringstream ss(s);
                std::string tok;
                while (std::getline(ss, tok, ','))
                    if (!tok.empty()) v.push_back(tok);
                return v;
            };
            auto envStr  = [](const char* k, const std::string& def) -> std::string {
                const char* v = std::getenv(k);
                return (v && std::string(v) != "") ? std::string(v) : def;
            };
            auto envBool = [](const char* k) -> bool {
                const char* v = std::getenv(k);
                return v && (std::string(v) == "true" || std::string(v) == "1");
            };
            auto envInt  = [](const char* k, int def) -> int {
                const char* v = std::getenv(k);
                if (!v || std::string(v).empty()) return def;
                try { return std::max(1, std::stoi(std::string(v))); } catch (...) { return def; }
            };
            auto envDbl  = [](const char* k, double def) -> double {
                const char* v = std::getenv(k);
                if (!v || std::string(v).empty()) return def;
                try { return std::stod(std::string(v)); } catch (...) { return def; }
            };

            // ── BOT profile — conservative automated trading ──────────────────
            optionsBotProfile_ = nox::options_signal::RiskProfile::bot();
            optionsBotProfile_.watchlist = parseWatchlist(
                envStr("OPTIONS_BOT_WATCHLIST", "SPY,QQQ,AAPL,TSLA,NVDA"));
            optionsBotProfile_.scan_interval_minutes =
                envInt("OPTIONS_BOT_SCAN_INTERVAL_MINUTES", 30);
            optionsBotProfile_.auto_execute =
                envBool("OPTIONS_BOT_AUTO_EXECUTE");
            optionsBotProfile_.free_capital_amount =
                envDbl("OPTIONS_BOT_FREE_CAPITAL_AMOUNT", 0.0);
            optionsBotProfile_.max_signals_per_scan =
                envInt("OPTIONS_BOT_MAX_SIGNALS", 3);

            // ── PERSONAL profile — high-risk-tolerance advisory signals ────────
            optionsPersonalProfile_ = nox::options_signal::RiskProfile::personal();
            optionsPersonalProfile_.watchlist = parseWatchlist(
                envStr("OPTIONS_PERSONAL_WATCHLIST", "SPY,QQQ,AAPL,TSLA,NVDA,AMZN,META"));
            optionsPersonalProfile_.scan_interval_minutes =
                envInt("OPTIONS_PERSONAL_SCAN_INTERVAL_MINUTES", 30);
            optionsPersonalProfile_.auto_execute = false; // personal signals are advisory only
            optionsPersonalProfile_.free_capital_amount =
                envDbl("OPTIONS_PERSONAL_FREE_CAPITAL_AMOUNT", 0.0);
            optionsPersonalProfile_.max_signals_per_scan =
                envInt("OPTIONS_PERSONAL_MAX_SIGNALS", 2);

            // ── BREAKOUT profile — LEAP advisory signals on strong trend breakouts ──
            // Always advisory (auto_execute forced false below regardless of the
            // factory default) — this scans for 6-month directional conviction
            // plays, not something to auto-fire real orders on.
            optionsBreakoutProfile_ = nox::options_signal::RiskProfile::breakout();
            optionsBreakoutProfile_.watchlist = parseWatchlist(
                envStr("OPTIONS_BREAKOUT_WATCHLIST",
                       "SPY,QQQ,IWM,AAPL,MSFT,NVDA,AMD,TSLA,AMZN,META,GOOGL,NFLX,COIN,PLTR,MSTR,SHOP,ARKK,SOXX,GLD,XLF"));
            optionsBreakoutProfile_.scan_interval_minutes =
                envInt("OPTIONS_BREAKOUT_SCAN_INTERVAL_MINUTES", 30);
            optionsBreakoutProfile_.auto_execute = false; // breakout signals are advisory only
            optionsBreakoutProfile_.free_capital_amount =
                envDbl("OPTIONS_BREAKOUT_FREE_CAPITAL_AMOUNT", 0.0);
            optionsBreakoutProfile_.max_signals_per_scan =
                envInt("OPTIONS_BREAKOUT_MAX_SIGNALS", 2);

            // ── Equity signal scanner (independent of Skeptic) ─────────────────
            equityScanEnabled_ = envBool("EQUITY_SCAN_ENABLED") ||
                [](){ const char* v = std::getenv("EQUITY_SCAN_ENABLED");
                      return !v || std::string(v) == ""; }(); // default on if unset
            // Honour explicit false
            if (const char* v = std::getenv("EQUITY_SCAN_ENABLED")) {
                std::string sv(v);
                std::transform(sv.begin(), sv.end(), sv.begin(),
                    [](unsigned char c){ return std::tolower(c); });
                equityScanEnabled_ = (sv == "true" || sv == "1" || sv == "yes" || sv == "");
            }
            equityWatchlist_ = parseWatchlist(
                envStr("EQUITY_SCAN_WATCHLIST", "AAPL,MSFT,NVDA,TSLA,AMZN,META,GOOGL,AMD"));
            equityScanIntervalMinutes_ = envInt("EQUITY_SCAN_INTERVAL_MINUTES", 30);
            equityMaxSignals_          = envInt("EQUITY_SCAN_MAX_SIGNALS", 2);
            equityBypassHours_         = envBool("EQUITY_SCAN_BYPASS_HOURS");

            // ── Futures signal scanner (signals only — see CLAUDE.md) ──────────
            futuresScanEnabled_ = envBool("FUTURES_SCAN_ENABLED"); // default off
            futuresWatchlist_ = parseWatchlist(envStr("FUTURES_WATCHLIST", "CL"));
            futuresScanIntervalMinutes_ = envInt("FUTURES_SCAN_INTERVAL_MINUTES", 60);
            futuresAlertThreshold_      = envDbl("FUTURES_ALERT_THRESHOLD", 0.5);
            massiveApiKey_ = envStr("MASSIVE_API_KEY", "");

            // Rule-based equity exits (defaults on; per-rule tunable via .env).
            {
                const char* re = std::getenv("EQUITY_RULE_EXITS_ENABLED");
                if (re && std::string(re) != "") {
                    std::string sv(re);
                    std::transform(sv.begin(), sv.end(), sv.begin(),
                        [](unsigned char c){ return std::tolower(c); });
                    equityRuleExitsEnabled_ = (sv == "true" || sv == "1" || sv == "yes");
                }
            }
            equityExitTakeProfitPct_ = envDbl("EQUITY_EXIT_TAKE_PROFIT_PCT", 0.15);
            equityExitStopLossPct_   = envDbl("EQUITY_EXIT_STOP_LOSS_PCT",   0.10);
            equityExitRsiCeiling_    = envDbl("EQUITY_EXIT_RSI_CEILING",     78.0);
            equityExitSmaBreak_      = [](){ const char* v = std::getenv("EQUITY_EXIT_SMA_BREAK");
                                            return !v || std::string(v) == "" ||
                                                   std::string(v) == "true" || std::string(v) == "1"; }();
            equityExitMaxHoldDays_   = envInt("EQUITY_EXIT_MAX_HOLD_DAYS", 0);

            // EQUITY_SELL_QTY_MODE: how to size SELL orders
            // "full" (default): liquidate entire position (original behavior)
            // "kelly": recalculate qty using same Kelly sizing as BUY
            // "prorated": scale by current notional vs entry notional
            equitySellQtyMode_ = envStr("EQUITY_SELL_QTY_MODE", "full");
            {
                std::string mode = equitySellQtyMode_;
                std::transform(mode.begin(), mode.end(), mode.begin(),
                    [](unsigned char c){ return std::tolower(c); });
                if (mode != "full" && mode != "kelly" && mode != "prorated") {
                    Logger::log("WARN",
                        "[EQUITY_SELL] Invalid EQUITY_SELL_QTY_MODE='" + equitySellQtyMode_ +
                        "'. Must be 'full', 'kelly', or 'prorated'. Defaulting to 'full'.");
                    equitySellQtyMode_ = "full";
                }
            }

            // Validation: exit rule conflicts (RULE-019).
            if (equityRuleExitsEnabled_ && equityExitMaxHoldDays_ > 0 &&
                equityExitTakeProfitPct_ <= 0.0 && equityExitStopLossPct_ <= 0.0 &&
                equityExitRsiCeiling_ <= 0.0 && !equityExitSmaBreak_) {
                Logger::log("WARN",
                    "[EQUITY_EXIT] Only MaxHoldDays is enabled. Positions will only exit after "
                    + std::to_string(equityExitMaxHoldDays_) + " days with no profit/loss gates. "
                    "Consider enabling EQUITY_EXIT_TAKE_PROFIT_PCT or EQUITY_EXIT_STOP_LOSS_PCT.");
            }
            if (!equityRuleExitsEnabled_ && equityExitMaxHoldDays_ > 0) {
                Logger::log("WARN",
                    "[EQUITY_EXIT] EQUITY_RULE_EXITS_ENABLED=false disables ALL rules including "
                    "EQUITY_EXIT_MAX_HOLD_DAYS=" + std::to_string(equityExitMaxHoldDays_) +
                    ". Set EQUITY_RULE_EXITS_ENABLED=true to enforce time-based exits.");
            }

            Logger::log("INFO", std::string("[EQUITY_EXIT] Rule-based exits ") +
                (equityRuleExitsEnabled_ ? "ENABLED" : "DISABLED") +
                " | TP=" + std::to_string(equityExitTakeProfitPct_ * 100.0) + "%" +
                " | SL=" + std::to_string(equityExitStopLossPct_ * 100.0) + "%" +
                " | RSI≥" + std::to_string(equityExitRsiCeiling_) +
                " | SMA20-break=" + (equityExitSmaBreak_ ? "on" : "off") +
                " | MaxHold=" + std::to_string(equityExitMaxHoldDays_) + "d" +
                " | SellQtyMode=" + equitySellQtyMode_);

            Logger::log("INFO", "[EQUITY_SCAN] " +
                std::string(equityScanEnabled_ ? "ENABLED" : "DISABLED") +
                " | Watchlist=" + std::to_string(equityWatchlist_.size()) + " tickers" +
                " | Interval=" + std::to_string(equityScanIntervalMinutes_) + "min" +
                " | MaxSignals=" + std::to_string(equityMaxSignals_) +
                (equityBypassHours_ ? " | BypassHours=ON" : ""));

            Logger::log("INFO", "[OPTIONS_SIGNAL] BOT profile: AutoExecute="
                + std::string(optionsBotProfile_.auto_execute ? "ON" : "OFF (advisory)")
                + " | Watchlist=" + std::to_string(optionsBotProfile_.watchlist.size()) + " tickers"
                + " | Interval=" + std::to_string(optionsBotProfile_.scan_interval_minutes) + "min");
            Logger::log("INFO", "[OPTIONS_SIGNAL] PERSONAL profile: always advisory"
                + std::string(" | Watchlist=")
                + std::to_string(optionsPersonalProfile_.watchlist.size()) + " tickers"
                + " | Interval=" + std::to_string(optionsPersonalProfile_.scan_interval_minutes) + "min"
                + (optionsPersonalProfile_.free_capital_amount > 0.0
                    ? " | FreeCapital=$" + std::to_string(optionsPersonalProfile_.free_capital_amount)
                    : ""));
            Logger::log("INFO", "[OPTIONS_SIGNAL] BREAKOUT profile: always advisory (LEAP)"
                + std::string(" | Watchlist=")
                + std::to_string(optionsBreakoutProfile_.watchlist.size()) + " tickers"
                + " | Interval=" + std::to_string(optionsBreakoutProfile_.scan_interval_minutes) + "min");
        }

        // Initialize and start the Position Manager
        try {
            auto order_router = std::make_shared<nox::options_router::OptionsOrderRouter>(
                alpacaBaseUrl, apiKey, apiSec
            );
            optionsOrderRouter_ = order_router; // keep a handle for reconciliation lookups
            // MEMORY_BANK_PATH must point to the volume-mounted data directory so the
            // options position DB survives container restarts. Default: /app/data.
            const char* mb_env = std::getenv("MEMORY_BANK_PATH");
            memory_bank_path = (mb_env && std::string(mb_env) != "")
                ? std::string(mb_env)
                : "/app/data/memory_bank.db";
            positionManager_ = std::make_unique<PositionManager>(memory_bank_path, *order_router);
            positionManager_->start_monitoring();
            Logger::log("INFO", "[POS_MANAGER] Position Manager initialized and monitoring thread started.");

            // Phase 1: the order ledger shares the same DB file (WAL makes the
            // second handle safe). Constructed after PositionManager so the file
            // already exists; failure here must not disable trading.
            try {
                orderLedger_ = std::make_unique<nox::execution::OrderLedger>(memory_bank_path);
                Logger::log("INFO", "[ORDER_LEDGER] Client-order-ID ledger initialized.");
                // Startup reconciliation — resolve any orders left pending/unknown
                // by a crash or restart against broker truth before scanning resumes.
                reconcile_options_orders();
            } catch (const std::exception& e) {
                Logger::log("WARN", "[ORDER_LEDGER] Failed to initialize order ledger: " +
                    std::string(e.what()) + ". Ghost-fill protection degraded.");
            }

            // Phase 2, item C: true 52-week historical IV Rank, read from the same
            // `historical_volatility` table the Python heartbeat collects (and the
            // Polygon backfill script seeds). Failure here just means every
            // generator keeps using the same-snapshot proxy — never disables trading.
            try {
                ivRankStore_ = std::make_unique<nox::execution::IvRankStore>(memory_bank_path);
                Logger::log("INFO", "[IV_RANK] Historical IV rank store initialized.");
            } catch (const std::exception& e) {
                Logger::log("WARN", "[IV_RANK] Failed to initialize IV rank store: " +
                    std::string(e.what()) + ". Falling back to same-snapshot IV rank proxy.");
            }

            // Phase 3: alpha-decay tier-down. heartbeat/alpha_decay_monitor.py owns
            // the rolling-Sharpe-vs-baseline math and writes the multiplier here;
            // failure to open the store just means sizing stays at 1.0 — never
            // disables trading.
            try {
                alphaDecayStore_ = std::make_unique<nox::execution::AlphaDecayStore>(memory_bank_path);
                Logger::log("INFO", "[ALPHA_DECAY] Alpha decay store initialized.");
            } catch (const std::exception& e) {
                Logger::log("WARN", "[ALPHA_DECAY] Failed to initialize alpha decay store: " +
                    std::string(e.what()) + ". Position sizing unaffected (multiplier=1.0).");
            }

            // Global kill switch — persisted so a restart during a pause never
            // silently resumes trading. Failure to open the store just means
            // isPaused() always fails open (never blocks trading on its own) —
            // it never disables trading by itself, matching every other
            // optional-store init in this constructor.
            try {
                killSwitch_ = std::make_unique<nox::execution::KillSwitchStore>(memory_bank_path);
                auto ks = killSwitch_->get();
                Logger::log("INFO", std::string("[KILL_SWITCH] Store initialized. State: ") +
                    (ks.paused ? ("PAUSED (" + ks.triggered_by + "): " + ks.reason) : "active"));
            } catch (const std::exception& e) {
                Logger::log("WARN", "[KILL_SWITCH] Failed to initialize kill switch store: " +
                    std::string(e.what()) + ". /pause and the daily-loss-limit halt are unavailable.");
            }
            if (const char* v = std::getenv("MAX_DAILY_LOSS_DOLLARS")) {
                try { maxDailyLossDollars_ = -std::abs(std::stod(v)); } catch (...) {}
            }

            // Futures signal audit trail (signal-only — see CLAUDE.md). Failure
            // here just disables the futures scan thread further below; it
            // never touches options/equity trading.
            if (futuresScanEnabled_) {
                try {
                    futuresSignalStore_ = std::make_shared<nox::execution::FuturesSignalStore>(memory_bank_path);
                    Logger::log("INFO", "[FUTURES_SCAN] Futures signal store initialized.");
                } catch (const std::exception& e) {
                    Logger::log("WARN", "[FUTURES_SCAN] Failed to initialize futures signal store: " +
                        std::string(e.what()) + ". Futures scan thread will not start.");
                    futuresScanEnabled_ = false;
                }
            }
        } catch (const std::exception& e) {
            Logger::log("WARN", "[POS_MANAGER] Failed to initialize Position Manager: " +
                std::string(e.what()) + ". Options position tracking disabled; signal processing continues.");
            TelegramNotifier::sendMessage(
                "⚠️ *Position Manager Unavailable*\n"
                "────────────────────────\n"
                "SQLite init failed: `" + std::string(e.what()) + "`\n"
                "Options position tracking is disabled.\n"
                "Signal processing and order execution are unaffected."
            );
            positionManager_ = nullptr;
        }

#ifdef IBKR_ENABLED
        // ── IBKR venue initialisation ─────────────────────────────────────────
        {
            const char* venue_env = std::getenv("EXECUTION_VENUE");
            execution_venue_ = (venue_env && std::string(venue_env) == "ibkr") ? "ibkr" : "alpaca";
            Logger::log("INFO", "[VENUE] Execution venue: " + execution_venue_);

            if (execution_venue_ == "ibkr") {
                const char* host_env = std::getenv("IBKR_GATEWAY_HOST");
                const char* port_env = std::getenv("IBKR_GATEWAY_PORT");
                std::string host = host_env ? host_env : "127.0.0.1";
                int         port = port_env ? std::stoi(port_env) : 4002; // 4002=paper, 4001=live

                ibkr_wrapper_ = std::make_unique<nox::ibkr::IBKRWrapper>();
                ibkr_conn_    = std::make_unique<nox::ibkr::IBKRConnection>(*ibkr_wrapper_);

                if (!ibkr_conn_->connect(host.c_str(), port)) {
                    std::cerr << "[FATAL] [IBKR] Failed to connect to IB Gateway at "
                              << host << ":" << port << std::endl;
                    std::exit(1);
                }

                // Wait up to 5 s for nextValidId handshake.
                int waited = 0;
                while (!ibkr_wrapper_->hasValidOrderId() && ++waited < 100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));

                if (!ibkr_wrapper_->hasValidOrderId()) {
                    std::cerr << "[FATAL] [IBKR] Timed out waiting for nextValidId from gateway.\n";
                    std::exit(1);
                }

                ibkr_router_ = std::make_unique<nox::ibkr::IBKROrderRouter>(*ibkr_conn_, *ibkr_wrapper_);
                Logger::log("INFO", "[IBKR] Gateway handshake complete — IBKR execution active.");
                TelegramNotifier::sendMessage(
                    "🔌 *IBKR Gateway Connected*\n"
                    "────────────────────────\n"
                    "• *Host:* " + host + ":" + std::to_string(port) + "\n"
                    "• *Venue:* Interactive Brokers (paper)\n"
                    "• *Status:* nextValidId received — ready to route orders."
                );
            }
        }
#endif

    }

    void run() {
        httplib::Server svr;

        svr.Get("/health", [](const httplib::Request &, httplib::Response &res) {
            json health_response = {{"status", "healthy"}};
            res.set_content(health_response.dump(), "application/json");
        });

        // Global kill switch — heartbeat/monitor.py's /pause and /resume Telegram
        // commands POST here. Body is optional JSON {"reason": "..."}; falls back
        // to a generic operator-halt reason if omitted/unparseable.
        svr.Post("/pause", [this](const httplib::Request& req, httplib::Response& res) {
            if (!killSwitch_) {
                res.status = 503;
                res.set_content(json{{"error", "kill switch store unavailable"}}.dump(), "application/json");
                return;
            }
            std::string reason = "Operator-triggered halt via /pause";
            try {
                auto body = json::parse(req.body);
                if (body.contains("reason") && body["reason"].is_string() && !body["reason"].get<std::string>().empty())
                    reason = body["reason"].get<std::string>();
            } catch (...) {}
            killSwitch_->pause(reason, "operator");
            Logger::log("WARN", "[KILL_SWITCH] Operator paused trading: " + reason);
            TelegramNotifier::sendMessage(
                "🛑 *TRADING PAUSED (operator)*\n"
                "────────────────────────\n"
                "• *Reason:* " + reason + "\n"
                "⛔ All new entries halted. Existing positions are untouched. Send /resume to clear."
            );
            res.set_content(json{{"status", "paused"}, {"reason", reason}}.dump(), "application/json");
        });

        svr.Post("/resume", [this](const httplib::Request&, httplib::Response& res) {
            if (!killSwitch_) {
                res.status = 503;
                res.set_content(json{{"error", "kill switch store unavailable"}}.dump(), "application/json");
                return;
            }
            killSwitch_->resume();
            Logger::log("INFO", "[KILL_SWITCH] Operator resumed trading.");
            TelegramNotifier::sendMessage("✅ *TRADING RESUMED*\nNew entries are no longer blocked.");
            res.set_content(json{{"status", "active"}}.dump(), "application/json");
        });

        svr.Get("/kill-switch-status", [this](const httplib::Request&, httplib::Response& res) {
            if (!killSwitch_) {
                res.set_content(json{{"paused", false}, {"available", false}}.dump(), "application/json");
                return;
            }
            auto ks = killSwitch_->get();
            res.set_content(json{
                {"paused",       ks.paused},
                {"reason",       ks.reason},
                {"triggered_by", ks.triggered_by},
                {"triggered_at", ks.triggered_at},
                {"available",    true}
            }.dump(), "application/json");
        });

        svr.Get("/last-report", [this](const httplib::Request &, httplib::Response &res) {
            json response = {
                {"last_analyst_report", last_analyst_report_time.empty() ? "Never" : last_analyst_report_time}
            };
            res.set_content(response.dump(), "application/json");
        });

        // Returns the last 50 authenticated signals received by the webhook.
        // Used by the heartbeat /signals Telegram command to verify signal flow.
        svr.Get("/recent-signals", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(signal_log_mutex_);
            json arr = json::array();
            for (const auto& e : signal_log_) {
                arr.push_back({
                    {"received_at", e.received_at},
                    {"ticker",      e.ticker},
                    {"action",      e.action},
                    {"price",       e.price},
                    {"rsi",         e.rsi},
                    {"vix",         e.vix},
                    {"source",      e.source}
                });
            }
            res.set_content(arr.dump(), "application/json");
        });

        // Simple diagnostic surface for CN-RULE-001/002: one command answers "is
        // CN A-share protection currently active, and what does it think it's
        // tracking" without grepping logs. gate_active mirrors the exact condition
        // used by both the board-lot truncation and the T+1 gate (cnBoardLotSize > 1).
        svr.Get("/cn-status", [this](const httplib::Request&, httplib::Response& res) {
            std::string today = get_today_date_string();
            json positions = json::array();
            {
                std::lock_guard<std::mutex> lock(china_positions_mutex_);
                for (const auto& kv : china_positions_) {
                    positions.push_back({
                        {"ticker",     kv.first},
                        {"entry_date", kv.second.entry_date},
                        {"cleared",    kv.second.entry_date < today}
                    });
                }
            }
            json response = {
                {"board_lot_size", cnBoardLotSize},
                {"gate_active",    cnBoardLotSize > 1},
                {"today",          today},
                {"positions",      positions}
            };
            res.set_content(response.dump(), "application/json");
        });

        svr.Get("/daily-options-accuracy", [this](const httplib::Request&, httplib::Response& res) {
            if (!positionManager_) {
                res.status = 500;
                res.set_content(json{{"error", "PositionManager not initialized"}}.dump(), "application/json");
                return;
            }

            try {
                std::string today = get_today_date_string();
                std::vector<json> trades;
                int wins = 0, losses = 0, breakeven = 0;
                double total_win = 0.0, total_loss = 0.0;
                double cumulative_pnl = 0.0;

                sqlite3* db = nullptr;
                if (sqlite3_open(memory_bank_path.c_str(), &db) != SQLITE_OK) {
                    res.status = 500;
                    res.set_content(json{{"error", "Cannot open database"}}.dump(), "application/json");
                    return;
                }

                sqlite3_busy_timeout(db, 5000);

                const char* sql = R"(
                    SELECT id, timestamp, ticker, action, price, pnl, detail
                    FROM trade_history
                    WHERE asset_class = 'option'
                    AND DATE(timestamp) = DATE(?)
                    ORDER BY timestamp ASC
                )";

                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                    sqlite3_close(db);
                    res.status = 500;
                    res.set_content(json{{"error", "Query failed"}}.dump(), "application/json");
                    return;
                }

                sqlite3_bind_text(stmt, 1, today.c_str(), -1, SQLITE_TRANSIENT);

                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    double pnl = sqlite3_column_double(stmt, 5);
                    std::string action = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                    std::string detail_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));

                    // Parse detail for OPEN: "STRATEGY_TYPE"
                    // Parse detail for CLOSE: "call|put $STRIKE | exit_reason"
                    std::string option_type = "unknown";
                    double strike = 0.0;
                    std::string exit_reason = "";

                    if (action == "CLOSE") {
                        // Format: "call 100 | 50% Profit Rule (Long)"
                        size_t pipe_pos = detail_str.find('|');
                        if (pipe_pos != std::string::npos) {
                            std::string type_strike = detail_str.substr(0, pipe_pos);
                            exit_reason = detail_str.substr(pipe_pos + 2); // skip "| "

                            // Parse "call 100" or "put 105.5"
                            size_t space_pos = type_strike.find(' ');
                            if (space_pos != std::string::npos) {
                                option_type = type_strike.substr(0, space_pos);
                                try {
                                    strike = std::stod(type_strike.substr(space_pos + 1));
                                } catch (...) {}
                            }
                        }
                    } else if (action == "OPEN") {
                        // detail_str contains strategy name (e.g., "LONG_CALL" or "CSP (multi-leg: ...)")
                        option_type = detail_str;
                    }

                    json trade;
                    trade["id"] = sqlite3_column_int(stmt, 0);
                    trade["timestamp"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    trade["ticker"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    trade["action"] = action;
                    trade["price"] = sqlite3_column_double(stmt, 4);
                    trade["pnl"] = pnl;
                    trade["option_type"] = option_type;
                    if (strike > 0.0) {
                        trade["strike"] = strike;
                    }
                    if (action == "CLOSE") {
                        trade["exit_reason"] = exit_reason;
                    }
                    trade["raw_detail"] = detail_str;

                    if (action == "CLOSE") {
                        cumulative_pnl += pnl;
                        if (pnl > 0.01) {
                            wins++;
                            total_win += pnl;
                        } else if (pnl < -0.01) {
                            losses++;
                            total_loss += pnl;
                        } else {
                            breakeven++;
                        }
                    }

                    trades.push_back(trade);
                }

                sqlite3_finalize(stmt);
                sqlite3_close(db);

                int total_trades = wins + losses + breakeven;
                double win_rate = (total_trades > 0) ? (100.0 * wins / total_trades) : 0.0;
                double avg_win = (wins > 0) ? (total_win / wins) : 0.0;
                double avg_loss = (losses > 0) ? (total_loss / losses) : 0.0;
                double profit_factor = (std::abs(total_loss) > 0.01) ? (total_win / std::abs(total_loss)) : 0.0;

                json response = {
                    {"date", today},
                    {"summary", {
                        {"total_trades", total_trades},
                        {"wins", wins},
                        {"losses", losses},
                        {"breakeven", breakeven},
                        {"win_rate_pct", win_rate},
                        {"avg_win_usd", avg_win},
                        {"avg_loss_usd", avg_loss},
                        {"profit_factor", profit_factor},
                        {"cumulative_pnl_usd", cumulative_pnl}
                    }},
                    {"trades", trades}
                };

                res.set_content(response.dump(), "application/json");

            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        svr.Post("/options/price", [](const httplib::Request& req, httplib::Response& res) {
            try {
                json body = json::parse(req.body);

                nox::options::OptionContract contract;
                contract.symbol         = body.value("symbol", "");
                contract.strike         = body.value("strike", 0.0);
                contract.underlying     = body.value("underlying", 0.0);
                contract.expiry         = body.value("expiry", 0.0);
                contract.risk_free_rate = body.value("risk_free_rate", 0.05);
                contract.volatility     = body.value("volatility", 0.0);
                contract.type           = (body.value("option_type", "call") == "put")
                                          ? nox::options::OptionType::Put
                                          : nox::options::OptionType::Call;

                bool solve_iv = body.value("solve_iv", false);
                nox::options::OptionGreeks greeks = nox::options::compute_greeks(contract, solve_iv);

                json response = {
                    {"symbol",             contract.symbol},
                    {"option_type",        (contract.type == nox::options::OptionType::Call) ? "call" : "put"},
                    {"price",              greeks.price},
                    {"delta",              greeks.delta},
                    {"gamma",              greeks.gamma},
                    {"theta",              greeks.theta},
                    {"vega",               greeks.vega},
                    {"rho",                greeks.rho},
                    {"implied_volatility", greeks.implied_volatility}
                };
                res.set_content(response.dump(), "application/json");

            } catch (const json::parse_error& e) {
                res.status = 400;
                res.set_content(std::string("Bad Request: ") + e.what(), "text/plain");
            } catch (const std::exception& e) {
                res.status = 422;
                res.set_content(std::string("Pricing error: ") + e.what(), "text/plain");
            }
        });

        svr.Post("/webhook", [this](const httplib::Request& req, httplib::Response& res) {
            std::string body = req.body;
            int success_count = 0;

            try {
                // Parse cleaner using standard json parser instead of hand-rolled brace split logic
                json root_payload = json::parse(body);

                auto process_single_chunk = [this, &success_count](const json& data) {
                    // RULE-004 Auth Gate: secret mismatch must be a SILENT DROP.
                    // Returning HTTP 400 here would fingerprint the auth boundary
                    // to a caller, leaking that the secret was wrong vs. malformed.
                    // The outer handler returns 400 only for schema/parse failures.
                    if (data.value("secret_key", "") != secret) {
                        Logger::log("WARN", "[EXECUTION] Unauthorized signal silently dropped (auth gate).");
                        return;
                    }

                    TradeSignal signal;
                    signal.ticker = data.value("ticker", "UNKNOWN");
                    signal.action = data.value("action", "HOLD");

                    // Fallbacks for potential mismatched data types in inputs
                    if (data.contains("price")) {
                        if (data["price"].is_number()) {
                            signal.price = data["price"].get<double>();
                        } else if (data["price"].is_string()) {
                            signal.price = std::stod(data["price"].get<std::string>());
                        }
                    }
                    if (data.contains("rsi")) {
                        if (data["rsi"].is_number()) {
                            signal.rsi = data["rsi"].get<double>();
                        } else if (data["rsi"].is_string()) {
                            signal.rsi = std::stod(data["rsi"].get<std::string>());
                        }
                    }
                    if (data.contains("vol")) {
                        if (data["vol"].is_number_integer()) {
                            signal.vol = data["vol"].get<long long>();
                        } else if (data["vol"].is_string()) {
                            signal.vol = std::stoll(data["vol"].get<std::string>());
                        }
                    }
                    if (data.contains("atr")) {
                        if (data["atr"].is_number()) {
                            signal.atr = data["atr"].get<double>();
                        } else if (data["atr"].is_string()) {
                            signal.atr = std::stod(data["atr"].get<std::string>());
                        }
                    }
                    if (data.contains("stop_loss_atr_multiplier")) {
                        if (data["stop_loss_atr_multiplier"].is_number()) {
                            signal.stop_loss_atr_multiplier = data["stop_loss_atr_multiplier"].get<double>();
                        } else if (data["stop_loss_atr_multiplier"].is_string()) {
                            signal.stop_loss_atr_multiplier = std::stod(data["stop_loss_atr_multiplier"].get<std::string>());
                        }
                    }
                    if (data.contains("vix")) {
                        if (data["vix"].is_number()) signal.vix = data["vix"].get<double>();
                        else if (data["vix"].is_string()) signal.vix = std::stod(data["vix"].get<std::string>());
                    }
                    if (data.contains("spy_price")) {
                        if (data["spy_price"].is_number()) signal.spy_price = data["spy_price"].get<double>();
                        else if (data["spy_price"].is_string()) signal.spy_price = std::stod(data["spy_price"].get<std::string>());
                    }
                    if (data.contains("spy_200_sma")) {
                        if (data["spy_200_sma"].is_number()) signal.spy_200_sma = data["spy_200_sma"].get<double>();
                        else if (data["spy_200_sma"].is_string()) signal.spy_200_sma = std::stod(data["spy_200_sma"].get<std::string>());
                    }
                    if (data.contains("risk_tier")) {
                        if (data["risk_tier"].is_number_integer()) {
                            signal.risk_tier = data["risk_tier"].get<int>();
                        } else if (data["risk_tier"].is_string()) {
                            signal.risk_tier = std::stoi(data["risk_tier"].get<std::string>());
                        }
                    }
                    // CN-RULE-001/002: Optional trade date for backtester mode.
                    // Expected format: "YYYY-MM-DD". If absent, live system date is used.
                    if (data.contains("trade_date") && data["trade_date"].is_string()) {
                        signal.trade_date = data["trade_date"].get<std::string>();
                    }

                    Logger::log("INFO", "Signal Parsed successfully: " + signal.action + " " + signal.ticker);
                    record_signal(signal);

                    // Fast path for analyst audit reports: acknowledge immediately
                    // so the caller does not time out waiting on downstream network
                    // work (Telegram delivery, Alpaca checks, etc.).
                    if (signal.action == "REPORT") {
                        if (data.contains("report_body")) {
                            Logger::log("INFO", "[EXECUTION] Analyst report: " + data.value("report_body", ""));
                        }
                        // Update the last analyst report timestamp
                        if (signal.ticker == "GLOBAL_AUDIT") {
                            auto now = std::chrono::system_clock::now();
                            auto time_t = std::chrono::system_clock::to_time_t(now);
                            std::tm tm_buf{};
                            std::stringstream ss;
                            ss << std::put_time(::gmtime_r(&time_t, &tm_buf), "%Y-%m-%dT%H:%M:%SZ");
                            last_analyst_report_time = ss.str();
                        }
                        success_count++;
                        return;
                    }

                    // ─── CRITICAL: Idempotency Check (prevent duplicate orders) ────────
                    auto [is_dup, cached_order_id] = check_idempotency(signal);
                    if (is_dup) {
                        Logger::log("CRITICAL",
                            "[IDEMPOTENCY] DUPLICATE SIGNAL DETECTED AND BLOCKED: " + signal.action + " " +
                            signal.ticker + " @ $" + std::to_string(signal.price) +
                            ". Previously placed order: " + (cached_order_id.empty() ? "UNKNOWN" : cached_order_id));
                        TelegramNotifier::sendMessage(
                            "🚫 *DUPLICATE ORDER BLOCKED*\n"
                            "────────────────────────\n"
                            "A duplicate signal was received within 5 minutes.\n"
                            "• *Action:* " + signal.action + "\n"
                            "• *Ticker:* " + signal.ticker + "\n"
                            "• *Price:* $" + std::to_string(signal.price) + "\n"
                            "• *Previous Order ID:* " + (cached_order_id.empty() ? "N/A" : cached_order_id) + "\n"
                            "⚠️ *Second order was NOT placed* — webhook caller should verify first attempt succeeded."
                        );
                        success_count++;
                        return;
                    }
                    // ──────────────────────────────────────────────────────────────

                    process(signal);
                    success_count++;
                };

                if (root_payload.is_array()) {
                    for (const auto& item : root_payload) {
                        process_single_chunk(item);
                    }
                } else if (root_payload.is_object()) {
                    process_single_chunk(root_payload);
                } else {
                    Logger::log("WARN", "Payload is neither an object nor an array.");
                }

            } catch (const json::parse_error& e) {
                // RULE-004 Schema Gate: malformed JSON is rejected with HTTP 400
                // before any field access or business logic executes.
                Logger::log("ERROR", "[EXECUTION] JSON parse error (schema gate): " + std::string(e.what()));
                res.status = 400;
                res.set_content("Bad Request: malformed JSON", "text/plain");
                return;
            } catch (const json::type_error& e) {
                Logger::log("ERROR", "[EXECUTION] Type mismatch in payload: " + std::string(e.what()));
                res.status = 400;
                res.set_content("Bad Request: type error", "text/plain");
                return;
            } catch (const std::exception& e) {
                Logger::log("ERROR", "[EXECUTION] Exception processing signals: " + std::string(e.what()));
                res.status = 500;
                res.set_content("Internal Server Error", "text/plain");
                return;
            }

            // RULE-004: Auth failures leave success_count == 0 but the payload
            // itself was valid JSON. Return 200 to avoid fingerprinting the auth
            // boundary — the drop has already been logged internally as WARN.
            res.status = 200;
            res.set_content("Processed " + std::to_string(success_count) + " signal(s)", "text/plain");
        }); // This perfectly closes the svr.Post router lambda

        // ── Options signal scanner — two threads, one per profile ──────────────
        // Threads are stored and joined on shutdown so SIGTERM drains cleanly
        // rather than terminating mid-scan or mid-order.
        auto launchOptionsThread = [this](nox::options_signal::RiskProfile profile, int stagger_seconds) {
            option_threads_.emplace_back([this, profile, stagger_seconds]() {
                std::string tg_token = std::getenv("TELEGRAM_BOT_TOKEN") ? std::getenv("TELEGRAM_BOT_TOKEN") : "";
                std::string tg_chat  = std::getenv("TELEGRAM_CHAT_ID")   ? std::getenv("TELEGRAM_CHAT_ID")   : "";

                nox::options_signal::OptionsSignalGenerator generator(
                    alpacaBaseUrl, apiKey, apiSec, tg_token, tg_chat, profile);

                // Phase 2, item C: all three profiles (bot/personal/breakout) get the
                // true historical IV rank, not just the auto-executing one — it feeds
                // scoring/thresholds for advisory alerts too.
                if (ivRankStore_) generator.set_iv_rank_store(ivRankStore_.get());
                if (alphaDecayStore_) generator.set_alpha_decay_store(alphaDecayStore_.get());

#ifdef IBKR_ENABLED
                // Phase 3: IBKR combo/BAG order routing. Options auto-execute
                // still requires the Alpaca-shaped OptionsSignal → contract
                // translation; only the FINAL broker call moves to IBKR.
                if (execution_venue_ == "ibkr" && ibkr_conn_ && ibkr_wrapper_) {
                    generator.set_order_execution_override(
                        [this](const nox::options_signal::OptionsSignal& s, int qty,
                               const std::string& /*client_oid*/) -> nox::options_router::OrderResult {
                            nox::ibkr::IBKROrderRouter ibkr_router(*ibkr_conn_, *ibkr_wrapper_);
                            auto r = ibkr_router.route(s, qty);
                            using D = nox::options_router::OrderDisposition;
                            D disp = r.disposition == nox::ibkr::IBKROrderDisposition::Accepted ? D::Accepted
                                    : r.disposition == nox::ibkr::IBKROrderDisposition::Timeout  ? D::Timeout
                                                                                                  : D::Rejected;
                            return {r.success, r.order_id, r.message, disp};
                        });
                }
#endif

                // Persist every auto-executed option so the exit monitor can manage
                // it (50%/stop/21-DTE) and it lands in the trade ledger for reports.
                generator.set_execution_recorder(
                    [this](const nox::options_signal::OptionsSignal& s, int qty) {
                        if (!positionManager_) return;
                        const bool is_short   = (s.strategy == "CSP" || s.strategy == "CC");
                        const bool single_leg = (s.strategy == "LONG_CALL" || s.strategy == "LONG_PUT" ||
                                                 s.strategy == "CSP"       || s.strategy == "CC");
                        std::string profile_type = is_short ? "short_premium" : "long";
                        std::string opt_type = (s.option_type == nox::options::OptionType::Call)
                                                   ? "call" : "put";
                        std::string entry_date = get_today_date_string();
                        if (single_leg) {
                            positionManager_->add_position(
                                s.underlying, opt_type, s.strike, qty, s.entry_price,
                                entry_date, profile_type, s.expiry_date);
                        } else {
                            auto legs = spread_legs_for(s.strategy, s.strike, s.strike2,
                                                        s.strike3, s.strike4);
                            if (!legs.empty()) {
                                positionManager_->add_spread_position(
                                    s.underlying, s.strategy, qty,
                                    signed_entry_debit_for(legs, s.entry_price),
                                    entry_date, s.expiry_date, legs);
                            }
                        }
                        positionManager_->record_trade(
                            s.underlying, "OPEN", "option",
                            static_cast<double>(qty), s.entry_price, s.rsi, 0.0, 0.0,
                            s.strategy + (single_leg ? "" : " (multi-leg)"));
                    });

                // ── Phase 1 pre-order gate (items 3 & 4) ──────────────────────
                // Runs BEFORE the order fires: position-exists check, then the 60s
                // duplicate blocker, then writes the 'pending' ledger row. Both
                // options threads share orderLedger_ + positionManager_.
                using OG = nox::options_signal::OptionsSignalGenerator::OrderGate;
                generator.set_pre_order_hook(
                    [this](const nox::options_signal::OptionsSignal& s, int qty,
                           const std::string& client_oid, const std::string& signature) -> OG {
                        // Ledger down → fail CLOSED, not open. A missing OrderLedger
                        // disables kill switch + risk cap + dedup + position-exists all
                        // at once; letting orders through here defeats every one of them.
                        if (!orderLedger_) return OG::BlockedError;
                        const bool single_leg = (s.strategy == "LONG_CALL" || s.strategy == "LONG_PUT" ||
                                                 s.strategy == "CSP"       || s.strategy == "CC");

                        // Global kill switch — checked first, before the risk cap.
                        // Blocks NEW entries only; closePositionImpl/closeSpreadPosition
                        // never go through this hook, so closing an existing position
                        // during a halt is still possible.
                        if (killSwitch_ && killSwitch_->isPaused()) {
                            auto ks = killSwitch_->get();
                            orderLedger_->logSignalEvent(s.underlying, s.strategy, signature, -1.0,
                                "suppressed_kill_switch", ks.reason);
                            return OG::BlockedKillSwitch;
                        }

                        // Phase 4, item 2: portfolio circuit breaker. Reflects the
                        // breach found at the end of the most recently completed
                        // monitor cycle (up to 5 min stale) — blocks NEW entries only;
                        // it never touches an already-open position (that's handled
                        // separately inside monitor_positions(), which force-closes
                        // only the specific position responsible for the breach).
                        if (positionManager_) {
                            auto breach = positionManager_->get_last_risk_breach();
                            if (breach.breached) {
                                orderLedger_->logSignalEvent(s.underlying, s.strategy, signature, -1.0,
                                    "suppressed_risk_cap", breach.reason);
                                return OG::BlockedRiskCap;
                            }
                        }

                        // Position-exists: ask the broker first — it's the only source
                        // that reflects a manual close/open and the only one that knows
                        // about multi-leg spreads at all (they never populate
                        // open_positions; single-leg only). Fall back to sqlite only if
                        // the broker call fails, rather than failing open — failing open
                        // here is exactly how the AAPL double-buy incident happened
                        // (a signal that keeps re-firing every scan with nothing to
                        // stop it once the 60s window has passed).
                        nox::options_router::OptionsOrderRouter router(alpacaBaseUrl, apiKey, apiSec);
                        bool broker_reachable = false;
                        bool broker_has_position = router.hasOpenOptionPosition(s.underlying, broker_reachable);

                        if (broker_reachable && broker_has_position) {
                            orderLedger_->logSignalEvent(s.underlying, s.strategy, signature, -1.0,
                                "gate_blocked_position_exists",
                                "broker already holds an open option position on this underlying");
                            return OG::BlockedPositionExists;
                        }
                        if (!broker_reachable) {
                            if (single_leg && positionManager_) {
                                std::string opt_type = (s.option_type == nox::options::OptionType::Call)
                                                           ? "call" : "put";
                                if (positionManager_->has_open_position(s.underlying, opt_type,
                                                                        s.strike, s.expiry_date)) {
                                    orderLedger_->logSignalEvent(s.underlying, s.strategy, signature, -1.0,
                                        "gate_blocked_position_exists",
                                        "position already open for this contract (sqlite fallback, broker unreachable)");
                                    return OG::BlockedPositionExists;
                                }
                            } else if (!single_leg &&
                                       orderLedger_->hasOpenMultiLegPosition(s.underlying, get_today_date_string())) {
                                orderLedger_->logSignalEvent(s.underlying, s.strategy, signature, -1.0,
                                    "gate_blocked_position_exists",
                                    "unexpired filled spread on file for this ticker (sqlite fallback, broker unreachable)");
                                return OG::BlockedPositionExists;
                            }
                        }

                        // 60s duplicate blocker (keyed on signature; catches an
                        // immediate retry before the position check above would see it,
                        // e.g. a network retry before the first fill posts to the broker).
                        if (orderLedger_->hasRecentActive(signature, 60)) {
                            orderLedger_->logSignalEvent(s.underlying, s.strategy, signature, -1.0,
                                "gate_blocked_duplicate",
                                "duplicate within 60s (network retry, not a new signal)");
                            return OG::BlockedDuplicate;
                        }

                        // Write the pending row BEFORE the HTTP call fires.
                        const bool is_short = (s.strategy == "CSP" || s.strategy == "CC");
                        nox::execution::OrderLedger::Order o;
                        o.client_oid      = client_oid;
                        o.ticker          = s.underlying;
                        o.strategy        = s.strategy;
                        o.signature       = signature;
                        o.side            = is_short ? "sell" : "buy";
                        o.option_type     = single_leg
                            ? ((s.option_type == nox::options::OptionType::Call) ? "call" : "put")
                            : "";
                        o.profile_type    = is_short ? "short_premium" : "long";
                        o.expiration_date = s.expiry_date;
                        o.status          = "pending";
                        o.strike          = s.strike;
                        o.strike2         = s.strike2;
                        o.qty             = static_cast<double>(qty);
                        o.entry_price     = s.entry_price;
                        orderLedger_->insertPending(o);
                        orderLedger_->logSignalEvent(s.underlying, s.strategy, signature, -1.0,
                            "submitted", "order POST about to fire (client_oid=" + client_oid + ")");
                        return OG::Allow;
                    });

                // Phase 2, item A: persist the gate/cap suppression trail run_scan()
                // logs before a candidate ever reaches the pre-order hook above —
                // together they let a query by signature answer "did this signal
                // regenerate because conditions still hold, or stay silent for a
                // documented reason?" (CLAUDE.md's signal-regeneration audit).
                generator.set_signal_event_hook(
                    [this](const std::string& ticker, const std::string& strategy,
                           const std::string& signature, double quality_score,
                           const std::string& outcome, const std::string& reason) {
                        if (orderLedger_)
                            orderLedger_->logSignalEvent(ticker, strategy, signature,
                                                         quality_score, outcome, reason);
                    });

                // Engine-wide prediction-quality logging: WS1/Skeptic (WS2/WS3/WS8)
                // compute a direction+confidence per ticker but had nowhere to
                // persist it before now — heartbeat/prediction_outcome_resolver.py
                // reads predictions_log and rolls it into weekly/monthly per-source
                // quality scores. Additive only; never affects sizing/gating.
                generator.set_prediction_log_hook(
                    [this](const std::string& source_type, const std::string& ticker,
                           const std::string& direction, double confidence,
                           const std::string& detail) {
                        if (orderLedger_)
                            orderLedger_->logPrediction(source_type, 0, ticker, direction,
                                                        confidence, detail);
                    });

                // Full-detail signal store: every generated candidate (submitted,
                // gate-suppressed, earnings/DTE/liquidity-skipped) with its full
                // contract/regime context — not just the suppressions above — so
                // past signals are queryable for analysis (execution/OrderLedger.hpp's
                // options_signals table).
                generator.set_generated_signal_hook(
                    [this](const nox::options_signal::OptionsSignalGenerator::GeneratedSignalInfo& info) {
                        if (!orderLedger_) return;
                        nox::execution::OrderLedger::GeneratedSignal gs;
                        gs.ticker              = info.ticker;
                        gs.strategy            = info.strategy;
                        gs.signature           = info.signature;
                        gs.direction           = info.direction;
                        gs.strike              = info.strike;
                        gs.strike2             = info.strike2;
                        gs.strike3             = info.strike3;
                        gs.strike4             = info.strike4;
                        gs.expiration_date     = info.expiration_date;
                        gs.dte                 = info.dte;
                        gs.macro_override_used = info.macro_override_used;
                        gs.iv_rank             = info.iv_rank;
                        gs.hrv30               = info.hrv30;
                        gs.quality_score       = info.quality_score;
                        gs.regime              = info.regime;
                        gs.vix_term_label      = info.vix_term_label;
                        gs.earnings_checked    = info.earnings_checked;
                        gs.outcome             = info.outcome;
                        gs.reason              = info.reason;
                        orderLedger_->logGeneratedSignal(gs);
                    });

                // Post-earnings drift research (passive — never gates or sizes a
                // trade): when a ticker is skipped for a confirmed earnings date,
                // record the pre-earnings price/technicals; separately, resolve
                // T+1/T+5 realized move once enough calendar time has passed.
                // Read the accumulated earnings_drift_observations table after a
                // few months to see if there's a real pattern before ever acting
                // on it live.
                generator.set_earnings_drift_hook(
                    [this](const nox::options_signal::OptionsSignalGenerator::EarningsDriftInfo& info) {
                        if (!orderLedger_) return;
                        nox::execution::OrderLedger::EarningsDriftObservation o;
                        o.ticker        = info.ticker;
                        o.earnings_date = info.earnings_date;
                        o.pre_price     = info.price;
                        o.pre_rsi       = info.rsi;
                        o.pre_sma20     = info.sma20;
                        o.pre_sma50     = info.sma50;
                        o.pre_atr       = info.atr;
                        o.direction     = info.direction;
                        orderLedger_->recordEarningsDriftObservation(o);
                    });
                generator.set_earnings_drift_pending_query(
                    [this]() -> std::vector<nox::options_signal::OptionsSignalGenerator::EarningsDriftPendingItem> {
                        std::vector<nox::options_signal::OptionsSignalGenerator::EarningsDriftPendingItem> out;
                        if (!orderLedger_) return out;
                        for (const auto& row : orderLedger_->getPendingEarningsDrift()) {
                            out.push_back({row.id, row.ticker, row.pre_price, row.day_offset});
                        }
                        return out;
                    });
                generator.set_earnings_drift_resolve_hook(
                    [this](long id, int day_offset, double price, double move_pct) {
                        if (!orderLedger_) return;
                        if (day_offset == 1)      orderLedger_->resolveEarningsDriftT1(id, price, move_pct);
                        else if (day_offset == 5) orderLedger_->resolveEarningsDriftT5(id, price, move_pct);
                    });

                // ── Phase 1 post-order hook (items 1 & 2) ─────────────────────
                // Records the router's disposition. 'unknown' (timeout/parse) means
                // reconciliation — not a guess — resolves the true outcome.
                generator.set_post_order_hook(
                    [this](const std::string& client_oid, const std::string& ledger_status,
                           const std::string& broker_order_id) {
                        if (orderLedger_)
                            orderLedger_->setStatus(client_oid, ledger_status, broker_order_id);
                    });

                // Stagger this profile's first scan so the three option threads
                // (bot/personal/breakout) don't all fire their per-ticker Alpaca
                // calls in the same instant every interval. They share a boot-time
                // reference and an identical scan_interval_minutes, so without this
                // they're structurally synchronized: 3 threads x ~2.5 req/sec each
                // (400ms/ticker) overlapping already exceeds Alpaca's documented
                // ~200 req/min ceiling, with no retry/backoff on this side to absorb
                // a resulting 429. Interruptible so shutdown doesn't hang.
                if (stagger_seconds > 0) {
                    std::unique_lock<std::mutex> lk(stop_mutex_);
                    stop_cv_.wait_for(lk, std::chrono::seconds(stagger_seconds),
                                       [this] { return !running_.load(); });
                }

                while (running_.load()) {
                    if (!nox::is_trading_day()) {
                        Logger::log("INFO", "[OPTIONS_SIGNAL][" + profile.name +
                                    "] Non-trading day — skipping scan.");
                        // Sleep until next interval (interruptible on shutdown)
                        std::unique_lock<std::mutex> lk(stop_mutex_);
                        stop_cv_.wait_for(lk,
                            std::chrono::minutes(profile.scan_interval_minutes),
                            [this] { return !running_.load(); });
                        continue;
                    }

                    try {
                        // Phase 1: reconcile pending/unknown orders against broker
                        // truth BEFORE evaluating new signals (throttled + idempotent,
                        // so the two options threads don't double-poll).
                        reconcile_options_orders();
                        // Global kill switch, automatic half: same throttle pattern,
                        // same two-thread-shared call site.
                        check_daily_loss_limit();

                        double equity = fetch_account_equity();
                        if (equity > 0.0) {
                            generator.run_scan(equity);
                        } else {
                            Logger::log("WARN", "[OPTIONS_SIGNAL][" + profile.name +
                                        "] Skipping scan — equity unavailable.");
                        }
                    } catch (const std::exception& e) {
                        Logger::log("WARN", "[OPTIONS_SIGNAL][" + profile.name +
                                    "] Scan exception: " + std::string(e.what()));
                    }
                    // Interruptible sleep: wakes immediately when shutdown() fires
                    // instead of blocking for the full scan interval on SIGTERM.
                    std::unique_lock<std::mutex> lk(stop_mutex_);
                    stop_cv_.wait_for(lk,
                        std::chrono::minutes(profile.scan_interval_minutes),
                        [this] { return !running_.load(); });
                }
                Logger::log("INFO", "[OPTIONS_SIGNAL][" + profile.name + "] thread exiting.");
            });
        };

        // Spread the three profiles' scans across the interval instead of all
        // firing at boot (see the stagger comment inside launchOptionsThread).
        int optionsStaggerSeconds = 300;
        if (const char* v = std::getenv("OPTIONS_THREAD_STAGGER_SECONDS")) {
            try { optionsStaggerSeconds = std::max(0, std::stoi(std::string(v))); } catch (...) {}
        }
        launchOptionsThread(optionsBotProfile_, 0);
        launchOptionsThread(optionsPersonalProfile_, optionsStaggerSeconds);
        launchOptionsThread(optionsBreakoutProfile_, 2 * optionsStaggerSeconds);

        // ── Equity signal scanner thread ──────────────────────────────────────
        if (equityScanEnabled_) {
            option_threads_.emplace_back([this]() {
                std::string tg_token = std::getenv("TELEGRAM_BOT_TOKEN") ? std::getenv("TELEGRAM_BOT_TOKEN") : "";
                std::string tg_chat  = std::getenv("TELEGRAM_CHAT_ID")   ? std::getenv("TELEGRAM_CHAT_ID")   : "";

                nox::equity_signal::EquitySignalGenerator scanner(
                    secret, tg_token, tg_chat,
                    equityWatchlist_, equityMaxSignals_, equityBypassHours_);

                // Brief startup delay so the HTTP server is listening before
                // the first scan tries to POST back to localhost:8080.
                std::unique_lock<std::mutex> lk(stop_mutex_);
                stop_cv_.wait_for(lk, std::chrono::seconds(15),
                    [this] { return !running_.load(); });
                if (!running_.load()) return;
                lk.unlock();

                while (running_.load()) {
                    if (!nox::is_trading_day()) {
                        Logger::log("INFO", "[EQUITY_SCAN] Non-trading day — skipping scan.");
                        // Sleep until next interval (interruptible on shutdown)
                        std::unique_lock<std::mutex> slk(stop_mutex_);
                        stop_cv_.wait_for(slk,
                            std::chrono::minutes(equityScanIntervalMinutes_),
                            [this] { return !running_.load(); });
                        continue;
                    }

                    try {
                        scanner.run_scan();
                    } catch (const std::exception& e) {
                        Logger::log("WARN", "[EQUITY_SCAN] Scan exception: " + std::string(e.what()));
                    }
                    std::unique_lock<std::mutex> slk(stop_mutex_);
                    stop_cv_.wait_for(slk,
                        std::chrono::minutes(equityScanIntervalMinutes_),
                        [this] { return !running_.load(); });
                }
                Logger::log("INFO", "[EQUITY_SCAN] Thread exiting.");
            });
        }
        // ────────────────────────────────────────────────────────────────────

        // ── Futures signal scanner thread (signals only, no order routing) ────
        if (futuresScanEnabled_ && futuresSignalStore_) {
            option_threads_.emplace_back([this]() {
                std::string tg_token = std::getenv("TELEGRAM_BOT_TOKEN") ? std::getenv("TELEGRAM_BOT_TOKEN") : "";
                std::string tg_chat  = std::getenv("TELEGRAM_CHAT_ID")   ? std::getenv("TELEGRAM_CHAT_ID")   : "";

                auto massiveClient = std::make_shared<nox::execution::MassiveFuturesClient>(massiveApiKey_);
                nox::futures_signal::FuturesSignalGenerator generator(
                    secret, tg_token, tg_chat,
                    futuresWatchlist_, futuresAlertThreshold_,
                    massiveClient, futuresSignalStore_);

                while (running_.load()) {
                    if (!nox::is_trading_day()) {
                        Logger::log("INFO", "[FUTURES_SCAN] Non-trading day — skipping scan.");
                        std::unique_lock<std::mutex> slk(stop_mutex_);
                        stop_cv_.wait_for(slk,
                            std::chrono::minutes(futuresScanIntervalMinutes_),
                            [this] { return !running_.load(); });
                        continue;
                    }

                    try {
                        generator.run_scan();
                    } catch (const std::exception& e) {
                        Logger::log("WARN", "[FUTURES_SCAN] Scan exception: " + std::string(e.what()));
                    }
                    std::unique_lock<std::mutex> slk(stop_mutex_);
                    stop_cv_.wait_for(slk,
                        std::chrono::minutes(futuresScanIntervalMinutes_),
                        [this] { return !running_.load(); });
                }
                Logger::log("INFO", "[FUTURES_SCAN] Thread exiting.");
            });
        }
        // ────────────────────────────────────────────────────────────────────

        // ── Trailing Stop Monitor Thread ──────────────────────────────────────
        // Detects when equity positions close (likely due to trailing stops hitting)
        // and records them as SELL signals automatically.
        option_threads_.emplace_back([this]() {
            monitor_trailing_stops();
        });
        // ────────────────────────────────────────────────────────────────────

        // Install SIGTERM/SIGINT handlers now that s_instance_ is set and
        // svr_ptr_ points at the live server.
        s_instance_ = this;
        svr_ptr_    = &svr;
        std::signal(SIGTERM, NoxEngine::handle_signal);
        std::signal(SIGINT,  NoxEngine::handle_signal);

        Logger::log("INFO", "Nox Execution Engine listening on 0.0.0.0:8080...");
        svr.listen("0.0.0.0", 8080);

        // listen() returned — either SIGTERM fired or the server stopped internally.
        // Ensure running_ is false and wake any threads still in wait_for().
        shutdown();
        for (auto& t : option_threads_)
            if (t.joinable()) t.join();
        Logger::log("INFO", "Nox Execution Engine shut down cleanly.");
    } // This closes void run()
}; // This closes class NoxEngine

NoxEngine* NoxEngine::s_instance_ = nullptr;

int main() {
    NoxEngine engine;
    engine.run();
    return 0;
}
