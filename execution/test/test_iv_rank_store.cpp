// test_iv_rank_store.cpp — Phase 2, item C: true 52-week historical IV Rank.
//
// Asserts IvRankStore's percentile formula and the 30-distinct-day gate match
// heartbeat/monitor.py's calculate_iv_rank() "full_history" method exactly
// (just rescaled 0-100 instead of Python's [0,1]) — both languages must agree
// against the shared historical_volatility table, since a mismatch here would
// mean the C++ engine and the Python heartbeat trade on different numbers for
// the same ticker on the same day.

#include "../IvRankStore.hpp"

#include <cstdio>
#include <ctime>
#include <iostream>
#include <sqlite3.h>
#include <string>

using nox::execution::IvRankStore;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb(const std::string& tag) {
    return "/tmp/nox_ivrank_test_" + tag + ".db";
}
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

// Seed `historical_volatility` directly with raw SQL — mirrors exactly what
// heartbeat/monitor.py's store_iv_snapshot() (or the Polygon backfill) would
// have written, without depending on either being importable from C++.
static void seedRow(const std::string& db_path, const std::string& ticker,
                    const std::string& date, double iv) {
    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS historical_volatility ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT NOT NULL, date DATE NOT NULL, "
        "implied_volatility REAL NOT NULL, snapshot_timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
        "UNIQUE(ticker, date));", 0, 0, nullptr);
    std::string sql = "INSERT OR REPLACE INTO historical_volatility (ticker, date, implied_volatility) "
                       "VALUES ('" + ticker + "', '" + date + "', " + std::to_string(iv) + ");";
    sqlite3_exec(db, sql.c_str(), 0, 0, nullptr);
    sqlite3_close(db);
}

static std::string dateFor(int days_ago) {
    time_t t = std::time(nullptr) - static_cast<time_t>(days_ago) * 86400;
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&t));
    return std::string(buf);
}

static void test_insufficient_history_falls_back() {
    std::cout << "\n[insufficient] fewer than 30 distinct days → full_history=false\n";
    std::string db = tmpDb("insufficient"); wipe(db);
    for (int i = 0; i < 10; ++i) seedRow(db, "TEST", dateFor(i), 0.20 + i * 0.01);

    IvRankStore store(db);
    auto r = store.computeRank("TEST", 0.25);
    CHECK(!r.full_history, "10 days of history is not enough — caller must fall back to the proxy");
    CHECK(r.days_available == 10, "days_available reports the true count for diagnostics");
    wipe(db);
}

static void test_full_history_percentile_matches_python_formula() {
    std::cout << "\n[full-history] 30+ days → (current-min)/(max-min)*100, clamped\n";
    std::string db = tmpDb("full"); wipe(db);
    // 40 distinct days, IV linearly spanning 0.10 .. 0.49 (deterministic min/max).
    for (int i = 0; i < 40; ++i) seedRow(db, "TEST", dateFor(i), 0.10 + i * 0.01);

    IvRankStore store(db);
    // current_iv = 0.10 (the min) → rank 0
    auto r_low = store.computeRank("TEST", 0.10);
    CHECK(r_low.full_history, "40 days qualifies for full_history");
    CHECK(std::abs(r_low.iv_rank - 0.0) < 0.5, "IV at the historical minimum ranks near 0");

    // current_iv = 0.49 (the max) → rank ~100
    auto r_high = store.computeRank("TEST", 0.49);
    CHECK(std::abs(r_high.iv_rank - 100.0) < 0.5, "IV at the historical maximum ranks near 100");

    // current_iv exactly at the midpoint of [0.10, 0.49] → rank ~50
    double mid = (0.10 + 0.49) / 2.0;
    auto r_mid = store.computeRank("TEST", mid);
    CHECK(std::abs(r_mid.iv_rank - 50.0) < 1.0, "IV at the midpoint ranks near 50");
    wipe(db);
}

static void test_clamps_outside_historical_range() {
    std::cout << "\n[clamp] current IV outside the historical range clamps to [0,100]\n";
    std::string db = tmpDb("clamp"); wipe(db);
    for (int i = 0; i < 35; ++i) seedRow(db, "TEST", dateFor(i), 0.20);
    // Perturb one row so max != min (otherwise the identical-IV branch takes over).
    seedRow(db, "TEST", dateFor(34), 0.30);

    IvRankStore store(db);
    auto r_under = store.computeRank("TEST", 0.05); // below the historical min
    CHECK(r_under.iv_rank == 0.0, "current IV below the historical min clamps to 0, not negative");

    auto r_over = store.computeRank("TEST", 0.90); // above the historical max
    CHECK(r_over.iv_rank == 100.0, "current IV above the historical max clamps to 100, not >100");
    wipe(db);
}

static void test_identical_iv_defaults_to_middle() {
    std::cout << "\n[degenerate] all historical IVs identical → rank defaults to 50\n";
    std::string db = tmpDb("identical"); wipe(db);
    for (int i = 0; i < 35; ++i) seedRow(db, "TEST", dateFor(i), 0.22);

    IvRankStore store(db);
    auto r = store.computeRank("TEST", 0.22);
    CHECK(r.full_history, "35 identical days still qualifies for full_history");
    CHECK(r.iv_rank == 50.0, "zero-width range can't divide — defaults to the middle, not NaN/inf");
    wipe(db);
}

static void test_ticker_isolation() {
    std::cout << "\n[isolation] one ticker's history never leaks into another's rank\n";
    std::string db = tmpDb("isolation"); wipe(db);
    for (int i = 0; i < 35; ++i) seedRow(db, "AAA", dateFor(i), 0.10 + i * 0.01);
    // BBB has no history at all.

    IvRankStore store(db);
    auto r_bbb = store.computeRank("BBB", 0.50);
    CHECK(!r_bbb.full_history, "a ticker with zero history never borrows another ticker's rows");
    CHECK(r_bbb.days_available == 0, "days_available is 0, not AAA's 35");
    wipe(db);
}

int main() {
    std::cout << "═══ Phase 2 IvRankStore (52-week historical IV rank) tests ═══\n";
    test_insufficient_history_falls_back();
    test_full_history_percentile_matches_python_formula();
    test_clamps_outside_historical_range();
    test_identical_iv_defaults_to_middle();
    test_ticker_isolation();

    std::cout << "\n"
              << (g_failures == 0 ? "✅ ALL IV-RANK-STORE TESTS PASSED\n"
                                  : "❌ " + std::to_string(g_failures) + " CHECK(S) FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
