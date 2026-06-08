#include "httplib.h"
#include "json.hpp"
#include "RegimeStateMachine.hpp"
#include <iostream>
#include <string>
#include <cmath>

using json = nlohmann::json;

// --- 1. TELEGRAM NOTIFIER ---
class TelegramNotifier {
public:
    static void sendMessage(std::string message) {
        std::string token = std::getenv("TELEGRAM_BOT_TOKEN") ? std::getenv("TELEGRAM_BOT_TOKEN") : "";
        std::string chatId = std::getenv("TELEGRAM_CHAT_ID") ? std::getenv("TELEGRAM_CHAT_ID") : "";

        if (token.empty() || chatId.empty()) return;

        httplib::Client cli("https://api.telegram.org");
        std::string path = "/bot" + token + "/sendMessage";
        
        json body = {
            {"chat_id", chatId},
            {"text", message},
            {"parse_mode", "Markdown"}
        };

        cli.Post(path.c_str(), body.dump(), "application/json");
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
    std::string ticker;
    std::string action;
    double price;
    double rsi;
    std::string vol;
    double atr;
    int risk_tier;
};

// --- 3. KELLY CALCULATOR ---
int calculate_kelly_size(double equity, double current_price) {
    if (current_price <= 0) return 1; 

    double win_rate = 0.55;  
    double win_loss_ratio = 2.0; 
    
    // The Kelly Formula: K% = W - ((1 - W) / R)
    double kelly_pct = win_rate - ((1.0 - win_rate) / win_loss_ratio);
    
    // Apply Half-Kelly for safety (0.5 multiplier)
    double adjusted_risk = kelly_pct * 0.5;
    
    // SAFETY RAILS:
    // Cap at 10% per trade, and minimum 1% if strategy is active
    if (adjusted_risk > 0.10) adjusted_risk = 0.10;
    if (adjusted_risk < 0) adjusted_risk = 0.01; 

    double dollar_amount = equity * adjusted_risk;
    int shares = static_cast<int>(std::floor(dollar_amount / current_price));
    
    return (shares > 0) ? shares : 1; 
}

// --- 3.5 RISK AGENT (Legacy ATR Logic) ---
class RiskAgent {
public:
    static double calculatePositionSize(double equity, double atr, int tier) {
        if (atr <= 0) return 0;
        double riskPercent = (tier == 3) ? 0.05 : 0.01; 
        double stopMultiplier = (tier == 3) ? 3.5 : 2.0;
        double dollarRisk = equity * riskPercent;
        double stopDistance = atr * stopMultiplier;
        return dollarRisk / stopDistance;
    }
};

// --- 4. EXECUTION AGENT ---
class ExecutionAgent {
public:
    // Updated to accept 'int' for mathematically precise share counts
    bool placeOrder(TradeSignal sig, int quantity, std::string apiKey, std::string apiSec) {
        if (quantity <= 0) return false;

        std::string msg = "🚀 *OPENCLAW TRADE*\n"
                         "*Ticker:* " + sig.ticker + "\n"
                         "*Action:* " + sig.action + "\n"
                         "*Kelly Qty:* " + std::to_string(quantity) + " Shares\n"
                         "*Price:* $" + std::to_string(sig.price) + "\n"
                         "*Risk Tier:* " + std::to_string(sig.risk_tier);

        Logger::log("EXEC", "Firing " + sig.action + " for " + std::to_string(quantity) + " shares of " + sig.ticker);
        TelegramNotifier::sendMessage(msg); 
        return true;
    }
};

// --- 5. THE ENGINE ---
class OpenClawEngine {
private:
    std::string secret;
    std::string apiKey;
    std::string apiSec;
    double accountEquity = 100000.0; // Updated to your $100k Paper Balance

    void process(TradeSignal sig) {
        // Now using the Kelly Calculator for position sizing
        int qty = calculate_kelly_size(accountEquity, sig.price);
        // Skip processing if the action is to just HOLD position
    if (sig.action == "HOLD") {
        std::cout << "[EXECUTION] Strategy indicates HOLD. No orders routed." << std::endl;
        return;
    }

    // TRANSMIT LIVE ORDER TO ALPACA PAPER TRADING API
    try {
        httplib::Client alpaca_cli("https://paper-api.alpaca.markets");
        
        // Inject your secure environment variables into the request headers
        httplib::Headers headers = {
            {"APCA-API-KEY-ID", apiKey},
            {"APCA-API-SECRET-KEY", apiSec},
            {"Content-Type", "application/json"}
        };

        // Structure the exact JSON payload Alpaca requires, using your dynamic Kelly qty
        json order_payload = {
            {"symbol", sig.ticker},
            {"qty", std::to_string(qty)}, // Uses your Kelly size!
            {"side", sig.action == "BUY" ? "buy" : "sell"},
            {"type", "market"},
            {"time_in_force", "day"}
        };

        std::cout << "[EXECUTION] Routing order to Alpaca: " << qty << " shares of " << sig.ticker << "..." << std::endl;

        auto res = alpaca_cli.Post("/v2/orders", headers, order_payload.dump(), "application/json");

        if (res && res->status == 200) {
            json response_data = json::parse(res->body);
            std::cout << "🎯 [ORDER EXECUTED] Alpaca Order ID: " 
                      << response_data.value("id", "UNKNOWN") << " | Status: " 
                      << response_data.value("status", "pending") << std::endl;
        } else {
            std::cerr << "⚠️ [ALPACA REJECTION] Exchange returned status code: " 
                      << (res ? std::to_string(res->status) : "TIMEOUT") << std::endl;
            if (res) {
                std::cerr << "Details: " << res->body << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "💥 Runtime Exception routing order to Alpaca: " << e.what() << std::endl;
    }
    }

public:
    OpenClawEngine() {
        secret = std::getenv("WEBHOOK_SECRET_TOKEN") ? std::getenv("WEBHOOK_SECRET_TOKEN") : "openclaw_alpha_777";
        apiKey = std::getenv("ALPACA_API_KEY") ? std::getenv("ALPACA_API_KEY") : "";
        apiSec = std::getenv("ALPACA_SECRET_KEY") ? std::getenv("ALPACA_SECRET_KEY") : "";
    }

    void run() {
        httplib::Server svr;

        svr.Post("/webhook", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                json data = json::parse(req.body);
                if (data.value("secret_key", "") != secret) {
                    res.status = 401;
                    res.set_content("Unauthorized", "text/plain");
                    return;
                }

                TradeSignal signal;
                signal.ticker = data.value("ticker", "UNKNOWN");
                signal.action = data.value("action", "HOLD");
                signal.price = data.value("price", 0.0);
                signal.rsi = data.value("rsi", 50.0);
                signal.vol = data.value("vol", 0.0);
                signal.atr = data.value("atr", 0.0);
                signal.risk_tier = data.value("risk_tier", 1);

                std::cout << "📥 Signal Received: " << signal.action << " " << signal.ticker << std::endl;
                
                // Route signal to state machine or notifier
                TelegramNotifier::sendMessage("🚀 Signal Parsed: " + signal.action + " " + signal.ticker);

                res.status = 200;
                res.set_content("Signal processed successfully", "text/plain");

            } catch (const json::parse_error& e) {
                std::cerr << "💥 [PARSING ERROR] Malformed JSON payload: " << e.what() << std::endl;
                std::cerr << "Raw Body: " << req.body << std::endl;
                res.status = 400;
                res.set_content("Malformed JSON Structure", "text/plain");
            } catch (const json::type_error& e) {
                std::cerr << "💥 [TYPE ERROR] Data type mismatch in signal fields: " << e.what() << std::endl;
                res.status = 400;
                res.set_content("Data Type Mismatch", "text/plain");
            } catch (const std::exception& e) {
                std::cerr << "💥 [SERVER ERROR] Standard exception: " << e.what() << std::endl;
                res.status = 500;
            } catch (...) {
                std::cerr << "💥 [CRITICAL] Unknown error occurred in webhook router." << std::endl;
                res.status = 500;
            }
        });

        std::cout << "⚡ OpenClaw Execution Engine listening on 0.0.0.0:8080..." << std::endl;
        svr.listen("0.0.0.0", 8080);
    }
}; // Closes class OpenClawEngine

int main() {
    OpenClawEngine engine;
    engine.run();
    return 0;
}
