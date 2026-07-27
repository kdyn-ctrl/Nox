// Verifies three of the July 10 fixes:
//   1. validateAndSnapExpiryDate() no longer snaps to a listed expiration
//      nearer than the effective DTE floor (the actual 0DTE/1DTE bug).
//   2. The macro-catalyst override lowers the floor to MACRO_MIN_DTE_FLOOR,
//      never below it.
//   3. hasEarningsWithin5Days() fails closed when the calendar fetch failed.
//   4. shouldUseMacroDTEOverride() reacts to both the date list and VIX ratio.
//
// These are private members, so this test uses a NOX_UNIT_TEST-gated friend
// declaration (see OptionsSignalGenerator.hpp) to reach them directly without
// spinning up the full run_scan() network stack. No effect on production
// builds, which never define NOX_UNIT_TEST.

#include "../httplib.h"

#ifndef NOX_UNIT_TEST
#define NOX_UNIT_TEST
#endif
#include "../OptionsSignalGenerator.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using nox::options_signal::OptionsSignalGenerator;
using nox::options_signal::RiskProfile;
using nox::options_signal::VixTermStructure;
using nox::options_signal::OptionsSignal;

// Test-only accessor: friended by OptionsSignalGenerator when NOX_UNIT_TEST is
// defined, so this test can reach private members without a network stack.
struct NoxUnitTestAccess {
    using EarningsCalendarResult = OptionsSignalGenerator::EarningsCalendarResult;

    static std::string validateAndSnapExpiryDate(const OptionsSignalGenerator& gen,
                                                  const std::string& underlying,
                                                  double strike1,
                                                  const std::string& target_expiry_date,
                                                  bool use_macro_override) {
        return gen.validateAndSnapExpiryDate(underlying, strike1, target_expiry_date, use_macro_override);
    }
    static long parseDateToEpochDays(const std::string& ymd) {
        return OptionsSignalGenerator::parseDateToEpochDays(ymd);
    }
    static bool hasEarningsWithin5Days(const OptionsSignalGenerator& gen,
                                        const std::string& ticker,
                                        const EarningsCalendarResult& cal) {
        return gen.hasEarningsWithin5Days(ticker, cal);
    }
    static bool hasRecentEarnings(const OptionsSignalGenerator& gen,
                                   const std::string& ticker,
                                   const EarningsCalendarResult& cal,
                                   long lookback_days) {
        return gen.hasRecentEarnings(ticker, cal, lookback_days);
    }
    static std::string structuralSuppressionReason(const OptionsSignalGenerator& gen,
                                                    const OptionsSignal& sig) {
        return gen.structuralSuppressionReason(sig);
    }
    static double effectiveLongDelta(const OptionsSignalGenerator& gen,
                                     int resolved_dte, double base_delta) {
        return gen.effectiveLongDelta(resolved_dte, base_delta);
    }
    static bool shouldUseMacroDTEOverride(const OptionsSignalGenerator& gen,
                                           const VixTermStructure& vts) {
        return gen.shouldUseMacroDTEOverride(vts);
    }
};

static std::string todayPlusDays(int days) {
    auto tp = std::chrono::system_clock::now() + std::chrono::hours(24 * days);
    auto t  = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d");
    return oss.str();
}

static std::string todayStr() { return todayPlusDays(0); }

// Minimal in-process /v2/options/contracts stand-in with a controllable
// contract list, so the "closest listed expiry" search can be driven with
// contracts both above and below the DTE floor.
class ContractsMockServer {
public:
    ContractsMockServer(int port, const std::vector<std::string>& expiries)
        : port_(port) {
        svr_.Get("/v2/options/contracts",
            [expiries](const httplib::Request&, httplib::Response& res) {
                nlohmann::json contracts = nlohmann::json::array();
                for (const auto& exp : expiries) {
                    contracts.push_back({{"expiration_date", exp}});
                }
                nlohmann::json body = {{"option_contracts", contracts}};
                res.set_content(body.dump(), "application/json");
            });
        thread_ = std::thread([this] { svr_.listen("127.0.0.1", port_); });
        while (!svr_.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ~ContractsMockServer() {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }
private:
    int port_;
    httplib::Server svr_;
    std::thread thread_;
};

static RiskProfile makeProfile() {
    RiskProfile p;
    p.name = "TEST";
    return p;
}

int main() {
    std::cout << "=== DTE floor bypass + earnings fail-closed tests ===\n\n";

    // ── 1. DTE floor bug: a near-term (1DTE) contract must never be picked ──
    // even if it's numerically "closest" to an artificially-close target.
    {
        std::cout << "[dte-floor] near-term listed contract is rejected below MIN_DTE_FLOOR\n";
        unsetenv("MACRO_MIN_DTE_FLOOR");
        unsetenv("MIN_DTE_FLOOR"); // default 7

        std::string near_term = todayPlusDays(1);   // 1 DTE — must be rejected
        std::string floor_ok  = todayPlusDays(10);   // 10 DTE — must survive
        std::string target    = todayPlusDays(3);    // artificially close target: closest
                                                       // listed contract to this is the 1DTE one,
                                                       // which is exactly the bug scenario.

        ContractsMockServer mock(18901, {near_term, floor_ok});
        OptionsSignalGenerator gen("http://127.0.0.1:18901", "k", "s", "", "", makeProfile());

        std::string result = NoxUnitTestAccess::validateAndSnapExpiryDate(gen, "TEST", 100.0, target, /*use_macro_override=*/false);
        long result_days  = NoxUnitTestAccess::parseDateToEpochDays(result);
        long today_days   = NoxUnitTestAccess::parseDateToEpochDays(todayStr());
        long dte           = result_days - today_days;

        std::cout << "  target=" << target << " near_term=" << near_term
                  << " floor_ok=" << floor_ok << " -> picked=" << result << " (dte=" << dte << ")\n";
        assert(dte >= 7 && "1DTE contract leaked through despite MIN_DTE_FLOOR=7");
        assert(result == floor_ok && "should have snapped to the only floor-compliant contract");
        std::cout << "  ✓ near-term contract rejected, floor-compliant contract chosen\n\n";
    }

    // ── 2. Macro override lowers the floor to MACRO_MIN_DTE_FLOOR, not below ──
    {
        std::cout << "[macro-override] floor drops to MACRO_MIN_DTE_FLOOR when active\n";
        setenv("MACRO_MIN_DTE_FLOOR", "2", 1);

        std::string too_near = todayPlusDays(1);   // 1 DTE — must still be rejected (below floor of 2)
        std::string ok_macro = todayPlusDays(3);    // 3 DTE — must survive (>= macro floor of 2)
        std::string target   = todayPlusDays(3);

        ContractsMockServer mock(18902, {too_near, ok_macro});
        OptionsSignalGenerator gen("http://127.0.0.1:18902", "k", "s", "", "", makeProfile());

        std::string result = NoxUnitTestAccess::validateAndSnapExpiryDate(gen, "TEST", 100.0, target, /*use_macro_override=*/true);
        long result_days = NoxUnitTestAccess::parseDateToEpochDays(result);
        long today_days  = NoxUnitTestAccess::parseDateToEpochDays(todayStr());
        long dte = result_days - today_days;

        std::cout << "  target=" << target << " too_near=" << too_near
                  << " ok_macro=" << ok_macro << " -> picked=" << result << " (dte=" << dte << ")\n";
        assert(dte >= 2 && "macro override still let a sub-floor contract through");
        assert(result == ok_macro && "should have snapped to the macro-floor-compliant contract");
        std::cout << "  ✓ macro override respected its own (lower) floor, never below it\n\n";

        unsetenv("MACRO_MIN_DTE_FLOOR");
    }

    // ── 3. Earnings gate fails closed when the calendar couldn't be fetched ──
    {
        std::cout << "[earnings] fail-closed when calendar fetch failed\n";
        NoxUnitTestAccess::EarningsCalendarResult invalid_calendar; // valid=false by default
        OptionsSignalGenerator gen("http://127.0.0.1:1", "k", "s", "", "", makeProfile());

        bool result = NoxUnitTestAccess::hasEarningsWithin5Days(gen, "AAPL", invalid_calendar);
        assert(result == true && "invalid calendar must fail closed (skip the ticker)");
        std::cout << "  ✓ invalid calendar => treated as earnings-risky (ticker skipped)\n";

        NoxUnitTestAccess::EarningsCalendarResult valid_empty;
        valid_empty.valid = true; // fetch succeeded, no earnings found for this ticker
        bool result2 = NoxUnitTestAccess::hasEarningsWithin5Days(gen, "AAPL", valid_empty);
        assert(result2 == false && "valid calendar with no entry should NOT skip the ticker");
        std::cout << "  ✓ valid calendar, no earnings on file => ticker not skipped\n";

        NoxUnitTestAccess::EarningsCalendarResult valid_with_event;
        valid_with_event.valid = true;
        valid_with_event.data["AAPL"] = {{todayPlusDays(2), "Q3 earnings"}};
        bool result3 = NoxUnitTestAccess::hasEarningsWithin5Days(gen, "AAPL", valid_with_event);
        assert(result3 == true && "earnings 2 days out should trigger the skip");
        std::cout << "  ✓ confirmed earnings within 5 days => ticker skipped\n\n";
    }

    // ── 4. Macro override trigger conditions ──
    {
        std::cout << "[macro-trigger] date list and VIX ratio both activate the override\n";
        OptionsSignalGenerator gen("http://127.0.0.1:1", "k", "s", "", "", makeProfile());

        unsetenv("MACRO_DTE_OVERRIDE_DATES");
        VixTermStructure normal_vts;
        normal_vts.valid = true;
        normal_vts.ratio = 1.05; // normal contango
        assert(NoxUnitTestAccess::shouldUseMacroDTEOverride(gen, normal_vts) == false);
        std::cout << "  ✓ normal VIX term structure => no override\n";

        VixTermStructure stressed_vts;
        stressed_vts.valid = true;
        stressed_vts.ratio = 0.80; // extreme backwardation
        assert(NoxUnitTestAccess::shouldUseMacroDTEOverride(gen, stressed_vts) == true);
        std::cout << "  ✓ extreme VIX backwardation => override activates\n";

        setenv("MACRO_DTE_OVERRIDE_DATES", todayStr().c_str(), 1);
        assert(NoxUnitTestAccess::shouldUseMacroDTEOverride(gen, normal_vts) == true);
        std::cout << "  ✓ today's date in MACRO_DTE_OVERRIDE_DATES => override activates\n";
        unsetenv("MACRO_DTE_OVERRIDE_DATES");
    }

    // ── 5. Post-earnings buffer (Phase 3.3) ──
    {
        std::cout << "[post-earnings] buffer blocks fresh entries just AFTER a report\n";
        OptionsSignalGenerator gen("http://127.0.0.1:1", "k", "s", "", "", makeProfile());

        NoxUnitTestAccess::EarningsCalendarResult reported_yesterday;
        reported_yesterday.valid = true;
        reported_yesterday.data["NFLX"] = {{todayPlusDays(-1), "Q2 earnings"}};
        // Within a 2-day buffer → blocked; NOT caught by the pre-earnings gate.
        assert(NoxUnitTestAccess::hasRecentEarnings(gen, "NFLX", reported_yesterday, 2) == true);
        assert(NoxUnitTestAccess::hasEarningsWithin5Days(gen, "NFLX", reported_yesterday) == false);
        std::cout << "  ✓ report 1 day ago, buffer=2 => post-earnings gate fires\n";

        // Outside the buffer window (reported 4 days ago, buffer 2) → allowed.
        NoxUnitTestAccess::EarningsCalendarResult reported_last_week;
        reported_last_week.valid = true;
        reported_last_week.data["NFLX"] = {{todayPlusDays(-4), "Q2 earnings"}};
        assert(NoxUnitTestAccess::hasRecentEarnings(gen, "NFLX", reported_last_week, 2) == false);
        std::cout << "  ✓ report 4 days ago, buffer=2 => outside window, not gated\n";

        // Buffer disabled (0) → never gates on the post side.
        assert(NoxUnitTestAccess::hasRecentEarnings(gen, "NFLX", reported_yesterday, 0) == false);
        std::cout << "  ✓ buffer=0 disables the post-earnings gate\n";

        // Invalid calendar must NOT double-count here (pre-gate fails closed).
        NoxUnitTestAccess::EarningsCalendarResult invalid;
        assert(NoxUnitTestAccess::hasRecentEarnings(gen, "NFLX", invalid, 2) == false);
        std::cout << "  ✓ invalid calendar => post-gate returns false (pre-gate owns fail-closed)\n\n";
    }

    // ── 6. Structural suppression: R:R gate + opt-in lottery-delta gate (3.A) ──
    {
        std::cout << "[structural] poor-R:R veto is now an actual gate\n";
        OptionsSignalGenerator gen("http://127.0.0.1:1", "k", "s", "", "", makeProfile());
        unsetenv("OPTIONS_MIN_RR_RATIO");
        unsetenv("OPTIONS_MIN_LONG_DELTA");

        // Defined debit spread, risk > reward → suppressed at the 1.0 default.
        OptionsSignal bad_rr;
        bad_rr.strategy   = "BEAR_PUT_SPREAD";
        bad_rr.max_risk   = 1532.0;
        bad_rr.max_reward = 820.0;   // 0.54:1
        bad_rr.greeks.delta = 0.50;
        assert(!NoxUnitTestAccess::structuralSuppressionReason(gen, bad_rr).empty());
        std::cout << "  ✓ 0.54:1 defined spread => suppressed (default floor 1.0)\n";

        // Healthy R:R passes.
        OptionsSignal good_rr;
        good_rr.strategy   = "BEAR_PUT_SPREAD";
        good_rr.max_risk   = 1532.0;
        good_rr.max_reward = 2468.0; // 1.6:1
        good_rr.greeks.delta = 0.50;
        assert(NoxUnitTestAccess::structuralSuppressionReason(gen, good_rr).empty());
        std::cout << "  ✓ 1.6:1 defined spread => passes\n";

        // Unlimited-reward long (sentinel) is skipped by the R:R gate.
        OptionsSignal long_call;
        long_call.strategy   = "LONG_CALL";
        long_call.max_risk   = 500.0;
        long_call.max_reward = 999999.0; // unlimited sentinel
        long_call.greeks.delta = 0.15;
        assert(NoxUnitTestAccess::structuralSuppressionReason(gen, long_call).empty());
        std::cout << "  ✓ unlimited-reward long => R:R gate skips it\n";

        // R:R gate disabled with <=0.
        setenv("OPTIONS_MIN_RR_RATIO", "0", 1);
        assert(NoxUnitTestAccess::structuralSuppressionReason(gen, bad_rr).empty());
        std::cout << "  ✓ OPTIONS_MIN_RR_RATIO=0 disables the R:R gate\n";
        unsetenv("OPTIONS_MIN_RR_RATIO");

        // Opt-in lottery-delta gate: off by default, catches low delta when set.
        assert(NoxUnitTestAccess::structuralSuppressionReason(gen, long_call).empty()
               && "delta gate is off by default");
        setenv("OPTIONS_MIN_LONG_DELTA", "0.30", 1);
        assert(!NoxUnitTestAccess::structuralSuppressionReason(gen, long_call).empty()
               && "delta 0.15 < 0.30 floor => suppressed when opted in");
        std::cout << "  ✓ lottery-delta gate off by default, fires at 0.15<0.30 when enabled\n";
        unsetenv("OPTIONS_MIN_LONG_DELTA");
        std::cout << "\n";
    }

    // ── 7. Short-DTE ITM long-leg floor (Phase 3.2) ──
    {
        std::cout << "[short-dte-itm] long leg forced ITM under the DTE threshold\n";
        OptionsSignalGenerator gen("http://127.0.0.1:1", "k", "s", "", "", makeProfile());
        unsetenv("OPTIONS_SHORT_DTE_ITM_THRESHOLD");
        unsetenv("OPTIONS_SHORT_DTE_MIN_LONG_DELTA");

        // 10 DTE (< 14) → OTM 0.45 long delta floored up to 0.60 (ITM).
        assert(NoxUnitTestAccess::effectiveLongDelta(gen, 10, 0.45) == 0.60);
        std::cout << "  ✓ 10 DTE => 0.45 long delta floored to 0.60 (ITM)\n";

        // Already-ITM long delta is never lowered.
        assert(NoxUnitTestAccess::effectiveLongDelta(gen, 10, 0.70) == 0.70);
        std::cout << "  ✓ 10 DTE => an already-0.70 delta is left untouched\n";

        // 30 DTE (>= 14) → unchanged.
        assert(NoxUnitTestAccess::effectiveLongDelta(gen, 30, 0.45) == 0.45);
        std::cout << "  ✓ 30 DTE => long delta unchanged (0.45)\n";

        // Env overrides for both threshold and floor.
        setenv("OPTIONS_SHORT_DTE_ITM_THRESHOLD", "21", 1);
        setenv("OPTIONS_SHORT_DTE_MIN_LONG_DELTA", "0.70", 1);
        assert(NoxUnitTestAccess::effectiveLongDelta(gen, 18, 0.45) == 0.70);
        std::cout << "  ✓ env: threshold=21 floor=0.70 => 18 DTE floors to 0.70\n";
        unsetenv("OPTIONS_SHORT_DTE_ITM_THRESHOLD");
        unsetenv("OPTIONS_SHORT_DTE_MIN_LONG_DELTA");
        std::cout << "\n";
    }

    std::cout << "\n✅ ALL DTE-FLOOR / EARNINGS / POST-EARNINGS / STRUCTURAL / SHORT-DTE-ITM TESTS PASSED\n";
    return 0;
}
