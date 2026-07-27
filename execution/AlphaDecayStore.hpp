#ifndef ALPHA_DECAY_STORE_HPP
#define ALPHA_DECAY_STORE_HPP

// AlphaDecayStore — Phase 3: reads the position-size tier multiplier that
// heartbeat/alpha_decay_monitor.py computes daily into the `alpha_decay_status`
// table. That script owns the rolling-30-day-vs-12-month Sharpe comparison and
// the Telegram alert; this class is a thin, fail-open reader so the C++
// engine can scale OptionsSignalGenerator's contract sizing without
// duplicating the Sharpe math in two languages.
//
// Opens its OWN sqlite3 handle to the SAME memory_bank.db file — the same
// multi-handle-to-one-file pattern OrderLedger/IvRankStore already
// established (WAL + busy_timeout let all sides coexist safely).

#include <sqlite3.h>
#include <mutex>
#include <stdexcept>
#include <string>

namespace nox::execution {

class AlphaDecayStore {
public:
    explicit AlphaDecayStore(const std::string& db_path) {
        if (sqlite3_open(db_path.c_str(), &db_)) {
            throw std::runtime_error("AlphaDecayStore: can't open database: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", 0, 0, nullptr);
        initialize();
    }

    ~AlphaDecayStore() {
        if (db_) sqlite3_close(db_);
    }

    AlphaDecayStore(const AlphaDecayStore&) = delete;
    AlphaDecayStore& operator=(const AlphaDecayStore&) = delete;

    // Fail-open: no rows yet (monitor hasn't run, or insufficient history)
    // returns 1.0 — normal sizing. Only an explicit `triggered=1` row scales
    // sizing down; we never invent a scale-down from absence of data.
    double getTierMultiplier() {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "SELECT tier_multiplier FROM alpha_decay_status "
            "ORDER BY id DESC LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return 1.0;
        }
        double multiplier = 1.0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            multiplier = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
        if (multiplier <= 0.0 || multiplier > 1.0) return 1.0; // sanity clamp
        return multiplier;
    }

private:
    void initialize() {
        std::lock_guard<std::mutex> lock(lock_);
        // Matches heartbeat/alpha_decay_monitor.py's schema exactly — same
        // shared table; whichever service starts first creates it.
        const char* sql =
            "CREATE TABLE IF NOT EXISTS alpha_decay_status ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "computed_at TEXT NOT NULL, "
            "rolling_sharpe_30d REAL, "
            "baseline_sharpe_12mo REAL, "
            "degraded_pct REAL, "
            "tier_multiplier REAL NOT NULL, "
            "triggered INTEGER NOT NULL, "
            "days_available INTEGER NOT NULL);";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, 0, 0, &err) != SQLITE_OK) {
            std::string msg = "AlphaDecayStore init: " + std::string(err ? err : "unknown");
            sqlite3_free(err);
            throw std::runtime_error(msg);
        }
    }

    sqlite3*   db_ = nullptr;
    std::mutex lock_;
};

} // namespace nox::execution

#endif // ALPHA_DECAY_STORE_HPP
