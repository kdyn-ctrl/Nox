// test_skeptic_intelligence.cpp — the decision layer that wires WS2 (alt-macro),
// WS3 (insider clusters) and the new China information-lag feed into sizing/
// gating. SkepticIntelligence.hpp is pure aggregation math (no HTTP/JSON/I/O),
// so these tests build Inputs directly. The live network fetch+parse lives in
// OptionsSignalGenerator; only the decision is exercised here.

#include "../SkepticIntelligence.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace nox::skeptic;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  \xE2\x9C\x97 FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  \xE2\x9C\x93 " << (msg) << "\n"; } \
} while (0)

static Knobs baseKnobs() {
    // Fixed knobs so the test is independent of any SKEPTIC_* env in the shell.
    Knobs k;
    k.insider_boost           = 1.25;
    k.insider_min_execs       = 2;
    k.insider_conflict_cut    = 0.75;
    k.altmacro_align_boost    = 1.15;
    k.altmacro_oppose_cut     = 0.60;
    k.altmacro_contradict_cut = 0.45;
    k.china_align_boost       = 1.30;
    k.china_fresh_extra       = 1.15;
    k.china_oppose_cut        = 0.50;
    k.size_mult_min           = 0.40;
    k.size_mult_max           = 1.60;
    k.suppress_enabled        = true;
    k.suppress_threshold      = 0.50;
    return k;
}

static bool approx(double a, double b) { return std::abs(a - b) < 1e-6; }

static void test_no_signals_is_noop() {
    std::cout << "\n[empty] no skeptic inputs -> 1.0x, no suppress\n";
    auto d = decide(Dir::Bullish, Inputs{}, baseKnobs());
    CHECK(approx(d.size_mult, 1.0), "empty inputs leave sizing unchanged");
    CHECK(!d.suppress, "empty inputs never suppress");
    CHECK(d.reason == "skeptic_neutral", "empty inputs report neutral");
}

static void test_insider_boosts_aligned_bull() {
    std::cout << "\n[insider] cluster buy + bullish trade -> boost\n";
    Inputs in; in.insider = {true, 3};
    auto d = decide(Dir::Bullish, in, baseKnobs());
    CHECK(approx(d.size_mult, 1.25), "3-insider cluster boosts a bullish trade 1.25x");
    CHECK(d.reason == "skeptic_boost", "reported as a boost");
}

static void test_insider_cuts_opposed_bear() {
    std::cout << "\n[insider] cluster buy vs a bearish trade -> cut\n";
    Inputs in; in.insider = {true, 3};
    auto d = decide(Dir::Bearish, in, baseKnobs());
    CHECK(approx(d.size_mult, 0.75), "insiders buying while short cuts sizing 0.75x");
    CHECK(!d.suppress, "a soft insider conflict cuts but never suppresses");
}

static void test_insider_below_min_execs_ignored() {
    std::cout << "\n[insider] single insider (< min_execs) -> ignored\n";
    Inputs in; in.insider = {true, 1};
    auto d = decide(Dir::Bullish, in, baseKnobs());
    CHECK(approx(d.size_mult, 1.0), "a lone insider below min_execs does not move sizing");
}

static void test_insider_neutral_trade_no_change() {
    std::cout << "\n[insider] cluster buy on a NEUTRAL (vol) trade -> no size change\n";
    Inputs in; in.insider = {true, 4};
    auto d = decide(Dir::Neutral, in, baseKnobs());
    CHECK(approx(d.size_mult, 1.0), "directional insider info can't size a straddle");
}

static void test_altmacro_aligned_and_opposed() {
    std::cout << "\n[altmacro] aligned boosts; opposed cuts\n";
    Inputs a; a.alt_macro = {true, Dir::Bullish, 0.6, false};
    CHECK(approx(decide(Dir::Bullish, a, baseKnobs()).size_mult, 1.15),
          "physical supply bullish + bullish trade -> 1.15x");
    Inputs b; b.alt_macro = {true, Dir::Bearish, 0.6, false};
    CHECK(approx(decide(Dir::Bullish, b, baseKnobs()).size_mult, 0.60),
          "physical supply bearish + bullish trade -> 0.60x cut");
}

static void test_altmacro_contradiction_hard_cut_and_suppress() {
    std::cout << "\n[altmacro] text-contradicts-physical, opposed -> hard cut + suppress\n";
    Inputs in; in.alt_macro = {true, Dir::Bearish, 0.8, true};
    auto d = decide(Dir::Bullish, in, baseKnobs());
    CHECK(approx(d.size_mult, 0.45), "contradiction-opposed applies the 0.45x hard cut");
    CHECK(d.suppress, "hard opposition at/under threshold suppresses the entry");
    CHECK(d.reason == "suppressed_skeptic", "suppression reason slug is set");
}

static void test_china_fresh_aligned_stacks() {
    std::cout << "\n[china] fresh aligned release stacks align x fresh, then clamps\n";
    Inputs in; in.china = {true, Dir::Bullish, 0.7, true, "caixin_pmi"};
    auto d = decide(Dir::Bullish, in, baseKnobs());
    // 1.30 * 1.15 = 1.495, under the 1.60 ceiling
    CHECK(approx(d.size_mult, 1.495), "fresh aligned china lag = align x fresh = 1.495x");
    CHECK(d.reason == "skeptic_boost", "reported as a boost");
}

static void test_china_fresh_opposed_suppresses() {
    std::cout << "\n[china] fresh opposed release -> hard opposition, suppress\n";
    Inputs in; in.china = {true, Dir::Bearish, 0.7, true, "caixin_pmi"};
    auto d = decide(Dir::Bullish, in, baseKnobs());
    CHECK(approx(d.size_mult, 0.50), "opposed china lag cuts to 0.50x");
    CHECK(d.suppress, "a FRESH opposed release is hard opposition -> suppress");
}

static void test_china_stale_opposed_cuts_no_suppress() {
    std::cout << "\n[china] stale opposed release -> cut but NOT suppress\n";
    Inputs in; in.china = {true, Dir::Bearish, 0.7, false, "caixin_pmi"};
    auto d = decide(Dir::Bullish, in, baseKnobs());
    CHECK(approx(d.size_mult, 0.50), "stale opposed china lag still cuts to 0.50x");
    CHECK(!d.suppress, "a STALE (already-priced) opposed release only sizes down");
}

static void test_clamp_ceiling() {
    std::cout << "\n[clamp] stacked boosts cannot exceed the ceiling\n";
    Inputs in;
    in.insider = {true, 5};
    in.alt_macro = {true, Dir::Bullish, 0.9, false};
    in.china = {true, Dir::Bullish, 0.9, true, "caixin_pmi"};
    auto d = decide(Dir::Bullish, in, baseKnobs());
    // raw = 1.25*1.15*1.30*1.15 = 2.15 -> clamped to 1.60
    CHECK(approx(d.size_mult, 1.60), "combined boost clamps at size_mult_max");
    CHECK(!d.suppress, "an all-aligned setup never suppresses");
}

static void test_suppress_disabled_only_cuts() {
    std::cout << "\n[suppress-off] disabling suppression still sizes down on hard opposition\n";
    Knobs k = baseKnobs();
    k.suppress_enabled = false;
    Inputs in; in.china = {true, Dir::Bearish, 0.7, true, "caixin_pmi"};
    auto d = decide(Dir::Bullish, in, k);
    CHECK(!d.suppress, "suppression disabled -> never suppress");
    CHECK(approx(d.size_mult, 0.50), "but the size cut still applies");
}

static void test_env_override() {
    std::cout << "\n[env] Knobs::fromEnv picks up an override\n";
    setenv("SKEPTIC_INSIDER_BOOST", "1.50", 1);
    Knobs k = Knobs::fromEnv();
    CHECK(approx(k.insider_boost, 1.50), "SKEPTIC_INSIDER_BOOST env override honored");
    unsetenv("SKEPTIC_INSIDER_BOOST");
    Knobs k2 = Knobs::fromEnv();
    CHECK(approx(k2.insider_boost, 1.25), "falls back to the fake-safe default when unset");
}

int main() {
    std::cout << "=== SkepticIntelligence decision-layer tests ===\n";
    test_no_signals_is_noop();
    test_insider_boosts_aligned_bull();
    test_insider_cuts_opposed_bear();
    test_insider_below_min_execs_ignored();
    test_insider_neutral_trade_no_change();
    test_altmacro_aligned_and_opposed();
    test_altmacro_contradiction_hard_cut_and_suppress();
    test_china_fresh_aligned_stacks();
    test_china_fresh_opposed_suppresses();
    test_china_stale_opposed_cuts_no_suppress();
    test_clamp_ceiling();
    test_suppress_disabled_only_cuts();
    test_env_override();

    std::cout << "\n";
    if (g_failures == 0) { std::cout << "All SkepticIntelligence tests passed.\n"; return 0; }
    std::cout << g_failures << " SkepticIntelligence test(s) failed.\n";
    return 1;
}
