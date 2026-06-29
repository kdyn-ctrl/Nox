#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "../shared/RegimeStateMachine.hpp"
#include "OptionEngine.hpp"
#include "OptionsSignalGenerator.hpp"
#include "PositionManager.hpp"
#include <iostream>
#include <string>
#include <cmath>
#include <thread>
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

// --- 1. TELEGRAM NOTIFIER ---
class TelegramNotifier {
public:
static void sendMessage(std::string message) {
        std::string token = std::getenv("TELEGRAM_BOT_TOKEN") ? std::getenv("TELEGRAM_BOT_TOKEN") : "";
        std::string chatId = std::getenv("TELEGRAM_CHAT_ID") ? std::getenv("TELEGRAM_CHAT_ID") : "";
        if (token.empty() || chatId.empty()) {
            std::cerr << "❌ [TELEGRAM] Missing Bot Token or Chat ID env variables." << std::endl;
            return;
        }

        httplib::Client cli("https://api.telegram.org");
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(10));
        std::string path = "/bot" + token + "/sendMessage";

        // Telegram's max limit is 4096; 4000 gives us a safe buffer for formatting tags
        const size_t MAX_LIMIT = 4000; 
        size_t offset = 0;

        // Loop and slice the report into multiple sequential messages if it's too long
        while (offset < message.length()) {
            std::string chunk = message.substr(offset, MAX_LIMIT);
            offset += MAX_LIMIT;

            json body = {
                {"chat_id", chatId},
                {"text", chunk},
                {"parse_mode", "Markdown"}
            };

            auto res = cli.Post(path.c_str(), body.dump(), "application/json");
            if (!res || res->status != 200) {
                std::cerr << "❌ [TELEGRAM ERROR] Failed to deliver report chunk. Status: " 
                          << (res ? std::to_string(res->status) : "No Response") << std::endl;
            }
        }
    }
};

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
    int         cnBoardLotSize;   // CN-RULE-001: configurable via CN_BOARD_LOT_SIZE (default 100)
    std::string cnPositionsPath;  // path for T+1 persistence file
    RegimeStateMachine regimeMachine;
    std::string last_analyst_report_time;

    // WS5 — pre-execution microstructure gate (rolling per-symbol spread baseline)
    nox::liquidity::LiquidityGate liquidity_gate_;

    // Position Manager (for options)
    std::unique_ptr<PositionManager> positionManager_;

    // Options signal scanner profiles (configured from env vars in the constructor)
    nox::options_signal::RiskProfile optionsBotProfile_;
    nox::options_signal::RiskProfile optionsPersonalProfile_;

    // ── Inbound signal log (last 50 authenticated signals, newest at back) ─────
    struct SignalLogEntry {
        std::string received_at;
        std::string ticker;
        std::string action;
        double price = 0.0;
        double rsi   = 0.0;
        double vix   = 0.0;
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
        signal_log_.push_back({ts.str(), s.ticker, s.action, s.price, s.rsi, s.vix});
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

    // Returns today's date as "YYYY-MM-DD" in the local system timezone.
    static std::string get_today_date_string() {
        auto now    = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* tm_ptr = std::localtime(&time_t);
        std::ostringstream oss;
        oss << std::put_time(tm_ptr, "%Y-%m-%d");
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

    void process(TradeSignal sig) {
        // Skip processing if the action is HOLD or a REPORT audit payload
        if (sig.action == "HOLD" || sig.action == "REPORT") {
            std::cout << "[EXECUTION] Strategy indicates HOLD/REPORT. No orders routed." << std::endl;
            return;
        }

        // --- SELL ROUTING: Close open position (enhanced) ---
        if (sig.action == "SELL") {
            // CN-RULE-002: T+1 Settlement Gate.
            // Chinese A-share rules prohibit same-day round trips: a position
            // bought on day T cannot be sold until T+1 or later.
            // If the sell signal arrives on the same calendar date as the
            // recorded entry, discard the signal entirely and log CRITICAL.
            {
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
                            ". Same-day round-trips are prohibited on Chinese A-shares.");
                        TelegramNotifier::sendMessage(
                            "🚫 *CN T+1 GATE BLOCKED*\n"
                            "────────────────────────\n"
                            "• *Ticker:* " + sig.ticker + "\n"
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
            try {
                httplib::Client alpaca_cli(alpacaBaseUrl);
                // RULE-008: Strict timeout handling
                alpaca_cli.set_connection_timeout(std::chrono::seconds(5));
                alpaca_cli.set_read_timeout(std::chrono::seconds(10));

                httplib::Headers headers = {
                    {"APCA-API-KEY-ID",     apiKey},
                    {"APCA-API-SECRET-KEY", apiSec}
                };

                // --- 1. Cancel all open orders for the symbol to avoid interference ---
                std::string cancel_path = "/v2/orders?symbol=" + sig.ticker;
                std::cout << "[EXECUTION] Canceling any existing orders for " << sig.ticker << " before closing position..." << std::endl;
                auto cancel_res = alpaca_cli.Delete(cancel_path.c_str(), headers);
                if (cancel_res && (cancel_res->status == 200 || cancel_res->status == 207)) {
                    // 207 Multi-Status can be returned. We consider it a success and proceed.
                    std::cout << "[EXECUTION] Existing orders for " << sig.ticker << " canceled (or none existed)." << std::endl;
                } else {
                    // We log a warning but proceed anyway, as closing the position is the primary goal.
                    std::string cancel_status = cancel_res ? std::to_string(cancel_res->status) : "TIMEOUT";
                    std::cerr << "⚠️ [EXECUTION] Could not verify order cancellation for " << sig.ticker
                              << ". Status: " << cancel_status << ". Proceeding to close position." << std::endl;
                }

                // --- 2. Liquidate the entire position ---
                std::string path = "/v2/positions/" + sig.ticker;
                std::cout << "[EXECUTION] Sending liquidate position request to Alpaca for " << sig.ticker << "..." << std::endl;
                auto res = alpaca_cli.Delete(path.c_str(), headers);

                if (res && res->status == 200) {
                    json response_data = json::parse(res->body);
                    std::cout << " [POSITION CLOSED] Alpaca response: " << response_data.dump(2) << std::endl;
                    TelegramNotifier::sendMessage(
                        "⚪ *POSITION CLOSED*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + sig.ticker + "\n"
                        "• *Trigger:* Webhook SELL Signal\n"
                        "• *Alpaca Order ID:* `" + response_data.value("id", "N/A") + "`"
                    );
                    // CN-RULE-002: Position is closed — remove from T+1 tracking map.
                    {
                        std::lock_guard<std::mutex> lock(china_positions_mutex_);
                        china_positions_.erase(sig.ticker);
                        persist_china_positions_locked();
                    }
                } else {
                    std::string status_code = res ? std::to_string(res->status) : "TIMEOUT";
                    std::string details     = res ? res->body : "No response received.";
                    
                    // A 404 here means we didn't have a position to close, which isn't a critical failure.
                    if (res && res->status == 404) {
                         std::cerr << "ℹ️ [CLOSE IGNORED] No open position for " << sig.ticker
                                  << " to close. Details: " << details << std::endl;
                         TelegramNotifier::sendMessage(
                                "ℹ️ *CLOSE IGNORED*\n"
                                "────────────────────────\n"
                                "• *Ticker:* " + sig.ticker + "\n"
                                "• *Details:* No open position found."
                            );
                    } else {
                        std::cerr << "⚠️ [CLOSE REJECTED] Failed to close " << sig.ticker
                                  << ". Status: " << status_code << ", Details: " << details << std::endl;
                        TelegramNotifier::sendMessage(
                            "🚨 *CLOSE REJECTED*\n"
                            "────────────────────────\n"
                            "• *Ticker:* " + sig.ticker + "\n"
                            "• *Status Code:* " + status_code + "\n"
                            "• *Details:* `" + details + "`"
                        );
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "💥 Runtime Exception closing position for " << sig.ticker << ": " << e.what() << std::endl;
            }
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
            TelegramNotifier::sendMessage("📊 *Regime Check:* " + regime.log_message);
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
                    "🟢 *BUY ORDER → IBKR*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + sig.ticker + "\n"
                    "• *Qty:* " + std::to_string(qty) + " shares\n"
                    "• *IBKR OrderId:* `" + std::to_string(oid) + "`"
                );

                // Record T+1 entry date for CN-RULE-002 enforcement.
                {
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

                    // CN-RULE-002: Record entry date for T+1 enforcement.
                    // Use the signal's trade_date if provided (backtester path);
                    // fall back to today's system date for live execution.
                    {
                        std::string entry_date = sig.trade_date.empty()
                            ? get_today_date_string()
                            : sig.trade_date;
                        std::lock_guard<std::mutex> lock(china_positions_mutex_);
                        china_positions_[sig.ticker] = ChinaPositionRecord{entry_date};
                        persist_china_positions_locked();
                        Logger::log("INFO",
                            "[CN-RULE-002] Recorded T+1 entry for " + sig.ticker +
                            " on " + entry_date + ". Sell gate active until T+1.");
                    }

                    TelegramNotifier::sendMessage(
                        "🟢 *BUY ORDER EXECUTED*\n"
                        "────────────────────────\n"
                        "• *Ticker:* " + sig.ticker + "\n"
                        "• *Quantity:* " + std::to_string(qty) + " Shares (Dynamic Kelly)\n"
                        "• *Order ID:* `" + order_id + "`"
                    );

                    // --- Place Trailing Stop Order ---
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
                            TelegramNotifier::sendMessage(
                                "🛡️ *TRAILING STOP SET*\n"
                                "────────────────────────\n"
                                "• *Ticker:* " + sig.ticker + "\n"
                                "• *Trail Offset:* $" + trail_offset_str
                            );
                        } else {
                            std::string sl_status = sl_res ? std::to_string(sl_res->status) : "TIMEOUT";
                            std::string sl_details = sl_res ? sl_res->body : "No response.";
                            std::cerr << "⚠️ [STOP-LOSS FAILED] Status: " << sl_status 
                                      << ", Details: " << sl_details << std::endl;
                            TelegramNotifier::sendMessage(
                                "🚨 *STOP-LOSS FAILED*\n"
                                "────────────────────────\n"
                                "• *Ticker:* " + sig.ticker + "\n"
                                "• *Status Code:* " + sl_status + "\n"
                                "• *Details:* `" + sl_details + "`"
                            );
                        }
                    } else {
                        Logger::log("WARN", "[EXECUTION] ATR or multiplier invalid, skipping trailing stop.");
                    }
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

        // CN-RULE-001: Board-lot size (default 100 for all A-share boards).
        // Override with CN_BOARD_LOT_SIZE if a non-standard lot size is needed.
        cnBoardLotSize = 100;
        const char* lot_env = std::getenv("CN_BOARD_LOT_SIZE");
        if (lot_env && std::string(lot_env) != "") {
            try {
                int parsed = std::stoi(std::string(lot_env));
                if (parsed > 0) {
                    cnBoardLotSize = parsed;
                } else {
                    std::cerr << "[WARN] [EXECUTION] CN_BOARD_LOT_SIZE must be positive. "
                              << "Using default of 100." << std::endl;
                }
            } catch (...) {
                std::cerr << "[WARN] [EXECUTION] CN_BOARD_LOT_SIZE is not a valid integer. "
                          << "Using default of 100." << std::endl;
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
            std::cerr << "[FATAL] [POS_MANAGER] Failed to initialize Position Manager: "
                      << e.what() << ". Refusing to start." << std::endl;
            std::exit(1);
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
                    {"vix",         e.vix}
                });
            }
            res.set_content(arr.dump(), "application/json");
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

                    TelegramNotifier::sendMessage("🚀 Signal Parsed: " + signal.action + " " + signal.ticker);
                    
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
        auto launchOptionsThread = [this](nox::options_signal::RiskProfile profile) {
            std::thread t([this, profile]() {
                std::string tg_token = std::getenv("TELEGRAM_BOT_TOKEN") ? std::getenv("TELEGRAM_BOT_TOKEN") : "";
                std::string tg_chat  = std::getenv("TELEGRAM_CHAT_ID")   ? std::getenv("TELEGRAM_CHAT_ID")   : "";

                nox::options_signal::OptionsSignalGenerator generator(
                    alpacaBaseUrl, apiKey, apiSec, tg_token, tg_chat, profile);

                while (true) {
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
                    std::this_thread::sleep_for(
                        std::chrono::minutes(profile.scan_interval_minutes));
                }
            });
            t.detach();
        };

        launchOptionsThread(optionsBotProfile_);
        launchOptionsThread(optionsPersonalProfile_);
        // ────────────────────────────────────────────────────────────────────

        Logger::log("INFO", "Nox Execution Engine listening on 0.0.0.0:8080...");
        svr.listen("0.0.0.0", 8080);
    } // This closes void run()
}; // This closes class NoxEngine

int main() {
    NoxEngine engine;
    engine.run();
    return 0;
}
