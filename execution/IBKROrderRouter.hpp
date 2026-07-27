#pragma once

// IBKROrderRouter — Phase 3: IBKR combo/BAG multi-leg order support.
//
// CLAUDE.md flagged this as the prerequisite before any IBKR multi-leg
// options strategy goes live: "IBKR (execution/IBKRClient.hpp/.cpp) has no
// combo/BAG multi-leg order support — single-leg only." This mirrors
// OptionsOrderRouter's route()/routeSingleLeg/routeSpread/routeStraddle/
// routeStrangle split for the same 8 strategies (LONG_CALL, LONG_PUT, CSP,
// CC, BULL_CALL_SPREAD, BEAR_PUT_SPREAD, STRADDLE, STRANGLE), but targets
// IBKR's native combo order model instead of Alpaca's `order_class: "mleg"`:
//
//   - Single-leg strategies place a plain OPT contract order.
//   - Multi-leg strategies place ONE order against a synthetic secType="BAG"
//     Contract whose comboLegs vector carries each leg's OWN action
//     (BUY/SELL) — this is what makes a BAG order atomic at IBKR the same
//     way Alpaca's mleg order_class is atomic (fill all legs or none), even
//     though the wire protocol is completely different.
//
// IBKR combo legs are identified by conId, not by symbol/strike/expiry —
// unlike Alpaca's OCC-symbol lookup, every leg must be qualified via
// reqContractDetails (IBKRConnection::qualifyContract, added alongside this
// file) before a ComboLeg can be built. That qualification call blocks the
// calling thread briefly; never call route() from the message-pump thread.
//
// NOT validated against a live IB Gateway or the real TWS API vendor source
// (see IBKR_MIGRATION.md) — this is unit-testable in shape (contract/order
// construction) but the end-to-end broker round-trip is the "still open"
// item Phase 3 flags, matching how Phase 1/2 shipped mock-tested infra with
// live/paper validation deferred.

#include "IBKRClient.hpp"
#include "OptionsSignalTypes.hpp"
#include "ComboLeg.h"

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace nox::ibkr {

enum class IBKROrderDisposition { Accepted, Rejected, Timeout };

struct IBKROrderResult {
    bool                  success     = false;
    std::string           order_id;
    std::string           message;
    IBKROrderDisposition  disposition = IBKROrderDisposition::Rejected;
};

class IBKROrderRouter {
public:
    IBKROrderRouter(IBKRConnection& conn, IBKRWrapper& wrapper)
        : conn_(conn), wrapper_(wrapper) {}

    // Main entry point — mirrors OptionsOrderRouter::route()'s strategy
    // dispatch exactly so callers (main.cpp) can swap venues without
    // touching the OptionsSignal → order translation logic itself.
    IBKROrderResult route(const nox::options_signal::OptionsSignal& sig, int qty_contracts) {
        const std::string& strategy = sig.strategy;

        if (strategy == "LONG_CALL" || strategy == "LONG_PUT" ||
            strategy == "CSP"       || strategy == "CC") {
            return routeSingleLeg(sig, qty_contracts);
        }
        if (strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD") {
            return routeSpread(sig, qty_contracts);
        }
        if (strategy == "STRADDLE") {
            return routeStraddle(sig, qty_contracts);
        }
        if (strategy == "STRANGLE") {
            return routeStrangle(sig, qty_contracts);
        }
        if (strategy == "REVERSE_IRON_CONDOR") {
            return routeReverseIronCondor(sig, qty_contracts);
        }
        return {false, "", "Unknown strategy: " + strategy, IBKROrderDisposition::Rejected};
    }

private:
    IBKRConnection& conn_;
    IBKRWrapper&    wrapper_;

    // "YYYY-MM-DD" (Alpaca/OptionsSignal convention) → "YYYYMMDD" (IBKR's
    // lastTradeDateOrContractMonth convention).
    static std::string toIbkrExpiry(const std::string& expiry_date) {
        std::string out;
        out.reserve(8);
        for (char c : expiry_date) if (c != '-') out.push_back(c);
        return out;
    }

    static Contract makeOptionContract(const std::string& underlying, double strike,
                                       const std::string& expiry_date, bool is_call) {
        Contract c;
        c.symbol   = underlying;
        c.secType  = "OPT";
        c.exchange = "SMART";
        c.currency = "USD";
        c.lastTradeDateOrContractMonth = toIbkrExpiry(expiry_date);
        c.strike   = strike;
        c.right    = is_call ? "C" : "P";
        c.multiplier = "100";
        return c;
    }

    // Qualifies one option leg and returns its conId, or 0 on failure — the
    // caller must never build a ComboLeg (or place a single-leg order) with
    // an unqualified conId.
    int qualifyLeg(const std::string& underlying, double strike,
                   const std::string& expiry_date, bool is_call) {
        Contract c = makeOptionContract(underlying, strike, expiry_date, is_call);
        return conn_.qualifyContract(c);
    }

    static ComboLegSPtr makeComboLeg(int conId, const std::string& action) {
        auto leg = std::make_shared<ComboLeg>();
        leg->conId   = conId;
        leg->ratio   = 1;
        leg->action  = action; // "BUY" / "SELL" — per-leg direction is what
                                // makes the BAG order carry a net debit/credit
        leg->exchange = "SMART";
        return leg;
    }

    // Best-effort early read of the broker's ack: placeOrder is async, so
    // this only catches a FAST reject (bad contract, margin, etc.) within a
    // short poll window — same "Accepted is not proof of a fill" contract
    // OptionsOrderRouter::submitOrder documents. Absence of a status update
    // within the window is treated as Accepted, not Timeout — IBKR often
    // doesn't emit an orderStatus callback at all until the order actually
    // works, unlike a REST call that either responds or times out.
    IBKROrderResult pollEarlyResult(OrderId oid, const std::string& label) {
        for (int i = 0; i < 20; ++i) { // ~1s at 50ms per poll
            OrderUpdate u;
            if (wrapper_.latestStatus(oid, u)) {
                if (u.status == "Rejected" || u.status == "Cancelled" || u.status == "ApiCancelled") {
                    return {false, std::to_string(oid),
                            "IBKR " + u.status + ": " + label, IBKROrderDisposition::Rejected};
                }
                break; // any other status (PreSubmitted/Submitted/Filled/...) = accepted
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return {true, std::to_string(oid), label, IBKROrderDisposition::Accepted};
    }

    // ── Single-leg (LONG_CALL, LONG_PUT, CSP, CC) ─────────────────────────────
    IBKROrderResult routeSingleLeg(const nox::options_signal::OptionsSignal& sig, int qty) {
        bool is_call = (sig.option_type == nox::options::OptionType::Call);
        int conId = qualifyLeg(sig.underlying, sig.strike, sig.expiry_date, is_call);
        if (conId == 0) {
            return {false, "", "Contract qualification failed/timed out for " + sig.underlying,
                    IBKROrderDisposition::Rejected};
        }

        Contract c = makeOptionContract(sig.underlying, sig.strike, sig.expiry_date, is_call);
        c.conId = conId;

        bool is_short = (sig.strategy == "CSP" || sig.strategy == "CC");

        Order order;
        order.action        = is_short ? "SELL" : "BUY";
        order.orderType     = "MKT";
        order.totalQuantity = static_cast<double>(qty);
        order.tif           = "DAY";

        OrderId oid = wrapper_.reserveOrderId();
        conn_.placeOrder(oid, c, order);
        return pollEarlyResult(oid, sig.underlying + " " + sig.strategy);
    }

    // Shared two-leg BAG builder for spread/straddle/strangle — the only
    // difference between the three strategies is which strikes/types/
    // actions each leg gets, so the combo-construction plumbing is common.
    IBKROrderResult routeTwoLegCombo(const nox::options_signal::OptionsSignal& sig, int qty,
                                     double strike1, bool call1, const std::string& action1,
                                     double strike2, bool call2, const std::string& action2,
                                     const std::string& label) {
        int conId1 = qualifyLeg(sig.underlying, strike1, sig.expiry_date, call1);
        int conId2 = qualifyLeg(sig.underlying, strike2, sig.expiry_date, call2);
        if (conId1 == 0 || conId2 == 0) {
            return {false, "", "Combo leg qualification failed for " + sig.underlying +
                    " (leg1=" + std::to_string(conId1) + " leg2=" + std::to_string(conId2) + ")",
                    IBKROrderDisposition::Rejected};
        }

        Contract bag;
        bag.symbol   = sig.underlying;
        bag.secType  = "BAG";
        bag.currency = "USD";
        bag.exchange = "SMART";
        bag.comboLegs = std::make_shared<std::vector<ComboLegSPtr>>();
        bag.comboLegs->push_back(makeComboLeg(conId1, action1));
        bag.comboLegs->push_back(makeComboLeg(conId2, action2));

        // Overall BAG order action is conventionally "BUY" — direction lives
        // per-leg via each ComboLeg's action above, mirroring how Alpaca's
        // mleg orders carry per-leg "side" under one parent order.
        Order order;
        order.action        = "BUY";
        order.orderType     = "MKT";
        order.totalQuantity = static_cast<double>(qty);
        order.tif           = "DAY";

        OrderId oid = wrapper_.reserveOrderId();
        conn_.placeOrder(oid, bag, order);
        return pollEarlyResult(oid, label);
    }

    // ── Spread (BULL_CALL_SPREAD, BEAR_PUT_SPREAD) ────────────────────────────
    IBKROrderResult routeSpread(const nox::options_signal::OptionsSignal& sig, int qty) {
        bool is_call = (sig.strategy == "BULL_CALL_SPREAD");
        return routeTwoLegCombo(sig, qty,
                                sig.strike,  is_call, "BUY",
                                sig.strike2, is_call, "SELL",
                                sig.underlying + " " + sig.strategy);
    }

    // ── Straddle (ATM call + ATM put, same strike) ────────────────────────────
    IBKROrderResult routeStraddle(const nox::options_signal::OptionsSignal& sig, int qty) {
        return routeTwoLegCombo(sig, qty,
                                sig.strike, true,  "BUY",
                                sig.strike, false, "BUY",
                                sig.underlying + " STRADDLE");
    }

    // ── Strangle (OTM call + OTM put, different strikes) ──────────────────────
    IBKROrderResult routeStrangle(const nox::options_signal::OptionsSignal& sig, int qty) {
        return routeTwoLegCombo(sig, qty,
                                sig.strike,  true,  "BUY",
                                sig.strike2, false, "BUY",
                                sig.underlying + " STRANGLE");
    }

    // Shared four-leg BAG builder for REVERSE_IRON_CONDOR — same pattern as
    // routeTwoLegCombo, just twice as many legs/conId qualifications.
    IBKROrderResult routeFourLegCombo(const nox::options_signal::OptionsSignal& sig, int qty,
                                      double strike1, bool call1, const std::string& action1,
                                      double strike2, bool call2, const std::string& action2,
                                      double strike3, bool call3, const std::string& action3,
                                      double strike4, bool call4, const std::string& action4,
                                      const std::string& label) {
        int conId1 = qualifyLeg(sig.underlying, strike1, sig.expiry_date, call1);
        int conId2 = qualifyLeg(sig.underlying, strike2, sig.expiry_date, call2);
        int conId3 = qualifyLeg(sig.underlying, strike3, sig.expiry_date, call3);
        int conId4 = qualifyLeg(sig.underlying, strike4, sig.expiry_date, call4);
        if (conId1 == 0 || conId2 == 0 || conId3 == 0 || conId4 == 0) {
            return {false, "", "Combo leg qualification failed for " + sig.underlying +
                    " (leg1=" + std::to_string(conId1) + " leg2=" + std::to_string(conId2) +
                    " leg3=" + std::to_string(conId3) + " leg4=" + std::to_string(conId4) + ")",
                    IBKROrderDisposition::Rejected};
        }

        Contract bag;
        bag.symbol   = sig.underlying;
        bag.secType  = "BAG";
        bag.currency = "USD";
        bag.exchange = "SMART";
        bag.comboLegs = std::make_shared<std::vector<ComboLegSPtr>>();
        bag.comboLegs->push_back(makeComboLeg(conId1, action1));
        bag.comboLegs->push_back(makeComboLeg(conId2, action2));
        bag.comboLegs->push_back(makeComboLeg(conId3, action3));
        bag.comboLegs->push_back(makeComboLeg(conId4, action4));

        Order order;
        order.action        = "BUY";
        order.orderType     = "MKT";
        order.totalQuantity = static_cast<double>(qty);
        order.tif           = "DAY";

        OrderId oid = wrapper_.reserveOrderId();
        conn_.placeOrder(oid, bag, order);
        return pollEarlyResult(oid, label);
    }

    // ── Reverse iron condor (long call + short call + long put + short put) ──
    // strike = long call, strike2 = short call (far OTM), strike3 = long put,
    // strike4 = short put (far OTM) — same convention as OptionsOrderRouter.
    IBKROrderResult routeReverseIronCondor(const nox::options_signal::OptionsSignal& sig, int qty) {
        return routeFourLegCombo(sig, qty,
            sig.strike,  true,  "BUY",   // long call
            sig.strike2, true,  "SELL",  // short call (far OTM)
            sig.strike3, false, "BUY",   // long put
            sig.strike4, false, "SELL",  // short put (far OTM)
            sig.underlying + " REVERSE_IRON_CONDOR");
    }
};

} // namespace nox::ibkr
