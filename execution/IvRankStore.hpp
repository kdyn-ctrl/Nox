#ifndef IV_RANK_STORE_HPP
#define IV_RANK_STORE_HPP

// IvRankStore — Phase 2, item C: reads the TRUE 52-week historical IV Rank
// from the `historical_volatility` table the Python heartbeat maintains (and
// the Polygon backfill script — heartbeat/polygon_iv_backfill.py — now seeds
// quickly instead of waiting ~30 trading days for it to accumulate
// organically). This replaces OptionsSignalGenerator::fetchIVData()'s
// same-snapshot proxy, which was explicitly a placeholder (see its own
// comment: "reserved for the true 52-week historical IV Rank... which is not
// yet wired into this generator").
//
// Opens its OWN sqlite3 handle to the SAME memory_bank.db file the Python
// heartbeat uses — the same multi-handle-to-one-file pattern OrderLedger
// already established (WAL + busy_timeout let both sides coexist safely).
// Creates the table if the Python side hasn't run yet, so read/write
// ordering between the two services never errors either way.
//
// The percentile formula and the >=30-distinct-days gate mirror
// heartbeat/monitor.py's calculate_iv_rank() "full_history" method exactly,
// so both languages agree on one formula against one shared table instead of
// maintaining two independent implementations that could silently drift.
// Python's scale is [0,1]; this returns [0,100] to match the existing C++
// convention (OptionsSignalTypes::iv_rank_buy_max/iv_rank_sell_min are 0-100).

#include <sqlite3.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>

namespace nox::execution {

class IvRankStore {
public:
    explicit IvRankStore(const std::string& db_path) {
        if (sqlite3_open(db_path.c_str(), &db_)) {
            throw std::runtime_error("IvRankStore: can't open database: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", 0, 0, nullptr);
        initialize();
    }

    ~IvRankStore() {
        if (db_) sqlite3_close(db_);
    }

    IvRankStore(const IvRankStore&) = delete;
    IvRankStore& operator=(const IvRankStore&) = delete;

    struct Result {
        bool   full_history   = false; // true once >=30 distinct days are available
        double iv_rank        = 50.0;  // 0-100 scale; only meaningful if full_history
        int    days_available = 0;
    };

    // current_iv must be the same annualized-decimal unit (e.g. 0.35 for 35%)
    // fetchIVData already computes as iv_level — historical rows are stored in
    // that same unit by both the Python collector and the Polygon backfill.
    Result computeRank(const std::string& ticker, double current_iv) {
        std::lock_guard<std::mutex> lock(lock_);
        Result out;
        const char* sql =
            "SELECT implied_volatility, date FROM historical_volatility "
            "WHERE ticker = ? ORDER BY date DESC LIMIT 252;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return out; // fail-open: caller falls back to the snapshot proxy
        }
        sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);

        double iv_min = 1e9, iv_max = -1e9;
        int rows = 0, distinct_dates = 0;
        std::string last_date;
        // Rows arrive ORDER BY date DESC; consecutive equal dates only happen if
        // the source ever double-writes a day, so a simple "changed since last
        // row" counter gives the correct distinct-day count in one pass.
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            double iv = sqlite3_column_double(stmt, 0);
            const unsigned char* d = sqlite3_column_text(stmt, 1);
            std::string date = d ? reinterpret_cast<const char*>(d) : "";
            if (date != last_date) { ++distinct_dates; last_date = date; }
            iv_min = std::min(iv_min, iv);
            iv_max = std::max(iv_max, iv);
            ++rows;
        }
        sqlite3_finalize(stmt);

        out.days_available = distinct_dates;
        if (rows == 0 || distinct_dates < 30) return out; // not enough history yet

        out.full_history = true;
        if ((iv_max - iv_min) < 1e-9) {
            out.iv_rank = 50.0; // all IVs identical — clamp to the middle of the range
        } else {
            double r = (current_iv - iv_min) / (iv_max - iv_min);
            r = std::max(0.0, std::min(1.0, r));
            out.iv_rank = r * 100.0;
        }
        return out;
    }

private:
    void initialize() {
        std::lock_guard<std::mutex> lock(lock_);
        // Matches heartbeat/monitor.py's schema exactly — same shared table;
        // whichever service starts first creates it, the other just uses it.
        const char* sql =
            "CREATE TABLE IF NOT EXISTS historical_volatility ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "ticker TEXT NOT NULL, "
            "date DATE NOT NULL, "
            "implied_volatility REAL NOT NULL, "
            "snapshot_timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "UNIQUE(ticker, date));"
            "CREATE INDEX IF NOT EXISTS idx_iv_ticker_date ON historical_volatility(ticker, date);";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, 0, 0, &err) != SQLITE_OK) {
            std::string msg = "IvRankStore init: " + std::string(err ? err : "unknown");
            sqlite3_free(err);
            throw std::runtime_error(msg);
        }
    }

    sqlite3*   db_ = nullptr;
    std::mutex lock_;
};

} // namespace nox::execution

#endif // IV_RANK_STORE_HPP
