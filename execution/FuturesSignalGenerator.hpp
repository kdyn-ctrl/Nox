#pragma once

// FuturesSignalGenerator — CLAUDE.md futures phase 1: signals only, no order
// routing, no IBKR dependency. Mirrors EquitySignalGenerator's shape (own
// scan loop, own Telegram alert, no shared base class — this repo doesn't use
// one for signal generators).
//
// Combines two independently-sourced signals for each futures contract:
//   1. Price momentum from Massive's Futures API (MassiveFuturesClient).
//   2. america_data_engine's alt_macro physical-supply-vs-political-text
//      verdict (GET /macro/alt), which already exists and needed no new
//      macro logic to reuse — this generator is the first consumer of it
//      outside america_data_engine itself.
//
// v1 scope is deliberately CL crude only, because alt_macro's HORMUZ
// chokepoint already lists "CL" in its tickers and its physical-stress score
// is a direct, un-invented proxy for crude supply pressure. Other contracts
// (VX, ES/NQ) are follow-ups once this pattern proves out, per the approved
// plan — not built here.

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "MassiveFuturesClient.hpp"
#include "FuturesSignalStore.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace nox::futures_signal {

class FuturesSignalGenerator {
public:
    FuturesSignalGenerator(std::string webhookSecret,
                            std::string tgToken,
                            std::string tgChatId,
                            std::vector<std::string> watchlist,
                            double alertThreshold,
                            std::shared_ptr<nox::execution::MassiveFuturesClient> massiveClient,
                            std::shared_ptr<nox::execution::FuturesSignalStore> store,
                            std::string altMacroBaseUrl = "http://america-data-engine:8001")
        : webhookSecret_(std::move(webhookSecret))
        , tgToken_(std::move(tgToken))
        , tgChatId_(std::move(tgChatId))
        , watchlist_(std::move(watchlist))
        , alertThreshold_(alertThreshold)
        , massiveClient_(std::move(massiveClient))
        , store_(std::move(store))
        , altMacroBaseUrl_(std::move(altMacroBaseUrl))
    {}

    void run_scan() {
        json altMacro = fetchAltMacro();

        for (const auto& contract : watchlist_) {
            try {
                evaluateContract(contract, altMacro);
            } catch (const std::exception& e) {
                log("WARN", "[FUTURES_SCAN] Exception on " + contract + ": " + e.what());
            }
        }
    }

private:
    std::string webhookSecret_;
    std::string tgToken_;
    std::string tgChatId_;
    std::vector<std::string> watchlist_;
    double alertThreshold_;
    std::shared_ptr<nox::execution::MassiveFuturesClient> massiveClient_;
    std::shared_ptr<nox::execution::FuturesSignalStore> store_;
    std::string altMacroBaseUrl_;

    static void log(const std::string& level, const std::string& msg) {
        std::cout << "[" << level << "] " << msg << std::endl;
    }

    static std::string fmt(double v, int decimals = 4) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(decimals) << v;
        return oss.str();
    }

    static long long nowUnix() {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    // Empty object on any failure — evaluateContract() falls back to
    // momentum-only when alt_macro is unreachable, same fail-open philosophy
    // as every other macro-input in this codebase.
    json fetchAltMacro() const {
        try {
            httplib::Client cli(altMacroBaseUrl_);
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));
            httplib::Headers headers = {{"X-Nox-Token", webhookSecret_}};
            auto res = cli.Get("/macro/alt", headers);
            if (!res || res->status != 200) {
                log("WARN", "[FUTURES_SCAN] alt_macro fetch failed — HTTP " +
                    (res ? std::to_string(res->status) : "TIMEOUT"));
                return json::object();
            }
            return json::parse(res->body);
        } catch (const std::exception& e) {
            log("WARN", "[FUTURES_SCAN] alt_macro fetch exception: " + std::string(e.what()));
            return json::object();
        }
    }

    // Finds the alt_macro region whose "tickers" list contains this contract's
    // root symbol. CL maps to the HORMUZ chokepoint today (see alt_macro.py's
    // CHOKEPOINTS table) — this lookup is generic so a future contract just
    // needs to appear in some region's tickers list, no new wiring required.
    json findMatchingRegion(const json& altMacro, const std::string& contract) const {
        if (!altMacro.contains("regions")) return json();
        for (const auto& region : altMacro.at("regions")) {
            if (!region.contains("tickers")) continue;
            for (const auto& t : region.at("tickers")) {
                if (t.is_string() && t.get<std::string>() == contract) return region;
            }
        }
        return json();
    }

public:
    // Pure combination logic, no I/O — public + static so tests can exercise
    // it directly with fabricated bar/region data instead of standing up a
    // mock HTTP server (there's nothing to mock here, just arithmetic).
    static nox::execution::FuturesSignal computeSignal(
            const std::string& contract,
            const nox::execution::MassiveFuturesClient::Bar& bar,
            const json& region,
            long long scanAt) {
        double momentum = 0.0;
        if (bar.valid && bar.prevClose > 1e-9) {
            momentum = (bar.close - bar.prevClose) / bar.prevClose;
        }

        nox::execution::FuturesSignal sig;
        sig.contract = contract;
        sig.price    = bar.close;
        sig.scan_at  = scanAt;

        bool haveMacro = !region.is_null() &&
                          region.value("verdict", "NO_DATA") != "NO_DATA";

        if (haveMacro) {
            sig.physical_stress  = region.value("physical_stress", json(nullptr)).is_null()
                                        ? 0.0 : region.at("physical_stress").get<double>();
            sig.political_signal = region.value("political_signal", json(nullptr)).is_null()
                                        ? 0.0 : region.at("political_signal").get<double>();
            sig.macro_verdict = region.value("verdict", "");
            sig.macro_bias    = region.value("bias", "");
            sig.direction     = (sig.macro_bias == "BULLISH_OIL") ? "BULLISH" : "BEARISH";
            double driver = (std::abs(sig.physical_stress) > 1e-9) ? sig.physical_stress
                                                                    : sig.political_signal;
            sig.quality_score = std::min(1.0, std::abs(driver));
            sig.reason = region.value("reason", "Alt-macro physical/political signal.");
        } else if (bar.valid) {
            sig.direction     = (momentum > 0.0) ? "BULLISH" : (momentum < 0.0 ? "BEARISH" : "NEUTRAL");
            sig.quality_score = std::min(1.0, std::abs(momentum) * 20.0);
            sig.reason = "No significant alt-macro signal this cycle — momentum-only bias.";
        } else {
            sig.direction     = "NEUTRAL";
            sig.quality_score = 0.0;
            sig.reason = "No market data and no macro signal this cycle.";
        }
        return sig;
    }

private:
    void evaluateContract(const std::string& contract, const json& altMacro) {
        auto bar = massiveClient_->getLatestDailyBars(contract);
        json region = findMatchingRegion(altMacro, contract);
        nox::execution::FuturesSignal sig = computeSignal(contract, bar, region, nowUnix());

        if (!store_->insert(sig)) {
            log("WARN", "[FUTURES_SCAN] Failed to persist signal for " + contract);
        }

        log("INFO", "[FUTURES_SCAN] " + contract + " — direction=" + sig.direction +
            " quality=" + fmt(sig.quality_score, 2) + " verdict=" + sig.macro_verdict);

        if (sig.direction != "NEUTRAL" && sig.quality_score >= alertThreshold_) {
            sendTelegram(
                "\U0001F6E2 *Futures Signal — " + contract + "*\n"
                "──────────────\n"
                "• *Direction:* " + sig.direction + "\n"
                "• *Quality:* " + fmt(sig.quality_score, 2) + "\n"
                "• *Macro verdict:* " + (sig.macro_verdict.empty() ? "n/a" : sig.macro_verdict) + "\n"
                "• *Reason:* " + sig.reason + "\n"
                "_Signal-only — no order was placed._"
            );
        }
    }

    void sendTelegram(const std::string& message) const {
        if (tgToken_.empty() || tgChatId_.empty()) return;
        try {
            httplib::Client cli("https://api.telegram.org");
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));
            json body = {
                {"chat_id",    tgChatId_},
                {"text",       message},
                {"parse_mode", "Markdown"}
            };
            cli.Post(("/bot" + tgToken_ + "/sendMessage").c_str(),
                     body.dump(), "application/json");
        } catch (...) {
            log("WARN", "[FUTURES_SCAN] Telegram delivery failed.");
        }
    }
};

} // namespace nox::futures_signal
