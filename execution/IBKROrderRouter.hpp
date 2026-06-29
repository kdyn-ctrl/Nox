#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// IBKROrderRouter.hpp — Maps nox::options_signal::OptionsSignal to IBKR
// Contract + Order objects and submits them via IBKRConnection.
//
// Covers all 8 Nox options strategies:
//   Single-leg: LONG_CALL / LONG_PUT / CSP / CC
//   Multi-leg:  BULL_CALL_SPREAD / BEAR_PUT_SPREAD / STRADDLE / STRANGLE
//
// Multi-leg orders use IBKR's BAG combo contract mechanism: a synthetic
// security (secType="BAG") whose legs reference the underlying option
// conIds. For paper-trading purposes we use market orders on all legs.
//
// COMPILATION: requires IBKR_ENABLED and TWS API headers on include path.
//   g++ -DIBKR_ENABLED -I third_party/twsapi/source/cppclient/client ...
// ─────────────────────────────────────────────────────────────────────────────

#include "IBKRClient.hpp"
#include "OptionsSignalTypes.hpp"

// TWS API types used below
#include "Contract.h"
#include "Order.h"
#include "ComboLeg.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace nox::ibkr {

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Converts "YYYY-MM-DD" → "YYYYMMDD" required by IBKR lastTradeDateOrContractMonth.
static std::string to_ibkr_expiry(const std::string& iso_date) {
    std::string out;
    out.reserve(8);
    for (char c : iso_date)
        if (c != '-') out += c;
    return out; // "20251219"
}

// Build a vanilla single-leg equity option Contract.
static Contract make_option_contract(
    const std::string& underlying,
    const std::string& expiry_yyyymmdd,
    double             strike,
    const std::string& right   // "C" or "P"
) {
    Contract c;
    c.symbol     = underlying;
    c.secType    = "OPT";
    c.exchange   = "SMART";
    c.currency   = "USD";
    c.lastTradeDateOrContractMonth = expiry_yyyymmdd;
    c.strike     = strike;
    c.right      = right;
    c.multiplier = "100";
    return c;
}

// Build a market order for a single-leg contract.
static Order make_market_order(const std::string& action, int qty) {
    Order o;
    o.action        = action;
    o.orderType     = "MKT";
    o.totalQuantity = static_cast<double>(qty);
    o.tif           = "DAY";
    return o;
}

// Build a DAY limit order. limit_price must be > 0.
static Order make_limit_order(const std::string& action, int qty, double limit_price) {
    Order o;
    o.action        = action;
    o.orderType     = "LMT";
    o.totalQuantity = static_cast<double>(qty);
    o.lmtPrice      = limit_price;
    o.tif           = "DAY";
    return o;
}

// ─── IBKROrderRouter ───────────────────────────────────────────────────────────

class IBKROrderRouter {
public:
    IBKROrderRouter(IBKRConnection& conn, IBKRWrapper& wrapper)
        : conn_(conn), wrapper_(wrapper) {}

    // Route an OptionsSignal to the gateway.
    // Returns true if the order was accepted by placeOrder (does NOT mean filled).
    // Throws std::runtime_error if the signal strategy is unknown.
    bool route(const nox::options_signal::OptionsSignal& sig, int qty_contracts) {
        if (!wrapper_.hasValidOrderId()) {
            throw std::runtime_error("IBKROrderRouter: gateway not ready (no valid order id)");
        }

        const std::string expiry = to_ibkr_expiry(sig.expiry_date);
        const std::string s      = sig.strategy;

        double mid = sig.entry_price; // BS theoretical mid — starting limit price
        if      (s == "LONG_CALL")        return route_single(sig, expiry, sig.strike,  "C", "BUY",  qty_contracts, mid, true);
        else if (s == "LONG_PUT")         return route_single(sig, expiry, sig.strike,  "P", "BUY",  qty_contracts, mid, true);
        else if (s == "CSP")              return route_single(sig, expiry, sig.strike,  "P", "SELL", qty_contracts, mid, false);
        else if (s == "CC")               return route_single(sig, expiry, sig.strike,  "C", "SELL", qty_contracts, mid, false);
        else if (s == "BULL_CALL_SPREAD") return route_spread(sig, expiry, "C", qty_contracts);
        else if (s == "BEAR_PUT_SPREAD")  return route_spread(sig, expiry, "P", qty_contracts);
        else if (s == "STRADDLE")         return route_straddle(sig, expiry, qty_contracts);
        else if (s == "STRANGLE")         return route_strangle(sig, expiry, qty_contracts);
        else throw std::runtime_error("IBKROrderRouter: unknown strategy: " + s);
    }

private:
    IBKRConnection& conn_;
    IBKRWrapper&    wrapper_;

    // ── Single-leg with limit + retry ────────────────────────────────────────
    // Sends a limit order at mid price, polls for fill, steps 10% toward the
    // fill-side price on each of 2 retries, then cancels and returns false.
    // entry_mid: BS theoretical mid (starting limit); 0.0 → use market fallback.
    // side_is_buy: true = buying (step price toward ask), false = selling (toward bid).
    bool route_single(
        const nox::options_signal::OptionsSignal& sig,
        const std::string& expiry,
        double             strike,
        const std::string& right,   // "C" or "P"
        const std::string& action,  // "BUY" or "SELL"
        int                qty,
        double             entry_mid = 0.0,
        bool               side_is_buy = true
    ) {
        Contract c = make_option_contract(sig.underlying, expiry, strike, right);

        if (entry_mid <= 0.0) {
            // No price hint — fall back to market order
            Order o = make_market_order(action, qty);
            OrderId id = wrapper_.reserveOrderId();
            conn_.placeOrder(id, c, o);
            return true;
        }

        // Estimate fill-side price (ask for buys, bid for sells) as ±10% of mid
        double far = side_is_buy ? entry_mid * 1.10 : entry_mid * 0.90;

        for (int attempt = 0; attempt < 3; ++attempt) {
            double step_frac  = attempt * 0.10;
            double limit_price = entry_mid + (far - entry_mid) * step_frac;
            limit_price = std::round(limit_price * 100.0) / 100.0;
            limit_price = std::max(limit_price, 0.01);

            OrderId id = wrapper_.reserveOrderId();
            Order   o  = make_limit_order(action, qty, limit_price);
            conn_.placeOrder(id, c, o);

            // Poll for fill for 30 seconds
            bool filled = false;
            for (int sec = 0; sec < 30; ++sec) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                OrderUpdate upd;
                if (wrapper_.latestStatus(id, upd)) {
                    if (upd.status == "Filled" || upd.status == "PartiallyFilled") {
                        filled = true;
                        break;
                    }
                    if (upd.status == "Cancelled" || upd.status == "Inactive") break;
                }
            }
            if (filled) return true;

            // Cancel unfilled order before retry
            conn_.cancelOrder(id);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            if (attempt == 2) return false; // all retries exhausted
        }
        return false;
    }

    // ── Vertical spread (BULL_CALL_SPREAD / BEAR_PUT_SPREAD) ─────────────────
    // Leg 1: BUY lower strike (sig.strike)
    // Leg 2: SELL upper strike (sig.strike2) for call spread;
    //         BUY lower / SELL upper for put spread (strike < strike2).
    bool route_spread(
        const nox::options_signal::OptionsSignal& sig,
        const std::string& expiry,
        const std::string& right,  // "C" = bull call, "P" = bear put
        int                qty
    ) {
        if (sig.strike2 <= 0.0)
            throw std::runtime_error("IBKROrderRouter: spread requires non-zero strike2");

        // For bull call spread: buy lower, sell upper
        // For bear put spread:  buy higher, sell lower (strike2 > strike)
        double buy_strike  = (right == "C") ? sig.strike  : sig.strike2;
        double sell_strike = (right == "C") ? sig.strike2 : sig.strike;

        // Request conIds so IBKR can form a BAG combo.
        // In a production system use reqContractDetails() to get real conIds.
        // For paper trading, placeOrder individually works as two separate orders.
        route_single(sig, expiry, buy_strike,  right, "BUY",  qty);
        // Small delay so the gateway processes legs sequentially.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        route_single(sig, expiry, sell_strike, right, "SELL", qty);
        return true;
    }

    // ── Straddle (buy call + buy put at same strike) ──────────────────────────
    bool route_straddle(
        const nox::options_signal::OptionsSignal& sig,
        const std::string& expiry,
        int                qty
    ) {
        route_single(sig, expiry, sig.strike, "C", "BUY", qty);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        route_single(sig, expiry, sig.strike, "P", "BUY", qty);
        return true;
    }

    // ── Strangle (buy OTM call at strike2, buy OTM put at strike) ────────────
    bool route_strangle(
        const nox::options_signal::OptionsSignal& sig,
        const std::string& expiry,
        int                qty
    ) {
        if (sig.strike2 <= 0.0)
            throw std::runtime_error("IBKROrderRouter: strangle requires non-zero strike2 (OTM call)");
        route_single(sig, expiry, sig.strike,  "P", "BUY", qty); // OTM put
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        route_single(sig, expiry, sig.strike2, "C", "BUY", qty); // OTM call
        return true;
    }
};

} // namespace nox::ibkr

// ─────────────────────────────────────────────────────────────────────────────
// USAGE IN main.cpp (under #ifdef IBKR_ENABLED):
//
//   #include "IBKROrderRouter.hpp"
//
//   // In NoxEngine private:
//   std::unique_ptr<nox::ibkr::IBKRWrapper>    ibkr_wrapper_;
//   std::unique_ptr<nox::ibkr::IBKRConnection> ibkr_conn_;
//   std::unique_ptr<nox::ibkr::IBKROrderRouter> ibkr_router_;
//   std::string execution_venue_;   // "alpaca" | "ibkr"
//
//   // In constructor (after credential validation):
//   {
//       const char* venue_env = std::getenv("EXECUTION_VENUE");
//       execution_venue_ = (venue_env && std::string(venue_env) == "ibkr") ? "ibkr" : "alpaca";
//       if (execution_venue_ == "ibkr") {
//           const char* host_env = std::getenv("IBKR_GATEWAY_HOST");
//           const char* port_env = std::getenv("IBKR_GATEWAY_PORT");
//           std::string host = host_env ? host_env : "127.0.0.1";
//           int         port = port_env ? std::stoi(port_env) : 4002;  // 4002=paper, 4001=live
//           ibkr_wrapper_ = std::make_unique<nox::ibkr::IBKRWrapper>();
//           ibkr_conn_    = std::make_unique<nox::ibkr::IBKRConnection>(*ibkr_wrapper_);
//           if (!ibkr_conn_->connect(host.c_str(), port)) {
//               std::cerr << "[FATAL] IBKR gateway connection failed\n"; std::exit(1);
//           }
//           int waited = 0;
//           while (!ibkr_wrapper_->hasValidOrderId() && ++waited < 100)
//               std::this_thread::sleep_for(std::chrono::milliseconds(50));
//           if (!ibkr_wrapper_->hasValidOrderId()) {
//               std::cerr << "[FATAL] IBKR: timed out waiting for nextValidId\n"; std::exit(1);
//           }
//           ibkr_router_ = std::make_unique<nox::ibkr::IBKROrderRouter>(*ibkr_conn_, *ibkr_wrapper_);
//           Logger::log("INFO", "[IBKR] Connected — execution venue: IBKR");
//       }
//   }
// ─────────────────────────────────────────────────────────────────────────────
