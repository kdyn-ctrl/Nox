#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <numeric>
#include <string>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <cctype>
#include "RegimeStateMachine.hpp" 
#include "httplib.h"               // Add this for HTTP Client capabilities
#include "nlohmann/json.hpp"       // Add this for JSON creation

// Lightweight Telegram Notifier using httplib
class TelegramNotifier {
public:
    TelegramNotifier(const std::string& token, const std::string& chat_id)
        : bot_token_(token), chat_id_(chat_id) {
        if (bot_token_.empty() || chat_id_.empty()) {
            std::cerr << "⚠️  [TELEGRAM] Token or Chat ID is empty. Notifications disabled." << std::endl;
            is_enabled_ = false;
        } else {
            std::cout << "[INFO] Telegram Notifier enabled for Chat ID: " << chat_id_ << std::endl;
            is_enabled_ = true;
        }
    }

    void send(const std::string& message) {
        if (!is_enabled_) return;

        std::string encoded_message = url_encode(message);

        httplib::Client cli("https://api.telegram.org");
        cli.set_connection_timeout(std::chrono::seconds(10));
        
        std::string path = "/bot" + bot_token_ + "/sendMessage?chat_id=" + chat_id_ + "&text=" + encoded_message;

        auto res = cli.Get(path.c_str());

        if (!res || res->status != 200) {
            std::cerr << "❌ [TELEGRAM] Failed to send message. Status: "
                      << (res ? std::to_string(res->status) : "No Response") << "\\n"
                      << (res ? res->body : "") << std::endl;
        }
    }

private:
    std::string url_encode(const std::string& value) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : value) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << '%' << std::setw(2) << std::uppercase << (int)(unsigned char)c;
            }
        }
        return escaped.str();
    }

    std::string bot_token_;
    std::string chat_id_;
    bool is_enabled_ = false;
};

using json = nlohmann::json;

// Holds SPY price and 200-day SMA fetched from Alpaca
struct SpyData {
    double price   = 0.0;
    double sma200  = 0.0;
    bool   valid   = false;
};

// Combined snapshot of all market inputs required by the regime state machine
struct MarketSnapshot {
    double vix        = 0.0;
    double spy_price  = 0.0;
    double spy_200_sma = 0.0;
    bool   valid      = false;
};

// Fetches the latest VIX close from Yahoo Finance.
// Returns -1.0 on any failure (network, parse, or missing data).
double fetch_vix() {
    try {
        httplib::Client cli("https://query1.finance.yahoo.com");
        cli.set_connection_timeout(std::chrono::seconds(10));
        cli.set_read_timeout(std::chrono::seconds(15));

        auto res = cli.Get("/v8/finance/chart/%5EVIX?interval=1d&range=2d");
        if (!res || res->status != 200) {
            std::cerr << "❌ [ANALYST] VIX fetch failed. Status: "
                      << (res ? std::to_string(res->status) : "No Response") << std::endl;
            return -1.0;
        }

        auto body = json::parse(res->body);
        const auto& closes = body.at("chart").at("result").at(0)
                                  .at("indicators").at("quote").at(0)
                                  .at("close");

        // Walk backwards to find the last non-null close value
        for (int i = static_cast<int>(closes.size()) - 1; i >= 0; --i) {
            if (!closes[i].is_null()) {
                return closes[i].get<double>();
            }
        }

        std::cerr << "❌ [ANALYST] VIX response contained no valid close values." << std::endl;
        return -1.0;

    } catch (const std::exception& e) {
        std::cerr << "💥 [ANALYST] VIX parse exception: " << e.what() << std::endl;
        return -1.0;
    }
}

// Fetches SPY current price and 200-day SMA from Yahoo Finance.
// Uses the same API already proven for VIX — no credentials required.
// Alpaca's free-tier data API returns null bars for paper accounts, so
// Yahoo Finance is the reliable zero-cost alternative.
SpyData fetch_spy_data() {
    try {
        httplib::Client cli("https://query1.finance.yahoo.com");
        cli.set_connection_timeout(std::chrono::seconds(10));
        cli.set_read_timeout(std::chrono::seconds(15));

        // range=1y gives ~252 daily bars — well above the 200 needed for SMA.
        // Yahoo returns bars in ascending order; last element is most recent.
        auto res = cli.Get("/v8/finance/chart/SPY?interval=1d&range=1y");
        if (!res || res->status != 200) {
            std::cerr << "❌ [ANALYST] SPY fetch failed. Status: "
                      << (res ? std::to_string(res->status) : "No Response") << std::endl;
            return {};
        }

        auto body = json::parse(res->body);
        const auto& closes = body.at("chart").at("result").at(0)
                                  .at("indicators").at("quote").at(0)
                                  .at("close");

        // Collect all non-null close values
        std::vector<double> valid_closes;
        valid_closes.reserve(closes.size());
        for (const auto& c : closes) {
            if (!c.is_null()) {
                valid_closes.push_back(c.get<double>());
            }
        }

        if (valid_closes.empty()) {
            std::cerr << "❌ [ANALYST] SPY response contained no valid close values." << std::endl;
            return {};
        }

        // RULE-001: We need at least 200 valid closes to compute a true SMA-200.
        // range=1y returns ~252 bars, but holidays/early closes can reduce that.
        // If we have fewer than 200 valid closes, the SMA is mathematically
        // undefined — returning stale or incorrect data is worse than aborting.
        if (valid_closes.size() < 200) {
            std::cerr << "❌ [ANALYST] Insufficient SPY bars for SMA-200 calculation. "
                      << "Got " << valid_closes.size() << ", need 200. Aborting cycle."
                      << std::endl;
            return {};
        }

        // Last entry is the most recent close (Yahoo returns ascending order)
        double current_price = valid_closes.back();

        // RULE-001 Fix: Average ONLY the last 200 closes, not all ~252 bars.
        // Averaging all bars was producing a ~252-day SMA, causing a lag-distorted
        // regime signal that delayed RISK_ON/RISK_OFF transitions.
        double sum = 0.0;
        for (size_t i = valid_closes.size() - 200; i < valid_closes.size(); ++i) {
            sum += valid_closes[i];
        }
        double sma200 = sum / 200.0;

        return {current_price, sma200, true};

    } catch (const std::exception& e) {
        std::cerr << "💥 [ANALYST] SPY data parse exception: " << e.what() << std::endl;
        return {};
    }
}

// Orchestrates both data fetches and returns a single combined MarketSnapshot.
// Both sources are Yahoo Finance — no Alpaca data API subscription required.
MarketSnapshot fetch_market_snapshot(TelegramNotifier& notifier) {
    double vix  = fetch_vix();
    SpyData spy = fetch_spy_data();

    bool had_error = false;
    if (vix < 0.0) {
        notifier.send("🔴 ANALYST CRITICAL: Failed to fetch VIX data from Yahoo Finance. Market analysis is compromised.");
        had_error = true;
    }
    if (!spy.valid) {
        notifier.send("🔴 ANALYST CRITICAL: Failed to fetch SPY data from Yahoo Finance. Market analysis is compromised.");
        had_error = true;
    }

    if (had_error) {
        std::cerr << "❌ [ANALYST] Market snapshot incomplete — VIX: " << vix
                  << ", SPY valid: " << std::boolalpha << spy.valid << std::endl;
        return {};
    }

    return {vix, spy.price, spy.sma200, true};
}

// Converts the Regime enum to a canonical string token for payload serialisation.
// RULE-016 fix: regime_state must be a clean enum label, not the full log_message.
static std::string regime_to_string(Regime r) {
    switch (r) {
        case Regime::RISK_ON:     return "RISK_ON";
        case Regime::TRANSITION:  return "TRANSITION";
        case Regime::RISK_OFF:    return "RISK_OFF";
        default:                  return "TRANSITION"; // safe default
    }
}

int main() {
    RegimeStateMachine regime_monitor;
    std::cout << "Nox Analyst Agent: ONLINE." << std::endl;

    // RULE-009 — Hard-abort on any missing credential or config variable.
    const char* env_token = std::getenv("WEBHOOK_SECRET_TOKEN");
    if (!env_token) {
        std::cerr << "[FATAL] [ANALYST] WEBHOOK_SECRET_TOKEN not set. Refusing to start." << std::endl;
        return 1;
    }
    std::string secret_token = env_token;

    const char* env_api_key = std::getenv("ALPACA_API_KEY");
    if (!env_api_key) {
        std::cerr << "[FATAL] [ANALYST] ALPACA_API_KEY not set. Refusing to start." << std::endl;
        return 1;
    }
    std::string alpaca_api_key = env_api_key;

    const char* env_api_secret = std::getenv("ALPACA_SECRET_KEY");
    if (!env_api_secret) {
        std::cerr << "[FATAL] [ANALYST] ALPACA_SECRET_KEY not set. Refusing to start." << std::endl;
        return 1;
    }
    std::string alpaca_api_secret = env_api_secret;

    const char* env_tg_token = std::getenv("TELEGRAM_BOT_TOKEN");
    const char* env_tg_chat_id = std::getenv("TELEGRAM_CHAT_ID");
    std::string tg_token = env_tg_token ? env_tg_token : "";
    std::string tg_chat_id = env_tg_chat_id ? env_tg_chat_id : "";
    TelegramNotifier notifier(tg_token, tg_chat_id);

    // RULE-001 — Cycle interval MUST come from the environment; never hardcoded.
    // Allows the interval to be tightened (e.g., to 1–4 h) during elevated-volatility
    // regimes without a code rebuild or container restart.
    const char* env_cycle_hours = std::getenv("ANALYST_CYCLE_HOURS");
    if (!env_cycle_hours) {
        std::cerr << "[FATAL] [ANALYST] ANALYST_CYCLE_HOURS not set. Refusing to start." << std::endl;
        return 1;
    }
    int cycle_hours = 0;
    try {
        cycle_hours = std::stoi(env_cycle_hours);
        if (cycle_hours <= 0) throw std::invalid_argument("must be positive");
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] [ANALYST] ANALYST_CYCLE_HOURS is invalid ('" << env_cycle_hours
                  << "'): " << e.what() << ". Refusing to start." << std::endl;
        return 1;
    }
    std::cout << "[INFO] [ANALYST] Cycle interval set to " << cycle_hours << " hour(s)." << std::endl;

    while (true) {
        // 1. Fetch live market data (VIX + SPY both from Yahoo Finance)
        MarketSnapshot snapshot = fetch_market_snapshot(notifier);

        if (!snapshot.valid) {
            std::cerr << "⚠️  [ANALYST] Failed to obtain a valid market snapshot. Skipping cycle." << std::endl;
            std::this_thread::sleep_for(std::chrono::minutes(5));
            continue;
        }

        double spy_price = snapshot.spy_price;

        // 2. Feed it to the State Machine
        AllocationStrategy current_strategy = regime_monitor.evaluate(
            snapshot.vix, snapshot.spy_price, snapshot.spy_200_sma
        );

        // 3. Log the heartbeat locally
        std::cout << "📊 " << current_strategy.log_message << std::endl;
        std::cout << "Capital Multiplier: " << current_strategy.capital_multiplier << std::endl;

        // 4. TRANSMIT SIGNAL VIA INTERNAL WEBHOOK (With Retries and Backoff)
        bool success = false;
        int max_retries = 5;
        unsigned int delay_seconds = 2;

        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            try {
                // Build the exact structural payload the execution engine expects
                json payload = {
                    {"secret_key", secret_token},
                    {"ticker", "GLOBAL_AUDIT"},
                    {"action", "REPORT"},
                    {"price", spy_price},
                    {"risk_tier", current_strategy.capital_multiplier == 1.0 ? 1 : 2},
                    {"vol", 0LL}, 
                    {"regime_state", regime_to_string(current_strategy.current_regime)},
                    {"atr", 0.0},
                    {"kelly_pct", 0.0},
                    {"report_body", current_strategy.log_message}
                };

                // Hit the execution container directly over the shared Docker bridge network
                httplib::Client cli("execution-engine", 8080);
                cli.set_connection_timeout(std::chrono::seconds(5));
                cli.set_read_timeout(std::chrono::seconds(10));

                auto res = cli.Post("/webhook", payload.dump(), "application/json");

                if (res && res->status == 200) {
                    std::cout << "✅ [ANALYST] Report payload successfully dispatched to Execution Engine." << std::endl;
                    success = true;
                    break;
                } else {
                    std::cerr << "❌ [ANALYST ERROR] Failed to route report (Attempt " << attempt << "/" << max_retries << "). Status: " 
                              << (res ? std::to_string(res->status) : "No Response") << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "💥 [ANALYST ERROR] Ingestion pipeline exception on attempt " << attempt << ": " << e.what() << std::endl;
            }

            if (attempt < max_retries) {
                std::cout << "⏳ Retrying in " << delay_seconds << " seconds..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
                delay_seconds *= 2; // Exponential backoff
            }
        }

        if (!success) {
            std::cerr << "💥 [ANALYST CRITICAL] All retry attempts exhausted. Failed to send payload to Execution Engine." << std::endl;
            notifier.send("🔴 ANALYST CRITICAL: All 5 retries exhausted. The Analyst C++ brain CANNOT communicate with the Execution Engine. Manual intervention required.");
        } else {
            std::string success_message = "✅ Analyst cycle complete. " + current_strategy.log_message;
            notifier.send(success_message);
        }

        // RULE-001 — Sleep for the env-configured interval, not a hardcoded 24 h.
        std::cout << "[INFO] [ANALYST] Cycle complete. Sleeping for "
                  << cycle_hours << " hour(s)." << std::endl;
        std::this_thread::sleep_for(std::chrono::hours(cycle_hours));
    }
   
     return 0;
}
