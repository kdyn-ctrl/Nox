// Pure-math tests for fractional-Kelly sizing (execution/BacktestKellySizing.hpp)
// — no OpenSSL/SQLite dependency.

#include "../BacktestKellySizing.hpp"

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
    std::cout << "=== BacktestKellySizing tests ===\n\n";

    std::cout << "[calculateKellyContracts — positive edge]\n";
    {
        // W=0.6842, R=2.316 (the documented live-engine OOS example) -> raw
        // Kelly ~54.8%. fraction=0.15 -> adjusted ~8.2%, below the 10% cap.
        int c = calculateKellyContracts(35000.0, 200.0, 0.6842, 2.316, 0.15, 0.10);
        CHECK(c > 0, "positive-edge trade sizes to a positive contract count (got " + std::to_string(c) + ")");
        // 35000 * 0.082 / 200 ≈ 14 contracts
        CHECK(c >= 12 && c <= 16, "sized contracts roughly match hand-computed adjusted Kelly (got " + std::to_string(c) + ")");
    }

    std::cout << "\n[calculateKellyContracts — hard cap clamps]\n";
    {
        // fraction=1.0 with the same W/R -> raw*1.0 = 54.8%, clamped to 10%.
        int c = calculateKellyContracts(35000.0, 200.0, 0.6842, 2.316, 1.0, 0.10);
        int expected = static_cast<int>(std::floor(35000.0 * 0.10 / 200.0));
        CHECK(c == expected, "adjusted risk clamped to hard cap before sizing (got " + std::to_string(c) + ", expected " + std::to_string(expected) + ")");
    }

    std::cout << "\n[calculateKellyContracts — negative Kelly halts]\n";
    {
        // W=0.3, R=1.0 -> kelly_pct = 0.3 - 0.7/1.0 = -0.4 <= 0.
        int c = calculateKellyContracts(35000.0, 200.0, 0.3, 1.0, 0.15, 0.10);
        CHECK(c == -1, "non-positive raw Kelly returns -1 (RULE-005 halt), not a forced 1 contract");
    }

    std::cout << "\n[calculateKellyContracts — sub-1-contract allocation halts]\n";
    {
        // Small edge, huge option price -> dollar_amount < option_price_per_contract.
        int c = calculateKellyContracts(1000.0, 50000.0, 0.55, 1.2, 0.15, 0.10);
        CHECK(c == -1, "sub-1-contract allocation halts rather than forcing 1 contract");
    }

    std::cout << "\n[calculateKellyContracts — invalid inputs halt]\n";
    {
        CHECK(calculateKellyContracts(35000.0, 0.0, 0.6, 2.0, 0.15, 0.10) == -1,
              "zero option price halts");
        CHECK(calculateKellyContracts(35000.0, 200.0, 0.6, 0.0, 0.15, 0.10) == -1,
              "zero win/loss ratio halts");
    }

    std::cout << "\n[RollingTradeStats — causal accumulation]\n";
    {
        RollingTradeStats stats(20);
        CHECK(stats.count() == 0, "starts empty");
        stats.record(1.0);  // win
        stats.record(1.0);  // win
        stats.record(-0.5); // loss
        stats.record(-0.5); // loss
        CHECK(stats.count() == 4, "count reflects recorded trades");
        auto [wr, wlr] = stats.stats();
        CHECK(std::abs(wr - 0.5) < 1e-9, "win rate = 0.5 for 2W/2L (got " + std::to_string(wr) + ")");
        CHECK(std::abs(wlr - 2.0) < 1e-9, "win/loss ratio = avg_win/avg_loss = 1.0/0.5 = 2.0 (got " + std::to_string(wlr) + ")");
    }

    std::cout << "\n[RollingTradeStats — window eviction]\n";
    {
        RollingTradeStats stats(3);
        stats.record(1.0);
        stats.record(1.0);
        stats.record(1.0);
        stats.record(-1.0); // evicts the oldest win
        CHECK(stats.count() == 3, "window caps at configured size");
        auto [wr, wlr] = stats.stats();
        CHECK(std::abs(wr - (2.0 / 3.0)) < 1e-9, "win rate reflects only the trailing window (got " + std::to_string(wr) + ")");
    }

    std::cout << "\n[RollingTradeStats — no losses yet returns wlr=0]\n";
    {
        RollingTradeStats stats(10);
        stats.record(1.0);
        stats.record(2.0);
        auto [wr, wlr] = stats.stats();
        CHECK(wlr == 0.0, "win/loss ratio is 0.0 (not a divide-by-zero) with no losses recorded yet");
    }

    std::cout << "\n[KellySizingConfig::fromEnv defaults]\n";
    {
        unsetenv("NOX_BT_KELLY_ENABLED");
        unsetenv("NOX_BT_KELLY_CAP");
        unsetenv("NOX_BT_KELLY_WINDOW");
        unsetenv("NOX_BT_KELLY_SWEEP");
        auto cfg = KellySizingConfig::fromEnv();
        CHECK(!cfg.enabled, "disabled by default (current-behavior-preserving)");
        CHECK(cfg.hard_cap == 0.10, "default hard cap 10%");
        CHECK(cfg.sweep_fractions.size() == 4, "default sweep has 4 fractions");
    }

    std::cout << "\n[KellySizingConfig::fromEnv overrides]\n";
    {
        setenv("NOX_BT_KELLY_ENABLED", "1", 1);
        setenv("NOX_BT_KELLY_CAP", "0.05", 1);
        setenv("NOX_BT_KELLY_WINDOW", "10", 1);
        setenv("NOX_BT_KELLY_SWEEP", "0.2,0.4", 1);
        auto cfg = KellySizingConfig::fromEnv();
        CHECK(cfg.enabled, "env override enables Kelly sizing");
        CHECK(cfg.hard_cap == 0.05, "env override -> hard cap 0.05");
        CHECK(cfg.rolling_window == 10, "env override -> window 10");
        CHECK(cfg.sweep_fractions.size() == 2 && cfg.sweep_fractions[0] == 0.2 && cfg.sweep_fractions[1] == 0.4,
              "env override -> sweep fractions [0.2, 0.4]");
        unsetenv("NOX_BT_KELLY_ENABLED");
        unsetenv("NOX_BT_KELLY_CAP");
        unsetenv("NOX_BT_KELLY_WINDOW");
        unsetenv("NOX_BT_KELLY_SWEEP");
    }

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "All BacktestKellySizing tests passed.\n";
        return 0;
    }
    std::cout << g_failures << " test(s) FAILED.\n";
    return 1;
}
