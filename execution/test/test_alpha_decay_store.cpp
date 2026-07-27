// test_alpha_decay_store.cpp — Phase 3: alpha-decay tier-down multiplier.
//
// AlphaDecayStore is a thin reader; heartbeat/alpha_decay_monitor.py owns the
// Sharpe math and writes the `alpha_decay_status` rows. These tests only
// assert the C++ read side: latest-row wins, fail-open on no data, sanity
// clamp on a malformed multiplier.

#include "../AlphaDecayStore.hpp"

#include <cstdio>
#include <iostream>
#include <sqlite3.h>
#include <string>

using nox::execution::AlphaDecayStore;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb(const std::string& tag) {
    return "/tmp/nox_alphadecay_test_" + tag + ".db";
}
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

// Seeds `alpha_decay_status` directly — mirrors exactly what
// heartbeat/alpha_decay_monitor.py's run_daily_check() would have written.
static void seedRow(const std::string& db_path, double tier_multiplier, int triggered) {
    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS alpha_decay_status ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, computed_at TEXT NOT NULL, "
        "rolling_sharpe_30d REAL, baseline_sharpe_12mo REAL, degraded_pct REAL, "
        "tier_multiplier REAL NOT NULL, triggered INTEGER NOT NULL, days_available INTEGER NOT NULL);",
        0, 0, nullptr);
    std::string sql = "INSERT INTO alpha_decay_status "
        "(computed_at, rolling_sharpe_30d, baseline_sharpe_12mo, degraded_pct, tier_multiplier, "
        " triggered, days_available) VALUES ('2026-01-01T00:00:00Z', 1.0, 1.0, 0.0, " +
        std::to_string(tier_multiplier) + ", " + std::to_string(triggered) + ", 100);";
    sqlite3_exec(db, sql.c_str(), 0, 0, nullptr);
    sqlite3_close(db);
}

static void test_no_data_fails_open_to_one() {
    std::cout << "\n[no-data] fresh table, no rows written yet → multiplier defaults to 1.0\n";
    std::string db = tmpDb("nodata"); wipe(db);

    AlphaDecayStore store(db); // constructor creates the table but writes no rows
    CHECK(store.getTierMultiplier() == 1.0, "no rows → 1.0, never invents a tier-down");
    wipe(db);
}

static void test_latest_row_wins() {
    std::cout << "\n[latest-row] most recent insert (highest id) is authoritative\n";
    std::string db = tmpDb("latest"); wipe(db);
    seedRow(db, 1.0, 0);   // day 1: healthy
    seedRow(db, 0.5, 1);   // day 2: triggered — tiered down
    seedRow(db, 1.0, 0);   // day 3: recovered

    AlphaDecayStore store(db);
    CHECK(store.getTierMultiplier() == 1.0, "latest row (recovered) wins, not the triggered middle row");
    wipe(db);
}

static void test_triggered_scales_sizing_down() {
    std::cout << "\n[triggered] a degraded regime's multiplier is read through unchanged\n";
    std::string db = tmpDb("triggered"); wipe(db);
    seedRow(db, 0.5, 1);

    AlphaDecayStore store(db);
    CHECK(store.getTierMultiplier() == 0.5, "triggered row's 0.5 multiplier passes through");
    wipe(db);
}

static void test_malformed_multiplier_clamps_to_safe_default() {
    std::cout << "\n[clamp] a multiplier outside (0,1] never reaches the sizing calc verbatim\n";
    std::string db = tmpDb("clamp"); wipe(db);
    seedRow(db, 2.5, 1);  // > 1.0 — would inflate risk, not reduce it

    AlphaDecayStore store(db);
    CHECK(store.getTierMultiplier() == 1.0, "out-of-range multiplier clamps to 1.0, not passed through");
    wipe(db);
}

int main() {
    std::cout << "═══ Phase 3 AlphaDecayStore (tier-down multiplier) tests ═══\n";
    test_no_data_fails_open_to_one();
    test_latest_row_wins();
    test_triggered_scales_sizing_down();
    test_malformed_multiplier_clamps_to_safe_default();

    std::cout << "\n"
              << (g_failures == 0 ? "✅ ALL ALPHA-DECAY-STORE TESTS PASSED\n"
                                  : "❌ " + std::to_string(g_failures) + " CHECK(S) FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
