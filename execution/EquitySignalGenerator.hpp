#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// Standalone equity momentum scanner — operates independently of the Skeptic
// workstreams. Uses the same RSI/ATR/SMA bar logic as the options scanner but
// posts BUY signals directly to the execution engine's own webhook so all
// existing gates (RSI floor, liquidity gate, Kelly sizing) still apply.
//
// Purpose: validate the full signal→execution→Alpaca pipeline and produce
// trade data while Skeptic signal conditions are quiet.
// ─────────────────────────────────────────────────────────────────────────────

namespace nox::equity_signal {

struct EquitySetup {
    std::string ticker;
    double price        = 0.0;
    double sma20        = 0.0;
    double sma50        = 0.0;
    double rsi          = 50.0;
    double atr          = 0.0;
    double vix          = 20.0;
    double spy_price    = 0.0;
    double spy_200_sma  = 0.0;
    double quality_score = 0.0;
};

class EquitySignalGenerator {
public:
    EquitySignalGenerator(const std::string& webhookSecret,
                          const std::string& tgToken,
                          const std::string& tgChatId,
                          const std::vector<std::string>& watchlist,
                          int maxSignals,
                          bool bypassHours)
        : webhookSecret_(webhookSecret)
        , tgToken_(tgToken)
        , tgChatId_(tgChatId)
        , watchlist_(watchlist)
        , maxSignals_(maxSignals)
        , bypassHours_(bypassHours)
    {}

    void run_scan() {
        if (!bypassHours_ && !isMarketHours()) {
            log("INFO", "[EQUITY_SCAN] Outside market hours — scan skipped.");
            return;
        }

        double vix      = fetchVix();
        SpyData spy     = fetchSpy();
        if (vix < 0) vix = 20.0;

        log("INFO", "[EQUITY_SCAN] Starting scan | Tickers=" +
            std::to_string(watchlist_.size()) +
            " | VIX=" + fmt(vix, 1) +
            " | MaxSignals=" + std::to_string(maxSignals_));

        std::vector<EquitySetup> candidates;

        for (const auto& ticker : watchlist_) {
            try {
                auto result = evaluateTicker(ticker, vix, spy);
                if (result) candidates.push_back(std::move(*result));
            } catch (const std::exception& e) {
                log("WARN", "[EQUITY_SCAN] Exception on " + ticker + ": " + e.what());
            }
            // Rate-limit Yahoo Finance requests
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }

        if (candidates.empty()) {
            log("INFO", "[EQUITY_SCAN] No qualifying setups this cycle.");
            return;
        }

        // Rank by quality score, dispatch top N
        std::sort(candidates.begin(), candidates.end(),
            [](const EquitySetup& a, const EquitySetup& b) {
                return a.quality_score > b.quality_score;
            });

        int dispatched = 0;
        for (const auto& setup : candidates) {
            if (dispatched >= maxSignals_) break;
            postSignal(setup);
            dispatched++;
        }

        int suppressed = static_cast<int>(candidates.size()) - dispatched;
        if (suppressed > 0) {
            log("INFO", "[EQUITY_SCAN] " + std::to_string(suppressed) +
                " lower-quality setup(s) suppressed by cap.");
        }
    }

private:
    std::string              webhookSecret_;
    std::string              tgToken_;
    std::string              tgChatId_;
    std::vector<std::string> watchlist_;
    int                      maxSignals_;
    bool                     bypassHours_;

    // ── Utilities ─────────────────────────────────────────────────────────────

    static void log(const std::string& level, const std::string& msg) {
        std::cout << "[" << level << "] " << msg << std::endl;
    }

    static std::string fmt(double v, int decimals = 2) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(decimals) << v;
        return oss.str();
    }

    // Mon–Fri 09:30–16:00 ET (approximate DST: Apr–Oct = UTC-4, else UTC-5)
    static bool isMarketHours() {
        auto now    = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&time_t, &utc);
        if (utc.tm_wday == 0 || utc.tm_wday == 6) return false;
        int offset_h = (utc.tm_mon >= 3 && utc.tm_mon <= 9) ? 4 : 5;
        int et_mins  = ((utc.tm_hour - offset_h + 24) % 24) * 60 + utc.tm_min;
        return et_mins >= 9 * 60 + 30 && et_mins < 16 * 60;
    }

    // ── Market data ───────────────────────────────────────────────────────────

    double fetchVix() const {
        try {
            httplib::Client cli("https://query1.finance.yahoo.com");
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(12));
            auto res = cli.Get("/v8/finance/chart/%5EVIX?interval=1d&range=2d");
            if (!res || res->status != 200) return -1.0;
            auto body = json::parse(res->body);
            const auto& closes = body.at("chart").at("result").at(0)
                                      .at("indicators").at("quote").at(0).at("close");
            for (int i = static_cast<int>(closes.size()) - 1; i >= 0; --i)
                if (!closes[i].is_null()) return closes[i].get<double>();
        } catch (...) {}
        return -1.0;
    }

    struct SpyData {
        double price  = 0.0;
        double sma200 = 0.0;
        bool   valid  = false;
    };

    SpyData fetchSpy() const {
        try {
            httplib::Client cli("https://query1.finance.yahoo.com");
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(15));
            auto res = cli.Get("/v8/finance/chart/SPY?interval=1d&range=1y");
            if (!res || res->status != 200) return {};
            auto body = json::parse(res->body);
            const auto& raw = body.at("chart").at("result").at(0)
                                   .at("indicators").at("quote").at(0).at("close");
            std::vector<double> vc;
            for (const auto& c : raw)
                if (!c.is_null()) vc.push_back(c.get<double>());
            if (vc.size() < 200) return {};
            double sum = 0.0;
            for (size_t i = vc.size() - 200; i < vc.size(); ++i) sum += vc[i];
            return {vc.back(), sum / 200.0, true};
        } catch (...) {}
        return {};
    }

public:
    struct BarData {
        double price = 0.0;
        double sma20 = 0.0;
        double sma50 = 0.0;
        double rsi14 = 50.0;
        double atr14 = 0.0;
        bool   valid = false;
    };

    // Identical RSI/ATR/SMA calculation to OptionsSignalGenerator for consistency.
    // Public + static so the execution engine's rule-based exit monitor evaluates
    // exits using the exact same indicators that produced the entry.
    static BarData fetchBars(const std::string& symbol) {
        try {
            httplib::Client cli("https://query1.finance.yahoo.com");
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(15));
            std::string path = "/v8/finance/chart/" + symbol + "?interval=1d&range=1y";
            auto res = cli.Get(path.c_str());
            if (!res || res->status != 200) return {};

            auto body   = json::parse(res->body);
            const auto& ohlcv = body.at("chart").at("result").at(0)
                                     .at("indicators").at("quote").at(0);
            const auto& raw_closes = ohlcv.at("close");
            const auto& raw_highs  = ohlcv.at("high");
            const auto& raw_lows   = ohlcv.at("low");

            std::vector<double> closes, highs, lows;
            for (size_t i = 0; i < raw_closes.size(); ++i) {
                if (!raw_closes[i].is_null() && !raw_highs[i].is_null() && !raw_lows[i].is_null()) {
                    closes.push_back(raw_closes[i].get<double>());
                    highs.push_back(raw_highs[i].get<double>());
                    lows.push_back(raw_lows[i].get<double>());
                }
            }
            if (closes.size() < 50) return {};

            double sma20 = 0.0, sma50 = 0.0;
            for (size_t i = closes.size() - 20; i < closes.size(); ++i) sma20 += closes[i];
            for (size_t i = closes.size() - 50; i < closes.size(); ++i) sma50 += closes[i];
            sma20 /= 20.0;
            sma50 /= 50.0;

            // RSI-14 (Wilder smoothed — identical to OptionsSignalGenerator)
            double avg_gain = 0.0, avg_loss = 0.0;
            size_t seed = closes.size() - 50;
            for (size_t i = seed + 1; i <= seed + 14; ++i) {
                double d = closes[i] - closes[i - 1];
                if (d > 0) avg_gain += d; else avg_loss -= d;
            }
            avg_gain /= 14.0; avg_loss /= 14.0;
            for (size_t i = seed + 15; i < closes.size(); ++i) {
                double d = closes[i] - closes[i - 1];
                double g = (d > 0) ? d : 0.0;
                double l = (d < 0) ? -d : 0.0;
                avg_gain = (avg_gain * 13.0 + g) / 14.0;
                avg_loss = (avg_loss * 13.0 + l) / 14.0;
            }
            double rsi = (avg_loss < 1e-9) ? 100.0 : 100.0 - (100.0 / (1.0 + avg_gain / avg_loss));

            // ATR-14
            double atr_sum = 0.0;
            size_t atr_start = closes.size() - 14;
            for (size_t i = atr_start; i < closes.size(); ++i) {
                double hl  = highs[i] - lows[i];
                double hpc = std::abs(highs[i] - closes[i - 1]);
                double lpc = std::abs(lows[i]  - closes[i - 1]);
                atr_sum += std::max({hl, hpc, lpc});
            }

            BarData d;
            d.price  = closes.back();
            d.sma20  = sma20;
            d.sma50  = sma50;
            d.rsi14  = rsi;
            d.atr14  = atr_sum / 14.0;
            d.valid  = true;
            return d;
        } catch (...) {}
        return {};
    }

private:
    // ── Signal qualification ──────────────────────────────────────────────────
    //
    // BUY criteria (all must pass):
    //   1. Price above both SMA20 and SMA50 — confirmed uptrend
    //   2. RSI 40–72 — momentum but not exhausted
    //   3. Price ≥ 0.3 ATR above SMA20 — some trend distance, not range-bound
    //
    // Quality score (higher = stronger setup):
    //   60% weight: SMA20 distance in ATR units (trend conviction)
    //   40% weight: RSI proximity to 55 (sweet spot — momentum without overbought)

    static double computeScore(const BarData& d) {
        if (d.price <= d.sma20) return -1.0;
        if (d.price <= d.sma50) return -1.0;
        if (d.rsi14 < 40.0 || d.rsi14 > 72.0) return -1.0;

        double sma_atrs = (d.atr14 > 0) ? (d.price - d.sma20) / d.atr14 : 0.0;
        if (sma_atrs < 0.3) return -1.0;

        double rsi_score = 1.0 - std::abs(d.rsi14 - 55.0) / 32.0;
        return sma_atrs * 0.6 + rsi_score * 0.4;
    }

    std::optional<EquitySetup> evaluateTicker(const std::string& ticker,
                                               double vix,
                                               const SpyData& spy) const {
        log("INFO", "[EQUITY_SCAN] Evaluating " + ticker + "...");
        BarData d = fetchBars(ticker);
        if (!d.valid) {
            log("WARN", "[EQUITY_SCAN] No bar data for " + ticker + " — skipping.");
            return std::nullopt;
        }

        double score = computeScore(d);
        if (score < 0.0) {
            log("INFO", "[EQUITY_SCAN] " + ticker + " — no setup (price=" + fmt(d.price) +
                " SMA20=" + fmt(d.sma20) + " SMA50=" + fmt(d.sma50) +
                " RSI=" + fmt(d.rsi14, 1) + "). Skipped.");
            return std::nullopt;
        }

        log("INFO", "[EQUITY_SCAN] " + ticker + " QUALIFIES — score=" + fmt(score) +
            " RSI=" + fmt(d.rsi14, 1) + " ATR=" + fmt(d.atr14));

        EquitySetup s;
        s.ticker       = ticker;
        s.price        = d.price;
        s.sma20        = d.sma20;
        s.sma50        = d.sma50;
        s.rsi          = d.rsi14;
        s.atr          = d.atr14;
        s.vix          = vix;
        s.spy_price    = spy.valid ? spy.price  : 0.0;
        s.spy_200_sma  = spy.valid ? spy.sma200 : 0.0;
        s.quality_score = score;
        return s;
    }

    // ── Signal dispatch ───────────────────────────────────────────────────────
    //
    // Posts to the execution engine's own webhook so Kelly sizing, RSI gate,
    // and liquidity gate all apply exactly as they do to Skeptic signals.

    void postSignal(const EquitySetup& s) const {
        json payload = {
            {"ticker",                   s.ticker},
            {"action",                   "BUY"},
            {"price",                    s.price},
            {"rsi",                      s.rsi},
            {"atr",                      s.atr},
            {"vol",                      0},
            {"vix",                      s.vix},
            {"spy_price",                s.spy_price},
            {"spy_200_sma",              s.spy_200_sma},
            {"risk_tier",                1},
            {"stop_loss_atr_multiplier", 2.0},
            {"secret_key",               webhookSecret_}
        };

        log("INFO", "[EQUITY_SCAN] Posting BUY for " + s.ticker +
            " (score=" + fmt(s.quality_score) + ")...");

        try {
            httplib::Client cli("http://localhost:8080");
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));
            auto res = cli.Post("/webhook", payload.dump(), "application/json");

            if (res && res->status == 200) {
                log("INFO", "[EQUITY_SCAN] Signal accepted — " + s.ticker);
                // No Telegram here: execution engine sends "BUY ORDER EXECUTED"
                // (or a gate-block message) which is the actionable confirmation.
            } else {
                std::string status = res ? std::to_string(res->status) : "TIMEOUT";
                log("WARN", "[EQUITY_SCAN] Webhook rejected " + s.ticker + " — HTTP " + status);
                sendTelegram(
                    "⚠️ *EQUITY SCAN — Webhook Failed*\n"
                    "• *Ticker:* " + s.ticker + "\n"
                    "• *HTTP Status:* " + status
                );
            }
        } catch (const std::exception& e) {
            log("WARN", "[EQUITY_SCAN] Failed to post signal for " + s.ticker +
                ": " + std::string(e.what()));
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
            log("WARN", "[EQUITY_SCAN] Telegram delivery failed.");
        }
    }
};

} // namespace nox::equity_signal
