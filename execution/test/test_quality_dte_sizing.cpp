// Verifies the quality-driven DTE selection and DTE/quality-tiered sizing
// added on top of buildContractParams()/assembleSignal(): a low-conviction
// setup on a theta-sensitive long-premium strategy gets pushed to a longer
// DTE floor (the AAPL scenario this was built to prevent — a 7-DTE OTM debit
// spread with no momentum behind it), a high-conviction setup is allowed to
// relax toward the existing global DTE floor, CSP/CC are untouched by any of
// this, and sizing composes a quality multiplier plus a hard short-DTE risk
// ceiling on top of the existing capital-tier budget.
//
// These are private members, reached via the same NOX_UNIT_TEST-gated friend
// pattern used throughout execution/test/ (see test_dte_earnings_fix.cpp).

#include "../httplib.h"

#ifndef NOX_UNIT_TEST
#define NOX_UNIT_TEST
#endif
#include "../OptionsSignalGenerator.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

using nox::options_signal::OptionsSignalGenerator;
using nox::options_signal::RiskProfile;
using nox::options_signal::OptionsSignal;
using nox::options_signal::UnderlyingData;

struct NoxUnitTestAccess {
    using ContractParams  = OptionsSignalGenerator::ContractParams;

    static ContractParams buildContractParams(const OptionsSignalGenerator& gen,
                                               const std::string& strategy,
                                               double spot, double atr, double rfr,
                                               double iv_sigma, double hrv30,
                                               double quality_score) {
        return gen.buildContractParams(strategy, spot, atr, rfr, iv_sigma, hrv30, quality_score);
    }

    static OptionsSignal assembleSignal(const OptionsSignalGenerator& gen,
                                         const std::string& ticker, const UnderlyingData& d,
                                         const std::string& strategy, const ContractParams& cp,
                                         double iv_rank, double iv_sigma, double rfr,
                                         double confidence, const std::string& tier,
                                         bool fc_mode, double allocated_capital, double hrv30,
                                         double quality_score) {
        return gen.assembleSignal(ticker, d, strategy, cp, iv_rank, iv_sigma, rfr,
                                   confidence, tier, fc_mode, allocated_capital, hrv30,
                                   quality_score);
    }
};

using ContractParams = NoxUnitTestAccess::ContractParams;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static RiskProfile makeProfile() {
    RiskProfile p;
    p.name = "TEST";
    return p;
}

static int resolvedDte(const ContractParams& cp) {
    return static_cast<int>(std::round(cp.expiry * 365.0));
}

static UnderlyingData makeUnderlying() {
    UnderlyingData d;
    d.price     = 150.0;
    d.sma20     = 145.0;
    d.sma50     = 140.0;
    d.rsi14     = 55.0;
    d.atr14     = 3.0;
    d.hrv30     = 0.20;
    d.vol20_avg = 2'000'000.0;
    d.vol_ratio = 1.1;
    d.macd_hist = 0.5;
    d.valid     = true;
    return d;
}

int main() {
    std::cout << "=== Quality-driven DTE + sizing tests ===\n\n";
    OptionsSignalGenerator gen("http://127.0.0.1:1", "k", "s", "", "", makeProfile());

    // ── 1. Low-quality debit spread gets pushed to the low-quality DTE floor,
    //      recreating and fixing the AAPL scenario (7-DTE OTM debit spread on
    //      a weak setup) ──────────────────────────────────────────────────
    {
        RiskProfile p = makeProfile();
        p.dte_spread = 7; // mirrors the original AAPL trade's short target
        OptionsSignalGenerator gen_short("http://127.0.0.1:1", "k", "s", "", "", p);
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen_short, "BULL_CALL_SPREAD", 150.0, 3.0, 0.05, 0.30, 0.20, /*quality*/0.10);
        CHECK(resolvedDte(cp) == 21,
              "low-quality BULL_CALL_SPREAD pushed to QUALITY_DTE_LOW_FLOOR_DAYS (21), got " +
              std::to_string(resolvedDte(cp)));
    }

    // ── 2. High-quality relaxes toward (never below) the global floor ─────
    {
        auto cp_high = NoxUnitTestAccess::buildContractParams(
            gen, "BULL_CALL_SPREAD", 150.0, 3.0, 0.05, 0.30, 0.20, /*quality*/0.90);
        CHECK(resolvedDte(cp_high) == 22,
              "high-quality BULL_CALL_SPREAD relaxes to ~22 DTE (45*(1-0.5)), got " +
              std::to_string(resolvedDte(cp_high)));

        auto cp_mid = NoxUnitTestAccess::buildContractParams(
            gen, "BULL_CALL_SPREAD", 150.0, 3.0, 0.05, 0.30, 0.20, /*quality*/0.40);
        CHECK(resolvedDte(cp_mid) == 45,
              "mid-band quality leaves profile default (45) unchanged, got " +
              std::to_string(resolvedDte(cp_mid)));
    }

    // ── 3. MIN_DTE_FLOOR always wins, even under aggressive high-quality relax ──
    {
        setenv("MIN_DTE_FLOOR", "30", 1);
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen, "BULL_CALL_SPREAD", 150.0, 3.0, 0.05, 0.30, 0.20, /*quality*/0.99);
        unsetenv("MIN_DTE_FLOOR");
        CHECK(resolvedDte(cp) == 30,
              "MIN_DTE_FLOOR=30 overrides high-quality relax, got " +
              std::to_string(resolvedDte(cp)));
    }

    // ── 4. Strategy scoping: CSP/CC never get the low-quality push ────────
    {
        RiskProfile p = makeProfile();
        p.dte_income = 5;
        OptionsSignalGenerator gen_income("http://127.0.0.1:1", "k", "s", "", "", p);
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen_income, "CSP", 150.0, 3.0, 0.05, 0.30, 0.20, /*quality*/0.05);
        CHECK(resolvedDte(cp) == 7,
              "CSP with low quality stays at the global MIN_DTE_FLOOR (7), not the "
              "21-day low-quality floor — got " + std::to_string(resolvedDte(cp)));
    }

    // ── 5. Sizing: quality multiplier scales contracts/max_risk when DTE is
    //      long enough that the short-DTE ceiling doesn't apply ────────────
    {
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen, "LONG_CALL", 150.0, 3.0, 0.05, 0.25, 0.20, /*quality*/0.50);
        CHECK(resolvedDte(cp) > 14, "sizing test fixture uses long enough DTE to skip the ceiling");

        UnderlyingData d = makeUnderlying();
        OptionsSignal sig_low = NoxUnitTestAccess::assembleSignal(
            gen, "AAPL", d, "LONG_CALL", cp, 50.0, 0.25, 0.05, 1.0, "STANDARD",
            false, 1'000'000.0, 0.20, /*quality*/0.10);
        OptionsSignal sig_high = NoxUnitTestAccess::assembleSignal(
            gen, "AAPL", d, "LONG_CALL", cp, 50.0, 0.25, 0.05, 1.0, "STANDARD",
            false, 1'000'000.0, 0.20, /*quality*/0.90);

        CHECK(sig_high.max_risk >= sig_low.max_risk,
              "high-quality sizing budget >= low-quality (multiplier composes upward)");
        // With capital large relative to per-contract cost, floor rounding is
        // small — the ratio should track QUALITY_SIZE_MULT_MAX/MIN (1.15/0.60 ≈ 1.92).
        if (sig_low.max_risk > 0.0) {
            double ratio = sig_high.max_risk / sig_low.max_risk;
            CHECK(ratio > 1.5 && ratio < 2.3,
                  "high/low max_risk ratio tracks the quality multiplier spread, got " +
                  std::to_string(ratio));
        }
    }

    // ── 6. Hard short-DTE ceiling caps risk regardless of tier, with partial
    //      quality relax — both must stay within their respective ceilings ──
    {
        RiskProfile p = makeProfile();
        p.dte_spread = 10; // short — triggers SHORT_DTE_THRESHOLD_DAYS (14) ceiling
        OptionsSignalGenerator gen_short("http://127.0.0.1:1", "k", "s", "", "", p);
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen_short, "BULL_CALL_SPREAD", 150.0, 3.0, 0.05, 0.30, 0.20, /*quality*/0.40);
        CHECK(resolvedDte(cp) <= 14, "fixture DTE is at/under the short-DTE threshold");

        // Capital large enough relative to per-contract cost that the ceiling
        // itself binds (not the pre-existing "at least 1 contract" floor).
        const double capital = 100'000.0;
        UnderlyingData d = makeUnderlying();
        OptionsSignal sig_low = NoxUnitTestAccess::assembleSignal(
            gen_short, "AAPL", d, "BULL_CALL_SPREAD", cp, 50.0, 0.30, 0.05, 1.0, "ADVANCED",
            false, capital, 0.20, /*quality*/0.10);
        OptionsSignal sig_high = NoxUnitTestAccess::assembleSignal(
            gen_short, "AAPL", d, "BULL_CALL_SPREAD", cp, 50.0, 0.30, 0.05, 1.0, "ADVANCED",
            false, capital, 0.20, /*quality*/0.90);

        // Low quality: ceiling stays at the raw SHORT_DTE_RISK_PCT_CEILING (1%),
        // then the low-quality size multiplier (0.60) scales it down further.
        CHECK(sig_low.max_risk <= capital * 0.010 * 0.60 + 1e-6,
              "low-quality short-DTE trade respects the 1% hard ceiling × 0.60 multiplier, max_risk=" +
              std::to_string(sig_low.max_risk));
        // High quality: ceiling relaxes halfway toward the tier's own pct (2%
        // for ADVANCED) → effective ceiling 1.5%, then the 1.15 size multiplier.
        CHECK(sig_high.max_risk <= capital * 0.015 * 1.15 + 1e-6,
              "high-quality short-DTE trade respects its relaxed ceiling × 1.15 multiplier, max_risk=" +
              std::to_string(sig_high.max_risk));
        CHECK(sig_high.max_risk > sig_low.max_risk,
              "high-quality short-DTE trade gets a larger (but still capped) budget than low-quality");
    }

    // ── 7. Env fail-open: unsetting all new env vars must not crash and must
    //      fall back to documented defaults ──────────────────────────────
    {
        unsetenv("QUALITY_DTE_HIGH_THRESHOLD");
        unsetenv("QUALITY_DTE_LOW_THRESHOLD");
        unsetenv("QUALITY_DTE_LOW_FLOOR_DAYS");
        unsetenv("QUALITY_DTE_HIGH_RELAX_PCT");
        unsetenv("QUALITY_SIZE_MULT_MIN");
        unsetenv("QUALITY_SIZE_MULT_MAX");
        unsetenv("SHORT_DTE_THRESHOLD_DAYS");
        unsetenv("SHORT_DTE_RISK_PCT_CEILING");
        unsetenv("SHORT_DTE_QUALITY_CEILING_RELAX");
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen, "LONG_CALL", 150.0, 3.0, 0.05, 0.25, 0.20, 0.5);
        CHECK(cp.expiry > 0.0, "fail-open: buildContractParams still produces a valid expiry with no env set");
    }

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "✅ All quality DTE/sizing tests passed.\n";
        return 0;
    }
    std::cout << "❌ " << g_failures << " test(s) failed.\n";
    return 1;
}
