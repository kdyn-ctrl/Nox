// Verifies the new options_signals table (July 10): every generated signal
// candidate — submitted, gate-suppressed, earnings-skipped, DTE-floor-skipped —
// can be persisted with its full contract/regime context and read back for
// analysis, not just orders that reached the broker (that's order_ledger).

#include "../OrderLedger.hpp"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

using nox::execution::OrderLedger;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb() { return "/tmp/nox_options_signals_test.db"; }
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

int main() {
    std::cout << "=== options_signals storage tests ===\n\n";
    std::string db = tmpDb();
    wipe(db);
    OrderLedger led(db);

    std::cout << "[storage] every outcome type is persisted and readable\n";

    OrderLedger::GeneratedSignal submitted;
    submitted.ticker = "SPY";
    submitted.strategy = "LONG_CALL";
    submitted.signature = "SPY|LONG_CALL|450|0|2026-08-15|buy";
    submitted.direction = "bullish";
    submitted.strike = 450.0;
    submitted.expiration_date = "2026-08-15";
    submitted.dte = 36;
    submitted.macro_override_used = false;
    submitted.iv_rank = 55.0;
    submitted.hrv30 = 0.22;
    submitted.quality_score = 0.87;
    submitted.regime = "RISK_ON";
    submitted.vix_term_label = "CONTANGO";
    submitted.earnings_checked = true;
    submitted.outcome = "submitted";
    submitted.reason = "";
    led.logGeneratedSignal(submitted);

    OrderLedger::GeneratedSignal suppressed;
    suppressed.ticker = "SPY";
    suppressed.strategy = "CSP";
    suppressed.signature = "SPY|CSP|440|0|2026-08-15|sell";
    suppressed.direction = "bullish";
    suppressed.strike = 440.0;
    suppressed.expiration_date = "2026-08-15";
    suppressed.dte = 36;
    suppressed.regime = "RISK_ON";
    suppressed.vix_term_label = "BACKWARDATION";
    suppressed.earnings_checked = true;
    suppressed.outcome = "suppressed_vix_term_gate";
    suppressed.reason = "VIX3M/VIX backwardation ratio=0.920";
    led.logGeneratedSignal(suppressed);

    OrderLedger::GeneratedSignal earnings_skip;
    earnings_skip.ticker = "AAPL";
    earnings_skip.strategy = "";
    earnings_skip.signature = "";
    earnings_skip.outcome = "skipped_earnings_confirmed";
    earnings_skip.reason = "earnings confirmed within 5 days";
    earnings_skip.earnings_checked = true;
    led.logGeneratedSignal(earnings_skip);

    OrderLedger::GeneratedSignal dte_skip;
    dte_skip.ticker = "QQQ";
    dte_skip.strategy = "LONG_PUT";
    dte_skip.signature = "QQQ|LONG_PUT|380|0|2026-07-11|buy";
    dte_skip.outcome = "skipped_no_contracts_within_floor";
    dte_skip.reason = "no listed expiration >= 7 DTE";
    dte_skip.macro_override_used = false;
    led.logGeneratedSignal(dte_skip);

    auto spy_rows = led.getGeneratedSignalsByTicker("SPY");
    CHECK(spy_rows.size() == 2, "SPY has exactly 2 rows (submitted + suppressed)");
    // Most recent first: suppressed was logged after submitted.
    CHECK(spy_rows[0].outcome == "suppressed_vix_term_gate", "most recent SPY row is the suppression");
    CHECK(spy_rows[0].vix_term_label == "BACKWARDATION", "vix_term_label persisted correctly");
    CHECK(spy_rows[1].outcome == "submitted", "oldest SPY row is the submitted signal");
    CHECK(spy_rows[1].strike == 450.0, "strike persisted correctly");
    CHECK(spy_rows[1].dte == 36, "dte persisted correctly");
    CHECK(spy_rows[1].quality_score == 0.87, "quality_score persisted correctly");
    CHECK(spy_rows[1].direction == "bullish", "direction persisted correctly");
    CHECK(spy_rows[1].macro_override_used == false, "macro_override_used defaults/persists false");

    auto aapl_rows = led.getGeneratedSignalsByTicker("AAPL");
    CHECK(aapl_rows.size() == 1, "AAPL has exactly 1 row (earnings skip)");
    CHECK(aapl_rows[0].outcome == "skipped_earnings_confirmed", "earnings-skip outcome persisted");

    auto qqq_rows = led.getGeneratedSignalsByTicker("QQQ");
    CHECK(qqq_rows.size() == 1, "QQQ has exactly 1 row (DTE-floor skip)");
    CHECK(qqq_rows[0].outcome == "skipped_no_contracts_within_floor", "DTE-floor-skip outcome persisted");

    auto empty_rows = led.getGeneratedSignalsByTicker("TSLA");
    CHECK(empty_rows.empty(), "ticker with no rows returns empty, not an error");

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "✅ ALL OPTIONS_SIGNALS STORAGE TESTS PASSED\n";
        return 0;
    } else {
        std::cout << "❌ " << g_failures << " test(s) failed\n";
        return 1;
    }
}
