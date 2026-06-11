#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <numeric>
#include "RegimeStateMachine.hpp" 
#include "httplib.h"               // Add this for HTTP Client capabilities
#include "nlohmann/json.hpp"       // Add this for JSON creation

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

// Fetches SPY current price and 200-day SMA from the Alpaca Data API.
// Returns an invalid SpyData struct on any failure.
SpyData fetch_spy_data(const std::string& api_key, const std::string& api_secret) {
    try {
        httplib::Client cli("https://data.alpaca.markets");
        cli.set_connection_timeout(std::chrono::seconds(10));
        cli.set_read_timeout(std::chrono::seconds(15));
        cli.set_default_headers({
            {"APCA-API-KEY-ID",     api_key},
            {"APCA-API-SECRET-KEY", api_secret}
        });

        auto res = cli.Get("/v2/stocks/SPY/bars?timeframe=1Day&limit=200&sort=desc");
        if (!res || res->status != 200) {
            std::cerr << "❌ [ANALYST] SPY bar fetch failed. Status: "
                      << (res ? std::to_string(res->status) : "No Response") << std::endl;
            return {};
        }

        auto body = json::parse(res->body);
        const auto& bars = body.at("bars");

        if (bars.empty()) {
            std::cerr << "❌ [ANALYST] SPY bars response was empty." << std::endl;
            return {};
        }

        // bars[0] is the most recent bar (sort=desc)
        double current_price = bars[0].at("c").get<double>();

        // Average all available closes for the SMA
        std::vector<double> closes;
        closes.reserve(bars.size());
        for (const auto& bar : bars) {
            closes.push_back(bar.at("c").get<double>());
        }
        double sma200 = std::accumulate(closes.begin(), closes.end(), 0.0) /
                        static_cast<double>(closes.size());

        return {current_price, sma200, true};

    } catch (const std::exception& e) {
        std::cerr << "💥 [ANALYST] SPY data parse exception: " << e.what() << std::endl;
        return {};
    }
}

// Orchestrates both data fetches and returns a single combined MarketSnapshot.
MarketSnapshot fetch_market_snapshot(const std::string& api_key, const std::string& api_secret) {
    double vix     = fetch_vix();
    SpyData spy    = fetch_spy_data(api_key, api_secret);

    if (vix < 0.0 || !spy.valid) {
        std::cerr << "❌ [ANALYST] Market snapshot incomplete — VIX: " << vix
                  << ", SPY valid: " << std::boolalpha << spy.valid << std::endl;
        return {};
    }

    return {vix, spy.price, spy.sma200, true};
}

int main() {
    RegimeStateMachine regime_monitor;
    std::cout << "OpenClaw Analyst Agent: ONLINE." << std::endl;


    // Fetch security token from environment variables
    const char* env_token = std::getenv("WEBHOOK_SECRET_TOKEN");
    if (!env_token) {
        std::cerr << "[FATAL] WEBHOOK_SECRET_TOKEN not set. Refusing to start." << std::endl;
        return 1;
    }
    std::string secret_token = env_token;

    const char* env_api_key = std::getenv("ALPACA_API_KEY");
    if (!env_api_key) {
        std::cerr << "[FATAL] ALPACA_API_KEY not set. Refusing to start." << std::endl;
        return 1;
    }
    std::string alpaca_api_key = env_api_key;

    const char* env_api_secret = std::getenv("ALPACA_SECRET_KEY");
    if (!env_api_secret) {
        std::cerr << "[FATAL] ALPACA_SECRET_KEY not set. Refusing to start." << std::endl;
        return 1;
    }
    std::string alpaca_api_secret = env_api_secret;

    while (true) {
        // 1. Fetch live market data (VIX from Yahoo Finance, SPY from Alpaca)
        MarketSnapshot snapshot = fetch_market_snapshot(alpaca_api_key, alpaca_api_secret);

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
                    {"regime_state", current_strategy.log_message},
                    {"atr", 0.0},
                    {"kelly_pct", 0.0},
                    {"report_body", current_strategy.log_message}
                };

                // Hit the execution container directly over the shared Docker bridge network
                httplib::Client cli("http://execution_engine:8080");
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
        }

        // Standard heartbeat interval (e.g., check/report once a day or on a loop)
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
   
     return 0;
}
