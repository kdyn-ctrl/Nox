// Pure-math tests for the adverse-selection fill model
// (execution/BacktestFillModel.hpp) — no OpenSSL/SQLite dependency, just the
// fill-probability/level math used by backtest_main.cpp's variant simulation.

#include "../BacktestFillModel.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

using namespace nox::backtest;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  \xE2\x9C\x97 FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  \xE2\x9C\x93 " << (msg) << "\n"; } \
} while (0)

int main() {
    std::cout << "=== BacktestFillModel tests ===\n\n";

    FillModelConfig naive_cfg;
    naive_cfg.mode = FillMode::Naive;
    FillModelConfig adverse_cfg;
    adverse_cfg.mode = FillMode::AdverseSelection;
    adverse_cfg.gamma = 0.3;
    adverse_cfg.queue_mult = 3.0;

    std::mt19937 rng(42);

    std::cout << "[never touched]\n";
    {
        // approached_from_above: level reached by falling low <= level.
        OptionBarRange bar{ /*low*/5.0, /*high*/6.0, /*close*/5.5 };
        auto out = simulateFill(bar, /*limit_level*/4.0, /*approached_from_above*/true,
                                100.0, 100.0, adverse_cfg, rng);
        CHECK(!out.touched && !out.filled && out.p_fill == 0.0,
              "bar range never reaches the level -> not touched, not filled");
    }

    std::cout << "\n[touch-only, Naive mode]\n";
    {
        // Falling onto level 5.0: low=4.5 touches, close=5.2 reverses (no cross).
        OptionBarRange bar{4.5, 5.5, 5.2};
        auto out = simulateFill(bar, 5.0, true, 100.0, 100.0, naive_cfg, rng);
        CHECK(out.touched && !out.crossed && out.filled && out.p_fill == 1.0,
              "Naive mode fills any touch at 100%");
    }

    std::cout << "\n[crossed, AdverseSelection mode]\n";
    {
        // Falling onto level 5.0: low=4.5, close=4.8 stays below -> crossed.
        OptionBarRange bar{4.5, 5.5, 4.8};
        auto out = simulateFill(bar, 5.0, true, 100.0, 100.0, adverse_cfg, rng);
        CHECK(out.touched && out.crossed && out.filled && out.p_fill == 1.0,
              "AdverseSelection mode: crossed level always fills at 100%");
    }

    std::cout << "\n[touch-only, AdverseSelection mode — exact probability]\n";
    {
        // day_vol=100, avg_vol=100, gamma=0.3, queue_mult=3 -> queue=300
        // p = 100*0.3/300 = 0.1
        OptionBarRange bar{4.5, 5.5, 5.2}; // touches, doesn't cross
        auto out = simulateFill(bar, 5.0, true, 100.0, 100.0, adverse_cfg, rng);
        CHECK(out.touched && !out.crossed, "touch-only, no cross");
        CHECK(std::abs(out.p_fill - 0.1) < 1e-9,
              "p_fill matches hand-computed value (got " + std::to_string(out.p_fill) + ")");
    }

    std::cout << "\n[probability saturates at 1.0]\n";
    {
        OptionBarRange bar{4.5, 5.5, 5.2};
        auto out = simulateFill(bar, 5.0, true, 1'000'000.0, 100.0, adverse_cfg, rng);
        CHECK(out.p_fill == 1.0, "large day_liquidity_proxy saturates p_fill at 1.0");
    }

    std::cout << "\n[force_certain overrides low-probability touch]\n";
    {
        OptionBarRange bar{4.5, 5.5, 5.2};
        auto out = simulateFill(bar, 5.0, true, 1.0, 100.0, adverse_cfg, rng, /*force_certain*/true);
        CHECK(out.filled && out.p_fill == 1.0,
              "force_certain=true fills a touch regardless of computed probability");
    }

    std::cout << "\n[statistical fill-fraction check]\n";
    {
        // Same fixture as the exact-probability test: p_fill = 0.1.
        OptionBarRange bar{4.5, 5.5, 5.2};
        std::mt19937 stat_rng(1234);
        int filled = 0, draws = 10000;
        for (int i = 0; i < draws; ++i) {
            auto out = simulateFill(bar, 5.0, true, 100.0, 100.0, adverse_cfg, stat_rng);
            if (out.filled) ++filled;
        }
        double observed = static_cast<double>(filled) / draws;
        CHECK(std::abs(observed - 0.1) < 0.02,
              "observed fill fraction ~0.1 over 10k draws (got " + std::to_string(observed) + ")");
    }

    std::cout << "\n[FillModelConfig::fromEnv defaults]\n";
    {
        unsetenv("NOX_BT_FILL_MODEL");
        unsetenv("NOX_BT_FILL_GAMMA");
        unsetenv("NOX_BT_FILL_QUEUE_MULT");
        unsetenv("NOX_BT_FILL_SEED");
        auto cfg = FillModelConfig::fromEnv();
        CHECK(cfg.mode == FillMode::Naive, "no env vars -> Naive (current-behavior-preserving)");
        CHECK(cfg.gamma == 0.2, "default gamma is 0.2");
        CHECK(cfg.queue_mult == 3.0, "default queue_mult is 3.0");
    }

    std::cout << "\n[FillModelConfig::fromEnv overrides]\n";
    {
        setenv("NOX_BT_FILL_MODEL", "adverse_selection", 1);
        setenv("NOX_BT_FILL_GAMMA", "0.5", 1);
        setenv("NOX_BT_FILL_QUEUE_MULT", "2.0", 1);
        setenv("NOX_BT_FILL_SEED", "7", 1);
        auto cfg = FillModelConfig::fromEnv();
        CHECK(cfg.mode == FillMode::AdverseSelection, "env override -> AdverseSelection");
        CHECK(cfg.gamma == 0.5, "env override -> gamma 0.5");
        CHECK(cfg.queue_mult == 2.0, "env override -> queue_mult 2.0");
        CHECK(cfg.rng_seed == 7, "env override -> rng_seed 7");
        unsetenv("NOX_BT_FILL_MODEL");
        unsetenv("NOX_BT_FILL_GAMMA");
        unsetenv("NOX_BT_FILL_QUEUE_MULT");
        unsetenv("NOX_BT_FILL_SEED");
    }

    std::cout << "\n[level/approach formulas — long]\n";
    {
        CHECK(profitTargetLevel(10.0, 0.5, true) == 15.0, "long profit target: entry + entry*pct");
        CHECK(stopLossLevel(10.0, 2.0, true) == -10.0, "long stop loss: entry - entry*mult");
        CHECK(approachedFromAboveForTarget(true) == false, "long target approached from below");
        CHECK(approachedFromAboveForStop(true) == true, "long stop approached from above");
    }

    std::cout << "\n[level/approach formulas — short]\n";
    {
        CHECK(profitTargetLevel(10.0, 0.5, false) == 5.0, "short profit target: entry - entry*pct");
        CHECK(stopLossLevel(10.0, 2.0, false) == 30.0, "short stop loss: entry + entry*mult");
        CHECK(approachedFromAboveForTarget(false) == true, "short target approached from above");
        CHECK(approachedFromAboveForStop(false) == false, "short stop approached from below");
    }

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "All BacktestFillModel tests passed.\n";
        return 0;
    }
    std::cout << g_failures << " test(s) FAILED.\n";
    return 1;
}
