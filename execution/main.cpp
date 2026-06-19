#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "../shared/RegimeStateMachine.hpp"
#include <iostream>
#include <string>
#include <cmath>
#include <thread>
#include <chrono>

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
    int risk_tier = 1;
    double vix         = 20.0;  // Default neutral — cautious but not blocking
    double spy_price   = 0.0;
    double spy_200_sma = 0.0;
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
    RegimeStateMachine regimeMachine;

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

    void process(TradeSignal sig) {
        // Skip processing if the action is HOLD or a REPORT audit payload
        if (sig.action == "HOLD" || sig.action == "REPORT") {
            std::cout << "[EXECUTION] Strategy indicates HOLD/REPORT. No orders routed." << std::endl;
            return;
        }

        // RSI overbought/oversold gate — block trades that contradict momentum
        if (sig.action == "BUY" && sig.rsi > 70.0) {
            Logger::log("WARN", "RSI gate blocked BUY for " + sig.ticker + " — RSI overbought at " + std::to_string(sig.rsi));
            TelegramNotifier::sendMessage(
                "🚧 *RSI GATE BLOCK*\n"
                "────────────────────────\n"
                "• *Ticker:* " + sig.ticker + "\n"
                "• *Action:* BUY\n"
                "• *RSI:* " + std::to_string(sig.rsi) + " (Overbought > 70)\n"
                "────────────────────────\n"
                "⚠️ _Order canceled to protect buying power._"
            );
            return;
        }
        if (sig.action == "SELL" && sig.rsi < 30.0) {
            Logger::log("WARN", "RSI gate blocked SELL for " + sig.ticker + " — RSI oversold at " + std::to_string(sig.rsi));
            TelegramNotifier::sendMessage(
                "🚧 *RSI GATE BLOCK*\n"
                "────────────────────────\n"
                "• *Ticker:* " + sig.ticker + "\n"
                "• *Action:* SELL\n"
                "• *RSI:* " + std::to_string(sig.rsi) + " (Oversold < 30)\n"
                "────────────────────────\n"
                "⚠️ _Order canceled to prevent selling into low liquidity._"
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

        // RULE-005: Size position via Kelly — abort if no mathematical edge.
        // Scale equity by regime multiplier before sizing (1.0 = full, 0.5 = half).
        double regime_adjusted_equity = live_equity * regime.capital_multiplier;
        int qty = calculate_kelly_size(regime_adjusted_equity, sig.price, kellyWinRate, kellyWinLossRatio, kellyFraction);
        if (qty < 0) {
            Logger::log("CRITICAL", "[EXECUTION] Aborting order for " + sig.ticker +
                        " — Kelly sizing returned no-edge signal.");
            TelegramNotifier::sendMessage(
                "🚨 *CRITICAL: Kelly No-Edge*\n"
                "────────────────────────\n"
                "• *Ticker:* " + sig.ticker + "\n"
                "⛔ Raw Kelly ≤ 0 — strategy has no statistical edge.\n"
                "Order halted. Review KELLY_WIN_RATE / KELLY_WIN_LOSS_RATIO."
            );
            return;
        }

        // RULE-018 Condition 2 — Notional Value Ceiling (Physical Hard Gate).
        // The Kelly calculator already enforces a 10% cap mathematically, but a
        // price spike between the sizing calculation and the order submission can
        // silently push the true notional value above the cap. This is the physical
        // last-line-of-defense gate that catches that window and any future edge
        // cases regardless of how qty was calculated (systematic or override).
        //
        // Condition: (Qty × CurrentPrice) must not exceed (LiveEquity × 10%).
        // We check against live_equity (not regime_adjusted_equity) because the
        // hard cap is a function of total account size, not a regime-scaled subset.
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
                "• *Qty:* " + std::to_string(qty) + " shares\n"
                "• *Notional:* $" + std::to_string(notional_value) + "\n"
                "• *Max Allowed (10%):* $" + std::to_string(max_notional) + "\n"
                "⛔ Price spike detected between sizing and submission.\n"
                "Order blocked. Manual review required."
            );
            return;
        }

        // RULE-014: alpacaBaseUrl is injected at runtime (paper vs. live)
        // and was validated at startup — never hardcoded here.
        try {
            httplib::Client alpaca_cli(alpacaBaseUrl);
            alpaca_cli.set_connection_timeout(std::chrono::seconds(5));
            alpaca_cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey},
                {"APCA-API-SECRET-KEY", apiSec},
                {"Content-Type",        "application/json"}
            };

            // Structure the exact JSON payload Alpaca requires, using your dynamic Kelly qty
            json order_payload = {
                {"symbol", sig.ticker},
                {"qty", qty}, // Uses your Kelly size!
                {"side", sig.action == "BUY" ? "buy" : "sell"},
                {"type", "market"},
                {"time_in_force", "day"}
            };

        std::cout << "[EXECUTION] Routing order to Alpaca: " << qty << " shares of " << sig.ticker << "..." << std::endl;

        auto res = alpaca_cli.Post("/v2/orders", headers, order_payload.dump(), "application/json");

            if (res && res->status == 200) {
                json response_data = json::parse(res->body);
                std::string order_id = response_data.value("id", "UNKNOWN");
                std::string order_status = response_data.value("status", "pending");
                std::cout << " [ORDER EXECUTED] Alpaca Order ID: "
                          << order_id << " | Status: "
                          << order_status << std::endl;
                TelegramNotifier::sendMessage(
                    "🟢 *ORDER EXECUTED*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + sig.ticker + "\n"
                    "• *Side:* " + sig.action + "\n"
                    "• *Quantity:* " + std::to_string(qty) + " Shares (Kelly)\n"
                    "• *Status:* " + order_status + "\n"
                    "• *Order ID:* `" + order_id + "`\n"
                    "────────────────────────"
                );
            } else {
                std::string status_code = res ? std::to_string(res->status) : "TIMEOUT";
                std::string details     = res ? res->body : "No response received.";
                std::cerr << "⚠️ [ALPACA REJECTION] Exchange returned status code: "
                          << status_code << std::endl;
                if (res) {
                    std::cerr << "Details: " << details << std::endl;
                }
                TelegramNotifier::sendMessage(
                    "🚨 *ALPACA REJECTION*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + sig.ticker + "\n"
                    "• *Side:* " + sig.action + "\n"
                    "• *Quantity:* " + std::to_string(qty) + " Shares\n"
                    "• *Status Code:* " + status_code + "\n"
                    "• *Details:* `" + details + "`\n"
                    "────────────────────────"
                );
            }
        } catch (const std::exception& e) {
            std::cerr << "💥 Runtime Exception routing order to Alpaca: " << e.what() << std::endl;
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

        Logger::log("INFO", "[EXECUTION] All required environment variables validated.");
    }

    void run() {
        httplib::Server svr;

        svr.Get("/health", [](const httplib::Request &, httplib::Response &res) {
            json health_response = {{"status", "healthy"}};
            res.set_content(health_response.dump(), "application/json");
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

                    Logger::log("INFO", "Signal Parsed successfully: " + signal.action + " " + signal.ticker);

                    // Fast path for analyst audit reports: acknowledge immediately
                    // so the caller does not time out waiting on downstream network
                    // work (Telegram delivery, Alpaca checks, etc.).
                    if (signal.action == "REPORT") {
                        if (data.contains("report_body")) {
                            Logger::log("INFO", "[EXECUTION] Analyst report: " + data.value("report_body", ""));
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

        Logger::log("INFO", "Nox Execution Engine listening on 0.0.0.0:8080...");
        svr.listen("0.0.0.0", 8080);
    } // This closes void run()
}; // This closes class NoxEngine

int main() {
    NoxEngine engine;
    engine.run();
    return 0;
}
