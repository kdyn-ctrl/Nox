#pragma once

// FuturesSignalStore — persistence for FuturesSignalGenerator's audit trail.
// Same multi-handle-to-one-file pattern as IvRankStore.hpp: its own sqlite3
// handle to the shared memory_bank.db, WAL + busy_timeout so it coexists
// safely with PositionManager/OrderLedger's handles and the Python heartbeat.
//
// This is signal-only storage — there is no order/fill column in this schema
// because FuturesSignalGenerator has no execution path at all (CLAUDE.md
// futures phase 1 is data-in/signal-out only, no IBKR/Alpaca order routing).

#include <sqlite3.h>
#include <mutex>
#include <stdexcept>
#include <string>

namespace nox::execution {

struct FuturesSignal {
    std::string contract;         // e.g. "CL"
    std::string direction;        // "BULLISH" | "BEARISH" | "NEUTRAL"
    double      price            = 0.0;
    double      physical_stress  = 0.0; // from america_data_engine's alt_macro /macro/alt
    double      political_signal = 0.0;
    std::string macro_verdict;    // CONFIRM | TEXT_CONTRADICTS_PHYSICAL | PHYSICAL_ONLY | ...
    std::string macro_bias;       // BULLISH_OIL | BEARISH_OIL | ""
    double      quality_score    = 0.0;
    std::string reason;
    long long   scan_at          = 0; // unix seconds
};

class FuturesSignalStore {
public:
    explicit FuturesSignalStore(const std::string& db_path) {
        if (sqlite3_open(db_path.c_str(), &db_)) {
            throw std::runtime_error("FuturesSignalStore: can't open database: " +
                                      std::string(sqlite3_errmsg(db_)));
        }
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", 0, 0, nullptr);
        initialize();
    }

    ~FuturesSignalStore() {
        if (db_) sqlite3_close(db_);
    }

    FuturesSignalStore(const FuturesSignalStore&) = delete;
    FuturesSignalStore& operator=(const FuturesSignalStore&) = delete;

    // Fail-open: a write failure is logged by the caller, never thrown, so a
    // storage hiccup can't take down the scan loop.
    bool insert(const FuturesSignal& s) {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "INSERT INTO futures_signals "
            "(contract, direction, price, physical_stress, political_signal, "
            " macro_verdict, macro_bias, quality_score, reason, scan_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_bind_text(stmt, 1, s.contract.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, s.direction.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, s.price);
        sqlite3_bind_double(stmt, 4, s.physical_stress);
        sqlite3_bind_double(stmt, 5, s.political_signal);
        sqlite3_bind_text(stmt, 6, s.macro_verdict.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, s.macro_bias.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 8, s.quality_score);
        sqlite3_bind_text(stmt, 9, s.reason.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 10, s.scan_at);

        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }

private:
    void initialize() {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "CREATE TABLE IF NOT EXISTS futures_signals ("
            "id               INTEGER PRIMARY KEY AUTOINCREMENT, "
            "contract         TEXT NOT NULL, "
            "direction        TEXT NOT NULL, "
            "price            REAL, "
            "physical_stress  REAL, "
            "political_signal REAL, "
            "macro_verdict    TEXT, "
            "macro_bias       TEXT, "
            "quality_score    REAL, "
            "reason           TEXT, "
            "scan_at          INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS idx_futures_signals_contract_time "
            "ON futures_signals(contract, scan_at);";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, 0, 0, &err) != SQLITE_OK) {
            std::string msg = "FuturesSignalStore init: " + std::string(err ? err : "unknown");
            sqlite3_free(err);
            throw std::runtime_error(msg);
        }
    }

    sqlite3*   db_ = nullptr;
    std::mutex lock_;
};

} // namespace nox::execution
