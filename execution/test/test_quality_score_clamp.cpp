// Verifies computeQualityScore() keeps its two highest-weighted, otherwise-
// unbounded components (sma_distance_atrs 40%, vol_deviation 20%) normalized
// to [0,1] before weighting. Before this fix, a ticker mid a sustained trend
// (several raw ATRs from its SMA20) could push quality_score well past the
// 0.55 "elite setup" ceiling the DTE/sizing logic elsewhere assumes, letting
// raw trend magnitude swamp RSI/MACD/volume confirmation in run_scan()'s
// per-scan ranking sort. No network calls — pure math.

#ifndef NOX_UNIT_TEST
#define NOX_UNIT_TEST
#endif
#include "../OptionsSignalGenerator.hpp"

#include <cmath>
#include <iostream>

using nox::options_signal::OptionsSignalGenerator;
using nox::options_signal::QualityScore;
using nox::options_signal::UnderlyingData;

struct NoxUnitTestAccess {
    static QualityScore computeQualityScore(const UnderlyingData& d, double iv_sigma,
                                              double rsi, const std::string& strategy) {
        return OptionsSignalGenerator::computeQualityScore(d, iv_sigma, rsi, strategy);
    }
};

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

int main() {
    std::cout << "=== computeQualityScore() clamp tests ===\n\n";

    {
        std::cout << "\n[extreme trend] 6 raw ATRs from SMA20 does not blow past 1.0\n";
        UnderlyingData d;
        d.price = 106.0; d.sma20 = 100.0; d.atr14 = 1.0; // 6.0 raw ATRs
        d.hrv30 = 0.20;
        d.vol_ratio = 1.0;
        d.macd_hist = 0.0;
        auto q = NoxUnitTestAccess::computeQualityScore(d, /*iv_sigma=*/0.20, /*rsi=*/50.0, "LONG_CALL");
        CHECK(q.quality_score <= 1.0, "quality_score clamped to <= 1.0 despite a 6-ATR trend");
        CHECK(q.sma_distance_atrs == 6.0, "raw sma_distance_atrs is preserved unclamped for logging");
    }

    {
        std::cout << "\n[extreme vol deviation] IV 5x HRV does not blow past 1.0\n";
        UnderlyingData d;
        d.price = 101.0; d.sma20 = 100.0; d.atr14 = 5.0; // ~0.2 raw ATRs, small
        d.hrv30 = 0.20;
        d.vol_ratio = 1.0;
        d.macd_hist = 0.0;
        auto q = NoxUnitTestAccess::computeQualityScore(d, /*iv_sigma=*/1.00, /*rsi=*/50.0, "LONG_CALL");
        CHECK(q.quality_score <= 1.0, "quality_score clamped to <= 1.0 despite a 5x IV/HRV deviation");
        CHECK(std::abs(q.vol_deviation - 4.0) < 1e-9, "raw vol_deviation is preserved unclamped for logging");
    }

    {
        std::cout << "\n[max everything] all five components maxed still caps at 1.0\n";
        UnderlyingData d;
        d.price = 130.0; d.sma20 = 100.0; d.atr14 = 1.0; // 30 raw ATRs — absurdly extreme
        d.hrv30 = 0.10;
        d.vol_ratio = 3.0; // vol_boost caps at 1.0 already
        d.macd_hist = 1.0;
        auto q = NoxUnitTestAccess::computeQualityScore(d, /*iv_sigma=*/2.00, /*rsi=*/100.0, "LONG_CALL");
        CHECK(q.quality_score <= 1.0 + 1e-9, "sum of five weighted, each-capped components never exceeds 1.0");
    }

    {
        std::cout << "\n[normal setup] a modest, realistic setup still scores in a sane sub-1.0 range\n";
        UnderlyingData d;
        d.price = 101.5; d.sma20 = 100.0; d.atr14 = 1.0; // 1.5 ATRs — a normal breakout, not extreme
        d.hrv30 = 0.20;
        d.vol_ratio = 1.4;
        d.macd_hist = 0.5;
        auto q = NoxUnitTestAccess::computeQualityScore(d, /*iv_sigma=*/0.24, /*rsi=*/62.0, "LONG_CALL");
        CHECK(q.quality_score > 0.0 && q.quality_score < 1.0, "realistic setup produces a non-trivial, non-saturated score");
    }

    std::cout << "\n" << (g_failures == 0 ? "✅ All quality-score clamp tests passed.\n"
                                           : "❌ " + std::to_string(g_failures) + " test(s) FAILED.\n");
    return g_failures == 0 ? 0 : 1;
}
