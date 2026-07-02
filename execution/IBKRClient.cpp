// ─────────────────────────────────────────────────────────────────────────────
// IBKRClient.cpp — implementation for IBKRClient.hpp.
// See the header for the full threading contract and build notes.
// ─────────────────────────────────────────────────────────────────────────────

#include "IBKRClient.hpp"

#include <iostream>
#include <stdexcept>

namespace nox::ibkr {

// ═══ ExecutionLogger ════════════════════════════════════════════════════════════

ExecutionLogger::ExecutionLogger(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error("ExecutionLogger: cannot open DB: " + msg);
    }
    // Serialise concurrent access at the SQLite layer as a second line of
    // defence; the mutex below is the primary guard.
    sqlite3_busy_timeout(db_, 5000);
    initSchema();
}

ExecutionLogger::~ExecutionLogger() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void ExecutionLogger::initSchema() {
    std::lock_guard<std::mutex> lock(db_lock_);
    const char* sql =
        "CREATE TABLE IF NOT EXISTS ibkr_fills ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  order_id INTEGER, exec_id TEXT, symbol TEXT, side TEXT,"
        "  shares REAL, price REAL, exec_time TEXT,"
        "  logged_at TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS ibkr_order_status ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  order_id INTEGER, status TEXT, filled REAL, remaining REAL,"
        "  avg_fill_price REAL, last_fill_price REAL, perm_id INTEGER, client_id INTEGER,"
        "  logged_at TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS ibkr_errors ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ref_id INTEGER, error_code INTEGER, message TEXT,"
        "  logged_at TEXT DEFAULT CURRENT_TIMESTAMP);";
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("ExecutionLogger: schema init failed: " + msg);
    }
}

void ExecutionLogger::logFill(const FillEvent& fill) {
    std::lock_guard<std::mutex> lock(db_lock_);
    const char* sql =
        "INSERT INTO ibkr_fills (order_id, exec_id, symbol, side, shares, price, exec_time) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64 (stmt, 1, static_cast<sqlite3_int64>(fill.order_id));
    sqlite3_bind_text  (stmt, 2, fill.exec_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 3, fill.symbol.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 4, fill.side.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, fill.shares);
    sqlite3_bind_double(stmt, 6, fill.price);
    sqlite3_bind_text  (stmt, 7, fill.time.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void ExecutionLogger::logStatus(const OrderUpdate& u) {
    std::lock_guard<std::mutex> lock(db_lock_);
    const char* sql =
        "INSERT INTO ibkr_order_status "
        "(order_id, status, filled, remaining, avg_fill_price, last_fill_price, perm_id, client_id) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64 (stmt, 1, static_cast<sqlite3_int64>(u.order_id));
    sqlite3_bind_text  (stmt, 2, u.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, u.filled);
    sqlite3_bind_double(stmt, 4, u.remaining);
    sqlite3_bind_double(stmt, 5, u.avg_fill_price);
    sqlite3_bind_double(stmt, 6, u.last_fill_price);
    sqlite3_bind_int   (stmt, 7, u.perm_id);
    sqlite3_bind_int   (stmt, 8, u.client_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void ExecutionLogger::logError(int id, int error_code, const std::string& message) {
    std::lock_guard<std::mutex> lock(db_lock_);
    const char* sql =
        "INSERT INTO ibkr_errors (ref_id, error_code, message) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int (stmt, 1, id);
    sqlite3_bind_int (stmt, 2, error_code);
    sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ═══ IBKRWrapper — EWrapper callbacks (message-pump thread) ══════════════════════

void IBKRWrapper::nextValidId(OrderId orderId) {
    // Gateway handshake complete. Seed the atomic counter and flag readiness.
    next_order_id_.store(orderId, std::memory_order_release);
    order_id_valid_.store(true, std::memory_order_release);
    std::cout << "[IBKR] nextValidId=" << orderId << " — connection ready\n";
}

void IBKRWrapper::orderStatus(OrderId orderId, const std::string& status,
                              double filled, double remaining, double avgFillPrice,
                              int permId, int /*parentId*/, double lastFillPrice,
                              int clientId, const std::string& /*whyHeld*/,
                              double /*mktCapPrice*/) {
    OrderUpdate u;
    u.order_id        = orderId;
    u.status          = status;
    u.filled          = filled;
    u.remaining       = remaining;
    u.avg_fill_price  = avgFillPrice;
    u.last_fill_price = lastFillPrice;
    u.perm_id         = permId;
    u.client_id       = clientId;

    {
        std::lock_guard<std::mutex> lock(order_lock_);
        order_state_[orderId] = u;
    }
    if (logger_) logger_->logStatus(u);
}

void IBKRWrapper::openOrder(OrderId orderId, const Contract& contract,
                            const Order& order, const OrderState& state) {
    // Confirms IBKR accepted the order definition; useful for margin/commission.
    std::cout << "[IBKR] openOrder id=" << orderId
              << " " << contract.symbol
              << " " << order.action << " " << order.totalQuantity
              << " status=" << state.status << '\n';
}

void IBKRWrapper::execDetails(int /*reqId*/, const Contract& contract,
                              const Execution& execution) {
    FillEvent fill;
    fill.order_id = execution.orderId;
    fill.exec_id  = execution.execId;
    fill.symbol   = contract.symbol;
    fill.side     = execution.side;     // "BOT" / "SLD"
    fill.shares   = execution.shares;
    fill.price    = execution.price;
    fill.time     = execution.time;

    if (logger_) logger_->logFill(fill);
    std::cout << "[IBKR] FILL " << fill.symbol << " " << fill.side
              << " " << fill.shares << " @ " << fill.price << '\n';
}

void IBKRWrapper::error(int id, int errorCode, const std::string& errorString) {
    // Codes 2104/2106/2158 are benign market-data farm "OK" notices.
    const bool benign = (errorCode == 2104 || errorCode == 2106 || errorCode == 2158);
    if (benign) {
        std::cout << "[IBKR] info(" << errorCode << "): " << errorString << '\n';
    } else {
        std::cerr << "[IBKR] ERROR id=" << id << " code=" << errorCode
                  << " : " << errorString << '\n';
    }
    if (logger_) logger_->logError(id, errorCode, errorString);
}

void IBKRWrapper::tickPrice(TickerId tickerId, TickType field, double price,
                            const TickAttrib& /*attribs*/) {
    MarketTick t;
    t.ticker_id = tickerId;
    t.field     = static_cast<int>(field);
    t.price     = price;
    t.is_size   = false;
    // Drop on overflow rather than block the socket path; consumer fell behind.
    tick_buffer_.push(t);
}

void IBKRWrapper::tickSize(TickerId tickerId, TickType field, int size) {
    MarketTick t;
    t.ticker_id = tickerId;
    t.field     = static_cast<int>(field);
    t.size      = size;
    t.is_size   = true;
    tick_buffer_.push(t);
}

bool IBKRWrapper::latestStatus(OrderId id, OrderUpdate& out) const {
    std::lock_guard<std::mutex> lock(order_lock_);
    auto it = order_state_.find(id);
    if (it == order_state_.end()) return false;
    out = it->second;
    return true;
}

// ═══ IBKRConnection — socket lifecycle + message pump ════════════════════════════

bool IBKRConnection::connect(const char* host, int port, int clientId) {
    if (running_.load(std::memory_order_acquire)) {
        std::cerr << "[IBKR] connect() ignored — already connected\n";
        return true;
    }

    if (!eConnect(host, port, clientId, /*extraAuth=*/false)) {
        std::cerr << "[IBKR] eConnect failed to " << host << ':' << port << '\n';
        return false;
    }

    // EReader owns the socket-read thread; it decodes bytes and issues signals.
    reader_ = std::make_unique<EReader>(this, &signal);
    reader_->start();

    running_.store(true, std::memory_order_release);
    pump_thread_ = std::thread(&IBKRConnection::messagePump, this);

    std::cout << "[IBKR] connected to " << host << ':' << port
              << " (clientId=" << clientId << ")\n";
    return true;
}

void IBKRConnection::messagePump() {
    // Single dispatch thread: waits for the reader's signal, then drains all
    // decoded messages, invoking EWrapper callbacks on this thread.
    while (running_.load(std::memory_order_acquire) && isConnected()) {
        signal.waitForSignal();
        reader_->processMsgs();
    }
}

void IBKRConnection::disconnect() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return; // already stopped — idempotent
    }

    // Wake the pump out of waitForSignal so it can observe running_ == false.
    signal.issueSignal();
    if (pump_thread_.joinable()) {
        pump_thread_.join();
    }

    eDisconnect();
    reader_.reset(); // joins the reader thread in its destructor
    std::cout << "[IBKR] disconnected\n";
}

} // namespace nox::ibkr
