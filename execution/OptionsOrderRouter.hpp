#pragma once

// OptionsOrderRouter — translates an OptionsSignal into real Alpaca options orders.
//
// Contract lookup flow:
//   1. Call Alpaca /v2/options/contracts to find contracts matching strike + expiry
//   2. Pick the best match by expiry proximity and strike
//   3. Place order via /v2/orders using the OCC symbol
//   4. Spreads and straddles/strangles use Alpaca's multi-leg (mleg) order class
//
// Controlled by OPTIONS_AUTO_EXECUTE env var.
// When disabled (default), the generator sends Telegram alerts only.
// When enabled, it alerts AND places real orders.

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "OptionsSignalTypes.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace nox::options_router {

// ─── Contract lookup result ───────────────────────────────────────────────────

struct AlpacaContract {
    std::string occ_symbol;    // e.g. "AAPL240801C00195000"
    double      strike       = 0.0;
    std::string expiry_date;   // "YYYY-MM-DD"
    std::string option_type;   // "call" or "put"
    bool        valid        = false;
};

// ─── Order result ─────────────────────────────────────────────────────────────

struct OrderResult {
    bool        success    = false;
    std::string order_id;
    std::string message;
    int         filled_qty = 0; // actual contracts filled (may be < requested on partial fill)
};

// ─── OptionsOrderRouter ───────────────────────────────────────────────────────

class OptionsOrderRouter {
public:
    OptionsOrderRouter(const std::string& alpacaUrl,
                       const std::string& apiKey,
                       const std::string& apiSec)
        : alpacaUrl_(alpacaUrl)
        , apiKey_(apiKey)
        , apiSec_(apiSec)
    {}

    // Verify the account holds enough shares for a covered call (100 per contract).
    // Returns true if shares are confirmed; false if the position is absent or the
    // API call fails. Callers should abort CC execution if this returns false.
    bool validateCCPosition(const std::string& underlying, int qty_contracts) const {
        try {
            auto cli = makeClient();
            auto res = cli.Get(("/v2/positions/" + underlying).c_str(), authHeaders());
            if (!res || res->status != 200) return false;
            json body  = json::parse(res->body);
            double qty = body.value("qty", 0.0);
            return qty >= static_cast<double>(qty_contracts) * 100.0;
        } catch (...) {
            return false;
        }
    }

    // Main entry point. Takes a fully assembled OptionsSignal and routes it.
    // Returns an OrderResult — caller logs and Telegrams based on outcome.
    OrderResult route(const nox::options_signal::OptionsSignal& sig, int qty_contracts = 1) {
        const std::string& strategy = sig.strategy;

        // Single-leg strategies
        if (strategy == "LONG_CALL" || strategy == "LONG_PUT" ||
            strategy == "CSP"       || strategy == "CC")
        {
            return routeSingleLeg(sig, qty_contracts);
        }

        // Spreads — two legs, same underlying, same expiry
        if (strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD") {
            return routeSpread(sig, qty_contracts);
        }

        // Straddle: ATM call + ATM put, same strike, same expiry
        if (strategy == "STRADDLE") {
            return routeStraddle(sig, qty_contracts);
        }

        // Strangle: OTM call + OTM put
        if (strategy == "STRANGLE") {
            return routeStrangle(sig, qty_contracts);
        }

        return {false, "", "Unknown strategy: " + strategy};
    }

    // PositionManager: public contract lookup and position close methods
    AlpacaContract lookupContract(const std::string& underlying,
                                  double             target_strike,
                                  const std::string& expiry_yyyy_mm_dd,
                                  const std::string& opt_type) const
    {
        return lookupContractImpl(underlying, target_strike, expiry_yyyy_mm_dd, opt_type);
    }

    OrderResult closePosition(const std::string& occ_symbol, int quantity, bool is_short_premium) const
    {
        return closePositionImpl(occ_symbol, quantity, is_short_premium);
    }

private:
    std::string alpacaUrl_;
    std::string apiKey_;
    std::string apiSec_;

    // ── Alpaca HTTP client factory ────────────────────────────────────────────

    httplib::Client makeClient() const {
        httplib::Client cli(alpacaUrl_);
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(10));
        return cli;
    }

    httplib::Headers authHeaders() const {
        return {
            {"APCA-API-KEY-ID",     apiKey_},
            {"APCA-API-SECRET-KEY", apiSec_},
            {"Content-Type",        "application/json"}
        };
    }

    // ── OCC symbol builder (for verification / display only) ─────────────────
    // Format: ROOT(6) + YYMMDD + C/P + STRIKE*1000(8 zero-padded)
    // Example: AAPL at $195.00 call expiring 2024-07-19 → AAPL  240719C00195000
    static std::string buildOCCSymbol(const std::string& root,
                                      const std::string& expiry_yyyy_mm_dd,
                                      const std::string& opt_type,
                                      double strike)
    {
        // Root: left-justified, padded to 6 chars with spaces
        std::string padded_root = root;
        padded_root.resize(6, ' ');

        // Expiry: YYMMDD (drop "20" prefix from year)
        // expiry_yyyy_mm_dd format: "2026-08-01"
        std::string yy = expiry_yyyy_mm_dd.substr(2, 2);
        std::string mm = expiry_yyyy_mm_dd.substr(5, 2);
        std::string dd = expiry_yyyy_mm_dd.substr(8, 2);

        // Type: C or P
        std::string type_char = (opt_type == "put" || opt_type == "PUT") ? "P" : "C";

        // Strike: multiply by 1000, 8-digit zero-padded integer
        long long strike_int = static_cast<long long>(std::round(strike * 1000.0));
        std::ostringstream strike_oss;
        strike_oss << std::setw(8) << std::setfill('0') << strike_int;

        return padded_root + yy + mm + dd + type_char + strike_oss.str();
    }

    // Alpaca contract lookup (for PositionManager and internal routing).
    // Searches for options contracts matching the given underlying, strike, expiry, and type.
    // Returns the best match (closest strike, then nearest expiry).
    AlpacaContract lookupContractImpl(const std::string& underlying,
                                     double             target_strike,
                                     const std::string& expiry_yyyy_mm_dd,
                                     const std::string& opt_type) const
    {
        auto cli = makeClient();

        // Widen the expiry window ±14 days around the target to find liquid contracts
        // We'll pick the closest expiry ≥ target
        std::string type_param = (opt_type == "put") ? "put" : "call";
        double strike_lo = target_strike * 0.90;
        double strike_hi = target_strike * 1.10;

        std::ostringstream path;
        path << "/v2/options/contracts"
             << "?underlying_symbols=" << underlying
             << "&type=" << type_param
             << "&strike_price_gte=" << std::fixed << std::setprecision(2) << strike_lo
             << "&strike_price_lte=" << std::fixed << std::setprecision(2) << strike_hi
             << "&expiration_date_gte=" << expiry_yyyy_mm_dd
             << "&limit=50";

        auto res = cli.Get(path.str().c_str(), authHeaders());
        if (!res || res->status != 200) {
            throw std::runtime_error("Contract lookup failed for " + underlying +
                                     " — HTTP " + (res ? std::to_string(res->status) : "timeout"));
        }

        json body = json::parse(res->body);
        const auto& contracts = body.value("option_contracts", json::array());
        if (contracts.empty()) {
            throw std::runtime_error("No contracts found for " + underlying +
                                     " strike≈" + std::to_string(target_strike));
        }

        // Pick best match: closest strike to target, then nearest expiry
        AlpacaContract best;
        double best_score = 1e9;
        for (const auto& c : contracts) {
            double s       = c.value("strike_price",  0.0);
            std::string ex = c.value("expiration_date", "");
            std::string sy = c.value("symbol", "");

            double strike_diff  = std::abs(s - target_strike);
            double expiry_score = (ex >= expiry_yyyy_mm_dd) ? 0.0 : 1000.0; // prefer ≥ target
            double score        = strike_diff + expiry_score;

            if (score < best_score) {
                best_score       = score;
                best.occ_symbol  = sy;
                best.strike      = s;
                best.expiry_date = ex;
                best.option_type = opt_type;
                best.valid       = true;
            }
        }
        return best;
    }

    // ── Live options quote (bid/ask/mid) ─────────────────────────────────────
    struct OptionsQuote {
        double bid = 0.0;
        double ask = 0.0;
        double mid = 0.0;
        bool   valid = false;
    };

    OptionsQuote fetchOptionsQuote(const std::string& occ_symbol) const {
        try {
            auto cli = makeClient();
            std::string path = "/v1beta1/options/quotes/latest?symbols=" + occ_symbol;
            auto res = cli.Get(path.c_str(), authHeaders());
            if (!res || res->status != 200) return {};
            json body = json::parse(res->body);
            const auto& quotes = body.value("quotes", json::object());
            if (!quotes.contains(occ_symbol)) return {};
            const auto& q = quotes.at(occ_symbol);
            double bid = q.value("bp", 0.0);
            double ask = q.value("ap", 0.0);
            if (bid > 0.0 && ask > 0.0 && ask >= bid)
                return {bid, ask, (bid + ask) / 2.0, true};
        } catch (...) {}
        return {};
    }

    // Poll Alpaca order status. Returns "filled", "partially_filled", "cancelled", etc.
    std::string checkOrderFill(const std::string& order_id) const {
        try {
            auto cli = makeClient();
            auto res = cli.Get(("/v2/orders/" + order_id).c_str(), authHeaders());
            if (!res || res->status != 200) return "unknown";
            return json::parse(res->body).value("status", "unknown");
        } catch (...) {
            return "unknown";
        }
    }

    // Returns the number of contracts actually filled so far (from Alpaca's filled_qty field).
    int fetchFilledQty(const std::string& order_id) const {
        try {
            auto cli = makeClient();
            auto res = cli.Get(("/v2/orders/" + order_id).c_str(), authHeaders());
            if (!res || res->status != 200) return 0;
            json body = json::parse(res->body);
            std::string fq = body.value("filled_qty", "0");
            return static_cast<int>(std::stod(fq));
        } catch (...) {
            return 0;
        }
    }

    void cancelAlpacaOrder(const std::string& order_id) const {
        try {
            auto cli = makeClient();
            cli.Delete(("/v2/orders/" + order_id).c_str(), authHeaders());
        } catch (...) {}
    }

    // Submit a limit order with mid → ask retry: 3 attempts stepping 10% toward ask each time.
    // side_is_buy=true: BUY order (step price UP toward ask to attract sellers).
    //               false: SELL order (step price DOWN toward bid to attract buyers).
    // limit_hint: starting limit price; 0.0 → fetch live quote to determine mid.
    OrderResult submitLimitWithRetry(json order_base,
                                     const std::string& occ_symbol,
                                     const std::string& label,
                                     double             limit_hint,
                                     bool               side_is_buy) const {
        OptionsQuote q = fetchOptionsQuote(occ_symbol);

        double mid = (q.valid) ? q.mid : limit_hint;
        double far  = q.valid ? (side_is_buy ? q.ask : q.bid) : 0.0;

        if (mid <= 0.0) {
            // No quote and no hint — fall back to market
            order_base["type"] = "market";
            order_base.erase("limit_price");
            return submitOrder(order_base, label + " [mkt fallback — no quote]");
        }
        if (far <= 0.0) far = side_is_buy ? mid * 1.10 : mid * 0.90;

        for (int attempt = 0; attempt < 3; ++attempt) {
            double step_frac  = attempt * 0.10; // 0%, 10%, 20% of (far - mid)
            double limit_price = mid + (far - mid) * step_frac;
            limit_price = std::round(limit_price * 100.0) / 100.0;
            limit_price = std::max(limit_price, 0.01);

            std::ostringstream lp_str;
            lp_str << std::fixed << std::setprecision(2) << limit_price;

            json order = order_base;
            order["type"]        = "limit";
            order["limit_price"] = lp_str.str();

            auto result = submitOrder(order, label + " lmt@$" + lp_str.str());
            if (!result.success) {
                // Alpaca rejected the limit structure (e.g., price out of range). Skip retry.
                if (attempt == 2) return result;
                continue;
            }

            // Poll for fill for up to 30 seconds (1-second ticks)
            bool filled = false;
            for (int sec = 0; sec < 30; ++sec) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::string status = checkOrderFill(result.order_id);
                if (status == "filled") {
                    result.filled_qty = fetchFilledQty(result.order_id);
                    filled = true;
                    break;
                }
                if (status == "partially_filled") {
                    // Wait the full 30s — Alpaca may finish the fill.
                    // If still partial at timeout, record only the filled portion.
                    result.filled_qty = fetchFilledQty(result.order_id);
                    filled = true; // accept partial fill; don't retry into the spread
                    break;
                }
                if (status == "cancelled" || status == "rejected" || status == "expired") break;
            }
            if (filled) return result;

            cancelAlpacaOrder(result.order_id);
            if (attempt == 2) {
                return {false, "", "Limit order unfilled after 3 attempts for " + label +
                        " — signal skipped to avoid chasing spread"};
            }
        }
        return {false, "", "Limit retry exhausted for " + label};
    }

    // Closes an open option position using a limit order (mid → fill direction retry).
    // is_short_premium=true → BUY to close (step price toward ask).
    // is_short_premium=false → SELL to close (step price toward bid).
    OrderResult closePositionImpl(const std::string& occ_symbol, int quantity, bool is_short_premium) const {
        std::string side = is_short_premium ? "buy" : "sell";
        json order = {
            {"symbol",          occ_symbol},
            {"qty",             quantity},
            {"side",            side},
            {"time_in_force",   "day"},
            {"position_effect", "close"}
        };
        return submitLimitWithRetry(order, occ_symbol, "CLOSE " + occ_symbol,
                                    0.0, is_short_premium);
    }


    // ── Single-leg order (LONG_CALL, LONG_PUT, CSP, CC) ──────────────────────

    OrderResult routeSingleLeg(const nox::options_signal::OptionsSignal& sig,
                                int qty_contracts) const
    {
        bool is_call = (sig.option_type == nox::options::OptionType::Call);
        std::string opt_type = is_call ? "call" : "put";

        AlpacaContract contract;
        try {
            contract = lookupContractImpl(sig.underlying, sig.strike,
                                         sig.expiry_date, opt_type);
        } catch (const std::exception& e) {
            return {false, "", std::string("Contract lookup failed: ") + e.what()};
        }

        // Long = buy to open; Short (CSP/CC) = sell to open
        bool is_short = (sig.strategy == "CSP" || sig.strategy == "CC");
        std::string side = is_short ? "sell" : "buy";

        json order = {
            {"symbol",          contract.occ_symbol},
            {"qty",             qty_contracts},
            {"side",            side},
            {"time_in_force",   "day"},
            {"position_effect", "open"}
        };

        // Use limit order with mid→fill retry. entry_price is the BS theoretical mid.
        // BUY: step toward ask; SELL (short premium): step toward bid.
        double limit_hint = (sig.entry_price > 0.0) ? sig.entry_price : 0.0;
        return submitLimitWithRetry(order, contract.occ_symbol, contract.occ_symbol,
                                    limit_hint, !is_short);
    }

    // ── Spread order (BULL_CALL_SPREAD, BEAR_PUT_SPREAD) ─────────────────────
    // Alpaca multi-leg: buy the primary leg, sell the wing leg.

    OrderResult routeSpread(const nox::options_signal::OptionsSignal& sig,
                             int qty_contracts) const
    {
        bool is_call  = (sig.strategy == "BULL_CALL_SPREAD");
        std::string opt_type = is_call ? "call" : "put";

        AlpacaContract buy_leg, sell_leg;
        try {
            buy_leg  = lookupContract(sig.underlying, sig.strike,  sig.expiry_date, opt_type);
            sell_leg = lookupContract(sig.underlying, sig.strike2, sig.expiry_date, opt_type);
        } catch (const std::exception& e) {
            return {false, "", std::string("Spread contract lookup failed: ") + e.what()};
        }

        json order = {
            {"type",          "market"},
            {"order_class",   "mleg"},
            {"time_in_force", "day"},
            {"qty",           qty_contracts},
            {"legs", json::array({
                {
                    {"symbol",          buy_leg.occ_symbol},
                    {"side",            "buy"},
                    {"ratio_qty",       1},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          sell_leg.occ_symbol},
                    {"side",            "sell"},
                    {"ratio_qty",       1},
                    {"position_effect", "open"}
                }
            })}
        };

        return submitOrder(order, buy_leg.occ_symbol + " / " + sell_leg.occ_symbol);
    }

    // ── Straddle (ATM call + ATM put, same strike) ────────────────────────────

    OrderResult routeStraddle(const nox::options_signal::OptionsSignal& sig,
                               int qty_contracts) const
    {
        AlpacaContract call_leg, put_leg;
        try {
            call_leg = lookupContract(sig.underlying, sig.strike, sig.expiry_date, "call");
            put_leg  = lookupContract(sig.underlying, sig.strike, sig.expiry_date, "put");
        } catch (const std::exception& e) {
            return {false, "", std::string("Straddle contract lookup failed: ") + e.what()};
        }

        json order = {
            {"type",          "market"},
            {"order_class",   "mleg"},
            {"time_in_force", "day"},
            {"qty",           qty_contracts},
            {"legs", json::array({
                {
                    {"symbol",          call_leg.occ_symbol},
                    {"side",            "buy"},
                    {"ratio_qty",       1},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          put_leg.occ_symbol},
                    {"side",            "buy"},
                    {"ratio_qty",       1},
                    {"position_effect", "open"}
                }
            })}
        };

        return submitOrder(order, call_leg.occ_symbol + " / " + put_leg.occ_symbol);
    }

    // ── Strangle (OTM call + OTM put, different strikes) ─────────────────────

    OrderResult routeStrangle(const nox::options_signal::OptionsSignal& sig,
                               int qty_contracts) const
    {
        // strike = call strike, strike2 = put strike
        AlpacaContract call_leg, put_leg;
        try {
            call_leg = lookupContract(sig.underlying, sig.strike,  sig.expiry_date, "call");
            put_leg  = lookupContract(sig.underlying, sig.strike2, sig.expiry_date, "put");
        } catch (const std::exception& e) {
            return {false, "", std::string("Strangle contract lookup failed: ") + e.what()};
        }

        json order = {
            {"type",          "market"},
            {"order_class",   "mleg"},
            {"time_in_force", "day"},
            {"qty",           qty_contracts},
            {"legs", json::array({
                {
                    {"symbol",          call_leg.occ_symbol},
                    {"side",            "buy"},
                    {"ratio_qty",       1},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          put_leg.occ_symbol},
                    {"side",            "buy"},
                    {"ratio_qty",       1},
                    {"position_effect", "open"}
                }
            })}
        };

        return submitOrder(order, call_leg.occ_symbol + " / " + put_leg.occ_symbol);
    }

    // ── Alpaca order submission ───────────────────────────────────────────────

    OrderResult submitOrder(const json& order_payload,
                            const std::string& label) const
    {
        auto cli = makeClient();

        auto res = cli.Post("/v2/orders",
                            authHeaders(),
                            order_payload.dump(),
                            "application/json");

        if (!res) {
            return {false, "", "Order POST timed out for " + label};
        }

        if (res->status == 200 || res->status == 201) {
            json resp    = json::parse(res->body);
            std::string id = resp.value("id", "UNKNOWN");
            return {true, id, "Order placed: " + label + " | ID=" + id};
        }

        // Parse Alpaca error body if present
        std::string err_detail;
        try {
            json err = json::parse(res->body);
            err_detail = err.value("message", res->body);
        } catch (...) {
            err_detail = res->body;
        }

        return {false, "", "Order rejected (" + std::to_string(res->status) + "): " + err_detail};
    }
};

} // namespace nox::options_router
