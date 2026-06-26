#ifndef POSITION_MANAGER_HPP
#define POSITION_MANAGER_HPP

#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <sqlite3.h>
#include <stdexcept>
#include <chrono>
#include <sstream>
#include <iomanip>

// Forward declaration
namespace nox { namespace options_router { class OptionsOrderRouter; } }
class TelegramNotifier;

struct OptionPosition {
    long id;
    std::string ticker;
    std::string option_type; // "call" or "put"
    double strike;
    int quantity;
    double entry_price;
    std::string entry_date;
    std::string profile_type; // "long" or "short_premium"
    std::string expiration_date;
};

class PositionManager {
public:
    PositionManager(const std::string& db_path, nox::options_router::OptionsOrderRouter& order_router)
        : db_path_(db_path), order_router_(order_router), db_(nullptr) {
        if (sqlite3_open(db_path.c_str(), &db_)) {
            throw std::runtime_error("Can't open database: " + std::string(sqlite3_errmsg(db_)));
        }
        initialize_database();
    }

    ~PositionManager() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    void start_monitoring() {
        monitoring_thread_ = std::thread(&PositionManager::monitor_positions, this);
    }

    void stop_monitoring() {
        {
            std::lock_guard<std::mutex> lock(monitor_lock_);
            run_monitoring_ = false;
        }
        // Wake the monitoring thread immediately so shutdown doesn't block for up
        // to a full 30-minute sleep cycle before join() can return.
        monitor_cv_.notify_all();
        if (monitoring_thread_.joinable()) {
            monitoring_thread_.join();
        }
    }

    // Record a new open option position in the database
    void add_position(const std::string& ticker,
                      const std::string& option_type,
                      double strike,
                      int quantity,
                      double entry_price,
                      const std::string& entry_date,
                      const std::string& profile_type,
                      const std::string& expiration_date)
    {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql = "INSERT INTO open_positions "
                          "(ticker, option_type, strike, quantity, entry_price, entry_date, profile_type, expiration_date) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, option_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, strike);
            sqlite3_bind_int(stmt, 4, quantity);
            sqlite3_bind_double(stmt, 5, entry_price);
            sqlite3_bind_text(stmt, 6, entry_date.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, profile_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 8, expiration_date.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

private:
    std::string db_path_;
    sqlite3* db_;
    std::mutex db_lock_;
    nox::options_router::OptionsOrderRouter& order_router_;
    std::thread monitoring_thread_;
    // Written by stop_monitoring() (caller thread), read by monitor_positions()
    // (monitoring thread) — must be atomic to avoid a data race / hoisted read.
    std::atomic<bool> run_monitoring_{true};
    // Guards the interruptible inter-cycle wait so stop_monitoring() can wake the
    // monitoring thread out of its sleep instead of waiting up to 30 minutes.
    std::mutex monitor_lock_;
    std::condition_variable monitor_cv_;

    void initialize_database() {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql = "CREATE TABLE IF NOT EXISTS open_positions ("
                          "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                          "ticker TEXT NOT NULL, "
                          "option_type TEXT NOT NULL, "
                          "strike REAL NOT NULL, "
                          "quantity INTEGER NOT NULL, "
                          "entry_price REAL NOT NULL, "
                          "entry_date TEXT NOT NULL, "
                          "profile_type TEXT NOT NULL, "
                          "expiration_date TEXT NOT NULL);";
        char* err_msg = nullptr;
        if (sqlite3_exec(db_, sql, 0, 0, &err_msg) != SQLITE_OK) {
            std::string err = "SQL error: " + std::string(err_msg);
            sqlite3_free(err_msg);
            throw std::runtime_error(err);
        }
    }

    void monitor_positions(); // Implementation will be in a .cpp file

    std::vector<OptionPosition> get_open_positions() {
        std::lock_guard<std::mutex> lock(db_lock_);
        std::vector<OptionPosition> positions;
        const char* sql = "SELECT id, ticker, option_type, strike, quantity, entry_price, entry_date, profile_type, expiration_date FROM open_positions;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                OptionPosition pos;
                pos.id = sqlite3_column_int64(stmt, 0);
                pos.ticker = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                pos.option_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                pos.strike = sqlite3_column_double(stmt, 3);
                pos.quantity = sqlite3_column_int(stmt, 4);
                pos.entry_price = sqlite3_column_double(stmt, 5);
                pos.entry_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                pos.profile_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                pos.expiration_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
                positions.push_back(pos);
            }
        }
        sqlite3_finalize(stmt);
        return positions;
    }
    
    void remove_position(long position_id) {
        std::lock_guard<std::mutex> lock(db_lock_);
        const char* sql = "DELETE FROM open_positions WHERE id = ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, position_id);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }
};

#endif // POSITION_MANAGER_HPP