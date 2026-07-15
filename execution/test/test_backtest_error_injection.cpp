// test_backtest_error_injection.cpp — the "does it still profit through
// operational errors?" model. BacktestErrorModel.hpp is pure, deterministic
// math (seeded PRNG, no I/O), so these tests pin the injection semantics and
// the survival verdict directly on synthetic TradeViews — the same testable
// shape as test_backtest_fill_model.cpp.

#include "../BacktestErrorModel.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace nox::backtest;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  \xE2\x9C\x97 FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  \xE2\x9C\x93 " << (msg) << "\n"; } \
} while (0)

static bool approx(double a, double b) { return std::abs(a - b) < 1e-6; }

static std::vector<TradeView> sampleTrades() {
    // entry_price, pnl, pnl_if_held_to_expiry, exit_reason
    return {
        {1.00,  0.50,  -0.20, "PROFIT_TARGET"}, // winner that would've LOST if held
        {1.00, -2.00,  -0.50, "STOP_LOSS"},     // stopped loser, less bad if held
        {2.00,  1.20,   1.20, "EXPIRY"},        // already ran to expiry
        {1.50,  0.30,   0.10, "PROFIT_TARGET"},
    };
}

static void test_no_errors_is_identity() {
    std::cout << "\n[noop] zero rates -> baseline unchanged\n";
    auto trades = sampleTrades();
    ErrorConfig cfg; // all rates 0
    CHECK(!cfg.active(), "a zero-rate config is inactive");
    auto impact = applyErrors(trades, cfg);
    bool same = true;
    for (size_t i = 0; i < trades.size(); ++i)
        if (!approx(impact.injected_pnls[i], trades[i].pnl)) same = false;
    CHECK(same, "no-op injection returns every baseline P&L unchanged");
    CHECK(impact.ghost_fills == 0 && impact.missed_exits == 0 && impact.adverse_fills == 0,
          "no errors fire when inactive");
}

static void test_missed_exit_uses_expiry_counterfactual() {
    std::cout << "\n[missed_exit] certain miss swaps in the held-to-expiry P&L\n";
    auto trades = sampleTrades();
    ErrorConfig cfg;
    cfg.missed_exit_rate = 1.0; // every early exit is missed
    cfg.seed = 7;
    auto impact = applyErrors(trades, cfg);
    // Trade 0: winner 0.50 -> held-to-expiry -0.20 (a real reversal).
    CHECK(approx(impact.injected_pnls[0], -0.20), "missed profit-target rides to a -0.20 expiry");
    // Trade 2 already EXPIRY -> untouched even at rate 1.0.
    CHECK(approx(impact.injected_pnls[2], 1.20), "an EXPIRY trade is never 'missed'");
    CHECK(impact.missed_exits == 3, "all three early-exit trades are flagged missed");
}

static void test_adverse_fill_haircut() {
    std::cout << "\n[adverse_fill] certain slippage subtracts pct*entry_price\n";
    std::vector<TradeView> one = {{2.00, 1.00, 1.00, "PROFIT_TARGET"}};
    ErrorConfig cfg;
    cfg.adverse_fill_rate = 1.0;
    cfg.adverse_slippage_pct = 0.25; // 0.25 * 2.00 = 0.50 haircut
    cfg.seed = 3;
    auto impact = applyErrors(one, cfg);
    CHECK(approx(impact.injected_pnls[0], 0.50), "1.00 pnl minus 0.50 slippage = 0.50");
    CHECK(impact.adverse_fills == 1, "adverse fill counted");
}

static void test_ghost_fill_doubles() {
    std::cout << "\n[ghost_fill] certain ghost doubles the lot's P&L (both directions)\n";
    std::vector<TradeView> two = {
        {1.00,  0.80, 0.80, "PROFIT_TARGET"},
        {1.00, -1.50, -1.50, "STOP_LOSS"},
    };
    ErrorConfig cfg;
    cfg.ghost_fill_rate = 1.0;
    cfg.seed = 11;
    auto impact = applyErrors(two, cfg);
    CHECK(approx(impact.injected_pnls[0], 1.60), "a ghost-duplicated winner doubles to +1.60");
    CHECK(approx(impact.injected_pnls[1], -3.00), "a ghost-duplicated loser doubles to -3.00");
    CHECK(impact.ghost_fills == 2, "both ghost fills counted");
}

static void test_determinism_same_seed() {
    std::cout << "\n[determinism] same seed -> identical injection\n";
    auto trades = sampleTrades();
    ErrorConfig cfg;
    cfg.ghost_fill_rate = 0.5; cfg.missed_exit_rate = 0.5; cfg.adverse_fill_rate = 0.5;
    cfg.seed = 42;
    auto a = applyErrors(trades, cfg);
    auto b = applyErrors(trades, cfg);
    bool identical = a.injected_pnls.size() == b.injected_pnls.size();
    for (size_t i = 0; identical && i < a.injected_pnls.size(); ++i)
        if (!approx(a.injected_pnls[i], b.injected_pnls[i])) identical = false;
    CHECK(identical, "a fixed seed reproduces the exact injected series");
}

static void test_different_seed_differs() {
    std::cout << "\n[determinism] different seed -> (generally) different injection\n";
    auto trades = sampleTrades();
    ErrorConfig cfg;
    cfg.ghost_fill_rate = 0.5; cfg.missed_exit_rate = 0.5; cfg.adverse_fill_rate = 0.5;
    cfg.seed = 1;
    auto a = applyErrors(trades, cfg);
    cfg.seed = 999;
    auto b = applyErrors(trades, cfg);
    bool any_diff = false;
    for (size_t i = 0; i < a.injected_pnls.size(); ++i)
        if (!approx(a.injected_pnls[i], b.injected_pnls[i])) any_diff = true;
    CHECK(any_diff, "a different seed changes at least one outcome");
}

static void test_summary_survival_verdict() {
    std::cout << "\n[summary] retention + survival verdict\n";
    // Baseline total: 0.50 - 2.00 + 1.20 + 0.30 = 0.00 ... make it clearly positive:
    std::vector<TradeView> t = {
        {1.00, 2.00, 0.50, "PROFIT_TARGET"},
        {1.00, 1.00, 0.20, "PROFIT_TARGET"},
        {1.00, 1.00, 1.00, "EXPIRY"},
    }; // baseline total = 4.00
    ErrorConfig cfg;
    cfg.missed_exit_rate = 1.0; // wrecks both winners: 0.50 + 0.20 + 1.00 = 1.70
    cfg.seed = 5;
    auto impact = applyErrors(t, cfg);
    auto s = summarize(t, impact);
    CHECK(approx(s.baseline_total, 4.00), "baseline total summed correctly");
    CHECK(approx(s.injected_total, 1.70), "injected total uses expiry counterfactuals");
    CHECK(s.still_profitable, "still profitable (1.70 > 0) despite losing most of the edge");
    CHECK(approx(s.retention, 1.70 / 4.00), "retention = injected/baseline");
}

static void test_summary_flips_to_loss() {
    std::cout << "\n[summary] enough error turns a thin edge into a loss\n";
    std::vector<TradeView> t = {
        {1.00, 0.30, -0.80, "PROFIT_TARGET"},
        {1.00, 0.20, -0.60, "PROFIT_TARGET"},
    }; // baseline 0.50; expiry counterfactuals both deeply negative
    ErrorConfig cfg;
    cfg.missed_exit_rate = 1.0;
    cfg.seed = 2;
    auto impact = applyErrors(t, cfg);
    auto s = summarize(t, impact);
    CHECK(s.injected_total < 0.0, "injected total goes negative");
    CHECK(!s.still_profitable, "verdict flips to NOT profitable");
}

static void test_env_config() {
    std::cout << "\n[env] ErrorConfig::fromEnv reads rates\n";
    setenv("BT_ERR_GHOST_FILL_RATE", "0.07", 1);
    setenv("BT_ERR_SEED", "123", 1);
    auto c = ErrorConfig::fromEnv();
    CHECK(approx(c.ghost_fill_rate, 0.07), "ghost-fill rate from env");
    CHECK(c.seed == 123u, "seed from env");
    CHECK(c.active(), "a non-zero rate makes the config active");
    unsetenv("BT_ERR_GHOST_FILL_RATE");
    unsetenv("BT_ERR_SEED");
}

int main() {
    std::cout << "=== Backtest error-injection resilience tests ===\n";
    test_no_errors_is_identity();
    test_missed_exit_uses_expiry_counterfactual();
    test_adverse_fill_haircut();
    test_ghost_fill_doubles();
    test_determinism_same_seed();
    test_different_seed_differs();
    test_summary_survival_verdict();
    test_summary_flips_to_loss();
    test_env_config();

    std::cout << "\n";
    if (g_failures == 0) { std::cout << "All error-injection tests passed.\n"; return 0; }
    std::cout << g_failures << " error-injection test(s) failed.\n";
    return 1;
}
