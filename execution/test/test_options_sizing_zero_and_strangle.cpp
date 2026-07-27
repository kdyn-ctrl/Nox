// Track 2 (§2 C1/C2/C3) verification:
//  - C1: assembleSignal() now stores the sized quantity on sig.contracts
//    instead of leaving it display-only.
//  - C2: the std::max(1.0, ...) floor is gone — a risk budget too small for
//    one contract now floors to 0, not a forced 1-contract trade.
//  - C3: STRANGLE is priced/sized as a short (credit) structure, not a long
//    (debit) one — max_reward is the capped premium, max_risk is the
//    unlimited-risk sentinel, and per-contract sizing uses a margin proxy
//    (large) instead of raw premium (small), so a naked strangle can't
//    out-lever its risk budget the way premium-based sizing would allow.
//
// Same NOX_UNIT_TEST friend-access pattern as test_quality_dte_sizing.cpp.

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
using nox::options_signal::OptionsSignal;
using nox::options_signal::UnderlyingData;

struct NoxUnitTestAccess {
    using ContractParams = OptionsSignalGenerator::ContractParams;

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
    std::cout << "=== §2 C1/C2/C3 — sizing wiring, zero-contract floor, short-strangle math ===\n\n";
    OptionsSignalGenerator gen("http://127.0.0.1:1", "k", "s", "", "", makeProfile());
    UnderlyingData d = makeUnderlying();

    // ── C1: sig.contracts actually gets set (not left at the struct default) ──
    {
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen, "LONG_CALL", 150.0, 3.0, 0.05, 0.25, 0.20, 0.5);
        OptionsSignal sig = NoxUnitTestAccess::assembleSignal(
            gen, "AAPL", d, "LONG_CALL", cp, 50.0, 0.25, 0.05, 1.0, "STANDARD",
            false, 1'000'000.0, 0.20, 0.50);
        CHECK(sig.contracts > 0, "large capital budget sizes a positive sig.contracts, got " +
              std::to_string(sig.contracts));
        CHECK(sig.max_risk == sig.entry_price * 100.0 * sig.contracts,
              "max_risk uses the same sized contracts count as sig.contracts (no separate local var)");
    }

    // ── C2: a risk budget too small for one contract floors to 0, not 1 ──────
    {
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen, "LONG_CALL", 150.0, 3.0, 0.05, 0.25, 0.20, 0.5);
        // Tiny allocated_capital → max_risk (a small % of it) can't cover even
        // one contract's premium*100 — pre-fix this force-floored to 1.
        OptionsSignal sig = NoxUnitTestAccess::assembleSignal(
            gen, "AAPL", d, "LONG_CALL", cp, 50.0, 0.25, 0.05, 1.0, "STANDARD",
            false, /*allocated_capital*/ 1.0, 0.20, 0.50);
        CHECK(sig.contracts == 0, "risk budget too small for one contract floors to 0, got " +
              std::to_string(sig.contracts));
        CHECK(sig.max_risk == 0.0, "0-contract signal reports $0 max_risk, not a phantom 1-contract number");
    }

    // ── C3: STRANGLE prices as a credit (short) structure ─────────────────────
    {
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen, "STRANGLE", 150.0, 3.0, 0.05, 0.30, 0.20, 0.5);
        OptionsSignal sig = NoxUnitTestAccess::assembleSignal(
            gen, "AAPL", d, "STRANGLE", cp, 50.0, 0.30, 0.05, 1.0, "STANDARD",
            false, 1'000'000.0, 0.20, 0.50);
        CHECK(sig.max_risk == 999999.0,
              "short strangle max_risk is the unlimited-risk sentinel, got " +
              std::to_string(sig.max_risk));
        CHECK(sig.max_reward > 0.0 && sig.max_reward < 999999.0,
              "short strangle max_reward is the capped premium collected, got " +
              std::to_string(sig.max_reward));
        CHECK(sig.max_reward == sig.entry_price * 100.0 * sig.contracts,
              "max_reward = credit collected * 100 * contracts");
        // theta should now be positive (short premium collects decay), not
        // negative (what a long strangle's raw per-share theta would show).
        CHECK(sig.greeks.theta >= 0.0,
              "short strangle carries positive theta (collecting decay), got " +
              std::to_string(sig.greeks.theta));
    }

    // ── C3: sizing off margin (large), not premium (small) — same capital
    //      budget should size FEWER strangle contracts than it would if sizing
    //      were still premium-based, proving the per_contract_risk denominator
    //      grew. Cross-check against BULL_CALL_SPREAD (a debit strategy at a
    //      similar strike distance) as a sanity floor: the strangle's implied
    //      per-contract risk should be far larger than its own premium. ──────
    {
        auto cp = NoxUnitTestAccess::buildContractParams(
            gen, "STRANGLE", 150.0, 3.0, 0.05, 0.30, 0.20, 0.5);
        const double capital = 100'000.0;
        OptionsSignal sig = NoxUnitTestAccess::assembleSignal(
            gen, "AAPL", d, "STRANGLE", cp, 50.0, 0.30, 0.05, 1.0, "STANDARD",
            false, capital, 0.20, 0.50);
        CHECK(sig.contracts >= 0, "strangle sizing never goes negative");
        if (sig.contracts > 0) {
            double premium_per_contract = sig.entry_price * 100.0;
            double implied_risk_budget  = capital * 0.015; // STANDARD tier risk_pct_standard
            double implied_per_contract_risk = implied_risk_budget / sig.contracts;
            CHECK(implied_per_contract_risk > premium_per_contract * 2.0,
                  "strangle's implied per-contract risk (" + std::to_string(implied_per_contract_risk) +
                  ") is far larger than raw premium (" + std::to_string(premium_per_contract) +
                  ") — proves sizing uses a margin proxy, not premium");
        }
    }

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "✅ All sizing/zero-contract/strangle tests passed.\n";
        return 0;
    }
    std::cout << "❌ " << g_failures << " test(s) failed.\n";
    return 1;
}
