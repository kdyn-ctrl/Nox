#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "../shared/RegimeStateMachine.hpp"
#include "../shared/TelegramNotifier.hpp"
#include "OptionEngine.hpp"
#include "EquitySignalGenerator.hpp"
#include "OptionsSignalGenerator.hpp"
#include "PositionManager.hpp"
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
    RegimeStateMachine regimeMachine;
    std::string last_analyst_report_time;

    // WS5 — pre-execution microstructure gate (rolling per-symbol spread baseline)
    nox::liquidity::LiquidityGate liquidity_gate_;

    // Position Manager (for options)
    std::unique_ptr<PositionManager> positionManager_;

    // Options signal scanner profiles (configured from env vars in the constructor)
    nox::options_signal::RiskProfile optionsBotProfile_;
    nox::options_signal::RiskProfile optionsPersonalProfile_;

    // Equity signal scanner config (independent of Skeptic)
    std::vector<std::string> equityWatchlist_;
    int    equityScanIntervalMinutes_ = 30;
    int    equityMaxSignals_          = 2;
    bool   equityScanEnabled_         = true;
    bool   equityBypassHours_         = false;

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

    // Closes an open equity position at Alpaca: cancels resting orders (including
    // the trailing stop), liquidates, records the exit to the trade ledger with the
    // supplied reason, notifies, and clears local tracking (T+1 + equity maps).
    // Shared by webhook SELL signals and the rule-based exit monitor so both close
    // through one tested path. `reason` is surfaced in the ledger and Telegram.
    void close_equity_position_alpaca(const std::string& ticker,
                                      const std::string& reason,
                                      double rsi) {
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
            {
                auto snap = alpaca_cli.Get(("/v2/positions/" + ticker).c_str(), headers);
                if (snap && snap->status == 200) {
                    try {
                        json p = json::parse(snap->body);
                        closed_qty   = std::stod(p.value("qty", "0"));
                        realized_pnl = std::stod(p.value("unrealized_pl", "0"));
                        exit_price   = std::stod(p.value("current_price", "0"));
                    } catch (...) { /* best-effort */ }
                }
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

            // --- 2. Liquidate the entire position ---
            std::string path = "/v2/positions/" + ticker;
            std::cout << "[EXECUTION] Sending liquidate position request to Alpaca for " << ticker
                      << " (reason: " << reason << ")..." << std::endl;
            auto res = alpaca_cli.Delete(path.c_str(), headers);

            if (res && res->status == 200) {
                json response_data = json::parse(res->body);
                std::cout << " [POSITION CLOSED] Alpaca response: " << response_data.dump(2) << std::endl;
                // Ledger: record the equity exit with best-effort realized P&L.
                if (positionManager_) {
                    positionManager_->record_trade(
                        ticker, "SELL", "equity",
                        closed_qty, exit_price, rsi, 0.0, realized_pnl,
                        reason + " order_id=" + response_data.value("id", "N/A"));
                }
                std::ostringstream pnl_ss;
                pnl_ss << std::showpos << std::fixed << std::setprecision(2) << realized_pnl;
                TelegramNotifier::sendMessage(
                    "⚪ *POSITION CLOSED*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + ticker + "\n"
                    "• *Reason:* " + reason + "\n"
                    "• *Est. P&L:* $" + pnl_ss.str() + "\n"
                    "• *Alpaca Order ID:* `" + response_data.value("id", "N/A") + "`"
                );
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
            } else {
                std::string status_code = res ? std::to_string(res->status) : "TIMEOUT";
                std::string details     = res ? res->body : "No response received.";

                // A 404 here means we didn't have a position to close, which isn't a critical failure.
                if (res && res->status == 404) {
                    std::cerr << "ℹ️ [CLOSE IGNORED] No open position for " << ticker
                              << " to close. Details: " << details << std::endl;
                    // Position already gone — clear stale tracking so we don't retry it.
                    {
                        std::lock_guard<std::mutex> lock(equity_positions_mutex_);
                        if (equity_positions_.erase(ticker) > 0) persist_equity_positions_locked();
                    }
                    TelegramNotifier::sendMessage(
                        "ℹ️ *CLOSE IGNORED*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + ticker + "\n"
                        "• *Details:* No open position found."
                    );
                } else {
                    std::cerr << "⚠️ [CLOSE REJECTED] Failed to close " << ticker
                              << ". Status: " << status_code << ", Details: " << details << std::endl;
                    TelegramNotifier::sendMessage(
                        "🚨 *CLOSE REJECTED*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + ticker + "\n"
                        "• *Status Code:* " + status_code + "\n"
                        "• *Details:* `" + details + "`"
                    );
                }
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

                json order_payload = {
                    {"symbol", sig.ticker},
                    {"qty", qty},
                    {"side", "buy"},
                    {"type", "market"},
                    {"time_in_force", "day"}
                };

                std::cout << "[EXECUTION] Routing BUY order to Alpaca: " << qty << " shares of " << sig.ticker << "..." << std::endl;
                auto res = alpaca_cli.Post("/v2/orders", headers, order_payload.dump(), "application/json");

                if (res && res->status == 200) {
                    json response_data = json::parse(res->body);
                    std::string order_id = response_data.value("id", "UNKNOWN");
                    std::cout << " [BUY ORDER EXECUTED] Alpaca Order ID: " << order_id << std::endl;

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
                            {"trail_price", trail_offset_str}
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
            optionsBotProfile_.qty_contracts =
                envInt("OPTIONS_BOT_QTY_CONTRACTS", 1);
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
            optionsPersonalProfile_.qty_contracts =
                envInt("OPTIONS_PERSONAL_QTY_CONTRACTS", 1);
            optionsPersonalProfile_.free_capital_amount =
                envDbl("OPTIONS_PERSONAL_FREE_CAPITAL_AMOUNT", 0.0);
            optionsPersonalProfile_.max_signals_per_scan =
                envInt("OPTIONS_PERSONAL_MAX_SIGNALS", 2);

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

            Logger::log("INFO", std::string("[EQUITY_EXIT] Rule-based exits ") +
                (equityRuleExitsEnabled_ ? "ENABLED" : "DISABLED") +
                " | TP=" + std::to_string(equityExitTakeProfitPct_ * 100.0) + "%" +
                " | SL=" + std::to_string(equityExitStopLossPct_ * 100.0) + "%" +
                " | RSI≥" + std::to_string(equityExitRsiCeiling_) +
                " | SMA20-break=" + (equityExitSmaBreak_ ? "on" : "off") +
                " | MaxHold=" + std::to_string(equityExitMaxHoldDays_) + "d");

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
        }

        // Initialize and start the Position Manager
        try {
            auto order_router = std::make_shared<nox::options_router::OptionsOrderRouter>(
                alpacaBaseUrl, apiKey, apiSec
            );
            // MEMORY_BANK_PATH must point to the volume-mounted data directory so the
            // options position DB survives container restarts. Default: /app/data.
            const char* mb_env = std::getenv("MEMORY_BANK_PATH");
            std::string memory_bank_path = (mb_env && std::string(mb_env) != "")
                ? std::string(mb_env)
                : "/app/data/memory_bank.db";
            positionManager_ = std::make_unique<PositionManager>(memory_bank_path, *order_router);
            positionManager_->start_monitoring();
            Logger::log("INFO", "[POS_MANAGER] Position Manager initialized and monitoring thread started.");
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
        auto launchOptionsThread = [this](nox::options_signal::RiskProfile profile) {
            option_threads_.emplace_back([this, profile]() {
                std::string tg_token = std::getenv("TELEGRAM_BOT_TOKEN") ? std::getenv("TELEGRAM_BOT_TOKEN") : "";
                std::string tg_chat  = std::getenv("TELEGRAM_CHAT_ID")   ? std::getenv("TELEGRAM_CHAT_ID")   : "";

                nox::options_signal::OptionsSignalGenerator generator(
                    alpacaBaseUrl, apiKey, apiSec, tg_token, tg_chat, profile);

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
                        // Only single-leg strategies map to the monitor's exit rules.
                        // Multi-leg (spreads/straddles) are logged but not auto-exited.
                        if (single_leg) {
                            positionManager_->add_position(
                                s.underlying, opt_type, s.strike, qty, s.entry_price,
                                entry_date, profile_type, s.expiry_date);
                        }
                        positionManager_->record_trade(
                            s.underlying, "OPEN", "option",
                            static_cast<double>(qty), s.entry_price, s.rsi, 0.0, 0.0,
                            s.strategy + (single_leg ? "" : " (multi-leg: exit not auto-managed)"));
                    });

                while (running_.load()) {
                    try {
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

        launchOptionsThread(optionsBotProfile_);
        launchOptionsThread(optionsPersonalProfile_);

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
