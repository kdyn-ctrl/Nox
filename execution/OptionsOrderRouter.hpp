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

// Disposition disambiguates the failure/success shape so callers can react
// correctly. The crux of the ghost-fill fix: a POST timeout (Timeout) means we
// DON'T KNOW the broker's state — it must NOT be collapsed into the same bucket
// as an explicit broker Rejected. Callers map Timeout → ledger 'unknown' (defer
// to reconciliation) and Rejected → ledger 'failed'.
//
// Note: Accepted means Alpaca returned 200/201 with the order in
// accepted/new/pending_new — it is NOT proof of a fill. Reconciliation upgrades
// the ledger row to 'filled' only on broker-confirmed fill status.
enum class OrderDisposition { Accepted, Rejected, Timeout, ParseError };

struct OrderResult {
    bool             success     = false;
    std::string      order_id;
    std::string      message;
    OrderDisposition disposition = OrderDisposition::Rejected;
};

// ─── Broker order status (reconciliation lookup) ──────────────────────────────

struct BrokerOrderStatus {
    bool        found     = false;  // true only when the broker returned a record
    bool        reachable = false;  // false → network/parse error (do NOT infer 'failed')
    std::string status;             // Alpaca: new/accepted/pending_new/filled/partially_filled/canceled/rejected/expired
    std::string broker_order_id;    // Alpaca's own order id
    double      filled_qty = 0.0;   // contracts actually filled — lets reconciliation
                                    // distinguish a truly-unfilled canceled/rejected
                                    // order (safe to reverse an optimistic booking)
                                    // from a partial fill (must NOT be reversed).
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

    // Naked-options approval: Alpaca's /v2/account exposes the account's
    // options trading level (0-3). Level 3 ("uncovered") is required to sell
    // an unhedged strangle — anything less and the broker rejects the order,
    // but we check first so the failure is a clear pre-flight message instead
    // of an opaque 403 mid-execution. Fails CLOSED (false) on any read error —
    // mirrors validateCCPosition's "prove it, don't assume it" stance, because
    // the failure mode here is naked short options risk, not a missed trade.
    bool validateNakedOptionsApproval() const {
        try {
            auto cli = makeClient();
            auto res = cli.Get("/v2/account", authHeaders());
            if (!res || res->status != 200) return false;
            json body = json::parse(res->body);
            int level = body.value("options_trading_level", 0);
            return level >= 3;
        } catch (...) {
            return false;
        }
    }

    // Main entry point. Takes a fully assembled OptionsSignal and routes it.
    // Returns an OrderResult — caller logs and Telegrams based on outcome.
    // client_oid: the caller-generated client_order_id, written to the ledger
    // BEFORE this call. Threaded down to submitOrder for broker-side idempotency
    // and later reconciliation lookup. Empty string → no client id (exit path).
    OrderResult route(const nox::options_signal::OptionsSignal& sig,
                      int qty_contracts = 1,
                      const std::string& client_oid = "") {
        const std::string& strategy = sig.strategy;

        // Single-leg strategies
        if (strategy == "LONG_CALL" || strategy == "LONG_PUT" ||
            strategy == "CSP"       || strategy == "CC")
        {
            return routeSingleLeg(sig, qty_contracts, client_oid);
        }

        // Spreads — two legs, same underlying, same expiry
        if (strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD") {
            return routeSpread(sig, qty_contracts, client_oid);
        }

        // Straddle: ATM call + ATM put, same strike, same expiry
        if (strategy == "STRADDLE") {
            return routeStraddle(sig, qty_contracts, client_oid);
        }

        // Strangle: OTM call + OTM put
        if (strategy == "STRANGLE") {
            return routeStrangle(sig, qty_contracts, client_oid);
        }

        // Reverse iron condor: bull call spread + bear put spread, 4 legs
        if (strategy == "REVERSE_IRON_CONDOR") {
            return routeReverseIronCondor(sig, qty_contracts, client_oid);
        }

        return {false, "", "Unknown strategy: " + strategy, OrderDisposition::Rejected};
    }

    // Reconciliation lookup — fetch a single order by the client_order_id we
    // assigned. Used by the engine's reconcile poll to resolve 'pending'/'unknown'
    // ledger rows against broker truth. Never throws; distinguishes "reachable but
    // no such order" (found=false, reachable=true → after grace, order never
    // landed) from "couldn't reach broker" (reachable=false → leave row untouched).
    BrokerOrderStatus getOrderByClientId(const std::string& client_oid) const {
        BrokerOrderStatus out;
        try {
            auto cli = makeClient();
            std::string path = "/v2/orders:by_client_order_id?client_order_id=" + client_oid;
            auto res = cli.Get(path.c_str(), authHeaders());
            if (!res) return out;                       // reachable stays false
            if (res->status == 404) {                   // broker reached, no such order
                out.reachable = true;
                return out;
            }
            if (res->status != 200) return out;         // transient/other → treat as unreachable
            json body = json::parse(res->body);
            out.reachable       = true;
            out.found           = true;
            out.status          = body.value("status", "");
            out.broker_order_id = body.value("id", "");
            // Alpaca reports filled_qty as a string ("0", "5", ...); parse defensively.
            try { out.filled_qty = std::stod(body.value("filled_qty", "0")); }
            catch (...) { out.filled_qty = 0.0; }
        } catch (...) {
            // parse/other error → leave reachable=false so caller does not self-inflict 'failed'
            return {};
        }
        return out;
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

    // Closes a multi-leg spread/straddle/strangle/reverse-iron-condor by
    // submitting one mleg order with every leg's side reversed from its entry
    // side (a leg bought to open is sold to close, and vice versa) and
    // position_effect="close" — the exit-side mirror of routeSpread() etc.
    // `legs` carries each leg's ORIGINAL entry side; this function flips it.
    struct CloseLegSpec {
        std::string option_type; // "call" or "put"
        double      strike = 0.0;
        std::string entry_side;  // "buy" or "sell", as originally submitted
        int         ratio_qty = 1; // this leg's per-unit qty; equal-ratio spreads leave this at 1
    };

    OrderResult closeSpreadPosition(const std::string& underlying,
                                    const std::string& expiry_date,
                                    const std::vector<CloseLegSpec>& legs,
                                    int qty_contracts) const
    {
        json order_legs = json::array();
        std::string label;
        for (const auto& leg : legs) {
            AlpacaContract c;
            try {
                c = lookupContractImpl(underlying, leg.strike, expiry_date, leg.option_type);
            } catch (const std::exception& e) {
                return {false, "", std::string("Spread close contract lookup failed: ") + e.what(),
                        OrderDisposition::Rejected};
            }
            if (!c.valid) {
                return {false, "", "Spread close leg not found: " + leg.option_type +
                        " " + std::to_string(leg.strike), OrderDisposition::Rejected};
            }
            std::string close_side = (leg.entry_side == "buy") ? "sell" : "buy";
            order_legs.push_back({
                {"symbol",          c.occ_symbol},
                {"side",            close_side},
                {"ratio_qty",       leg.ratio_qty},
                {"position_effect", "close"}
            });
            if (!label.empty()) label += " / ";
            label += c.occ_symbol;
        }

        json order = {
            {"type",          "market"},
            {"order_class",   "mleg"},
            {"time_in_force", "day"},
            {"qty",           qty_contracts},
            {"legs",          order_legs}
        };
        // Exit path: not ledger-tracked (mirrors closePositionImpl) → no client_oid.
        return submitOrder(order, "CLOSE " + label);
    }

    // Broker-truth check: does the account currently hold ANY open option
    // position (single- or multi-leg — a spread's legs list individually) on
    // this underlying? This is the authoritative pre-order gate: a spread's
    // legs never land in the local open_positions table (single-leg only),
    // and even single-leg local state can drift from the broker (a manual
    // close in the Alpaca/Robinhood UI, a reconciliation lag). Sets
    // `reachable=false` on any network/parse failure so the caller can fall
    // back to the sqlite ledger instead of misreading "no positions."
    bool hasOpenOptionPosition(const std::string& underlying, bool& reachable) const {
        reachable = false;
        try {
            auto cli = makeClient();
            auto res = cli.Get("/v2/positions", authHeaders());
            if (!res || res->status != 200) return false;
            reachable = true;
            json body = json::parse(res->body);
            if (!body.is_array()) return false;
            for (const auto& pos : body) {
                if (pos.value("asset_class", "") != "us_option") continue;
                std::string symbol = pos.value("symbol", "");
                if (symbol.size() <= underlying.size()) continue;
                if (symbol.compare(0, underlying.size(), underlying) != 0) continue;
                // OCC symbols follow ROOT + YYMMDD..., so the char right after
                // the root must be a digit — guards "AA" from matching "AAPL...".
                if (!std::isdigit(static_cast<unsigned char>(symbol[underlying.size()]))) continue;
                return true;
            }
            return false;
        } catch (...) {
            reachable = false;
            return false;
        }
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
            // Alpaca returns strike_price as a string ("300.00"), not a number.
            double s = 0.0;
            if (c.contains("strike_price")) {
                if (c["strike_price"].is_number())
                    s = c["strike_price"].get<double>();
                else if (c["strike_price"].is_string())
                    try { s = std::stod(c["strike_price"].get<std::string>()); } catch (...) {}
            }
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

    // Closes an open option position by submitting a market order.
    // is_short_premium=true: BUY to close (short position); false: SELL to close (long position).
    // Alpaca options orders use the side (buy/sell) to implicitly close; position_effect is not used.
    OrderResult closePositionImpl(const std::string& occ_symbol, int quantity, bool is_short_premium) const {
        std::string side = is_short_premium ? "buy" : "sell"; // Buy to close short, sell to close long
        // Without position_intent, Alpaca can't distinguish this close from a
        // naked buy_to_open/sell_to_open and rejects it (403 "not eligible to
        // trade uncovered option contracts") on accounts without naked-option
        // approval — mirrors the per-leg "position_effect":"close" that
        // closeSpreadPosition() already sets below.
        std::string position_intent = is_short_premium ? "buy_to_close" : "sell_to_close";
        json order = {
            {"symbol",          occ_symbol},
            {"qty",             quantity},
            {"side",            side},
            {"type",            "market"},
            {"time_in_force",   "day"},
            {"position_intent", position_intent}
        };
        // Exit path: not ledger-tracked in Phase 1 → no client_oid.
        return submitOrder(order, "CLOSE " + occ_symbol);
    }


    // ── Single-leg order (LONG_CALL, LONG_PUT, CSP, CC) ──────────────────────

    OrderResult routeSingleLeg(const nox::options_signal::OptionsSignal& sig,
                                int qty_contracts,
                                const std::string& client_oid) const
    {
        bool is_call = (sig.option_type == nox::options::OptionType::Call);
        std::string opt_type = is_call ? "call" : "put";

        AlpacaContract contract;
        try {
            contract = lookupContractImpl(sig.underlying, sig.strike,
                                         sig.expiry_date, opt_type);
        } catch (const std::exception& e) {
            return {false, "", std::string("Contract lookup failed: ") + e.what(),
                    OrderDisposition::Rejected};
        }

        // Long = buy to open; Short (CSP/CC) = sell to open
        bool is_short = (sig.strategy == "CSP" || sig.strategy == "CC");
        std::string side = is_short ? "sell" : "buy";

        json order = {
            {"symbol",          contract.occ_symbol},
            {"qty",             qty_contracts},
            {"side",            side},
            {"type",            "market"},
            {"time_in_force",   "day"},
            {"position_effect", "open"}
        };

        return submitOrder(order, contract.occ_symbol, client_oid);
    }

    // ── Spread order (BULL_CALL_SPREAD, BEAR_PUT_SPREAD) ─────────────────────
    // Alpaca multi-leg: buy the primary leg, sell the wing leg.

    OrderResult routeSpread(const nox::options_signal::OptionsSignal& sig,
                             int qty_contracts,
                             const std::string& client_oid) const
    {
        bool is_call  = (sig.strategy == "BULL_CALL_SPREAD");
        std::string opt_type = is_call ? "call" : "put";

        AlpacaContract buy_leg, sell_leg;
        try {
            buy_leg  = lookupContract(sig.underlying, sig.strike,  sig.expiry_date, opt_type);
            sell_leg = lookupContract(sig.underlying, sig.strike2, sig.expiry_date, opt_type);
        } catch (const std::exception& e) {
            return {false, "", std::string("Spread contract lookup failed: ") + e.what(),
                    OrderDisposition::Rejected};
        }

        // Validate that both legs were found successfully
        if (!buy_leg.valid || !sell_leg.valid) {
            return {false, "", "Spread legs not found: buy_leg=" +
                    std::string(buy_leg.valid ? "OK" : "MISSING") +
                    " sell_leg=" + std::string(sell_leg.valid ? "OK" : "MISSING"),
                    OrderDisposition::Rejected};
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
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          sell_leg.occ_symbol},
                    {"side",            "sell"},
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                }
            })}
        };

        return submitOrder(order, buy_leg.occ_symbol + " / " + sell_leg.occ_symbol, client_oid);
    }

    // ── Straddle (ATM call + ATM put, same strike) ────────────────────────────

    OrderResult routeStraddle(const nox::options_signal::OptionsSignal& sig,
                               int qty_contracts,
                               const std::string& client_oid) const
    {
        AlpacaContract call_leg, put_leg;
        try {
            call_leg = lookupContract(sig.underlying, sig.strike, sig.expiry_date, "call");
            put_leg  = lookupContract(sig.underlying, sig.strike, sig.expiry_date, "put");
        } catch (const std::exception& e) {
            return {false, "", std::string("Straddle contract lookup failed: ") + e.what(),
                    OrderDisposition::Rejected};
        }

        // Validate that both legs were found successfully
        if (!call_leg.valid || !put_leg.valid) {
            return {false, "", "Straddle legs not found: call_leg=" +
                    std::string(call_leg.valid ? "OK" : "MISSING") +
                    " put_leg=" + std::string(put_leg.valid ? "OK" : "MISSING"),
                    OrderDisposition::Rejected};
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
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          put_leg.occ_symbol},
                    {"side",            "buy"},
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                }
            })}
        };

        return submitOrder(order, call_leg.occ_symbol + " / " + put_leg.occ_symbol, client_oid);
    }

    // ── Strangle (OTM call + OTM put, different strikes) ─────────────────────

    OrderResult routeStrangle(const nox::options_signal::OptionsSignal& sig,
                               int qty_contracts,
                               const std::string& client_oid) const
    {
        // strike = call strike, strike2 = put strike
        AlpacaContract call_leg, put_leg;
        try {
            call_leg = lookupContract(sig.underlying, sig.strike,  sig.expiry_date, "call");
            put_leg  = lookupContract(sig.underlying, sig.strike2, sig.expiry_date, "put");
        } catch (const std::exception& e) {
            return {false, "", std::string("Strangle contract lookup failed: ") + e.what(),
                    OrderDisposition::Rejected};
        }

        // Validate that both legs were found successfully
        if (!call_leg.valid || !put_leg.valid) {
            return {false, "", "Strangle legs not found: call_leg=" +
                    std::string(call_leg.valid ? "OK" : "MISSING") +
                    " put_leg=" + std::string(put_leg.valid ? "OK" : "MISSING"),
                    OrderDisposition::Rejected};
        }

        // Short strangle: sells both legs for a net credit — the income thesis
        // selectStrategy() actually chose this under (vol RICH → prefer_sell).
        // Naked short both directions: uncapped upside risk, large downside risk
        // to the put strike. See executeSignal()'s naked-options approval check.
        json order = {
            {"type",          "market"},
            {"order_class",   "mleg"},
            {"time_in_force", "day"},
            {"qty",           qty_contracts},
            {"legs", json::array({
                {
                    {"symbol",          call_leg.occ_symbol},
                    {"side",            "sell"},
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          put_leg.occ_symbol},
                    {"side",            "sell"},
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                }
            })}
        };

        return submitOrder(order, call_leg.occ_symbol + " / " + put_leg.occ_symbol, client_oid);
    }

    // ── Reverse Iron Condor (long call + short call + long put + short put) ──
    // strike = long call, strike2 = short call (far OTM), strike3 = long put,
    // strike4 = short put (far OTM). Alpaca's mleg order_class supports up to
    // 4 legs, same JSON shape as the 2-leg strategies above, just extended.

    OrderResult routeReverseIronCondor(const nox::options_signal::OptionsSignal& sig,
                                        int qty_contracts,
                                        const std::string& client_oid) const
    {
        AlpacaContract long_call, short_call, long_put, short_put;
        try {
            long_call  = lookupContract(sig.underlying, sig.strike,  sig.expiry_date, "call");
            short_call = lookupContract(sig.underlying, sig.strike2, sig.expiry_date, "call");
            long_put   = lookupContract(sig.underlying, sig.strike3, sig.expiry_date, "put");
            short_put  = lookupContract(sig.underlying, sig.strike4, sig.expiry_date, "put");
        } catch (const std::exception& e) {
            return {false, "", std::string("Reverse iron condor contract lookup failed: ") + e.what(),
                    OrderDisposition::Rejected};
        }

        if (!long_call.valid || !short_call.valid || !long_put.valid || !short_put.valid) {
            return {false, "", "Reverse iron condor legs not found: long_call=" +
                    std::string(long_call.valid ? "OK" : "MISSING") +
                    " short_call=" + std::string(short_call.valid ? "OK" : "MISSING") +
                    " long_put=" + std::string(long_put.valid ? "OK" : "MISSING") +
                    " short_put=" + std::string(short_put.valid ? "OK" : "MISSING"),
                    OrderDisposition::Rejected};
        }

        json order = {
            {"type",          "market"},
            {"order_class",   "mleg"},
            {"time_in_force", "day"},
            {"qty",           qty_contracts},
            {"legs", json::array({
                {
                    {"symbol",          long_call.occ_symbol},
                    {"side",            "buy"},
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          short_call.occ_symbol},
                    {"side",            "sell"},
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          long_put.occ_symbol},
                    {"side",            "buy"},
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                },
                {
                    {"symbol",          short_put.occ_symbol},
                    {"side",            "sell"},
                    {"ratio_qty",       1.0},
                    {"position_effect", "open"}
                }
            })}
        };

        return submitOrder(order,
            long_call.occ_symbol + " / " + short_call.occ_symbol + " / " +
            long_put.occ_symbol + " / " + short_put.occ_symbol, client_oid);
    }

    // ── Alpaca order submission ───────────────────────────────────────────────

    OrderResult submitOrder(const json& order_payload,
                            const std::string& label,
                            const std::string& client_oid = "") const
    {
        auto cli = makeClient();

        // Inject our client_order_id (single top-level field — works for both
        // single-leg and mleg parent orders) so the broker dedupes retries and
        // reconciliation can look this order up after a timeout.
        json payload = order_payload;
        if (!client_oid.empty())
            payload["client_order_id"] = client_oid;

        auto res = cli.Post("/v2/orders",
                            authHeaders(),
                            payload.dump(),
                            "application/json");

        if (!res) {
            // GHOST-FILL FIX: a timeout is NOT a rejection. We do not know the
            // broker's state — the order may have filled. Flag it Timeout so the
            // caller records the ledger row as 'unknown' and lets reconciliation
            // (real broker evidence) decide, rather than guessing 'failed'.
            return {false, "", "Order POST timed out for " + label, OrderDisposition::Timeout};
        }

        if (res->status == 200 || res->status == 201) {
            try {
                json resp    = json::parse(res->body);
                std::string id = resp.value("id", "UNKNOWN");
                return {true, id, "Order placed: " + label + " | ID=" + id,
                        OrderDisposition::Accepted};
            } catch (const std::exception& e) {
                // 2xx but unparseable — the order likely landed. Treat as Timeout
                // semantics (unknown) so reconciliation confirms, not 'failed'.
                return {false, "", "Failed to parse success response: " + std::string(e.what()),
                        OrderDisposition::ParseError};
            }
        }

        std::cerr << "[ORDER_ROUTER] Rejected order payload for " << label << ": "
                  << payload.dump() << " | raw response (" << res->status << "): "
                  << res->body << std::endl;

        // Parse Alpaca error body if present
        std::string err_detail;
        try {
            json err = json::parse(res->body);
            // Try to extract message field safely
            if (err.contains("message")) {
                if (err["message"].is_string()) {
                    err_detail = err["message"].get<std::string>();
                } else {
                    err_detail = err["message"].dump();
                }
            } else {
                err_detail = err.dump();
            }
        } catch (const json::parse_error& pe) {
            err_detail = "JSON parse error in Alpaca response: " + std::string(pe.what());
        } catch (const json::type_error& te) {
            err_detail = "JSON type error in Alpaca response: " + std::string(te.what());
        } catch (const std::exception& e) {
            err_detail = res->body;
        }

        return {false, "", "Order rejected (" + std::to_string(res->status) + "): " + err_detail,
                OrderDisposition::Rejected};
    }
};

} // namespace nox::options_router
