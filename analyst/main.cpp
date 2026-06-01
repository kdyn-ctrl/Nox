#include <iostream>
#include <chrono>
#include <thread>
#include "RegimeStateMachine.hpp" 
#include "httplib.h"               // Add this for HTTP Client capabilities
#include "nlohmann/json.hpp"       // Add this for JSON creation

using json = nlohmann::json;

int main() {
    RegimeStateMachine regime_monitor;
    std::cout << "OpenClaw Analyst Agent: ONLINE." << std::endl;

    // Fetch security token from environment variables
    const char* env_token = std::getenv("WEBHOOK_SECRET_TOKEN");
    std::string secret_token = env_token ? env_token : "openclaw_alpha_777";

    while (true) {
        // 1. Fetch data from Alpaca/Polygon here (VIX, SPY, SMA)
        double current_vix = 18.5;  
        double spy_price = 510.0;   
        double spy_200_sma = 480.0; 

        // 2. Feed it to the State Machine
        AllocationStrategy current_strategy = regime_monitor.evaluate(current_vix, spy_price, spy_200_sma);

        // 3. Log the heartbeat locally
        std::cout << "📊 " << current_strategy.log_message << std::endl;
        std::cout << "Capital Multiplier: " << current_strategy.capital_multiplier << std::endl;

        // 4. TRANSMIT SIGAL VIA INTERNAL WEBHOOK
        try {
            // Build the exact structural payload the execution engine expects
            json payload = {
                {"secret_key", secret_token},
                {"ticker", "SPY"},
                {"action", current_strategy.capital_multiplier > 0 ? "BUY" : "HOLD"},
                {"price", spy_price},
                {"risk_tier", current_strategy.capital_multiplier == 1.0 ? 1 : 2}
            };

            // Hit the execution container directly over the shared Docker bridge network
            httplib::Client cli("http://execution-engine:8080");
            auto res = cli.Post("/webhook", payload.dump(), "application/json");

            if (res && res->status == 200) {
                std::cout << "🚀 [WEBHOOK SUCCESS] Signal transmitted to Execution Engine safely." << std::endl;
            } else {
                std::cerr << "⚠️ [WEBHOOK FAILURE] Execution engine returned status: " 
                          << (res ? std::to_string(res->status) : "NO RESPONSE") << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "💥 Network Exception during webhook transmission: " << e.what() << std::endl;
        }

        // Sleep for 15 minutes before checking the regime again
        std::this_thread::sleep_for(std::chrono::minutes(15));
    }

    return 0;
}
