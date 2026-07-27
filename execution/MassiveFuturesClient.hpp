#pragma once

// MassiveFuturesClient — thin REST client for Massive's (formerly Polygon.io)
// Futures API. Signal-only: this client has no order-placement capability at
// all, by design (CLAUDE.md futures phase is data-in/signal-out only).
//
// Auth + request shape mirrors heartbeat/polygon_iv_backfill.py's
// _polygon_get(): API key passed as a query param, not a header. Massive is
// the 2025-10-30 rebrand of Polygon.io and keeps the same aggregates-endpoint
// convention on the equities/options side; the exact Futures API path should
// be confirmed against Massive's current docs at integration time since that
// product shipped after this client was written.

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"

#include <chrono>
#include <iostream>
#include <string>

using json = nlohmann::json;

namespace nox::execution {

class MassiveFuturesClient {
public:
    MassiveFuturesClient(std::string apiKey, std::string baseUrl = "https://api.massive.co")
        : apiKey_(std::move(apiKey)), baseUrl_(std::move(baseUrl)) {}

    struct Bar {
        double close    = 0.0;
        double prevClose = 0.0;
        bool   valid    = false;
    };

    // `contract` is a futures root symbol, e.g. "CL". Returns the latest daily
    // close plus the prior day's close so callers can derive a simple momentum
    // sign without this client needing its own indicator math.
    Bar getLatestDailyBars(const std::string& contract) const {
        Bar out;
        if (apiKey_.empty()) return out; // fail-open: no key configured yet

        try {
            httplib::Client cli(baseUrl_);
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(12));

            std::string path = "/futures/vX/aggs/ticker/" + contract +
                                "/range/1/day/2/" +
                                "?adjusted=true&sort=desc&limit=2&apiKey=" + apiKey_;
            auto res = cli.Get(path.c_str());
            if (!res || res->status != 200) {
                log("WARN", "[MASSIVE_FUTURES] " + contract + " request failed — HTTP " +
                    (res ? std::to_string(res->status) : "TIMEOUT"));
                return out;
            }

            auto body = json::parse(res->body);
            const auto& results = body.at("results");
            if (results.size() < 2) return out;

            out.close     = results.at(0).at("c").get<double>();
            out.prevClose = results.at(1).at("c").get<double>();
            out.valid     = true;
        } catch (const std::exception& e) {
            log("WARN", "[MASSIVE_FUTURES] " + contract + " parse/fetch error: " + std::string(e.what()));
        }
        return out;
    }

private:
    std::string apiKey_;
    std::string baseUrl_;

    static void log(const std::string& level, const std::string& msg) {
        std::cout << "[" << level << "] " << msg << std::endl;
    }
};

} // namespace nox::execution
