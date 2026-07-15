// Verifies the sector/trend macro gate: computeEma() correctness on known
// series, and sectorConflicts()'s truth table (bullish/bearish bias ×
// sector uptrend/downtrend/neutral/invalid), including its fail-open
// behavior on an unfetchable snapshot or a neutral bias. No network calls —
// fetchSectorTrend() itself isn't exercised here: this project tests the
// pure-math/decision layer, not live HTTP.

#include "../httplib.h"

#ifndef NOX_UNIT_TEST
#define NOX_UNIT_TEST
#endif
#include "../OptionsSignalGenerator.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using nox::options_signal::OptionsSignalGenerator;
using nox::options_signal::RiskProfile;
using nox::options_signal::DirectionalBias;

struct NoxUnitTestAccess {
    using SectorSnapshot = OptionsSignalGenerator::SectorSnapshot;

    static double computeEma(const std::vector<double>& closes, int period) {
        return OptionsSignalGenerator::computeEma(closes, period);
    }
    static bool sectorConflicts(DirectionalBias bias, const SectorSnapshot& sec) {
        return OptionsSignalGenerator::sectorConflicts(bias, sec);
    }
};

using SectorSnapshot = NoxUnitTestAccess::SectorSnapshot;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  \xE2\x9C\x97 FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  \xE2\x9C\x93 " << (msg) << "\n"; } \
} while (0)

int main() {
    std::cout << "=== Sector/trend gate tests ===\n\n";

    // ── computeEma() ───────────────────────────────────────────────────────
    {
        // Strongly, steadily uptrending series: EMA20 (fast) should sit above
        // EMA50 (slow) once both are computable.
        std::vector<double> up;
        for (int i = 0; i < 60; ++i) up.push_back(100.0 + i * 1.0);
        double fast = NoxUnitTestAccess::computeEma(up, 20);
        double slow = NoxUnitTestAccess::computeEma(up, 50);
        CHECK(fast > 0.0 && slow > 0.0, "computeEma produces positive values on a valid series");
        CHECK(fast > slow, "EMA20 > EMA50 on a steadily uptrending series");

        std::vector<double> down;
        for (int i = 0; i < 60; ++i) down.push_back(200.0 - i * 1.0);
        double dfast = NoxUnitTestAccess::computeEma(down, 20);
        double dslow = NoxUnitTestAccess::computeEma(down, 50);
        CHECK(dfast < dslow, "EMA20 < EMA50 on a steadily downtrending series");

        // Too-short series → 0.0 sentinel (caller treats as invalid).
        std::vector<double> tooShort{1.0, 2.0, 3.0};
        CHECK(NoxUnitTestAccess::computeEma(tooShort, 20) == 0.0,
              "computeEma returns 0.0 sentinel when series shorter than period");
    }

    // ── sectorConflicts() truth table ──────────────────────────────────────
    {
        SectorSnapshot downtrend;
        downtrend.price = 90.0; downtrend.ema_fast = 95.0; downtrend.ema_slow = 100.0; downtrend.valid = true;

        SectorSnapshot uptrend;
        uptrend.price = 110.0; uptrend.ema_fast = 105.0; uptrend.ema_slow = 100.0; uptrend.valid = true;

        CHECK(NoxUnitTestAccess::sectorConflicts(DirectionalBias::Bullish, downtrend) == true,
              "bullish bias + sector downtrend -> conflict");
        CHECK(NoxUnitTestAccess::sectorConflicts(DirectionalBias::Bullish, uptrend) == false,
              "bullish bias + sector uptrend -> no conflict");
        CHECK(NoxUnitTestAccess::sectorConflicts(DirectionalBias::Bearish, uptrend) == true,
              "bearish bias + sector uptrend -> conflict");
        CHECK(NoxUnitTestAccess::sectorConflicts(DirectionalBias::Bearish, downtrend) == false,
              "bearish bias + sector downtrend -> no conflict");
        CHECK(NoxUnitTestAccess::sectorConflicts(DirectionalBias::Neutral, downtrend) == false,
              "neutral bias never conflicts — nothing directional to contradict");
        CHECK(NoxUnitTestAccess::sectorConflicts(DirectionalBias::Neutral, uptrend) == false,
              "neutral bias never conflicts (uptrend case too)");

        SectorSnapshot invalid;
        invalid.valid = false;
        CHECK(NoxUnitTestAccess::sectorConflicts(DirectionalBias::Bullish, invalid) == false,
              "invalid/unfetchable snapshot fails open — no suppression");

        SectorSnapshot flat;
        flat.price = 100.0; flat.ema_fast = 100.0; flat.ema_slow = 100.0; flat.valid = true;
        CHECK(NoxUnitTestAccess::sectorConflicts(DirectionalBias::Bullish, flat) == false,
              "flat/ambiguous trend (fast == slow) does not register as a conflict");
    }

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "All sector/trend gate tests passed.\n";
        return 0;
    }
    std::cout << g_failures << " test(s) FAILED.\n";
    return 1;
}
