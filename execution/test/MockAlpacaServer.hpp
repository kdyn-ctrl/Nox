#ifndef MOCK_ALPACA_SERVER_HPP
#define MOCK_ALPACA_SERVER_HPP

// MockAlpacaServer — a lean, in-process Alpaca stand-in for Phase 1 tests.
//
// It implements just the endpoints the options order path touches:
//   GET  /v2/options/contracts            → one canned contract (so route() gets past lookup)
//   POST /v2/orders                       → behavior switched by mode()
//   GET  /v2/orders:by_client_order_id    → returns a recorded order or 404
//
// Failure modes (the only ones worth testing, per CLAUDE.md item 7):
//   Normal    — 200 accepted, order recorded.
//   Ghost     — records the order as FILLED, then sleeps past the client read
//               timeout so the client sees no response (the ghost fill). The
//               broker holds a filled order our process never learned about.
//   Rate429   — 429, nothing recorded (accidental-retry territory).
//   Server500 — 500, nothing recorded (transient; signal regenerates next scan).
//   Malformed — 200 with a non-JSON body; broker recorded it as accepted.

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "../httplib.h"
#include "../nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace nox::test {

using json = nlohmann::json;

class MockAlpacaServer {
public:
    enum class Mode { Normal, Ghost, Rate429, Server500, Malformed };

    explicit MockAlpacaServer(int port) : port_(port) {
        // GET /v2/options/contracts — one contract so lookupContract() succeeds.
        svr_.Get("/v2/options/contracts",
                 [](const httplib::Request&, httplib::Response& res) {
            json body = {{"option_contracts", json::array({
                {{"symbol", "TEST260821C00100000"},
                 {"strike_price", "100.00"},
                 {"expiration_date", "2026-08-21"}}
            })}};
            res.set_content(body.dump(), "application/json");
        });

        // POST /v2/orders — the switchable path.
        svr_.Post("/v2/orders",
                  [this](const httplib::Request& req, httplib::Response& res) {
            std::string coid;
            try {
                json b = json::parse(req.body);
                coid = b.value("client_order_id", "");
                std::lock_guard<std::mutex> lk(mtx_);
                last_order_body_ = b;
            } catch (...) {}
            if (!coid.empty()) {
                std::lock_guard<std::mutex> lk(mtx_);
                post_counts_[coid]++;
            }
            Mode m = mode_.load();

            if (m == Mode::Rate429)  { res.status = 429; res.set_content("{\"message\":\"rate limit\"}", "application/json"); return; }
            if (m == Mode::Server500){ res.status = 500; res.set_content("{\"message\":\"server error\"}", "application/json"); return; }

            if (m == Mode::Ghost) {
                // Broker fills it, then the response is lost (sleep past the
                // client's 10s read timeout). Our process never sees this fill.
                record(coid, "filled");
                std::this_thread::sleep_for(std::chrono::seconds(ghost_delay_s_));
                json ok = {{"id", broker_id(coid)}, {"status", "filled"}, {"client_order_id", coid}};
                res.set_content(ok.dump(), "application/json");
                return;
            }

            if (m == Mode::Malformed) {
                record(coid, "accepted"); // broker did accept it
                res.set_content("{ this is not valid json", "application/json");
                return;
            }

            // Normal
            record(coid, "accepted");
            json ok = {{"id", broker_id(coid)}, {"status", "accepted"}, {"client_order_id", coid}};
            res.set_content(ok.dump(), "application/json");
        });

        // GET /v2/orders:by_client_order_id?client_order_id=...
        svr_.Get("/v2/orders:by_client_order_id",
                 [this](const httplib::Request& req, httplib::Response& res) {
            std::string coid = req.get_param_value("client_order_id");
            std::string status;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = orders_.find(coid);
                if (it != orders_.end()) status = it->second;
            }
            if (status.empty()) { res.status = 404; res.set_content("{\"message\":\"order not found\"}", "application/json"); return; }
            json body = {{"id", broker_id(coid)}, {"status", status}, {"client_order_id", coid}};
            res.set_content(body.dump(), "application/json");
        });

        // GET /v2/account — options_trading_level for validateNakedOptionsApproval().
        svr_.Get("/v2/account", [this](const httplib::Request&, httplib::Response& res) {
            json body = {{"options_trading_level", options_trading_level_.load()}};
            res.set_content(body.dump(), "application/json");
        });
    }

    void set_options_trading_level(int level) { options_trading_level_.store(level); }

    // Last POST /v2/orders body, parsed — lets a test inspect the legs/sides
    // actually submitted (e.g. STRANGLE routing as sell/sell, not buy/buy).
    json last_order_body() {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_order_body_;
    }

    ~MockAlpacaServer() { stop(); }

    void start() {
        thread_ = std::thread([this] { svr_.listen("127.0.0.1", port_); });
        // Wait until the server is actually accepting connections.
        for (int i = 0; i < 200 && !svr_.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void stop() {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    void set_mode(Mode m) { mode_.store(m); }
    void set_ghost_delay(int seconds) { ghost_delay_s_ = seconds; }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    int post_count(const std::string& coid) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = post_counts_.find(coid);
        return it == post_counts_.end() ? 0 : it->second;
    }

    // Force a recorded order's status (used to seed a divergent state in tests).
    void force_status(const std::string& coid, const std::string& status) {
        record(coid, status);
    }

private:
    void record(const std::string& coid, const std::string& status) {
        if (coid.empty()) return;
        std::lock_guard<std::mutex> lk(mtx_);
        orders_[coid] = status;
    }
    static std::string broker_id(const std::string& coid) { return "brk-" + coid; }

    httplib::Server           svr_;
    std::thread               thread_;
    int                       port_;
    std::atomic<Mode>         mode_{Mode::Normal};
    int                       ghost_delay_s_ = 12; // > client 10s read timeout
    std::mutex                mtx_;
    std::map<std::string, std::string> orders_;      // client_oid → status
    std::map<std::string, int>         post_counts_; // client_oid → POST attempts
    std::atomic<int>          options_trading_level_{3}; // default: naked-options approved
    json                      last_order_body_ = json::object();
};

} // namespace nox::test

#endif // MOCK_ALPACA_SERVER_HPP
