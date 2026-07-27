#ifndef KILL_SWITCH_STORE_HPP
#define KILL_SWITCH_STORE_HPP

// KillSwitchStore — global trading halt, persisted so a process restart
// mid-day never silently un-pauses trading after an operator-triggered
// `/pause` or an automatic daily-loss-limit breach. This is the CLAUDE.md
// "state exception" case: signal logic can self-correct on fresh
// information, but a halt decision made for a REASON (operator judgment or
// a loss limit) must not be forgotten just because the engine restarted.
//
// Opens its OWN sqlite3 handle to the SAME memory_bank.db file — the same
// multi-handle-to-one-file pattern OrderLedger/IvRankStore/AlphaDecayStore
// already established (WAL + busy_timeout let all sides coexist safely).
//
// Single-row table (id=1, upserted) — there is exactly one kill-switch state
// for the whole engine, not one per ticker/strategy.

#include <sqlite3.h>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <string>

namespace nox::execution {

struct KillSwitchState {
    bool        paused      = false;
    std::string reason;
    std::string triggered_by; // "operator" or "daily_loss_limit"
    long        triggered_at = 0;
};

class KillSwitchStore {
public:
    explicit KillSwitchStore(const std::string& db_path) {
        if (sqlite3_open(db_path.c_str(), &db_)) {
            throw std::runtime_error("KillSwitchStore: can't open database: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", 0, 0, nullptr);
        initialize();
    }

    ~KillSwitchStore() {
        if (db_) sqlite3_close(db_);
    }

    KillSwitchStore(const KillSwitchStore&) = delete;
    KillSwitchStore& operator=(const KillSwitchStore&) = delete;

    void pause(const std::string& reason, const std::string& triggered_by) {
        std::lock_guard<std::mutex> lock(lock_);
        upsert(true, reason, triggered_by);
    }

    void resume() {
        std::lock_guard<std::mutex> lock(lock_);
        upsert(false, "", "");
    }

    // Fail-open: any read error (table missing, DB unreachable) returns
    // not-paused rather than accidentally halting trading forever because a
    // sqlite read hiccuped. A read hiccup is transient; halting on a false
    // read failure is a self-inflicted, indefinite outage with no signal
    // behind it — the opposite of the ghost-fill philosophy's intent.
    KillSwitchState get() {
        std::lock_guard<std::mutex> lock(lock_);
        KillSwitchState s;
        const char* sql =
            "SELECT paused, reason, triggered_by, triggered_at "
            "FROM kill_switch_state WHERE id = 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return s;
        }
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            s.paused = sqlite3_column_int(stmt, 0) != 0;
            const unsigned char* r = sqlite3_column_text(stmt, 1);
            const unsigned char* t = sqlite3_column_text(stmt, 2);
            s.reason       = r ? std::string(reinterpret_cast<const char*>(r)) : std::string();
            s.triggered_by = t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
            s.triggered_at = static_cast<long>(sqlite3_column_int64(stmt, 3));
        }
        sqlite3_finalize(stmt);
        return s;
    }

    bool isPaused() { return get().paused; }

private:
    void initialize() {
        std::lock_guard<std::mutex> lock(lock_);
        const char* sql =
            "CREATE TABLE IF NOT EXISTS kill_switch_state ("
            "id INTEGER PRIMARY KEY CHECK (id = 1), "
            "paused INTEGER NOT NULL DEFAULT 0, "
            "reason TEXT NOT NULL DEFAULT '', "
            "triggered_by TEXT NOT NULL DEFAULT '', "
            "triggered_at INTEGER NOT NULL DEFAULT 0);"
            "INSERT OR IGNORE INTO kill_switch_state (id, paused, reason, triggered_by, triggered_at) "
            "VALUES (1, 0, '', '', 0);";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, 0, 0, &err) != SQLITE_OK) {
            std::string msg = "KillSwitchStore init: " + std::string(err ? err : "unknown");
            sqlite3_free(err);
            throw std::runtime_error(msg);
        }
    }

    // Must be called with lock_ held.
    void upsert(bool paused, const std::string& reason, const std::string& triggered_by) {
        const char* sql =
            "UPDATE kill_switch_state SET paused=?, reason=?, triggered_by=?, triggered_at=? "
            "WHERE id = 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_bind_int  (stmt, 1, paused ? 1 : 0);
        sqlite3_bind_text (stmt, 2, reason.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 3, triggered_by.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, static_cast<long>(std::time(nullptr)));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3*   db_ = nullptr;
    std::mutex lock_;
};

} // namespace nox::execution

#endif // KILL_SWITCH_STORE_HPP
