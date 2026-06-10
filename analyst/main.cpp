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
                {"ticker", "GLOBAL_AUDIT"},
                {"action", "REPORT"},
                {"price", spy_price},
                {"risk_tier", current_strategy.capital_multiplier == 1.0 ? 1 : 2},
                {"vol", 0}, // Explicitly matching your long long setup
                {"regime_state", current_strategy.log_message},
                {"atr", 4.82}, // Replace with your dynamic ATR variable
                {"kelly_pct", 1.45}, // Replace with your dynamic Kelly variable
                {"report_body", "YOUR_LONG_FORM_TEXT_ANALYSIS_STRING_GOES_HERE"} 
            };
		// Hit the execution container directly over the shared Docker bridge network
            httplib::Client cli("http://execution_engine:8080");
            auto res = cli.Post("/webhook", payload.dump(), "application/json");

            if (res && res->status == 200) {
                std::cout << "✅ [ANALYST] Report payload successfully dispatched to Execution Engine." << std::endl;
            } else {
                std::cerr << "❌ [ANALYST ERROR] Failed to route report. Status: " 
                          << (res ? std::to_string(res->status) : "No Response") << std::endl;
            }

        } catch (const std::exception& e) {
            std::cerr << "💥 [ANALYST CRITICAL] Ingestion pipeline exception: " << e.what() << std::endl;
        }

        // Standard heartbeat interval (e.g., check/report once a day or on a loop)
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
   
     return 0;
}
