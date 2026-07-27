// test_portfolio_risk_manager.cpp — Phase 4, item 2: portfolio circuit
// breaker. PortfolioRiskManager.hpp is pure aggregation/decision math (no
// I/O), so these tests build synthetic PositionGreekContribution vectors
// directly rather than seeding a database — mirrors how OptionEngine's own
// Black-Scholes math is tested without a broker.

#include "../PortfolioRiskManager.hpp"

#include <iostream>
#include <string>

using nox::risk::PositionGreekContribution;
using nox::risk::RiskTargets;
using nox::risk::evaluate;
using nox::risk::aggregate;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static RiskTargets tightTargets() {
    RiskTargets t;
    t.max_abs_delta        = 100.0;
    t.max_abs_vega         = 100.0;
    t.max_options_notional = 10000.0;
    t.max_equity_notional  = 10000.0;
    return t;
}

static void test_empty_portfolio_never_breaches() {
    std::cout << "\n[empty] no open positions → no breach, nothing to close\n";
    auto breach = evaluate({}, tightTargets());
    CHECK(!breach.breached, "empty portfolio never breaches");
    CHECK(breach.position_to_close == -1, "no position identified when there's nothing to close");
}

static void test_within_target_no_breach() {
    std::cout << "\n[within-target] modest single position stays under every cap\n";
    std::vector<PositionGreekContribution> positions = {
        {1, "AAPL", 30.0, 20.0, 5000.0},
    };
    auto breach = evaluate(positions, tightTargets());
    CHECK(!breach.breached, "30 delta / 20 vega / $5000 notional all under 100/100/$10000 targets");
}

static void test_delta_breach_picks_largest_contributor() {
    std::cout << "\n[delta-breach] net delta exceeds target → largest same-direction contributor flagged\n";
    std::vector<PositionGreekContribution> positions = {
        {1, "AAPL", 40.0, 10.0, 1000.0},   // smaller long-delta contributor
        {2, "TSLA", 80.0, 10.0, 1000.0},   // larger long-delta contributor — should be picked
        {3, "MSFT", -5.0, 10.0, 1000.0},   // opposite direction, irrelevant to the breach
    };
    // net_delta = 40 + 80 - 5 = 115 > 100
    auto breach = evaluate(positions, tightTargets());
    CHECK(breach.breached, "net delta 115 exceeds target 100");
    CHECK(breach.position_to_close == 2, "TSLA (80 delta) is the largest same-direction contributor, not AAPL or MSFT");
}

static void test_negative_delta_breach_respects_direction() {
    std::cout << "\n[short-delta-breach] net delta breaches negative → only negative-side contributors eligible\n";
    std::vector<PositionGreekContribution> positions = {
        {1, "AAPL", -60.0, 5.0, 1000.0},
        {2, "TSLA", -70.0, 5.0, 1000.0},   // most negative — should be picked
        {3, "MSFT", 20.0, 5.0, 1000.0},    // positive-delta position; must never be picked to close a negative breach
    };
    // net_delta = -60 - 70 + 20 = -110, |net_delta| > 100
    auto breach = evaluate(positions, tightTargets());
    CHECK(breach.breached, "net delta -110 breaches |target| 100");
    CHECK(breach.position_to_close == 2, "TSLA (-70) is the largest contributor in the breaching (negative) direction");
}

static void test_vega_breach_only_when_delta_within_target() {
    std::cout << "\n[vega-breach] delta stays within target, vega alone breaches\n";
    std::vector<PositionGreekContribution> positions = {
        {1, "AAPL", 10.0, 60.0, 1000.0},
        {2, "TSLA", 10.0, 50.0, 1000.0},
    };
    // net_delta = 20 (fine), net_vega = 110 > 100
    auto breach = evaluate(positions, tightTargets());
    CHECK(breach.breached, "vega 110 exceeds target 100 while delta is fine");
    CHECK(breach.reason.find("vega") != std::string::npos, "breach reason names vega, not delta");
    CHECK(breach.position_to_close == 1, "AAPL (60 vega) is the largest vega contributor");
}

static void test_notional_breach_is_last_check() {
    std::cout << "\n[notional-breach] delta/vega both fine, only notional breaches\n";
    std::vector<PositionGreekContribution> positions = {
        {1, "AAPL", 5.0, 5.0, 6000.0},
        {2, "TSLA", 5.0, 5.0, 6000.0},
    };
    // net_delta=10, net_vega=10 (both fine), options_notional = 12000 > 10000
    auto breach = evaluate(positions, tightTargets());
    CHECK(breach.breached, "notional 12000 exceeds target 10000");
    CHECK(breach.reason.find("notional") != std::string::npos, "breach reason names notional");
}

static void test_env_override_and_fake_safe_default() {
    std::cout << "\n[env-override] RiskTargets::fromEnv honors a valid override, ignores an invalid one\n";
    setenv("MAX_PORTFOLIO_DELTA", "250", 1);
    setenv("MAX_PORTFOLIO_VEGA", "not_a_number", 1);
    auto t = RiskTargets::fromEnv();
    CHECK(t.max_abs_delta == 250.0, "valid MAX_PORTFOLIO_DELTA=250 overrides the default");
    CHECK(t.max_abs_vega == 2000.0, "malformed MAX_PORTFOLIO_VEGA falls back to the fake-safe default, not 0/garbage");
    unsetenv("MAX_PORTFOLIO_DELTA");
    unsetenv("MAX_PORTFOLIO_VEGA");
}

int main() {
    std::cout << "═══ Phase 4 PortfolioRiskManager (circuit breaker) tests ═══\n";
    test_empty_portfolio_never_breaches();
    test_within_target_no_breach();
    test_delta_breach_picks_largest_contributor();
    test_negative_delta_breach_respects_direction();
    test_vega_breach_only_when_delta_within_target();
    test_notional_breach_is_last_check();
    test_env_override_and_fake_safe_default();

    std::cout << "\n"
              << (g_failures == 0 ? "✅ ALL PORTFOLIO-RISK-MANAGER TESTS PASSED\n"
                                  : "❌ " + std::to_string(g_failures) + " CHECK(S) FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
