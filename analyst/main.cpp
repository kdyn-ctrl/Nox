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
#include <ctime>
#include "../shared/RegimeStateMachine.hpp"
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

// ---------------------------------------------------------------------------
// WS4 — sentiment scaffolding.
// A scored headline as it will arrive from the NLP layer (WS1 Contradiction
// Vector). Stored with its emission time so its weight can be decayed on read.
// dt_hours is computed by the caller against "now" each cycle.
struct SentimentScore {
    SignalCategory category = SignalCategory::GENERIC;
    double         score_0  = 0.0;   // magnitude at emission, signed [-1, 1]
    double         age_hours = 0.0;  // hours since emission
};

// Applies exponential half-life decay to a batch of sentiment scores and
// returns the net (summed) decayed weight. WS1 will populate `scores`; until
// then this runs over an empty batch and yields 0.0, a harmless no-op.
static double apply_decay_to_sentiment(const HalfLifeDecay& decay,
                                       const std::vector<SentimentScore>& scores) {
    double net = 0.0;
    for (const auto& s : scores) {
        net += decay.decayed_score(s.category, s.score_0, s.age_hours);
    }
    return net;
}

// Maps a Contradiction Vector category string to the WS4 SignalCategory enum.
static SignalCategory category_from_string(const std::string& c) {
    if (c == "GEOPOLITICAL")   return SignalCategory::GEOPOLITICAL;
    if (c == "MACRO_ECONOMIC") return SignalCategory::MACRO_ECONOMIC;
    if (c == "EARNINGS")       return SignalCategory::EARNINGS;
    if (c == "TECHNICAL")      return SignalCategory::TECHNICAL;
    return SignalCategory::GENERIC;
}

// Hours elapsed since an ISO-8601 UTC timestamp ("2026-06-28T12:00:00Z").
// Returns 0.0 on parse failure or future timestamps (→ no decay applied).
static double hours_since_iso(const std::string& iso) {
    std::tm tm{};
    std::istringstream ss(iso);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0.0;
    std::time_t emitted = timegm(&tm);   // interpret parsed fields as UTC
    double dt = std::difftime(std::time(nullptr), emitted) / 3600.0;
    return dt > 0.0 ? dt : 0.0;
}

// WS1→WS4 connector: pull the latest decayed-sentiment feed from the
// america-data-engine Contradiction Vector over the shared Docker network.
// Returns an empty batch on any failure — the analyst degrades to VIX/SPY only.
static std::vector<SentimentScore> fetch_sentiment_scores(const std::string& secret) {
    std::vector<SentimentScore> out;
    try {
        httplib::Client cli("america-data-engine", 8001);
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(10));
        httplib::Headers headers = {{"X-Nox-Token", secret}};

        auto res = cli.Get("/contradiction/us", headers);
        if (!res || res->status != 200) {
            std::cerr << "[WARN] [ANALYST] Contradiction feed fetch failed. Status: "
                      << (res ? std::to_string(res->status) : "No Response") << std::endl;
            return out;
        }

        auto body = json::parse(res->body);
        if (!body.contains("sentiment_scores") || !body.at("sentiment_scores").is_array()) {
            return out;
        }
        for (const auto& s : body.at("sentiment_scores")) {
            SentimentScore ss;
            if (s.contains("category") && s.at("category").is_string())
                ss.category = category_from_string(s.at("category").get<std::string>());
            if (s.contains("score_0") && s.at("score_0").is_number())
                ss.score_0 = s.at("score_0").get<double>();
            if (s.contains("emitted_at") && s.at("emitted_at").is_string())
                ss.age_hours = hours_since_iso(s.at("emitted_at").get<std::string>());
            out.push_back(ss);
        }
        std::cout << "[INFO] [ANALYST] Pulled " << out.size()
                  << " sentiment score(s) from Contradiction Vector." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[WARN] [ANALYST] Contradiction feed parse error: " << e.what() << std::endl;
    }
    return out;
}

// Reads a boolean .env flag. Treats "true"/"1"/"yes" (any case) as true.
static bool env_flag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v) return fallback;
    std::string s(v);
    for (char& c : s) c = static_cast<char>(std::tolower(c));
    return s == "true" || s == "1" || s == "yes";
}

// Reads a positive double .env flag, falling back on absence or parse failure.
static double env_double(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

int main() {
    RegimeStateMachine regime_monitor;
    std::cout << "Nox Analyst Agent: ONLINE." << std::endl;

    // WS4 — Information Half-Life / Regime Decay configuration.
    // All decay/reset behaviour is bypassable ONLY via explicit .env flags so
    // backtests can replay raw, undecayed sentiment deterministically.
    HalfLifeDecay sentiment_decay;
    sentiment_decay.set_half_life_hours(SignalCategory::GEOPOLITICAL,
                                        env_double("HALFLIFE_GEOPOLITICAL_HOURS", 6.0));
    sentiment_decay.set_half_life_hours(SignalCategory::MACRO_ECONOMIC,
                                        env_double("HALFLIFE_MACRO_HOURS", 48.0));
    sentiment_decay.set_half_life_hours(SignalCategory::EARNINGS,
                                        env_double("HALFLIFE_EARNINGS_HOURS", 72.0));
    sentiment_decay.set_half_life_hours(SignalCategory::TECHNICAL,
                                        env_double("HALFLIFE_TECHNICAL_HOURS", 12.0));
    sentiment_decay.set_bypass(env_flag("HALFLIFE_DECAY_BYPASS", false));
    regime_monitor.set_reset_bypass(env_flag("REGIME_RESET_BYPASS", false));
    const double catalyst_vix_jump = env_double("CATALYST_VIX_JUMP", 8.0);

    std::cout << "[INFO] [ANALYST] WS4 decay "
              << (sentiment_decay.is_bypassed() ? "BYPASSED" : "active")
              << "; catalyst VIX jump threshold = " << catalyst_vix_jump << std::endl;

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
    double cycle_hours = 0.0;
    try {
        cycle_hours = std::stod(env_cycle_hours);
        if (cycle_hours <= 0.0) throw std::invalid_argument("must be positive");
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

        // 1b. WS4 — detect macro catalysts (VIX shock) BEFORE evaluating the
        // regime, so a reset can force a cautious TRANSITION for this cycle.
        bool catalyst_fired = regime_monitor.detect_volatility_catalyst(
            snapshot.vix, catalyst_vix_jump);

        // 1c. WS4 — apply half-life decay to NLP sentiment. The batch is pulled
        // live from the WS1 Contradiction Vector (already cross-checked against
        // IV skew); on a regime_reset the prior macro NLP weights are zeroed.
        std::vector<SentimentScore> sentiment_scores = fetch_sentiment_scores(secret_token);
        bool macro_reset = regime_monitor.consume_regime_reset();
        if (macro_reset) {
            sentiment_scores.clear(); // zero prior macro NLP weights
            std::cout << "[ANALYST] regime_reset consumed — sentiment weights zeroed." << std::endl;
        }
        double net_sentiment = apply_decay_to_sentiment(sentiment_decay, sentiment_scores);

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
                    {"report_body", current_strategy.log_message},
                    {"vix", snapshot.vix},
                    {"spy_price", snapshot.spy_price},
                    {"spy_200_sma", snapshot.spy_200_sma},
                    // WS4 — decay/reset telemetry for downstream consumers.
                    {"macro_reset", macro_reset},
                    {"catalyst_fired", catalyst_fired},
                    {"net_sentiment", net_sentiment},
                    {"halflife_bypassed", sentiment_decay.is_bypassed()}
                };

                // Hit the execution container directly over the shared Docker bridge network
                httplib::Client cli("execution-engine", 8080);
                cli.set_connection_timeout(std::chrono::seconds(5));
                cli.set_read_timeout(std::chrono::seconds(10));

                auto res = cli.Post("/webhook", payload.dump(), "application/json");

                if (res && res->status == 200) {
                    std::cout << "[INFO] [ANALYST] Report payload successfully dispatched to Execution Engine." << std::endl;
                    success = true;
                    break;
                } else {
                    std::cerr << "[ERROR] [ANALYST] Failed to route report (Attempt " << attempt << "/" << max_retries << "). Status: " 
                              << (res ? std::to_string(res->status) : "No Response") << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] [ANALYST] Ingestion pipeline exception on attempt " << attempt << ": " << e.what() << std::endl;
            }

            if (attempt < max_retries) {
                std::cout << "⏳ Retrying in " << delay_seconds << " seconds..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
                delay_seconds *= 2; // Exponential backoff
            }
        }

        if (!success) {
            std::cerr << "[ERROR] [ANALYST] CRITICAL: All retry attempts exhausted. Failed to send payload to Execution Engine." << std::endl;
            notifier.send("🔴 ANALYST CRITICAL: All 5 retries exhausted. The Analyst C++ brain CANNOT communicate with the Execution Engine. Manual intervention required.");
        } else {
            std::string success_message = "✅ Analyst cycle complete. " + current_strategy.log_message;
            notifier.send(success_message);
        }

        // RULE-001 — Sleep for the env-configured interval, not a hardcoded 24 h.
        std::cout << "[INFO] [ANALYST] Cycle complete. Sleeping for "
                  << cycle_hours << " hour(s)." << std::endl;
        auto sleep_duration = std::chrono::duration<double>(cycle_hours * 3600.0);
        std::this_thread::sleep_for(sleep_duration);
    }
   
     return 0;
}
