#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Walk-forward optimization (WFO) fold generation for the backtester.
// ─────────────────────────────────────────────────────────────────────────────
// Answers the "walk-forward, not a static train/test split" ask directly:
// train on a rolling window, pick the best parameter combo from a grid using
// ONLY that window's data, then apply it to the immediately-following
// out-of-sample test window, slide forward by step_days, repeat. The
// out-of-sample trades across every fold are what actually get reported —
// never the in-sample (train) performance, which is optimistic by
// construction since the params were chosen to fit it.
//
// Point-in-time note: bar-level indicators in backtest_main.cpp already only
// ever read bars[0..idx] ("strict no-lookahead", see calcRSI/calcSMA/calcHRV).
// This header's fold generator is what turns that non-lookahead PROPERTY at
// the bar level into a non-lookahead PROCEDURE at the parameter-selection
// level: a fold's test window is bar-index-disjoint from and strictly after
// its train window, so a parameter can never be chosen using information
// from the period it's then graded on.
//
// Pure math — no network/SQLite/file I/O — so fold generation is
// independently unit testable (see execution/test/test_backtest_walk_forward.cpp).
// The actual trade simulation stays in backtest_main.cpp (it needs the
// Trade/BacktestConfig types and simulateTrade()), this header only owns the
// fold boundaries + config + grid.

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace nox::backtest {

struct WalkForwardConfig {
    bool   enabled     = false; // NOX_BT_WFO_ENABLED
    int    train_days   = 252;   // NOX_BT_WFO_TRAIN_DAYS  (~1 trading year)
    int    test_days    = 63;    // NOX_BT_WFO_TEST_DAYS   (~1 quarter)
    int    step_days    = 63;    // NOX_BT_WFO_STEP_DAYS   (slide increment)
    std::vector<double> profit_grid = {0.30, 0.50, 0.75}; // NOX_BT_WFO_PROFIT_GRID
    // Values >= 1.0 all clamp to the SAME level for a long (its value floors
    // at 0 — see BacktestFillModel.hpp's stopLossLevel, audit §3 C2), so a
    // grid of {1.5, 2.0, 3.0} was a no-op for every long strategy: every
    // candidate mapped to the identical unreachable-then-clamped level, so
    // the grid search always "chose" stop_grid.front() by construction, not
    // by comparison. These values are < 1.0 so they actually differ once
    // clamped (50%/75%/100% of premium at risk before bailing).
    std::vector<double> stop_grid   = {0.50, 0.75, 1.0};   // NOX_BT_WFO_STOP_GRID

    static WalkForwardConfig fromEnv() {
        WalkForwardConfig cfg;
        if (const char* v = std::getenv("NOX_BT_WFO_ENABLED")) {
            std::string s(v);
            cfg.enabled = (s == "1" || s == "true" || s == "yes");
        }
        if (const char* v = std::getenv("NOX_BT_WFO_TRAIN_DAYS")) {
            try { cfg.train_days = std::stoi(v); } catch (...) {}
        }
        if (const char* v = std::getenv("NOX_BT_WFO_TEST_DAYS")) {
            try { cfg.test_days = std::stoi(v); } catch (...) {}
        }
        if (const char* v = std::getenv("NOX_BT_WFO_STEP_DAYS")) {
            try { cfg.step_days = std::stoi(v); } catch (...) {}
        }
        auto parseGrid = [](const char* env_name, std::vector<double>& out) {
            if (const char* v = std::getenv(env_name)) {
                std::vector<double> vals;
                std::istringstream ss(v);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    if (tok.empty()) continue;
                    try { vals.push_back(std::stod(tok)); } catch (...) {}
                }
                if (!vals.empty()) out = vals;
            }
        };
        parseGrid("NOX_BT_WFO_PROFIT_GRID", cfg.profit_grid);
        parseGrid("NOX_BT_WFO_STOP_GRID",   cfg.stop_grid);
        return cfg;
    }
};

struct WalkForwardFold {
    size_t train_start = 0, train_end = 0; // [train_start, train_end)
    size_t test_start  = 0, test_end  = 0; // [test_start, test_end); test_start == train_end
};

// Generates walk-forward folds sliding by step_days over [0, n_bars). Each
// fold's test window immediately follows (never overlaps, never precedes)
// its own train window — the defining walk-forward property, distinct from
// k-fold cross-validation where folds can be evaluated in any order.
inline std::vector<WalkForwardFold> generateFolds(size_t n_bars, int train_days,
                                                  int test_days, int step_days) {
    std::vector<WalkForwardFold> folds;
    if (train_days <= 0 || test_days <= 0 || step_days <= 0) return folds;

    size_t train_start = 0;
    while (true) {
        size_t train_end = train_start + static_cast<size_t>(train_days);
        size_t test_start = train_end;
        size_t test_end   = test_start + static_cast<size_t>(test_days);
        if (test_end > n_bars) break;
        folds.push_back({train_start, train_end, test_start, test_end});
        train_start += static_cast<size_t>(step_days);
    }
    return folds;
}

} // namespace nox::backtest
