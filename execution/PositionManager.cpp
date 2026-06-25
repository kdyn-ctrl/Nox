#include "PositionManager.hpp"
#include "OptionsOrderRouter.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"
#include <thread>
#include <chrono>

// Forward declare TelegramNotifier to avoid including main.cpp
class TelegramNotifier {
public:
    static void sendMessage(std::string message);
};


// Helper to get current date as YYYY-MM-DD
std::string get_current_date() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d");
    return ss.str();
}

// Helper to calculate days between two dates
int days_between(const std::string& date1_str, const std::string& date2_str) {
    std::tm date1_tm = {};
    std::tm date2_tm = {};
    std::stringstream ss1(date1_str);
    std::stringstream ss2(date2_str);
    ss1 >> std::get_time(&date1_tm, "%Y-%m-%d");
    ss2 >> std::get_time(&date2_tm, "%Y-%m-%d");

    auto time1 = std::mktime(&date1_tm);
    auto time2 = std::mktime(&date2_tm);

    return std::abs(time2 - time1) / (60 * 60 * 24);
}

double get_option_price_from_alpaca(const OptionPosition& position,
                                     const std::string& alpaca_base_url,
                                     const std::string& api_key,
                                     const std::string& api_secret,
                                     const std::string& occ_symbol)
{
    try {
        // Fetch latest option quote from Alpaca market data API
        httplib::Client cli(alpaca_base_url);
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(10));

        httplib::Headers headers = {
            {"APCA-API-KEY-ID",     api_key},
            {"APCA-API-SECRET-KEY", api_secret}
        };

        std::string path = "/v1beta1/options/quotes/latest?symbols=" + occ_symbol;
        auto res = cli.Get(path.c_str(), headers);

        if (!res || res->status != 200) {
            std::cerr << "[POS_MANAGER] Quote fetch failed for " << occ_symbol
                      << " — HTTP " << (res ? std::to_string(res->status) : "timeout") << std::endl;
            return -1.0;
        }

        json body = json::parse(res->body);

        // Alpaca returns quotes in format: {"quotes": {"OCC_SYMBOL": {...}}}
        if (!body.contains("quotes") || body["quotes"].empty()) {
            std::cerr << "[POS_MANAGER] No quotes returned for " << occ_symbol << std::endl;
            return -1.0;
        }

        auto& quote = body["quotes"][occ_symbol];

        // Use bid price for short positions (we want to close at bid),
        // ask price for long positions (we want to close at ask).
        // For simplicity, use mid-point: (bid + ask) / 2
        double bid = quote.value("bp", 0.0);  // bid price
        double ask = quote.value("ap", 0.0);  // ask price

        if (bid <= 0.0 && ask <= 0.0) {
            std::cerr << "[POS_MANAGER] Invalid quote data for " << occ_symbol
                      << " (bid=" << bid << ", ask=" << ask << ")" << std::endl;
            return -1.0;
        }

        // Mid-point price
        double current_price = (bid > 0 && ask > 0) ? (bid + ask) / 2.0 : (bid > 0 ? bid : ask);

        std::cout << "[POS_MANAGER] Quote for " << occ_symbol << ": bid=" << bid
                  << ", ask=" << ask << ", mid=" << current_price << std::endl;

        return current_price;

    } catch (const std::exception& e) {
        std::cerr << "[POS_MANAGER] Exception fetching quote for " << occ_symbol
                  << ": " << e.what() << std::endl;
        return -1.0;
    }
}


void PositionManager::monitor_positions() {
    while (run_monitoring_) {
        // Wait 30 minutes between monitoring cycles (RULE-008: respects Alpaca
        // timeouts), but wake early if stop_monitoring() is called so shutdown is
        // immediate. wait_for returns true when the predicate (stop requested)
        // becomes true → break the loop; false on timeout → run the next cycle.
        {
            std::unique_lock<std::mutex> lock(monitor_lock_);
            if (monitor_cv_.wait_for(lock, std::chrono::minutes(30),
                                     [this] { return !run_monitoring_.load(); })) {
                break;
            }
        }

        auto open_positions = get_open_positions();
        if (open_positions.empty()) {
            std::cout << "[POS_MANAGER] No open positions to monitor." << std::endl;
            continue;
        }

        std::cout << "[POS_MANAGER] Checking " << open_positions.size() << " open position(s)..." << std::endl;

        for (const auto& pos : open_positions) {
            double current_price = -1.0;
            std::string occ_symbol;

            try {
                // We need the exact OCC symbol to get a quote and to close the order.
                auto contract = order_router_.lookupContract(
                    pos.ticker, pos.strike, pos.expiration_date, pos.option_type
                );
                if (!contract.valid) {
                    std::cerr << "[POS_MANAGER] Could not find contract for " << pos.ticker << std::endl;
                    continue;
                }
                occ_symbol = contract.occ_symbol;

                // Fetch real-time market price from Alpaca for the option contract
                // NOTE: Alpaca credentials come from OptionsOrderRouter but we'd need to expose them.
                // For now, use environment variables as fallback (same ones the engine uses).
                const char* api_key_env = std::getenv("ALPACA_API_KEY");
                const char* api_sec_env = std::getenv("ALPACA_SECRET_KEY");
                const char* base_url_env = std::getenv("ALPACA_BASE_URL");

                if (!api_key_env || !api_sec_env || !base_url_env) {
                    std::cerr << "[POS_MANAGER] Missing ALPACA credentials in environment." << std::endl;
                    continue;
                }

                current_price = get_option_price_from_alpaca(
                    pos,
                    std::string(base_url_env),
                    std::string(api_key_env),
                    std::string(api_sec_env),
                    occ_symbol
                );

            } catch (const std::exception& e) {
                std::cerr << "[POS_MANAGER] Error looking up contract or price for "
                          << pos.ticker << ": " << e.what() << std::endl;
                continue;
            }

            if (current_price < 0) {
                std::cerr << "[POS_MANAGER] Failed to get current price for " << pos.ticker << std::endl;
                continue;
            }

            bool exit_triggered = false;
            std::string exit_reason;

            // Rule evaluation...
            if (pos.profile_type == "long" && current_price >= pos.entry_price * 1.50) {
                exit_triggered = true;
                exit_reason = "50% Profit Rule (Long)";
            } else if (pos.profile_type == "short_premium" && current_price <= pos.entry_price * 0.50) {
                exit_triggered = true;
                exit_reason = "50% Profit Rule (Short Premium)";
            }

            if (!exit_triggered && pos.profile_type == "short_premium") {
                if (days_between(get_current_date(), pos.expiration_date) <= 21) {
                    exit_triggered = true;
                    exit_reason = "21 DTE Rule";
                }
            }

            if (!exit_triggered) {
                if (pos.profile_type == "long" && current_price <= pos.entry_price * 0.50) {
                    exit_triggered = true;
                    exit_reason = "Stop Loss Rule (Long)";
                } else if (pos.profile_type == "short_premium" && current_price >= pos.entry_price * 2.0) {
                    exit_triggered = true;
                    exit_reason = "Stop Loss Rule (Short Premium)";
                }
            }

            if (exit_triggered) {
                std::cout << "[POS_MANAGER] EXIT TRIGGERED for " << pos.ticker 
                          << " (ID: " << pos.id << "): " << exit_reason << std::endl;

                bool is_short = (pos.profile_type == "short_premium");
                auto result = order_router_.closePosition(occ_symbol, pos.quantity, is_short);

                if (result.success) {
                    std::cout << "[POS_MANAGER] Successfully closed position " << pos.id 
                              << ". Order ID: " << result.order_id << std::endl;
                    
                    // Log transaction (placeholder)
                    std::cout << "LOG: " << pos.ticker << " " << pos.strike << " " << pos.option_type 
                              << " closed for " << exit_reason << ". Price: " << current_price << std::endl;

                    // Fire Telegram alert
                    std::stringstream tg_msg;
                    tg_msg << "✅ *Option Position Closed*\n"
                           << "────────────────────────\n"
                           << "• *Ticker:* " << pos.ticker << "\n"
                           << "• *Type:* " << pos.option_type << " @ " << pos.strike << "\n"
                           << "• *Reason:* " << exit_reason << "\n"
                           << "• *Entry Price:* " << pos.entry_price << "\n"
                           << "• *Exit Price:* " << current_price << "\n"
                           << "• *Order ID:* `" << result.order_id << "`\n"
                           << "• *Quantity:* " << pos.quantity;
                    TelegramNotifier::sendMessage(tg_msg.str());
                    
                    // Remove from database
                    remove_position(pos.id);
                } else {
                    std::cerr << "[POS_MANAGER] FAILED to close position " << pos.id
                              << ". Reason: " << result.message << std::endl;
                    std::stringstream err_msg;
                    err_msg << "🚨 *CRITICAL: Position Close FAILED*\n"
                            << "────────────────────────\n"
                            << "• *Ticker:* " << pos.ticker << "\n"
                            << "• *Position ID:* " << pos.id << "\n"
                            << "• *Reason:* " << result.message;
                    TelegramNotifier::sendMessage(err_msg.str());
                }
            }
        }
    }
}
