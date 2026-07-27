// Verifies the new REVERSE_IRON_CONDOR strategy is actually reachable from
// selectStrategy() for the scenario it was added for: cheap IV + no
// directional bias ("expect a breakout, don't know which way"). Also checks
// the existing bullish/bearish cheap-IV branches still map to their spreads.

#include "../httplib.h"

#ifndef NOX_UNIT_TEST
#define NOX_UNIT_TEST
#endif
#include "../OptionsSignalGenerator.hpp"

#include <cassert>
#include <iostream>
#include <string>

using nox::options_signal::OptionsSignalGenerator;
using nox::options_signal::RiskProfile;
using nox::options_signal::DirectionalBias;

struct NoxUnitTestAccess {
    static std::string selectStrategy(const OptionsSignalGenerator& gen,
                                       DirectionalBias bias, double iv_rank, double iv_level,
                                       double hrv, const std::string& tier,
                                       double sma_atrs, double rsi, bool above_sma50) {
        return gen.selectStrategy(bias, iv_rank, iv_level, hrv, tier, sma_atrs, rsi, above_sma50);
    }
};

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static RiskProfile makeProfile(bool enforce_tier_gates) {
    RiskProfile p;
    p.name = "TEST";
    p.enforce_tier_gates = enforce_tier_gates;
    return p;
}

int main() {
    std::cout << "=== REVERSE_IRON_CONDOR strategy-selection tests ===\n\n";

    // ADVANCED tier, tier gates enforced — REVERSE_IRON_CONDOR is allowed here
    // (falls into the same catch-all bucket as STRANGLE/STRADDLE).
    OptionsSignalGenerator gen("http://127.0.0.1:1", "k", "s", "", "", makeProfile(true));

    std::cout << "[neutral+cheap] no directional bias, IV cheap vs HRV -> REVERSE_IRON_CONDOR\n";
    // hrv=0.30, iv_level=0.20 -> iv_level < hrv*0.90 (0.27) -> vol_cheap=true
    std::string s1 = NoxUnitTestAccess::selectStrategy(gen, DirectionalBias::Neutral,
                                                        50.0, 0.20, 0.30, "ADVANCED",
                                                        0.0, 50.0, true);
    CHECK(s1 == "REVERSE_IRON_CONDOR", "neutral + cheap IV -> REVERSE_IRON_CONDOR (got: " + s1 + ")");

    std::cout << "\n[bullish+cheap] directional bullish bias, IV cheap -> BULL_CALL_SPREAD (unchanged)\n";
    std::string s2 = NoxUnitTestAccess::selectStrategy(gen, DirectionalBias::Bullish,
                                                        50.0, 0.20, 0.30, "ADVANCED",
                                                        0.0, 55.0, true);
    CHECK(s2 == "BULL_CALL_SPREAD", "bullish + cheap IV -> BULL_CALL_SPREAD (got: " + s2 + ")");

    std::cout << "\n[bearish+cheap] directional bearish bias, IV cheap -> BEAR_PUT_SPREAD (unchanged)\n";
    std::string s3 = NoxUnitTestAccess::selectStrategy(gen, DirectionalBias::Bearish,
                                                        50.0, 0.20, 0.30, "ADVANCED",
                                                        0.0, 45.0, true);
    CHECK(s3 == "BEAR_PUT_SPREAD", "bearish + cheap IV -> BEAR_PUT_SPREAD (got: " + s3 + ")");

    std::cout << "\n[tier-gated] STARTER tier cannot access REVERSE_IRON_CONDOR -> falls back to STRADDLE\n";
    std::string s4 = NoxUnitTestAccess::selectStrategy(gen, DirectionalBias::Neutral,
                                                        50.0, 0.20, 0.30, "STARTER",
                                                        0.0, 50.0, true);
    // STARTER tier only allows LONG_CALL/LONG_PUT per strategyAllowed() -> both
    // REVERSE_IRON_CONDOR and STRADDLE are blocked -> falls through to LONG_CALL.
    CHECK(s4 == "LONG_CALL", "STARTER tier neutral+cheap falls back to LONG_CALL (got: " + s4 + ")");

    std::cout << "\n[tier-open] personal/breakout profile (enforce_tier_gates=false) -> REVERSE_IRON_CONDOR allowed\n";
    OptionsSignalGenerator gen_open("http://127.0.0.1:1", "k", "s", "", "", makeProfile(false));
    std::string s5 = NoxUnitTestAccess::selectStrategy(gen_open, DirectionalBias::Neutral,
                                                        50.0, 0.20, 0.30, "STARTER",
                                                        0.0, 50.0, true);
    CHECK(s5 == "REVERSE_IRON_CONDOR", "tier gates off -> REVERSE_IRON_CONDOR reachable even on STARTER (got: " + s5 + ")");

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "✅ ALL STRATEGY-SELECTION TESTS PASSED\n";
        return 0;
    }
    std::cout << "❌ " << g_failures << " test(s) failed\n";
    return 1;
}
