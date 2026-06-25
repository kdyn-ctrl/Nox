#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// IBKRClient.hpp — native Interactive Brokers TWS C++ API integration for Nox.
//
// Replaces the Alpaca REST execution path with a persistent socket connection to
// a headless IB Gateway:
//     port 4002 → paper trading
//     port 4001 → live trading
//
// Architecture (mirrors the official TWS reader/processor split):
//
//     ┌────────────────┐  bytes   ┌──────────────┐  signal  ┌─────────────────┐
//     │  IB Gateway    │ ───────► │  EReader      │ ───────► │  message-pump   │
//     │  (TCP socket)  │          │  (own thread) │          │  thread         │
//     └────────────────┘          └──────────────┘          └────────┬────────┘
//                                                                     │ dispatch
//                                                          EWrapper callbacks
//                                                                     │
//                          ┌──────────────────────────────────────────┴──────┐
//                          │  IBKRWrapper (this file)                         │
//                          │   • ticks  → lock-free SPSC ring buffer          │
//                          │   • orders → mutex-guarded state + DB logging    │
//                          └──────────────────────────────────────────────────┘
//                                                                     │
//                                          consumed by the main execution thread
//
// THREADING CONTRACT
//   • EReader runs on its own thread (spawned by EReader::start), decoding the
//     raw byte stream and signalling the pump thread.
//   • IBKRConnection runs a single message-pump thread that calls
//     EReader::processMsgs(); ALL EWrapper callbacks therefore execute on that
//     one thread. It is the single producer for the tick ring buffer.
//   • The main execution thread is the single consumer of the ring buffer and
//     the sole caller of the public order-entry methods.
//   • Shared order/connection state is guarded by std::mutex; the next-order-id
//     counter is a std::atomic; market ticks use a lock-free SPSC ring buffer so
//     the socket path never blocks on the consumer.
//   • DB logging is serialised by ExecutionLogger's own mutex.
//
// TARGET API: TWS API stable 9.81. filled/remaining are `double` (the `Decimal`
// migration landed in 10.10+); `error` is the 3-argument overload. See the
// build note at the bottom of this file for newer-version adjustments.
//
// This unit is intentionally standalone and is NOT wired into main.cpp.
// ─────────────────────────────────────────────────────────────────────────────

// ── TWS API headers (provided by the IBKR TWS API source/, add to include path) ─
#include "EWrapper.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Contract.h"
#include "Order.h"
#include "OrderState.h"
#include "Execution.h"
#include "CommonDefs.h"
#include "TagValue.h"

// ── sqlite3 for thread-safe execution logging (matches PositionManager) ─────────
#include <sqlite3.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace nox::ibkr {

// ─── Plain-old-data events handed to the main execution thread ──────────────────

// A single market data tick. Trivially copyable so it can live in a lock-free
// ring buffer without per-element allocation.
struct MarketTick {
    TickerId   ticker_id = 0;
    int        field     = 0;     // maps to IBKR TickType (BID, ASK, LAST, …)
    double     price     = 0.0;
    int        size      = 0;     // populated for size ticks; 0 for price ticks
    bool       is_size   = false; // true → size tick, false → price tick
};

// Snapshot of an order-status callback, copied out under lock for consumers.
struct OrderUpdate {
    OrderId     order_id        = 0;
    std::string status;
    double      filled          = 0.0;
    double      remaining       = 0.0;
    double      avg_fill_price  = 0.0;
    double      last_fill_price = 0.0;
    int         perm_id         = 0;
    int         client_id       = 0;
};

// A confirmed execution (fill) from execDetails.
struct FillEvent {
    OrderId     order_id   = 0;
    std::string exec_id;
    std::string symbol;
    std::string side;        // "BOT" / "SLD"
    double      shares       = 0.0;
    double      price        = 0.0;
    std::string time;        // IBKR-formatted execution time
};

// ─── Lock-free single-producer / single-consumer ring buffer ────────────────────
//
// Producer  = message-pump thread (IBKRWrapper tick callbacks).
// Consumer  = main execution thread.
// Capacity MUST be a power of two; one slot is reserved to distinguish full/empty.
template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer side. Returns false (and drops the item) when the buffer is full.
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full — never block the socket path
        }
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buffer_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    std::array<T, Capacity>      buffer_{};
    std::atomic<std::size_t>     head_{0}; // written by producer only
    std::atomic<std::size_t>     tail_{0}; // written by consumer only
};

// ─── Thread-safe execution logger (sqlite3 + mutex) ─────────────────────────────
//
// Decoupled from PositionManager so this unit stays standalone. Every write is
// serialised by its own mutex; safe to call from the message-pump thread.
class ExecutionLogger {
public:
    explicit ExecutionLogger(const std::string& db_path);
    ~ExecutionLogger();

    ExecutionLogger(const ExecutionLogger&)            = delete;
    ExecutionLogger& operator=(const ExecutionLogger&) = delete;

    void logFill(const FillEvent& fill);
    void logStatus(const OrderUpdate& update);
    // Persists every IBKR error/notification for post-mortem analysis.
    void logError(int id, int error_code, const std::string& message);

private:
    void initSchema();

    sqlite3*   db_ = nullptr;
    std::mutex db_lock_;
};

// ─── IBKRWrapper — EWrapper implementation (async socket callbacks) ──────────────
//
// Derives from DefaultEWrapper so the ~150 unused callbacks get no-op defaults;
// only the five required by the migration brief (plus market-data ticks) are
// overridden. All callbacks run on the single message-pump thread.
class IBKRWrapper : public DefaultEWrapper {
public:
    explicit IBKRWrapper(ExecutionLogger* logger = nullptr) : logger_(logger) {}

    // ── Required EWrapper overrides ─────────────────────────────────────────────
    void nextValidId(OrderId orderId) override;
    void orderStatus(OrderId orderId, const std::string& status,
                     double filled, double remaining, double avgFillPrice,
                     int permId, int parentId, double lastFillPrice,
                     int clientId, const std::string& whyHeld,
                     double mktCapPrice) override;
    void openOrder(OrderId orderId, const Contract& contract,
                   const Order& order, const OrderState& state) override;
    void execDetails(int reqId, const Contract& contract,
                     const Execution& execution) override;
    void error(int id, int errorCode, const std::string& errorString) override;

    // ── Market data ─────────────────────────────────────────────────────────────
    void tickPrice(TickerId tickerId, TickType field, double price,
                   const TickAttrib& attribs) override;
    void tickSize(TickerId tickerId, TickType field, int size) override;

    // ── Consumer-facing accessors (main execution thread) ───────────────────────

    // True once the gateway has handed us a valid starting order id.
    bool hasValidOrderId() const noexcept {
        return order_id_valid_.load(std::memory_order_acquire);
    }

    // Atomically reserve the next order id for a placeOrder call.
    OrderId reserveOrderId() noexcept {
        return next_order_id_.fetch_add(1, std::memory_order_acq_rel);
    }

    // Drain one tick from the lock-free buffer. Returns false when empty.
    bool popTick(MarketTick& out) noexcept { return tick_buffer_.pop(out); }

    // Copy the latest known status for an order, if any.
    bool latestStatus(OrderId id, OrderUpdate& out) const;

private:
    ExecutionLogger* logger_ = nullptr; // not owned

    std::atomic<OrderId> next_order_id_{0};
    std::atomic<bool>    order_id_valid_{false};

    SpscRingBuffer<MarketTick, 4096> tick_buffer_;

    mutable std::mutex                         order_lock_;
    std::unordered_map<OrderId, OrderUpdate>   order_state_; // guarded by order_lock_
};

// ─── IBKRConnection — EClientSocket lifecycle + message pump ─────────────────────
//
// SignalHolder is listed first in the base list so the EReaderOSSignal is fully
// constructed before EClientSocket's constructor captures its address.
namespace detail {
struct SignalHolder {
    EReaderOSSignal signal;
    SignalHolder() : signal(/*waitTimeoutMs=*/2000) {}
};
} // namespace detail

class IBKRConnection : private detail::SignalHolder, public EClientSocket {
public:
    explicit IBKRConnection(IBKRWrapper& wrapper)
        : detail::SignalHolder()
        , EClientSocket(&wrapper, &signal)
        , wrapper_(wrapper) {}

    ~IBKRConnection() override { disconnect(); }

    IBKRConnection(const IBKRConnection&)            = delete;
    IBKRConnection& operator=(const IBKRConnection&) = delete;

    // Open the socket, start the reader thread, and launch the message pump.
    // host is typically "127.0.0.1"; port 4002 (paper) or 4001 (live).
    bool connect(const char* host, int port, int clientId = 0);

    // Stop the pump, join threads, and close the socket. Idempotent.
    void disconnect();

private:
    void messagePump();

    IBKRWrapper&             wrapper_;
    std::unique_ptr<EReader> reader_;
    std::thread              pump_thread_;
    std::atomic<bool>        running_{false};
};

} // namespace nox::ibkr

// ─────────────────────────────────────────────────────────────────────────────
// BUILD INTEGRATION
//   1. Vendor the IBKR TWS API C++ source under a path on the include search,
//      e.g.  -I third_party/twsapi/source/cppclient/client
//   2. Compile the TWS client .cpp files together with IBKRClient.cpp, e.g.:
//        g++ -std=c++17 -pthread \
//            -I third_party/twsapi/source/cppclient/client \
//            execution/IBKRClient.cpp \
//            third_party/twsapi/source/cppclient/client/*.cpp \
//            -lsqlite3 -lssl -lcrypto -o nox_ibkr_test
//   3. For TWS API ≥ 10.10 change `double filled/remaining` to `Decimal` in
//      orderStatus and `int size` to `Decimal` in tickSize, and switch `error`
//      to the 5-argument overload (adds time_t + advancedOrderRejectJson).
// ─────────────────────────────────────────────────────────────────────────────
