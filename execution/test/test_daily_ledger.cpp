// test_daily_ledger.cpp — Phase 2, item B: live P&L tracking.
//
// Asserts the daily_ledger upsert semantics PositionManager relies on: repeated
// intraday unrealized snapshots overwrite (last-known-value-for-the-day), while
// realized P&L accumulates additively (a partial close followed by a full close
// on the same day must sum, not clobber). Also confirms unrealized and realized
// coexist correctly for the same (date, ticker, asset_class, detail) key.

#include "../PositionManager.hpp"
#include "../OptionsOrderRouter.hpp"

#include <cstdio>
#include <iostream>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb(const std::string& tag) {
    return "/tmp/nox_ledger_test_" + tag + ".db";
}
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

static void test_unrealized_overwrites_across_cycles() {
    std::cout << "\n[unrealized] repeated snapshots overwrite mark/unrealized, not accumulate\n";
    std::string db = tmpDb("unreal"); wipe(db);
    nox::options_router::OptionsOrderRouter router("http://localhost:1", "k", "s");
    PositionManager pm(db, router);

    pm.upsert_unrealized("2026-07-09", "TEST", "option", "call 100.00 2026-08-21",
                         1.0, 2.50, 3.00, 50.0);
    pm.upsert_unrealized("2026-07-09", "TEST", "option", "call 100.00 2026-08-21",
                         1.0, 2.50, 3.20, 70.0);

    auto rows = pm.get_daily_ledger("2026-07-09");
    CHECK(rows.size() == 1, "one row per (date,ticker,asset_class,detail), not one per snapshot");
    CHECK(rows[0].mark_price == 3.20, "latest mark price wins (overwrite, not sum)");
    CHECK(rows[0].unrealized_pnl == 70.0, "latest unrealized_pnl wins");
    CHECK(rows[0].realized_pnl == 0.0, "untouched realized_pnl stays 0 for a still-open position");
    wipe(db);
}

static void test_realized_accumulates_same_day() {
    std::cout << "\n[realized] partial close then full close on the same day sum, not clobber\n";
    std::string db = tmpDb("real"); wipe(db);
    nox::options_router::OptionsOrderRouter router("http://localhost:1", "k", "s");
    PositionManager pm(db, router);

    pm.add_realized("2026-07-09", "TEST", "equity", "", 40.0);  // partial close
    pm.add_realized("2026-07-09", "TEST", "equity", "", 25.0);  // full close later same day

    auto rows = pm.get_daily_ledger("2026-07-09");
    CHECK(rows.size() == 1, "both closes land on one row for the day");
    CHECK(rows[0].realized_pnl == 65.0, "realized P&L accumulates additively (40 + 25)");
    wipe(db);
}

static void test_unrealized_then_realized_same_key() {
    std::cout << "\n[coexist] a position that closes keeps its last unrealized snapshot + gains realized\n";
    std::string db = tmpDb("mixed"); wipe(db);
    nox::options_router::OptionsOrderRouter router("http://localhost:1", "k", "s");
    PositionManager pm(db, router);

    pm.upsert_unrealized("2026-07-09", "TEST", "option", "call 100.00 2026-08-21",
                         1.0, 2.50, 3.75, 125.0);
    pm.add_realized("2026-07-09", "TEST", "option", "call 100.00 2026-08-21", 125.0);

    auto rows = pm.get_daily_ledger("2026-07-09");
    CHECK(rows.size() == 1, "unrealized snapshot and realized close share one row (same key)");
    CHECK(rows[0].mark_price == 3.75, "last mark price before close is preserved");
    CHECK(rows[0].realized_pnl == 125.0, "realized P&L recorded on close");
    wipe(db);
}

int main() {
    std::cout << "═══ Phase 2 daily_ledger (live P&L) tests ═══\n";
    test_unrealized_overwrites_across_cycles();
    test_realized_accumulates_same_day();
    test_unrealized_then_realized_same_key();

    std::cout << "\n"
              << (g_failures == 0 ? "✅ ALL DAILY-LEDGER TESTS PASSED\n"
                                  : "❌ " + std::to_string(g_failures) + " CHECK(S) FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
