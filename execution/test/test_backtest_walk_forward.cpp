// Pure-math tests for walk-forward fold generation
// (execution/BacktestWalkForward.hpp) — no OpenSSL/SQLite dependency.

#include "../BacktestWalkForward.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace nox::backtest;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

int main() {
    std::cout << "=== BacktestWalkForward tests ===\n\n";

    std::cout << "[basic fold generation — non-overlapping]\n";
    {
        // n_bars=500, train=252, test=63, step=63:
        // fold0: train[0,252) test[252,315)
        // fold1: train[63,315) test[315,378)
        // fold2: train[126,378) test[378,441)
        // fold3: train[189,441) test[441,504) -> 504 > 500, excluded
        auto folds = generateFolds(500, 252, 63, 63);
        CHECK(folds.size() == 3, "3 folds fit in 500 bars (got " + std::to_string(folds.size()) + ")");
        CHECK(folds[0].train_start == 0 && folds[0].train_end == 252, "fold0 train window [0,252)");
        CHECK(folds[0].test_start == 252 && folds[0].test_end == 315, "fold0 test window [252,315)");
        CHECK(folds[1].train_start == 63, "fold1 train slides forward by step_days");
    }

    std::cout << "\n[test window immediately follows train window — no gap, no overlap]\n";
    {
        auto folds = generateFolds(1000, 100, 20, 20);
        for (const auto& f : folds) {
            CHECK(f.test_start == f.train_end, "test_start == train_end (no gap/overlap) for every fold");
        }
    }

    std::cout << "\n[not enough bars for even one fold]\n";
    {
        auto folds = generateFolds(100, 252, 63, 63);
        CHECK(folds.empty(), "fewer bars than train+test produces zero folds");
    }

    std::cout << "\n[exact fit — one fold exactly reaches n_bars]\n";
    {
        auto folds = generateFolds(315, 252, 63, 63); // train_end=252, test_end=315 == n_bars
        CHECK(folds.size() == 1, "exact boundary fit produces exactly one fold");
    }

    std::cout << "\n[off-by-one — one bar short excludes the fold]\n";
    {
        auto folds = generateFolds(314, 252, 63, 63); // test_end=315 > 314
        CHECK(folds.empty(), "one bar short of the exact fit produces zero folds");
    }

    std::cout << "\n[invalid config returns no folds rather than looping forever]\n";
    {
        CHECK(generateFolds(1000, 0, 63, 63).empty(), "train_days=0 -> no folds");
        CHECK(generateFolds(1000, 252, 0, 63).empty(), "test_days=0 -> no folds");
        CHECK(generateFolds(1000, 252, 63, 0).empty(), "step_days=0 -> no folds (would infinite-loop otherwise)");
    }

    std::cout << "\n[overlapping test windows when step < test_days]\n";
    {
        // step=30 < test_days=63 means consecutive test windows overlap —
        // legal (a smaller step just re-tests more densely), just documenting
        // the behavior explicitly so it's not mistaken for a bug.
        auto folds = generateFolds(500, 252, 63, 30);
        CHECK(folds.size() >= 2, "smaller step than test_days still produces multiple folds");
        if (folds.size() >= 2) {
            CHECK(folds[1].test_start < folds[0].test_end,
                  "consecutive test windows overlap when step_days < test_days (by design)");
        }
    }

    std::cout << "\n[WalkForwardConfig::fromEnv defaults]\n";
    {
        unsetenv("NOX_BT_WFO_ENABLED");
        unsetenv("NOX_BT_WFO_TRAIN_DAYS");
        unsetenv("NOX_BT_WFO_PROFIT_GRID");
        auto cfg = WalkForwardConfig::fromEnv();
        CHECK(!cfg.enabled, "disabled by default (current-behavior-preserving)");
        CHECK(cfg.train_days == 252, "default train_days = 252");
        CHECK(cfg.profit_grid.size() == 3, "default profit grid has 3 values");
    }

    std::cout << "\n[WalkForwardConfig::fromEnv overrides]\n";
    {
        setenv("NOX_BT_WFO_ENABLED", "1", 1);
        setenv("NOX_BT_WFO_TRAIN_DAYS", "100", 1);
        setenv("NOX_BT_WFO_PROFIT_GRID", "0.4,0.6", 1);
        auto cfg = WalkForwardConfig::fromEnv();
        CHECK(cfg.enabled, "env override enables WFO");
        CHECK(cfg.train_days == 100, "env override -> train_days 100");
        CHECK(cfg.profit_grid.size() == 2 && cfg.profit_grid[0] == 0.4 && cfg.profit_grid[1] == 0.6,
              "env override -> profit grid [0.4, 0.6]");
        unsetenv("NOX_BT_WFO_ENABLED");
        unsetenv("NOX_BT_WFO_TRAIN_DAYS");
        unsetenv("NOX_BT_WFO_PROFIT_GRID");
    }

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "All BacktestWalkForward tests passed.\n";
        return 0;
    }
    std::cout << g_failures << " test(s) FAILED.\n";
    return 1;
}
