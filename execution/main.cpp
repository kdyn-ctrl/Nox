#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
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
};

// --- 3. KELLY CALCULATOR ---
int calculate_kelly_size(double equity, double current_price) {
    if (current_price <= 0) return 1; 

    const char* wr_env  = std::getenv("KELLY_WIN_RATE");
    const char* wlr_env = std::getenv("KELLY_WIN_LOSS_RATIO");
    double win_rate      = wr_env  ? std::stod(wr_env)  : 0.55;
    double win_loss_ratio = wlr_env ? std::stod(wlr_env) : 2.0;
    
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

// --- 4. THE ENGINE ---
class OpenClawEngine {
private:
    std::string secret;
    std::string apiKey;
    std::string apiSec;

    double fetch_account_equity() {
        try {
            httplib::Client alpaca_cli("https://paper-api.alpaca.markets");
            alpaca_cli.set_connection_timeout(std::chrono::seconds(5));
            alpaca_cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID", apiKey},
                {"APCA-API-SECRET-KEY", apiSec}
            };

            auto res = alpaca_cli.Get("/v2/account", headers);
            if (res && res->status == 200) {
                json account_data = json::parse(res->body);
                return std::stod(account_data.value("portfolio_value", "10000.0"));
            } else {
                Logger::log("WARN", "Failed to fetch account equity from Alpaca. Status: " +
                            (res ? std::to_string(res->status) : "TIMEOUT") + ". Falling back to $10,000.");
            }
        } catch (const std::exception& e) {
            Logger::log("WARN", "Exception fetching account equity: " + std::string(e.what()) + ". Falling back to $10,000.");
        }
        return 10000.0;
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

        // Fetch live equity, then size the position with Kelly
        double live_equity = fetch_account_equity();
        int qty = calculate_kelly_size(live_equity, sig.price);

        // TRANSMIT LIVE ORDER TO ALPACA PAPER TRADING API
        try {
            httplib::Client alpaca_cli("https://paper-api.alpaca.markets");
            alpaca_cli.set_connection_timeout(std::chrono::seconds(5));
            alpaca_cli.set_read_timeout(std::chrono::seconds(10));
            
            // Inject your secure environment variables into the request headers
            httplib::Headers headers = {
                {"APCA-API-KEY-ID", apiKey},
                {"APCA-API-SECRET-KEY", apiSec},
                {"Content-Type", "application/json"}
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
    OpenClawEngine() {
        const char* env_secret = std::getenv("WEBHOOK_SECRET_TOKEN");
        const char* env_key    = std::getenv("ALPACA_API_KEY");
        const char* env_sec    = std::getenv("ALPACA_SECRET_KEY");

        if (!env_secret) {
            std::cerr << "[FATAL] WEBHOOK_SECRET_TOKEN env variable is not set. Exiting." << std::endl;
            std::exit(1);
        }
        if (!env_key) {
            std::cerr << "[FATAL] ALPACA_API_KEY env variable is not set. Exiting." << std::endl;
            std::exit(1);
        }
        if (!env_sec) {
            std::cerr << "[FATAL] ALPACA_SECRET_KEY env variable is not set. Exiting." << std::endl;
            std::exit(1);
        }

        secret = env_secret;
        apiKey = env_key;
        apiSec = env_sec;
    }

    void run() {
        httplib::Server svr;

        svr.Post("/webhook", [this](const httplib::Request& req, httplib::Response& res) {
            std::string body = req.body;
            int success_count = 0;

            try {
                // Parse cleaner using standard json parser instead of hand-rolled brace split logic
                json root_payload = json::parse(body);

                auto process_single_chunk = [this, &success_count](const json& data) {
                    if (data.value("secret_key", "") != secret) {
                        Logger::log("WARN", "Unauthorized signal dropped by security shield.");
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
                    if (data.contains("risk_tier")) {
                        if (data["risk_tier"].is_number_integer()) {
                            signal.risk_tier = data["risk_tier"].get<int>();
                        } else if (data["risk_tier"].is_string()) {
                            signal.risk_tier = std::stoi(data["risk_tier"].get<std::string>());
                        }
                    }

                    Logger::log("INFO", "Signal Parsed successfully: " + signal.action + " " + signal.ticker);
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
                Logger::log("ERROR", "JSON parse error: " + std::string(e.what()));
            } catch (const json::type_error& e) {
                Logger::log("ERROR", "Type mismatch in payload: " + std::string(e.what()));
            } catch (const std::exception& e) {
                Logger::log("ERROR", "Exception processing signals: " + std::string(e.what()));
            }

            // Global Webhook Router Responses & Safety Nets
            try {
                if (success_count > 0) {
                    res.status = 200;
                    res.set_content("Processed " + std::to_string(success_count) + " signal(s)", "text/plain");
                } else {
                    res.status = 400;
                    res.set_content("No valid signals processed", "text/plain");
                }
            } catch (const std::exception& e) {
                Logger::log("ERROR", "Standard exception: " + std::string(e.what()));
                res.status = 500;
            } catch (...) {
                Logger::log("ERROR", "Unknown error occurred in webhook router.");
                res.status = 500;
            }
        }); // This perfectly closes the svr.Post router lambda

        Logger::log("INFO", "OpenClaw Execution Engine listening on 0.0.0.0:8080...");
        svr.listen("0.0.0.0", 8080);
    } // This closes void run()
}; // This closes class OpenClawEngine

int main() {
    OpenClawEngine engine;
    engine.run();
    return 0;
}
