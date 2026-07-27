// Verifies the passive post-earnings-drift research trail (earnings_drift_
// observations table, OrderLedger.hpp): recording is idempotent per
// (ticker, earnings_date), and the pending query only surfaces a row once
// enough calendar days have passed since that earnings date — 1 day for the
// T+1 fill, 5 for T+5 — never before, and never again once resolved.

#include "../OrderLedger.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

using nox::execution::OrderLedger;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb() { return "/tmp/nox_earnings_drift_test.db"; }
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

static std::string todayPlusDays(int days) {
    auto tp = std::chrono::system_clock::now() + std::chrono::hours(24 * days);
    auto t  = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d");
    return oss.str();
}

int main() {
    std::cout << "=== earnings_drift_observations tests ===\n\n";
    std::string db = tmpDb();
    wipe(db);
    OrderLedger led(db);

    OrderLedger::EarningsDriftObservation o;
    o.ticker        = "AAPL";
    o.earnings_date = todayPlusDays(3); // 3 days out — within the 5-day gate window
    o.pre_price     = 200.0;
    o.pre_rsi       = 55.0;
    o.pre_sma20     = 195.0;
    o.pre_sma50     = 190.0;
    o.pre_atr       = 3.0;
    o.direction     = "bullish";

    std::cout << "[not-yet-eligible] earnings still 3 days out -> no pending T+1/T+5 rows\n";
    led.recordEarningsDriftObservation(o);
    auto pending_early = led.getPendingEarningsDrift();
    CHECK(pending_early.empty(), "future earnings date has nothing pending yet");

    std::cout << "\n[record+dedup] same (ticker, earnings_date) recorded twice, "
                  "then earnings date moves into the past -> still only ONE pending row, not two\n";
    OrderLedger::EarningsDriftObservation o_dup = o;
    o_dup.earnings_date = todayPlusDays(-1); // now eligible for T+1
    led.recordEarningsDriftObservation(o_dup);
    led.recordEarningsDriftObservation(o_dup); // duplicate — must be ignored, not a second row
    auto pending_dedup = led.getPendingEarningsDrift();
    CHECK(pending_dedup.size() == 1,
          "double-recording the same (ticker, earnings_date) yields exactly one pending row (got " +
          std::to_string(pending_dedup.size()) + ")");

    std::cout << "\n[T+1 eligible, T+5 not] earnings 1 day ago\n";
    wipe(db);
    OrderLedger led2(db);
    OrderLedger::EarningsDriftObservation o2 = o;
    o2.ticker        = "MSFT";
    o2.earnings_date = todayPlusDays(-1); // yesterday
    o2.pre_price     = 400.0;
    led2.recordEarningsDriftObservation(o2);

    auto pending_t1 = led2.getPendingEarningsDrift();
    CHECK(pending_t1.size() == 1, "exactly one pending item (T+1 only, not T+5 yet)");
    if (!pending_t1.empty()) {
        CHECK(pending_t1[0].ticker == "MSFT", "pending item is for MSFT");
        CHECK(pending_t1[0].day_offset == 1, "day_offset is 1, not 5");
        CHECK(pending_t1[0].pre_price == 400.0, "pre_price carried through correctly");
    }

    led2.resolveEarningsDriftT1(pending_t1[0].id, 420.0, 5.0);
    auto pending_after_t1 = led2.getPendingEarningsDrift();
    CHECK(pending_after_t1.empty(), "resolved T+1 with earnings only 1 day old -> nothing left pending");

    std::cout << "\n[T+1 and T+5 both eligible] earnings 6 days ago, neither resolved yet\n";
    wipe(db);
    OrderLedger led3(db);
    OrderLedger::EarningsDriftObservation o3 = o;
    o3.ticker        = "NVDA";
    o3.earnings_date = todayPlusDays(-6);
    o3.pre_price     = 100.0;
    led3.recordEarningsDriftObservation(o3);

    auto pending_both = led3.getPendingEarningsDrift();
    CHECK(pending_both.size() == 2, "both T+1 and T+5 pending for 6-day-old earnings");
    bool has_t1 = false, has_t5 = false;
    long id_for_row = 0;
    for (const auto& p : pending_both) {
        if (p.day_offset == 1) { has_t1 = true; id_for_row = p.id; }
        if (p.day_offset == 5) has_t5 = true;
    }
    CHECK(has_t1 && has_t5, "day_offset 1 and 5 both present");

    led3.resolveEarningsDriftT1(id_for_row, 110.0, 10.0);
    auto pending_t5_only = led3.getPendingEarningsDrift();
    CHECK(pending_t5_only.size() == 1 && pending_t5_only[0].day_offset == 5,
          "after resolving T+1, only T+5 remains pending for the same row");

    led3.resolveEarningsDriftT5(pending_t5_only[0].id, 115.0, 15.0);
    CHECK(led3.getPendingEarningsDrift().empty(), "after resolving both, nothing pending");

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "✅ ALL EARNINGS-DRIFT TESTS PASSED\n";
        return 0;
    }
    std::cout << "❌ " << g_failures << " test(s) failed\n";
    return 1;
}
