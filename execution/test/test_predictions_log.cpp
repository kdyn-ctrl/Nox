// test_predictions_log.cpp — engine-wide prediction-quality logging
// (CLAUDE.md's "Engine-Wide Prediction Quality Scoring").
//
// Pins OrderLedger::logPrediction()'s write path: the predictions_log table
// gets created, a row lands with the right source_type/ticker/direction/
// confidence, and multiple sources for the same ticker don't collide (no
// UNIQUE constraint on this table beyond the primary key — every logged
// call should persist independently, unlike signal_events' signature-keyed
// dedup). heartbeat/prediction_outcome_resolver.py is what actually resolves
// these against real price action; this test only proves the C++ write side.

#include "../OrderLedger.hpp"

#include <cstdio>
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <vector>

using nox::execution::OrderLedger;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb() { return "/tmp/nox_predictions_log_test.db"; }
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

struct Row {
    std::string source_type, ticker, direction, detail;
    double confidence;
};

static std::vector<Row> readAll(const std::string& dbPath) {
    sqlite3* db = nullptr;
    sqlite3_open(dbPath.c_str(), &db);
    std::vector<Row> out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT source_type, ticker, direction, confidence, detail FROM predictions_log ORDER BY id ASC;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Row r;
            r.source_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            r.ticker       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            r.direction    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            r.confidence   = sqlite3_column_double(stmt, 3);
            const unsigned char* d = sqlite3_column_text(stmt, 4);
            r.detail = d ? reinterpret_cast<const char*>(d) : "";
            out.push_back(r);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

int main() {
    std::string db = tmpDb();
    wipe(db);

    {
        OrderLedger ledger(db);
        ledger.logPrediction("ws1_contradiction", 0, "AAPL", "BULLISH", 0.75, "CONTRADICT_BULLISH");
        ledger.logPrediction("skeptic_altmacro", 0, "XOM", "BEARISH", 0.40, "physical_supply");
        ledger.logPrediction("skeptic_insider", 0, "AAPL", "BULLISH", 0.60, "insider_cluster(3)");
    }

    auto rows = readAll(db);
    CHECK(rows.size() == 3, "all three logPrediction calls persisted independently");
    if (rows.size() == 3) {
        CHECK(rows[0].source_type == "ws1_contradiction" && rows[0].ticker == "AAPL" &&
              rows[0].direction == "BULLISH" && std::abs(rows[0].confidence - 0.75) < 1e-9,
              "WS1 row has expected source_type/ticker/direction/confidence");
        CHECK(rows[1].source_type == "skeptic_altmacro" && rows[1].direction == "BEARISH",
              "Skeptic alt-macro row has expected source_type/direction");
        CHECK(rows[2].ticker == "AAPL" && rows[2].source_type == "skeptic_insider",
              "second AAPL row from a different source did not collide with the first");
    }

    // Re-opening the same db path (own handle, matching the multi-handle-to-
    // one-file pattern) must not lose or duplicate existing rows.
    {
        OrderLedger ledger2(db);
        ledger2.logPrediction("china_lag_ws8", 0, "BABA", "BEARISH", 0.90, "caixin_pmi,fresh");
    }
    auto rows2 = readAll(db);
    CHECK(rows2.size() == 4, "reopening the ledger appends rather than reinitializing the table");

    wipe(db);

    std::cout << (g_failures == 0 ? "\nAll predictions_log tests passed.\n"
                                  : "\n" + std::to_string(g_failures) + " test(s) FAILED.\n");
    return g_failures == 0 ? 0 : 1;
}
