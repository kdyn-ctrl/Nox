// test_ibkr_combo_router.cpp — Phase 3: IBKR combo/BAG multi-leg order support.
//
// CLAUDE.md flagged IBKRClient as "single-leg only" and named combo/BAG
// support the prerequisite before any IBKR multi-leg options strategy goes
// live. Built against execution/test/ibkr_stub/ (hand-written stand-ins for
// the real TWS API vendor source — see that directory's CommonDefs.h for why)
// since the actual TWS API isn't vendored in this repo. The stub's
// reqContractDetails responds synchronously with a deterministic
// conId = strike*10 + (1 for call / 2 for put), and placeOrder records every
// (Contract, Order) it receives — so these tests assert on the EXACT
// combo-leg wiring IBKROrderRouter would have sent a real gateway, not just
// "it didn't crash."
//
// What this proves: contract/order construction is correct for all 8
// strategies. What this does NOT prove: that a live IB Gateway accepts the
// order — that's the "still open" live/paper validation Phase 3 flags,
// mirroring how Phase 1/2 shipped mock-tested infra first.

#include "ibkr_stub/EClientSocket.h" // pulls in the rest of the stub set
#include "../IBKROrderRouter.hpp"
#include "../OptionsSignalTypes.hpp"

#include <iostream>
#include <string>

using nox::ibkr::IBKROrderRouter;
using nox::ibkr::IBKRWrapper;
using nox::ibkr::IBKRConnection;
using nox::ibkr::IBKROrderDisposition;
namespace osig = nox::options_signal;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  \xE2\x9C\x97 FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  \xE2\x9C\x93 " << (msg) << "\n"; } \
} while (0)

static osig::OptionsSignal makeSignal(const std::string& strategy, double strike, double strike2,
                                      nox::options::OptionType type) {
    osig::OptionsSignal s;
    s.underlying  = "AAPL";
    s.strategy    = strategy;
    s.expiry_date = "2026-09-18";
    s.strike      = strike;
    s.strike2     = strike2;
    s.option_type = type;
    return s;
}

static osig::OptionsSignal makeSignal4(const std::string& strategy,
                                       double strike, double strike2,
                                       double strike3, double strike4) {
    osig::OptionsSignal s = makeSignal(strategy, strike, strike2, nox::options::OptionType::Call);
    s.strike3 = strike3;
    s.strike4 = strike4;
    return s;
}

static void resetLog() { EClientSocket::placed_orders_.clear(); }

static void test_long_call_single_leg() {
    std::cout << "\n[LONG_CALL] single-leg OPT contract, action=BUY\n";
    resetLog();
    IBKRWrapper wrapper;
    IBKRConnection conn(wrapper);
    IBKROrderRouter router(conn, wrapper);

    auto sig = makeSignal("LONG_CALL", 150.0, 0.0, nox::options::OptionType::Call);
    auto res = router.route(sig, 2);

    CHECK(res.success, "LONG_CALL accepted");
    CHECK(EClientSocket::placed_orders_.size() == 1, "exactly one order placed (no combo)");
    const auto& placed = EClientSocket::placed_orders_.back();
    CHECK(placed.contract.secType == "OPT", "single-leg contract secType is OPT, not BAG");
    CHECK(placed.contract.right == "C", "right=C for a call");
    CHECK(placed.contract.conId == 1501, "conId resolved from qualification (150*10+1)");
    CHECK(placed.order.action == "BUY", "LONG_CALL action is BUY");
    CHECK(placed.order.totalQuantity == 2.0, "qty_contracts threaded through to totalQuantity");
}

static void test_csp_is_a_sell() {
    std::cout << "\n[CSP] single-leg OPT contract, action=SELL (short premium)\n";
    resetLog();
    IBKRWrapper wrapper;
    IBKRConnection conn(wrapper);
    IBKROrderRouter router(conn, wrapper);

    auto sig = makeSignal("CSP", 140.0, 0.0, nox::options::OptionType::Put);
    auto res = router.route(sig, 1);

    CHECK(res.success, "CSP accepted");
    const auto& placed = EClientSocket::placed_orders_.back();
    CHECK(placed.contract.right == "P", "CSP sells a put");
    CHECK(placed.order.action == "SELL", "CSP/CC are short premium — SELL, not BUY");
}

static void test_bull_call_spread_combo_legs() {
    std::cout << "\n[BULL_CALL_SPREAD] BAG contract with buy-lower/sell-higher call legs\n";
    resetLog();
    IBKRWrapper wrapper;
    IBKRConnection conn(wrapper);
    IBKROrderRouter router(conn, wrapper);

    auto sig = makeSignal("BULL_CALL_SPREAD", 150.0, 160.0, nox::options::OptionType::Call);
    auto res = router.route(sig, 1);

    CHECK(res.success, "spread accepted");
    const auto& placed = EClientSocket::placed_orders_.back();
    CHECK(placed.contract.secType == "BAG", "spread is a single BAG order, not two separate orders");
    CHECK(static_cast<bool>(placed.contract.comboLegs), "comboLegs populated");
    CHECK(placed.contract.comboLegs->size() == 2, "exactly two legs");
    CHECK((*placed.contract.comboLegs)[0]->conId == 1501, "leg 1 = the 150 strike (buy)");
    CHECK((*placed.contract.comboLegs)[0]->action == "BUY", "leg 1 action is BUY (long the near strike)");
    CHECK((*placed.contract.comboLegs)[1]->conId == 1601, "leg 2 = the 160 strike (sell)");
    CHECK((*placed.contract.comboLegs)[1]->action == "SELL", "leg 2 action is SELL (short the wing)");
    CHECK(EClientSocket::placed_orders_.size() == 1, "the whole spread is ONE atomic order, mirroring Alpaca's mleg");
}

static void test_bear_put_spread_combo_legs() {
    std::cout << "\n[BEAR_PUT_SPREAD] BAG contract with buy-higher/sell-lower put legs\n";
    resetLog();
    IBKRWrapper wrapper;
    IBKRConnection conn(wrapper);
    IBKROrderRouter router(conn, wrapper);

    auto sig = makeSignal("BEAR_PUT_SPREAD", 150.0, 140.0, nox::options::OptionType::Put);
    auto res = router.route(sig, 1);

    CHECK(res.success, "bear put spread accepted");
    const auto& placed = EClientSocket::placed_orders_.back();
    CHECK((*placed.contract.comboLegs)[0]->conId == 1502, "leg 1 = the 150 put (buy)");
    CHECK((*placed.contract.comboLegs)[0]->action == "BUY", "leg 1 BUY");
    CHECK((*placed.contract.comboLegs)[1]->conId == 1402, "leg 2 = the 140 put (sell)");
    CHECK((*placed.contract.comboLegs)[1]->action == "SELL", "leg 2 SELL");
}

static void test_straddle_call_and_put_same_strike() {
    std::cout << "\n[STRADDLE] BAG contract, call+put both BUY at the same strike\n";
    resetLog();
    IBKRWrapper wrapper;
    IBKRConnection conn(wrapper);
    IBKROrderRouter router(conn, wrapper);

    auto sig = makeSignal("STRADDLE", 150.0, 0.0, nox::options::OptionType::Call);
    auto res = router.route(sig, 1);

    CHECK(res.success, "straddle accepted");
    const auto& placed = EClientSocket::placed_orders_.back();
    CHECK((*placed.contract.comboLegs)[0]->conId == 1501, "leg 1 = 150 call");
    CHECK((*placed.contract.comboLegs)[1]->conId == 1502, "leg 2 = 150 put (same strike, different right)");
    CHECK((*placed.contract.comboLegs)[0]->action == "BUY" && (*placed.contract.comboLegs)[1]->action == "BUY",
          "both legs BUY — straddle is a net debit, not a spread");
}

static void test_strangle_call_and_put_different_strikes() {
    std::cout << "\n[STRANGLE] BAG contract, OTM call + OTM put at different strikes, both BUY\n";
    resetLog();
    IBKRWrapper wrapper;
    IBKRConnection conn(wrapper);
    IBKROrderRouter router(conn, wrapper);

    auto sig = makeSignal("STRANGLE", 160.0, 140.0, nox::options::OptionType::Call);
    auto res = router.route(sig, 1);

    CHECK(res.success, "strangle accepted");
    const auto& placed = EClientSocket::placed_orders_.back();
    CHECK((*placed.contract.comboLegs)[0]->conId == 1601, "leg 1 = 160 call");
    CHECK((*placed.contract.comboLegs)[1]->conId == 1402, "leg 2 = 140 put");
}

static void test_reverse_iron_condor_four_legs() {
    std::cout << "\n[REVERSE_IRON_CONDOR] BAG contract, 4 legs: long call, short call, long put, short put\n";
    resetLog();
    IBKRWrapper wrapper;
    IBKRConnection conn(wrapper);
    IBKROrderRouter router(conn, wrapper);

    // strike=long call, strike2=short call, strike3=long put, strike4=short put
    auto sig = makeSignal4("REVERSE_IRON_CONDOR", 155.0, 165.0, 145.0, 135.0);
    auto res = router.route(sig, 1);

    CHECK(res.success, "reverse iron condor accepted");
    const auto& placed = EClientSocket::placed_orders_.back();
    CHECK(placed.contract.comboLegs->size() == 4, "exactly 4 combo legs");
    CHECK((*placed.contract.comboLegs)[0]->conId == 1551, "leg 1 = 155 long call");
    CHECK((*placed.contract.comboLegs)[0]->action == "BUY", "leg 1 action = BUY");
    CHECK((*placed.contract.comboLegs)[1]->conId == 1651, "leg 2 = 165 short call");
    CHECK((*placed.contract.comboLegs)[1]->action == "SELL", "leg 2 action = SELL");
    CHECK((*placed.contract.comboLegs)[2]->conId == 1452, "leg 3 = 145 long put");
    CHECK((*placed.contract.comboLegs)[2]->action == "BUY", "leg 3 action = BUY");
    CHECK((*placed.contract.comboLegs)[3]->conId == 1352, "leg 4 = 135 short put");
    CHECK((*placed.contract.comboLegs)[3]->action == "SELL", "leg 4 action = SELL");
}

static void test_unknown_strategy_rejected() {
    std::cout << "\n[unknown] an unrecognized strategy string is rejected, never guessed at\n";
    resetLog();
    IBKRWrapper wrapper;
    IBKRConnection conn(wrapper);
    IBKROrderRouter router(conn, wrapper);

    auto sig = makeSignal("IRON_CONDOR", 150.0, 160.0, nox::options::OptionType::Call);
    auto res = router.route(sig, 1);

    CHECK(!res.success, "unknown strategy fails");
    CHECK(res.disposition == IBKROrderDisposition::Rejected, "disposition is Rejected");
    CHECK(EClientSocket::placed_orders_.empty(), "no order was ever placed for an unrecognized strategy");
}

int main() {
    std::cout << "\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90 Phase 3 IBKR combo/BAG order-construction tests (stub TWS headers) \xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\n";
    test_long_call_single_leg();
    test_csp_is_a_sell();
    test_bull_call_spread_combo_legs();
    test_bear_put_spread_combo_legs();
    test_straddle_call_and_put_same_strike();
    test_strangle_call_and_put_different_strikes();
    test_reverse_iron_condor_four_legs();
    test_unknown_strategy_rejected();

    std::cout << "\n"
              << (g_failures == 0 ? "\xE2\x9C\x85 ALL IBKR COMBO/BAG TESTS PASSED\n"
                                  : "\xE2\x9D\x8C " + std::to_string(g_failures) + " CHECK(S) FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
