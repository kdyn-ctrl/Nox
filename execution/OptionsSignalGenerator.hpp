#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "OptionEngine.hpp"
#include "OptionsSignalTypes.hpp"
#include "OptionsOrderRouter.hpp"
#include "SkepticIntelligence.hpp"
#include "../shared/RegimeStateMachine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <functional>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// WS5 — Liquidity Vacuum / Microstructure Gate.
// ─────────────────────────────────────────────────────────────────────────────
// Before ANY execution we read the live bid-ask spread. We keep a rolling
// per-symbol baseline of recent (relative) spreads and abort the trade when the
// current spread sits more than N standard deviations above that baseline —
// the fingerprint of a liquidity vacuum (flash move, halt, news gap) where a
// market order would be filled at a punitive price regardless of how strong the
// alpha signal is. N and the bypass are .env-configurable.
//
// Venue-neutral: callers feed it a spread observed from whatever source they
// have (Alpaca latest quote on the REST path, IBKR L2 via IBKRClient when the
// gateway is wired in). The gate only does the statistics + the abort decision.
namespace nox::liquidity {

struct GateResult {
    bool        allow   = true;   // false → abort the trade
    double      spread  = 0.0;    // the observed (relative) spread
    double      mean    = 0.0;    // baseline mean
    double      stddev  = 0.0;    // baseline sample stddev
    double      zscore  = 0.0;    // (spread - mean) / stddev
    bool        warming = false;  // true while baseline is still filling
    std::string reason;
};

class LiquidityGate {
public:
    LiquidityGate() { loadConfig(); }

    // Evaluate a freshly observed spread for `symbol` against its rolling
    // baseline, then record it. The observation is scored BEFORE being added so
    // a single vacuum spike cannot inflate the baseline it is judged against.
    GateResult evaluate(const std::string& symbol, double spread) {
        GateResult r;
        r.spread = spread;
        auto& hist = history_[symbol];

        if (bypass_) {
            r.reason = "gate bypassed (.env)";
            record(hist, spread);
            return r;
        }
        // A non-positive / invalid spread means we have no usable microstructure
        // read. Fail OPEN (allow) but flag it — blocking on missing data would
        // halt all trading whenever a quote feed hiccups.
        if (spread <= 0.0) {
            r.reason = "no valid spread read — gate skipped (fail-open)";
            return r;
        }
        if (hist.size() < min_samples_) {
            r.warming = true;
            r.reason  = "baseline warming up (" + std::to_string(hist.size()) +
                        "/" + std::to_string(min_samples_) + " samples)";
            record(hist, spread);
            return r;
        }

        auto [mean, sd] = stats(hist);
        r.mean   = mean;
        r.stddev = sd;
        r.zscore = (sd > 1e-12) ? (spread - mean) / sd : 0.0;

        if (r.zscore > n_stddev_) {
            r.allow  = false;
            r.reason = "spread " + num(spread) + " is " + num(r.zscore) +
                       "σ above baseline mean " + num(mean) +
                       " (threshold " + num(n_stddev_) + "σ) — liquidity vacuum";
        } else {
            r.reason = "spread within " + num(n_stddev_) + "σ of baseline";
        }
        record(hist, spread);
        return r;
    }

    bool   bypassed()  const { return bypass_; }
    double threshold() const { return n_stddev_; }

private:
    void loadConfig() {
        if (const char* v = std::getenv("LIQUIDITY_GATE_STDDEV")) {
            try { n_stddev_ = std::stod(v); } catch (...) {}
        }
        if (const char* v = std::getenv("LIQUIDITY_GATE_WINDOW")) {
            try { window_ = std::max<std::size_t>(5, std::stoul(v)); } catch (...) {}
        }
        if (const char* v = std::getenv("LIQUIDITY_GATE_MIN_SAMPLES")) {
            try { min_samples_ = std::max<std::size_t>(3, std::stoul(v)); } catch (...) {}
        }
        if (const char* v = std::getenv("LIQUIDITY_GATE_BYPASS")) {
            std::string s(v);
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            bypass_ = (s == "true" || s == "1" || s == "yes");
        }
    }

    void record(std::deque<double>& h, double spread) {
        h.push_back(spread);
        if (h.size() > window_) h.pop_front();
    }

    static std::pair<double, double> stats(const std::deque<double>& h) {
        double mean = 0.0;
        for (double x : h) mean += x;
        mean /= static_cast<double>(h.size());
        double var = 0.0;
        for (double x : h) var += (x - mean) * (x - mean);
        // Sample variance (n-1) — h.size() >= min_samples_ (>=3) here.
        var /= static_cast<double>(h.size() - 1);
        return {mean, std::sqrt(var)};
    }

    static std::string num(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4) << v;
        return oss.str();
    }

    std::map<std::string, std::deque<double>> history_;
    double      n_stddev_    = 3.0;  // abort beyond N sigma
    std::size_t window_      = 50;   // rolling baseline length
    std::size_t min_samples_ = 10;   // warm-up before the gate is active
    bool        bypass_      = false;
};

} // namespace nox::liquidity

#ifdef NOX_UNIT_TEST
// Forward declaration for the test-only friend grant below — the actual
// struct is defined per-test-file at global scope (see
// execution/test/test_quality_dte_sizing.cpp / test_sector_trend_gate.cpp).
struct NoxUnitTestAccess;
#endif

namespace nox::options_signal {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class DirectionalBias { Bullish, Bearish, Neutral };

// ─── QualityScore — setup conviction, computed from raw inputs only ───────────
// No OptionsSignal dependency, so it's computable as soon as strategy/bias are
// chosen — well before contract params or sizing are decided. This is what
// makes it usable to influence DTE selection and position sizing, not just
// ranking after the fact (see computeQualityScore()).
struct QualityScore {
    double quality_score     = 0.0;
    double sma_distance_atrs = 0.0; // how far price is from SMA20 in ATR units
    double vol_deviation     = 0.0; // abs(IV/HRV - 1.0)
    double rsi_extremity     = 0.0; // abs(RSI - 50) / 50
};

// ─── ScoredSignal — internal ranking wrapper ──────────────────────────────────
// run_scan() collects these, sorts by quality_score descending, then dispatches
// only the top max_signals_per_scan. The score is venue-agnostic: it measures
// setup conviction regardless of strategy type.
struct ScoredSignal {
    OptionsSignal signal;
    std::string   formatted_alert;
    double        quality_score = 0.0;
    // Raw components (logged for transparency)
    double sma_distance_atrs = 0.0; // how far price is from SMA20 in ATR units
    double vol_deviation     = 0.0; // abs(IV/HRV - 1.0)
    double rsi_extremity     = 0.0; // abs(RSI - 50) / 50
};

// ─── Structures ───────────────────────────────────────────────────────────────

struct UnderlyingData {
    double price    = 0.0;
    double sma20    = 0.0;
    double sma50    = 0.0;
    double rsi14    = 0.0;
    double atr14    = 0.0;
    double hrv30    = 0.20; // 30-day historical realized volatility (annualized)
    bool   valid    = false;
};

// Returned by fetchIVData — actual IV level from Alpaca snapshot + display rank
struct IVData {
    double iv_level = 0.20; // annualized implied volatility (use in Black-Scholes)
    double iv_rank  = 50.0; // within-snapshot relative position (display only)
    bool   vol_rich = false; // iv_level > hrv30 * 1.20 — sell-premium environment
};


// ─── OptionsSignalGenerator ───────────────────────────────────────────────────

class OptionsSignalGenerator {
public:
    // Profile-driven constructor — all risk parameters come from the RiskProfile.
    OptionsSignalGenerator(const std::string& alpacaUrl,
                           const std::string& apiKey,
                           const std::string& apiSec,
                           const std::string& tgToken,
                           const std::string& tgChatId,
                           RiskProfile        profile)
        : alpacaUrl_(alpacaUrl)
        , apiKey_(apiKey)
        , apiSec_(apiSec)
        , tgToken_(tgToken)
        , tgChatId_(tgChatId)
        , profile_(std::move(profile))
    {}

    // Optional hook invoked after an option order is successfully placed.
    // The engine wires this to PositionManager so the fill is persisted to
    // open_positions (for exit monitoring) and to the trade_history ledger.
    // Kept as a callback so this header stays decoupled from PositionManager.
    using ExecutionRecorder = std::function<void(const OptionsSignal& sig, int qty_contracts)>;
    void set_execution_recorder(ExecutionRecorder cb) { execution_recorder_ = std::move(cb); }

    // Entry point — called once per scan cycle from the engine's background thread.
    void run_scan(double live_equity) {
        // ── Market hours gate ─────────────────────────────────────────────────
        // Options markets are only open Mon–Fri 9:30–16:00 ET. Scanning on
        // weekends produces stale signals from Friday's closing data.
        if (!isMarketHours()) {
            log("INFO", "[OPTIONS_SCAN][" + profile_.name +
                "] Outside market hours — scan skipped.");
            return;
        }

        const auto& watchlist = profile_.watchlist;
        double effective_capital = resolveCapital(live_equity);
        std::string tier         = computeCapitalTier(effective_capital);
        bool  fc_mode            = (profile_.free_capital_amount > 0.0);

        log("INFO", "[OPTIONS_SCAN][" + profile_.name + "] Tier=" + tier +
            " | Capital=$" + fmt(effective_capital, 0) +
            " | Tickers=" + std::to_string(watchlist.size()) +
            " | MaxSignals=" + std::to_string(profile_.max_signals_per_scan));

        double vix      = fetchVix();
        SpySnapshot spy = fetchSpy();

        // Sector/trend gate: fetch each distinct sector ETF in this scan's
        // watchlist once (not once per ticker — many tickers share a sector).
        std::unordered_map<std::string, SectorSnapshot> sector_cache;
        for (const auto& ticker : watchlist) {
            auto sector_it = sectorEtfMap().find(ticker);
            if (sector_it == sectorEtfMap().end()) continue;
            if (sector_cache.count(sector_it->second)) continue;
            sector_cache[sector_it->second] = fetchSectorTrend(sector_it->second);
        }

        AllocationStrategy regime{};
        if (vix > 0.0 && spy.valid) {
            regime = regimeMachine_.evaluate(vix, spy.price, spy.sma200);
            log("INFO", "[OPTIONS_SCAN] Regime: " + regime.log_message);
        } else {
            log("WARN", "[OPTIONS_SCAN] Could not fetch VIX/SPY — regime defaulting to TRANSITION.");
            regime.current_regime     = Regime::TRANSITION;
            regime.capital_multiplier = 0.5;
            regime.log_message        = "TRANSITION (data unavailable)";
            vix = 20.0;
        }

        auto earnings_calendar = fetchEarningsCalendar();

        // ── Stage 1: evaluate all tickers, collect scored candidates ──────────
        std::vector<ScoredSignal> candidates;
        for (const auto& ticker : watchlist) {
            try {
                if (hasEarningsWithin5Days(ticker, earnings_calendar)) {
                    log("INFO", "[OPTIONS_SCAN][EARNINGS_GATE] " + ticker +
                        " has earnings within 5 days — skipped.");
                    continue;
                }
                auto result = evaluateTicker(ticker, effective_capital, tier,
                                             fc_mode, vix, spy, regime, sector_cache);
                if (result) candidates.push_back(std::move(*result));
            } catch (const std::exception& e) {
                log("WARN", "[OPTIONS_SCAN] Exception on " + ticker + ": " + e.what());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }

        if (candidates.empty()) {
            log("INFO", "[OPTIONS_SCAN][" + profile_.name +
                "] No qualifying setups this cycle.");
            return;
        }

        // ── Stage 2: rank by quality, dispatch top max_signals_per_scan ───────
        std::sort(candidates.begin(), candidates.end(),
            [](const ScoredSignal& a, const ScoredSignal& b) {
                return a.quality_score > b.quality_score;
            });

        int limit      = profile_.max_signals_per_scan;
        int dispatched = 0;

        for (const auto& sc : candidates) {
            if (dispatched >= limit) break;

            // Sector/trend gate: suppress auto-execution when the signal's bias
            // opposes its own sector ETF's EMA trend, even if the broad SPY/VIX
            // regime looks fine (a sector-wide rotation, not a market-wide one).
            // Advisory alerts still go out; only live execution is blocked.
            double sector_gate_enabled = 1.0;
            if (const char* v = std::getenv("SECTOR_TREND_GATE_ENABLED")) { try { sector_gate_enabled = std::stod(v); } catch (...) {} }
            if (sc.signal.sector_conflict && sector_gate_enabled > 0.5) {
                log("WARN", "[OPTIONS_SCAN][SECTOR_GATE] " + sc.signal.underlying +
                    " (" + sc.signal.sector_etf + ") execution suppressed — bias opposes sector trend.");
                sendTelegram(sc.formatted_alert);
                sendTelegram(
                    "⚠️ *SECTOR TREND GATE — " + sc.signal.underlying + "*\n"
                    "────────────────────────\n"
                    "Strategy: " + sc.signal.strategy + "\n"
                    "Sector ETF " + sc.signal.sector_etf + " trend opposes this signal's bias.\n"
                    "Auto-execution suppressed — sector-wide headwind.\n"
                    "_Advisory signal still valid._"
                );
                dispatched++;
                continue;
            }

            sendTelegram(sc.formatted_alert);
            if (profile_.auto_execute) {
                double rel_spread = fetchUnderlyingSpread(sc.signal.underlying);
                auto gate = liquidity_gate_.evaluate(sc.signal.underlying, rel_spread);
                if (!gate.allow) {
                    log("WARN", "[OPTIONS_EXEC][LIQUIDITY_GATE] " + sc.signal.underlying +
                        " execution aborted — " + gate.reason);
                    sendTelegram(
                        "🛑 *LIQUIDITY GATE — " + sc.signal.underlying + "*\n"
                        "────────────────────────\n"
                        "Auto-execution aborted: " + gate.reason + ".\n"
                        "_Advisory signal still valid — review manually._"
                    );
                    dispatched++;  // counts against the cap; alert was dispatched
                    continue;
                }
                executeSignal(sc.signal);
            }
            log("INFO", "[OPTIONS_SCAN] Dispatched #" + std::to_string(dispatched + 1) +
                ": " + sc.signal.underlying + " / " + sc.signal.strategy +
                " | score=" + fmt(sc.quality_score, 2) +
                " (SMA=" + fmt(sc.sma_distance_atrs, 1) + "xATR" +
                " vol=" + fmt(sc.vol_deviation * 100.0, 0) + "%" +
                " RSI=" + fmt(sc.signal.rsi, 0) + ")");
            dispatched++;
        }

        int suppressed = static_cast<int>(candidates.size()) - dispatched;
        if (suppressed > 0) {
            log("INFO", "[OPTIONS_SCAN][" + profile_.name + "] " +
                std::to_string(suppressed) + " lower-quality setup(s) suppressed by cap.");
        }
    }

#ifdef NOX_UNIT_TEST
    // Test-only access to private members (buildContractParams, assembleSignal,
    // computeEma, sectorConflicts, etc.) — see execution/test/test_quality_dte_sizing.cpp
    // and execution/test/test_sector_trend_gate.cpp. Declared at global scope
    // (::NoxUnitTestAccess) since the test files define it there, not inside
    // nox::options_signal.
    friend struct ::NoxUnitTestAccess;
#endif

private:
    // ── Config ────────────────────────────────────────────────────────────────
    std::string alpacaUrl_;
    std::string apiKey_;
    std::string apiSec_;
    std::string tgToken_;
    std::string tgChatId_;
    RiskProfile profile_;
    ExecutionRecorder execution_recorder_;         // optional fill-persistence hook
    RegimeStateMachine regimeMachine_;
    nox::liquidity::LiquidityGate liquidity_gate_; // WS5 microstructure gate

    // ── Helpers ───────────────────────────────────────────────────────────────

    static void log(const std::string& level, const std::string& msg) {
        std::cout << "[" << level << "] " << msg << std::endl;
    }

    static std::string fmt(double v, int decimals = 2) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(decimals) << v;
        return oss.str();
    }

    // ── Capital / tier logic ──────────────────────────────────────────────────

    double resolveCapital(double live_equity) const {
        if (profile_.free_capital_amount > 0.0) return profile_.free_capital_amount;
        return live_equity;
    }

    static std::string computeCapitalTier(double capital) {
        if (capital >= 75000.0) return "FREE_CAPITAL";
        if (capital >= 30000.0) return "ADVANCED";
        if (capital >= 5000.0)  return "STANDARD";
        return "STARTER";
    }

    // Risk dollars per trade — uses profile-specific percentages
    double computeMaxRisk(double capital, const std::string& tier) const {
        if (tier == "FREE_CAPITAL") return capital * profile_.risk_pct_free;
        if (tier == "ADVANCED")     return capital * profile_.risk_pct_advanced;
        if (tier == "STANDARD")     return capital * profile_.risk_pct_standard;
        return capital * profile_.risk_pct_starter;
    }

    // Returns true if a strategy is allowed — tier gates honoured only when enforce_tier_gates is set
    bool strategyAllowed(const std::string& strategy, const std::string& tier) const {
        if (!profile_.enforce_tier_gates) return true; // personal: all strategies open

        if (tier == "STARTER") {
            return strategy == "LONG_CALL" || strategy == "LONG_PUT";
        }
        if (tier == "STANDARD") {
            return strategy == "LONG_CALL"  || strategy == "LONG_PUT" ||
                   strategy == "CSP"        || strategy == "CC";
        }
        return true; // ADVANCED / FREE_CAPITAL
    }

    // ── Regime confidence multiplier ──────────────────────────────────────────

    double regimeConfidence(Regime r, const std::string& strategy) const {
        bool is_long_premium = (strategy == "LONG_CALL"  || strategy == "LONG_PUT" ||
                                strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD" ||
                                strategy == "STRADDLE"   || strategy == "STRANGLE");

        if (r == Regime::RISK_OFF && is_long_premium) {
            // Bot: hard suppress. Personal: show warning (50% confidence) but don't block.
            return profile_.enforce_regime_gate ? 0.0 : 0.50;
        }
        if (r == Regime::TRANSITION) return 0.65;
        return 1.0; // RISK_ON
    }

    // ── Market hours gate ─────────────────────────────────────────────────────
    // Returns true Mon–Fri between 09:00 and 16:00 ET (approximate DST handling).
    // Prevents stale weekend / after-hours signals from firing.
    static bool isMarketHours() {
        auto now    = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&time_t, &utc);

        if (utc.tm_wday == 0 || utc.tm_wday == 6) return false; // Sat/Sun

        // Approximate ET offset: Apr(3)–Oct(9) = UTC-4 (EDT), else UTC-5 (EST)
        int offset_h = (utc.tm_mon >= 3 && utc.tm_mon <= 9) ? 4 : 5;
        int et_mins  = ((utc.tm_hour - offset_h + 24) % 24) * 60 + utc.tm_min;

        return et_mins >= 9 * 60 && et_mins < 16 * 60;
    }

    // ── Signal quality score ──────────────────────────────────────────────────
    // Combines three independent conviction signals into a single rank value.
    // Higher = stronger setup. Computed from raw underlying/IV inputs only (no
    // OptionsSignal dependency) so it's available BEFORE DTE selection and
    // sizing decide anything — not just after the fact for ranking (see
    // evaluateTicker(), buildContractParams(), assembleSignal()).
    static QualityScore computeQualityScore(const UnderlyingData& d, double iv_sigma, double rsi) {
        QualityScore q;
        q.sma_distance_atrs = (d.atr14 > 0)
            ? std::abs(d.price - d.sma20) / d.atr14 : 0.0;
        q.vol_deviation     = (d.hrv30 > 0.01)
            ? std::abs(iv_sigma / d.hrv30 - 1.0) : 0.0;
        q.rsi_extremity     = std::abs(rsi - 50.0) / 50.0;
        // Weights: trend conviction matters most, vol signal second, RSI third
        q.quality_score     = q.sma_distance_atrs * 0.50
                             + q.vol_deviation      * 0.30
                             + q.rsi_extremity      * 0.20;
        return q;
    }

    // Thin wrapper for the ranking/logging path in run_scan() — computes the
    // same score from an already-assembled OptionsSignal.
    static ScoredSignal scoreSignal(const OptionsSignal& sig,
                                    const std::string& formatted_alert,
                                    const UnderlyingData& d) {
        QualityScore q = computeQualityScore(d, sig.iv_level, sig.rsi);
        ScoredSignal sc;
        sc.signal            = sig;
        sc.formatted_alert   = formatted_alert;
        sc.quality_score     = q.quality_score;
        sc.sma_distance_atrs = q.sma_distance_atrs;
        sc.vol_deviation     = q.vol_deviation;
        sc.rsi_extremity     = q.rsi_extremity;
        return sc;
    }

    // ── Market data: live bid-ask spread (WS5 liquidity gate input) ────────────
    //
    // Returns the underlying's RELATIVE spread (ask-bid)/mid from Alpaca's latest
    // quote, or -1.0 on any failure. Relative (not absolute) so the rolling
    // baseline is scale-free and comparable across symbols and price levels.
    // The underlying's spread is a clean liquidity proxy: a vacuum in the stock
    // implies punitive option fills too. IBKR L2 is the richer source when wired.
    double fetchUnderlyingSpread(const std::string& symbol) const {
        try {
            httplib::Client cli("https://data.alpaca.markets");
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey_},
                {"APCA-API-SECRET-KEY", apiSec_}
            };

            std::string path = "/v2/stocks/" + symbol + "/quotes/latest?feed=iex";
            auto res = cli.Get(path.c_str(), headers);
            if (!res || res->status != 200) return -1.0;

            auto body = json::parse(res->body);
            const auto& q = body.at("quote");
            double bid = q.value("bp", 0.0);
            double ask = q.value("ap", 0.0);
            if (bid <= 0.0 || ask <= 0.0 || ask < bid) return -1.0;

            double mid = (ask + bid) / 2.0;
            return (mid > 0.0) ? (ask - bid) / mid : -1.0;
        } catch (...) {
            return -1.0;
        }
    }

    // ── Market data: VIX ──────────────────────────────────────────────────────

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
            for (int i = static_cast<int>(closes.size()) - 1; i >= 0; --i) {
                if (!closes[i].is_null()) return closes[i].get<double>();
            }
        } catch (...) {}
        return -1.0;
    }

    // ── Market data: SPY ──────────────────────────────────────────────────────

    struct SpySnapshot {
        double price  = 0.0;
        double sma200 = 0.0;
        bool   valid  = false;
    };

    SpySnapshot fetchSpy() const {
        try {
            httplib::Client cli("https://query1.finance.yahoo.com");
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(15));

            auto res = cli.Get("/v8/finance/chart/SPY?interval=1d&range=1y");
            if (!res || res->status != 200) return {};

            auto body = json::parse(res->body);
            const auto& closes = body.at("chart").at("result").at(0)
                                      .at("indicators").at("quote").at(0).at("close");

            std::vector<double> valid_closes;
            for (const auto& c : closes) {
                if (!c.is_null()) valid_closes.push_back(c.get<double>());
            }
            if (valid_closes.size() < 200) return {};

            double price = valid_closes.back();
            double sum   = 0.0;
            for (size_t i = valid_closes.size() - 200; i < valid_closes.size(); ++i)
                sum += valid_closes[i];

            return {price, sum / 200.0, true};
        } catch (...) {}
        return {};
    }

    // ── Market data: sector/trend gate ─────────────────────────────────────────
    //
    // The broad VIX/SPY regime gate doesn't catch a sector-wide move against a
    // single-name signal (e.g. a bullish tech name firing straight into a tech
    // rotation while SPY itself is fine). This computes a sector ETF's
    // EMA-fast-vs-EMA-slow trend, fetched once per distinct ETF per scan cycle
    // (see run_scan()'s sector_cache), and is checked at dispatch time the same
    // way the RISK_OFF regime gate is.

    struct SectorSnapshot {
        double price    = 0.0;
        double ema_fast = 0.0;
        double ema_slow = 0.0;
        bool   valid    = false;
    };

    // Seeds on the simple average of the first `period` closes, then runs an
    // EMA forward over the remainder. Returns 0.0 (treat as invalid) if there
    // isn't enough history.
    static double computeEma(const std::vector<double>& closes, int period) {
        if (static_cast<int>(closes.size()) < period) return 0.0;
        double seed = 0.0;
        for (int i = 0; i < period; ++i) seed += closes[static_cast<size_t>(i)];
        seed /= period;
        double k   = 2.0 / (period + 1.0);
        double ema = seed;
        for (size_t i = static_cast<size_t>(period); i < closes.size(); ++i)
            ema = closes[i] * k + ema * (1.0 - k);
        return ema;
    }

    // Ticker → sector ETF. Structural lookup data (not a tuned threshold), so
    // it's a static table rather than env-configured. Unmapped tickers fail
    // open — the gate is simply skipped for them.
    static const std::unordered_map<std::string, std::string>& sectorEtfMap() {
        static const std::unordered_map<std::string, std::string> m = {
            {"AAPL", "XLK"}, {"MSFT", "XLK"}, {"NVDA", "XLK"}, {"AMD", "XLK"},
            {"AVGO", "XLK"}, {"ORCL", "XLK"}, {"SMCI", "XLK"}, {"SOXX", "XLK"},
            {"GOOGL", "XLC"}, {"META", "XLC"}, {"NFLX", "XLC"}, {"DIS", "XLC"},
            {"AMZN", "XLY"}, {"TSLA", "XLY"}, {"SHOP", "XLY"}, {"UBER", "XLY"},
            {"JPM", "XLF"}, {"GS", "XLF"}, {"BAC", "XLF"}, {"SOFI", "XLF"},
            {"XOM", "XLE"}, {"F", "XLE"},
            {"LLY", "XLV"}, {"JNJ", "XLV"}, {"UNH", "XLV"},
        };
        return m;
    }

    SectorSnapshot fetchSectorTrend(const std::string& etf_symbol) const {
        try {
            int ema_fast_period = 20, ema_slow_period = 50;
            if (const char* v = std::getenv("SECTOR_TREND_EMA_FAST")) { try { ema_fast_period = std::max(2, std::stoi(v)); } catch (...) {} }
            if (const char* v = std::getenv("SECTOR_TREND_EMA_SLOW")) { try { ema_slow_period = std::max(2, std::stoi(v)); } catch (...) {} }

            httplib::Client cli("https://query1.finance.yahoo.com");
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(15));

            auto res = cli.Get(("/v8/finance/chart/" + etf_symbol + "?interval=1d&range=6mo").c_str());
            if (!res || res->status != 200) return {};

            auto body = json::parse(res->body);
            const auto& closes = body.at("chart").at("result").at(0)
                                      .at("indicators").at("quote").at(0).at("close");

            std::vector<double> valid_closes;
            for (const auto& c : closes) {
                if (!c.is_null()) valid_closes.push_back(c.get<double>());
            }
            if (valid_closes.size() < static_cast<size_t>(ema_slow_period)) return {};

            SectorSnapshot s;
            s.price    = valid_closes.back();
            s.ema_fast = computeEma(valid_closes, ema_fast_period);
            s.ema_slow = computeEma(valid_closes, ema_slow_period);
            s.valid    = (s.ema_fast > 0.0 && s.ema_slow > 0.0);
            return s;
        } catch (...) {}
        return {};
    }

    // True if `bias` opposes the sector's own trend — a bullish signal into a
    // sector downtrend, or a bearish signal into a sector uptrend. Neutral
    // bias never conflicts (nothing directional to contradict); an unfetched/
    // ambiguous snapshot fails open (no conflict).
    static bool sectorConflicts(DirectionalBias bias, const SectorSnapshot& sec) {
        if (!sec.valid || bias == DirectionalBias::Neutral) return false;
        bool downtrend = sec.ema_fast < sec.ema_slow && sec.price < sec.ema_fast;
        bool uptrend   = sec.ema_fast > sec.ema_slow && sec.price > sec.ema_fast;
        if (bias == DirectionalBias::Bullish) return downtrend;
        return uptrend; // Bearish
    }

    // ── Skeptic intelligence — WS2 alt-macro + WS3 insider + China lag ────────
    // The piece that finally makes the non-contradiction Skeptic workstreams
    // move real trades. Queries the america-data-engine's /insider/clusters and
    // /macro/alt feeds (already live, never previously consumed) and the
    // china-data-engine's /lag/macro feed, folding them into one
    // size-multiplier / suppress verdict via the pure SkepticIntelligence
    // decision layer.
    //
    // Fail-open: any unreachable/malformed feed contributes an empty input, so
    // decide() returns a 1.0x no-op — a dead Skeptic never blocks or distorts
    // execution.
    nox::skeptic::Decision fetchSkepticIntelligence(const std::string& ticker,
                                                    nox::skeptic::Dir dir) const {
        using namespace nox::skeptic;
        Inputs in;
        try {
            const char* secret = std::getenv("WEBHOOK_SECRET_TOKEN");
            if (!secret) return Decision{}; // no auth → treat as no-op (1.0x)

            httplib::Client cli("http://america-data-engine:8001");
            cli.set_connection_timeout(std::chrono::seconds(3));
            cli.set_read_timeout(std::chrono::seconds(5));
            httplib::Headers headers = {{"X-Nox-Token", secret}};

            auto getJson = [&](const char* path, json& out) -> bool {
                try {
                    auto res = cli.Get(path, headers);
                    if (!res || res->status != 200) return false;
                    out = json::parse(res->body);
                    return true;
                } catch (...) { return false; }
            };

            auto toDir = [](const std::string& s) -> Dir {
                std::string l; l.reserve(s.size());
                for (char c : s) l.push_back(static_cast<char>(::tolower(c)));
                if (l.find("bull") != std::string::npos) return Dir::Bullish;
                if (l.find("bear") != std::string::npos) return Dir::Bearish;
                return Dir::Neutral;
            };

            // WS3 — insider Form 4 buy-clusters.
            json insider;
            if (getJson("/insider/clusters", insider) && insider.contains("signals")) {
                for (const auto& s : insider["signals"]) {
                    if (s.value("ticker", "") == ticker) {
                        in.insider.has_cluster   = true;
                        in.insider.insider_count = s.value("insider_count", 0);
                        break;
                    }
                }
            }

            // WS2 — alt-macro physical-supply verdict. Match the ticker against
            // each chokepoint region's exposed instruments.
            json alt;
            if (getJson("/macro/alt", alt) && alt.contains("regions")) {
                for (const auto& r : alt["regions"]) {
                    bool applies = false;
                    if (r.contains("tickers"))
                        for (const auto& t : r["tickers"])
                            if (t.get<std::string>() == ticker) { applies = true; break; }
                    if (!applies) continue;
                    std::string bias = r.value("bias", "");
                    if (bias.empty() || r.value("bias", json()).is_null()) continue;
                    in.alt_macro.applies  = true;
                    in.alt_macro.bias     = toDir(bias); // "BULLISH_OIL"/"BEARISH_OIL"
                    double ps = r.value("physical_stress", 0.0);
                    in.alt_macro.strength = std::abs(ps);
                    in.alt_macro.text_contradicts_physical =
                        (r.value("verdict", "") == "TEXT_CONTRADICTS_PHYSICAL");
                    break; // first matching region wins
                }
            }

            // China macro information-lag feed (WS8). Lives on the separate
            // china-data-engine (which owns the PMI + retail-media lag data),
            // not the america-data-engine the WS2/WS3 feeds come from.
            const char* cn_base = std::getenv("CHINA_DATA_ENGINE_URL");
            httplib::Client cn_cli(cn_base ? cn_base : "http://china-data-engine:8000");
            cn_cli.set_connection_timeout(std::chrono::seconds(3));
            cn_cli.set_read_timeout(std::chrono::seconds(5));
            json cn;
            bool cn_ok = false;
            try {
                auto res = cn_cli.Get("/lag/macro", headers);
                if (res && res->status == 200) { cn = json::parse(res->body); cn_ok = true; }
            } catch (...) { cn_ok = false; }
            if (cn_ok && cn.contains("results")) {
                for (const auto& e : cn["results"]) {
                    if (e.value("ticker", "") != ticker) continue;
                    std::string bias = e.value("bias", "");
                    if (bias.empty()) break;
                    in.china.applies  = true;
                    in.china.bias     = toDir(bias);
                    in.china.strength = e.value("strength", 0.0);
                    in.china.fresh    = e.value("fresh", false);
                    in.china.release  = e.value("release", "");
                    break;
                }
            }
        } catch (...) {
            return Decision{}; // any unexpected failure → no-op
        }

        Decision d = decide(dir, in, Knobs::fromEnv());
        if (d.reason != "skeptic_neutral") {
            log(d.suppress ? "WARN" : "INFO",
                "[SKEPTIC][" + ticker + "] " + d.reason +
                " size_mult=" + fmt(d.size_mult, 2) + " — " + d.detail);
        }
        return d;
    }

    // ── Market data: Earnings Calendar (america-data-engine) ──────────────────

    struct EarningsEvent {
        std::string date;
        std::string description;
    };

    using EarningsCalendar = std::map<std::string, std::vector<EarningsEvent>>;

    EarningsCalendar fetchEarningsCalendar() const {
        EarningsCalendar result;
        try {
            httplib::Client cli("http://america-data-engine:8001");
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            // Construct the authorization header (WEBHOOK_SECRET_TOKEN)
            const char* webhook_secret = std::getenv("WEBHOOK_SECRET_TOKEN");
            if (!webhook_secret) {
                log("WARN", "[EARNINGS_FETCH] WEBHOOK_SECRET_TOKEN not set; skipping earnings fetch.");
                return result;
            }

            httplib::Headers headers;
            headers.emplace("X-Nox-Token", webhook_secret);

            auto res = cli.Get("/earnings/calendar", headers);
            if (!res || res->status != 200) {
                log("WARN", "[EARNINGS_FETCH] america-data-engine returned status " +
                    std::to_string(res ? res->status : 0) + "; earnings gate disabled.");
                return result;
            }

            auto body = json::parse(res->body);
            const auto& calendar = body.at("earnings_calendar");

            for (auto it = calendar.begin(); it != calendar.end(); ++it) {
                std::string ticker = it.key();
                const auto& events = it.value();

                std::vector<EarningsEvent> ticker_events;
                for (const auto& event : events) {
                    ticker_events.push_back({
                        event.at("date").get<std::string>(),
                        event.value("description", "")
                    });
                }
                result[ticker] = ticker_events;
            }

            int total_events = 0;
            for (const auto& pair : result) {
                total_events += pair.second.size();
            }
            log("INFO", "[EARNINGS_FETCH] Loaded earnings calendar: " +
                std::to_string(total_events) + " event(s).");

        } catch (const std::exception& e) {
            log("WARN", "[EARNINGS_FETCH] Exception fetching earnings calendar: " +
                std::string(e.what()) + "; earnings gate disabled.");
        }
        return result;
    }

    bool hasEarningsWithin5Days(const std::string& ticker,
                                 const EarningsCalendar& calendar) const {
        auto it = calendar.find(ticker);
        if (it == calendar.end()) {
            return false; // No earnings found for this ticker
        }

        // Get today's date (system local time, converted to YYYY-MM-DD string)
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* gm_time = std::gmtime(&time_t);

        std::ostringstream oss;
        oss << std::put_time(gm_time, "%Y-%m-%d");
        std::string today_str = oss.str();

        // Parse today's date
        int today_year, today_month, today_day;
        std::sscanf(today_str.c_str(), "%d-%d-%d", &today_year, &today_month, &today_day);

        // Check each earnings event for this ticker
        for (const auto& event : it->second) {
            int event_year, event_month, event_day;
            std::sscanf(event.date.c_str(), "%d-%d-%d", &event_year, &event_month, &event_day);

            // Simple date comparison: convert both to day-of-year for same year, else compare years
            auto days_until_event = [](int y1, int m1, int d1, int y2, int m2, int d2) -> long {
                // Count days from date1 to date2
                // This is a simplified comparison — proper implementation would use chrono
                if (y1 != y2) {
                    return y2 > y1 ? 1000 : -1000; // Different years, approximate
                }

                // Same year: convert month/day to day-of-year
                int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                if ((y1 % 4 == 0 && y1 % 100 != 0) || (y1 % 400 == 0)) {
                    days_in_month[2] = 29; // Leap year
                }

                int doy1 = d1;
                for (int i = 1; i < m1; ++i) doy1 += days_in_month[i];

                int doy2 = d2;
                for (int i = 1; i < m2; ++i) doy2 += days_in_month[i];

                return static_cast<long>(doy2 - doy1);
            };

            long days_diff = days_until_event(today_year, today_month, today_day,
                                               event_year, event_month, event_day);

            // Earnings within 5 days (inclusive of today)
            if (days_diff >= 0 && days_diff <= 5) {
                return true;
            }
        }

        return false;
    }

    // ── Market data: Underlying OHLCV (Yahoo Finance) ─────────────────────────

    UnderlyingData fetchUnderlyingBars(const std::string& symbol) const {
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

            // SMA-20 and SMA-50
            double sma20 = 0.0, sma50 = 0.0;
            for (size_t i = closes.size() - 20; i < closes.size(); ++i) sma20 += closes[i];
            for (size_t i = closes.size() - 50; i < closes.size(); ++i) sma50 += closes[i];
            sma20 /= 20.0;
            sma50 /= 50.0;

            // RSI-14 (Wilder's smoothed — seed on bars [-50,-37], smooth over [-36,-1])
            // Requires closes.size() >= 50, which is already enforced above.
            double avg_gain = 0.0, avg_loss = 0.0;
            size_t rsi_seed = closes.size() - 50;
            for (size_t i = rsi_seed + 1; i <= rsi_seed + 14; ++i) {
                double diff = closes[i] - closes[i - 1];
                if (diff > 0) avg_gain += diff; else avg_loss -= diff;
            }
            avg_gain /= 14.0; avg_loss /= 14.0;
            for (size_t i = rsi_seed + 15; i < closes.size(); ++i) {
                double diff = closes[i] - closes[i - 1];
                double g = (diff > 0) ? diff : 0.0;
                double l = (diff < 0) ? -diff : 0.0;
                avg_gain = (avg_gain * 13.0 + g) / 14.0;
                avg_loss = (avg_loss * 13.0 + l) / 14.0;
            }
            double rsi = (avg_loss < 1e-9) ? 100.0 : 100.0 - (100.0 / (1.0 + avg_gain / avg_loss));

            // ATR-14: average of true range over last 14 bars
            double atr_sum = 0.0;
            size_t atr_start = closes.size() - 14;
            for (size_t i = atr_start; i < closes.size(); ++i) {
                double hl  = highs[i] - lows[i];
                double hpc = std::abs(highs[i] - closes[i - 1]);
                double lpc = std::abs(lows[i]  - closes[i - 1]);
                atr_sum += std::max({hl, hpc, lpc});
            }

            // HRV-30: annualized close-to-close realized volatility (mean=0 assumption)
            double hrv_sq_sum = 0.0;
            size_t hrv_start  = closes.size() - 31; // 31 prices → 30 log-returns
            for (size_t i = hrv_start + 1; i < closes.size(); ++i) {
                double r = std::log(closes[i] / closes[i - 1]);
                hrv_sq_sum += r * r;
            }
            double hrv30 = std::sqrt(hrv_sq_sum / 30.0 * 252.0);

            UnderlyingData d;
            d.price  = closes.back();
            d.sma20  = sma20;
            d.sma50  = sma50;
            d.rsi14  = rsi;
            d.atr14  = atr_sum / 14.0;
            d.hrv30  = hrv30;
            d.valid  = true;
            return d;
        } catch (...) {}
        return {};
    }

    // ── IV data via Alpaca options snapshot (falls back to VIX proxy) ───────────
    //
    // Returns:
    //   iv_level — actual annualized implied volatility from the snapshot average.
    //              Use this as σ in Black-Scholes; it is the real market vol estimate.
    //   iv_rank  — where the snapshot average sits within the snapshot's own IV
    //              spread (display only; NOT a 52-week percentile rank).
    //   vol_rich — true when iv_level > hrv30 * 1.20, meaning options are pricing in
    //              ~20% more vol than the stock recently realized → sell-premium edge.

    IVData fetchIVData(const std::string& symbol, double vix_fallback, double hrv30) const {
        IVData result;

        try {
            httplib::Client cli(alpacaUrl_);
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey_},
                {"APCA-API-SECRET-KEY", apiSec_}
            };

            std::string path = "/v2/options/snapshots/" + symbol + "?limit=50&feed=indicative";
            auto res = cli.Get(path.c_str(), headers);
            if (!res || res->status != 200) throw std::runtime_error("snapshot unavailable");

            auto body = json::parse(res->body);
            const auto& snapshots = body.at("snapshots");

            double iv_min = 9999.0, iv_max = 0.0, iv_sum = 0.0;
            int    iv_count = 0;
            for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
                const auto& snap = it.value();
                if (snap.contains("greeks") && !snap["greeks"]["iv"].is_null()) {
                    double iv = snap["greeks"]["iv"].get<double>();
                    if (iv > 0.0 && iv < 5.0) { // sanity: reject clearly bad values
                        iv_min  = std::min(iv_min, iv);
                        iv_max  = std::max(iv_max, iv);
                        iv_sum += iv;
                        ++iv_count;
                    }
                }
            }
            if (iv_count == 0) throw std::runtime_error("no IV data");

            result.iv_level = iv_sum / iv_count; // actual annualized IV (use in BS)
            result.iv_rank  = ((iv_max - iv_min) > 1e-6)
                              ? (result.iv_level - iv_min) / (iv_max - iv_min) * 100.0
                              : 50.0;

        } catch (...) {
            // VIX proxy: equity IV is typically VIX * 1.3 (single-stock vol premium)
            double vix_sigma   = vix_fallback / 100.0;
            result.iv_level    = vix_sigma * 1.30;
            result.iv_rank     = (vix_fallback < 15.0) ? 20.0
                               : (vix_fallback > 30.0) ? 70.0
                               : (vix_fallback - 15.0) / 15.0 * 50.0 + 20.0;
        }

        // Vol richness: IV priced in at least 20% more than recently realized
        if (hrv30 > 0.01)
            result.vol_rich = (result.iv_level > hrv30 * 1.20);

        return result;
    }

    // ── Directional bias from technicals ─────────────────────────────────────

    static DirectionalBias computeBias(const UnderlyingData& d) {
        bool above_20  = d.price > d.sma20;
        bool above_50  = d.price > d.sma50;
        bool rsi_bull  = d.rsi14 >= 40.0 && d.rsi14 <= 65.0;
        bool rsi_bear  = d.rsi14 >= 35.0 && d.rsi14 <= 60.0;
        bool near_sma  = std::abs(d.price - d.sma20) < d.atr14;

        if (above_20 && above_50 && rsi_bull) return DirectionalBias::Bullish;
        if (!above_20 && !above_50 && rsi_bear) return DirectionalBias::Bearish;
        if (near_sma && d.rsi14 >= 40.0 && d.rsi14 <= 60.0) return DirectionalBias::Neutral;
        return DirectionalBias::Neutral;
    }

    // ── Strategy selection ────────────────────────────────────────────────────
    //
    // Two complementary signals drive buy-premium vs sell-premium preference:
    //
    //   vol_rich  — IV > HRV * 1.20: options pricing in 20%+ more vol than was
    //               recently realized. Sell premium: receive the variance risk
    //               premium and let theta decay work in your favour.
    //
    //   vol_cheap — IV < HRV * 0.90: options are underpricing actual vol. Buy
    //               premium: implied vol is likely to mean-revert upward.
    //
    //   iv_rank   — snapshot-relative position only (display/secondary context).
    //               NOT used to gate strategy selection: it measures where the
    //               average IV sits within the current chain's own min/max spread
    //               (intra-chain skew dispersion), which is unrelated to whether
    //               vol is rich versus what the stock actually realizes. Gating on
    //               it would flip trades on a meaningless number. The profile
    //               iv_rank_buy_max / iv_rank_sell_min thresholds are reserved for
    //               the true 52-week historical IV Rank (heartbeat subsystem),
    //               pending the C++ ↔ heartbeat integration (see private roadmap).

    std::string selectStrategy(DirectionalBias bias, double /*iv_rank*/, double iv_level,
                               double hrv, const std::string& tier) const {
        bool vol_rich  = (hrv > 0.01) && (iv_level > hrv * 1.20);
        bool vol_cheap = (hrv > 0.01) && (iv_level < hrv * 0.90);

        // Gate purely on the HRV-based variance-premium signal (the documented
        // primary edge). Snapshot iv_rank is intentionally not a trigger.
        bool prefer_sell = vol_rich;
        bool prefer_buy  = vol_cheap;

        // When conflicting: variance premium is more reliable → sell wins
        if (prefer_sell && prefer_buy) prefer_buy = false;
        // Default: buy premium if no signal fires
        if (!prefer_sell && !prefer_buy) prefer_buy = true;

        if (bias == DirectionalBias::Bullish) {
            if (prefer_sell) {
                if (strategyAllowed("CSP", tier)) return "CSP";
                return "LONG_CALL"; // fallback if tier doesn't permit CSP
            }
            if (strategyAllowed("BULL_CALL_SPREAD", tier)) return "BULL_CALL_SPREAD";
            return "LONG_CALL";
        }

        if (bias == DirectionalBias::Bearish) {
            if (prefer_sell) {
                if (strategyAllowed("CC", tier)) return "CC";
                return "LONG_PUT";
            }
            if (strategyAllowed("BEAR_PUT_SPREAD", tier)) return "BEAR_PUT_SPREAD";
            return "LONG_PUT";
        }

        // Neutral — vol play
        if (prefer_sell) {
            if (strategyAllowed("STRANGLE", tier)) return "STRANGLE"; // defined-risk income
            if (strategyAllowed("CSP", tier))      return "CSP";
        }
        if (prefer_buy) {
            if (strategyAllowed("STRADDLE", tier)) return "STRADDLE"; // vol expansion play
        }
        if (strategyAllowed("CSP", tier)) return "CSP";
        return "LONG_CALL";
    }

    // ── Contract parameter construction ───────────────────────────────────────
    //
    // Selects strike(s) and DTE based on strategy type.
    // Uses delta targets computed via Black-Scholes.
    // Returns false if no viable contract found.

    struct ContractParams {
        double strike  = 0.0;
        double strike2 = 0.0; // 0 = single-leg
        double expiry  = 0.0; // years to expiration
        std::string expiry_date;
    };

    ContractParams buildContractParams(const std::string& strategy,
                                       double spot, double atr,
                                       double rfr, double iv_sigma,
                                       double quality_score = 0.5) const {
        ContractParams p;

        // Target DTE from profile
        int target_dte = profile_.dte_long;
        if (strategy == "CSP" || strategy == "CC")
            target_dte = profile_.dte_income;
        else if (strategy == "STRADDLE" || strategy == "STRANGLE" ||
                 strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD")
            target_dte = profile_.dte_spread;

        // Quality-driven DTE — theta-sensitive long-premium strategies only.
        // A weak setup (low momentum/conviction) needs more time for the thesis
        // to play out or theta kills it before it can; a strong setup doesn't
        // need to overpay for time it won't use. Short-premium strategies
        // (CSP/CC) are excluded — decay is their edge, not their enemy.
        bool is_theta_sensitive_long = (strategy == "LONG_CALL" || strategy == "LONG_PUT" ||
                                        strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD");
        if (is_theta_sensitive_long) {
            double q_high = 0.55, q_low = 0.25, high_relax_pct = 0.50;
            int    low_floor_days = 21;
            if (const char* v = std::getenv("QUALITY_DTE_HIGH_THRESHOLD")) { try { q_high = std::stod(v); } catch (...) {} }
            if (const char* v = std::getenv("QUALITY_DTE_LOW_THRESHOLD"))  { try { q_low  = std::stod(v); } catch (...) {} }
            if (const char* v = std::getenv("QUALITY_DTE_LOW_FLOOR_DAYS")) { try { low_floor_days = std::max(1, std::stoi(v)); } catch (...) {} }
            if (const char* v = std::getenv("QUALITY_DTE_HIGH_RELAX_PCT")) { try { high_relax_pct = std::stod(v); } catch (...) {} }

            if (quality_score >= q_high) {
                int relaxed = static_cast<int>(target_dte * (1.0 - high_relax_pct));
                target_dte = std::max(1, relaxed); // never below 1 day
            } else if (quality_score <= q_low) {
                target_dte = std::max(target_dte, low_floor_days); // push weak setups out, never shrink strong ones
            }
            // mid-band: profile default stands unchanged
        }

        p.expiry = target_dte / 365.0;

        // Approximate expiry date string (calendar days from today)
        // Use gmtime_r — thread-safe reentrant variant (POSIX); std::gmtime uses a
        // shared static buffer that would be corrupted by concurrent scan threads.
        auto now    = std::chrono::system_clock::now();
        auto exp_tp = now + std::chrono::hours(24 * target_dte);
        auto exp_t  = std::chrono::system_clock::to_time_t(exp_tp);
        std::tm tm_buf{};
        gmtime_r(&exp_t, &tm_buf);
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d");
        p.expiry_date = oss.str();

        // Target strikes via delta search
        // Long/spread: Δ ≈ 0.45 (near-ATM)
        // Income (short): Δ ≈ 0.25 (OTM)
        // Spread second leg: Δ ≈ 0.15

        auto findStrikeForDelta = [&](double target_delta,
                                      nox::options::OptionType opt_type) -> double {
            // Use standard listed-option strike increments:
            //   $0.50 for stocks priced below $25, $1.00 for $25–$200, $5.00 above $200.
            // This avoids generating strikes that don't exist on any exchange.
            double step = (spot < 25.0) ? 0.50 : (spot < 200.0) ? 1.0 : 5.0;
            double atm  = std::round(spot / step) * step;

            double best_strike = atm;
            double best_diff   = 1e9;
            for (int offset = -30; offset <= 30; ++offset) {
                double s = atm + offset * step;
                if (s <= 0) continue;
                nox::options::OptionContract c;
                c.underlying     = spot;
                c.strike         = s;
                c.expiry         = p.expiry;
                c.risk_free_rate = rfr;
                c.volatility     = iv_sigma;
                c.type           = opt_type;
                double d = std::abs(nox::options::bs_delta(c, iv_sigma));
                if (std::abs(d - target_delta) < best_diff) {
                    best_diff   = std::abs(d - target_delta);
                    best_strike = s;
                }
            }
            return best_strike;
        };

        const double d_long = profile_.delta_long;
        const double d_inc  = profile_.delta_income;
        const double d_wing = profile_.delta_spread_wing;

        if (strategy == "LONG_CALL") {
            p.strike = findStrikeForDelta(d_long, nox::options::OptionType::Call);
        } else if (strategy == "LONG_PUT") {
            p.strike = findStrikeForDelta(d_long, nox::options::OptionType::Put);
        } else if (strategy == "CSP") {
            p.strike = findStrikeForDelta(d_inc, nox::options::OptionType::Put);
        } else if (strategy == "CC") {
            p.strike = findStrikeForDelta(d_inc, nox::options::OptionType::Call);
        } else if (strategy == "BULL_CALL_SPREAD") {
            p.strike  = findStrikeForDelta(d_long, nox::options::OptionType::Call);
            p.strike2 = findStrikeForDelta(d_wing, nox::options::OptionType::Call);
            if (p.strike2 <= p.strike) p.strike2 = p.strike + atr;
        } else if (strategy == "BEAR_PUT_SPREAD") {
            p.strike  = findStrikeForDelta(d_long, nox::options::OptionType::Put);
            p.strike2 = findStrikeForDelta(d_wing, nox::options::OptionType::Put);
            if (p.strike2 >= p.strike) p.strike2 = p.strike - atr;
        } else if (strategy == "STRADDLE") {
            p.strike = std::round(spot);
        } else if (strategy == "STRANGLE") {
            p.strike  = findStrikeForDelta(d_inc, nox::options::OptionType::Call);
            p.strike2 = findStrikeForDelta(d_inc, nox::options::OptionType::Put);
        }
        return p;
    }

    // ── Signal assembly ───────────────────────────────────────────────────────

    OptionsSignal assembleSignal(const std::string& ticker,
                                 const UnderlyingData& d,
                                 const std::string& strategy,
                                 const ContractParams& cp,
                                 double iv_rank, double iv_sigma,
                                 double rfr, double confidence,
                                 const std::string& tier,
                                 bool fc_mode, double allocated_capital,
                                 double hrv30, double quality_score = 0.5) const
    {
        using namespace nox::options;

        OptionsSignal sig;
        sig.underlying        = ticker;
        sig.strategy          = strategy;
        sig.expiry_date       = cp.expiry_date;
        sig.strike            = cp.strike;
        sig.strike2           = cp.strike2;
        sig.iv_rank           = iv_rank;
        sig.iv_level          = iv_sigma;
        sig.hrv30             = hrv30;
        sig.rsi               = d.rsi14;
        sig.atr               = d.atr14;
        sig.confidence        = confidence;
        sig.capital_tier      = tier;
        sig.free_capital_mode = fc_mode;
        sig.allocated_capital = allocated_capital;

        // Primary leg
        OptionContract primary;
        primary.underlying     = d.price;
        primary.strike         = cp.strike;
        primary.expiry         = cp.expiry;
        primary.risk_free_rate = rfr;
        primary.volatility     = iv_sigma;

        bool is_call = (strategy == "LONG_CALL" || strategy == "CC" ||
                        strategy == "BULL_CALL_SPREAD" || strategy == "STRADDLE");
        primary.type   = is_call ? OptionType::Call : OptionType::Put;
        sig.option_type = primary.type;

        OptionGreeks g1 = compute_greeks(primary);

        double max_risk   = computeMaxRisk(allocated_capital, tier);

        // Quality-driven sizing multiplier — a low-conviction setup gets a
        // smaller slice of the risk budget, a high-conviction one a slightly
        // larger one. Applied standalone (no AlphaDecayStore/decay_scale on
        // this codebase — see CLAUDE.md).
        double quality_size_mult = 1.0;
        {
            double q_low = 0.25, q_high = 0.55, mult_min = 0.60, mult_max = 1.15;
            if (const char* v = std::getenv("QUALITY_DTE_LOW_THRESHOLD"))  { try { q_low  = std::stod(v); } catch (...) {} }
            if (const char* v = std::getenv("QUALITY_DTE_HIGH_THRESHOLD")) { try { q_high = std::stod(v); } catch (...) {} }
            if (const char* v = std::getenv("QUALITY_SIZE_MULT_MIN"))      { try { mult_min = std::stod(v); } catch (...) {} }
            if (const char* v = std::getenv("QUALITY_SIZE_MULT_MAX"))      { try { mult_max = std::stod(v); } catch (...) {} }
            if (quality_score <= q_low)       quality_size_mult = mult_min;
            else if (quality_score >= q_high) quality_size_mult = mult_max;
            else if (q_high > q_low)
                quality_size_mult = mult_min + (mult_max - mult_min) * (quality_score - q_low) / (q_high - q_low);
        }

        // Hard short-DTE risk ceiling — a structural cap on convexity risk, not
        // a smooth regime multiplier. A trade landing at/under the short-DTE
        // threshold can't exceed a small fixed % of capital regardless of tier,
        // though a high-quality setup earns back some room toward the tier's
        // normal risk_pct rather than being starved as hard as a weak one.
        {
            int resolved_dte = static_cast<int>(std::round(cp.expiry * 365.0));
            int short_dte_days = 14;
            double ceiling_pct = 0.010, relax = 0.50, q_high = 0.55;
            if (const char* v = std::getenv("SHORT_DTE_THRESHOLD_DAYS"))        { try { short_dte_days = std::max(1, std::stoi(v)); } catch (...) {} }
            if (const char* v = std::getenv("SHORT_DTE_RISK_PCT_CEILING"))      { try { ceiling_pct = std::stod(v); } catch (...) {} }
            if (const char* v = std::getenv("SHORT_DTE_QUALITY_CEILING_RELAX")) { try { relax = std::stod(v); } catch (...) {} }
            if (const char* v = std::getenv("QUALITY_DTE_HIGH_THRESHOLD"))      { try { q_high = std::stod(v); } catch (...) {} }

            if (resolved_dte <= short_dte_days && allocated_capital > 0.0) {
                double effective_ceiling_pct = ceiling_pct;
                if (quality_score >= q_high) {
                    double tier_pct = max_risk / allocated_capital;
                    if (tier_pct > ceiling_pct)
                        effective_ceiling_pct = ceiling_pct + (tier_pct - ceiling_pct) * relax;
                }
                max_risk = std::min(max_risk, allocated_capital * effective_ceiling_pct);
            }
        }

        double contracts  = std::max(1.0, std::floor((max_risk * quality_size_mult) / (g1.price * 100.0)));

        // Per-contract P&L geometry
        if (strategy == "LONG_CALL" || strategy == "LONG_PUT") {
            sig.entry_price = g1.price;
            sig.max_risk    = g1.price * 100.0 * contracts;
            sig.max_reward  = 999999.0; // theoretically unlimited — sentinel
            sig.breakeven   = (strategy == "LONG_CALL")
                              ? cp.strike + g1.price
                              : cp.strike - g1.price;
            sig.greeks      = g1;

        } else if (strategy == "CSP" || strategy == "CC") {
            // Short premium — receive credit, max risk is the spread width
            sig.entry_price = g1.price;
            sig.max_risk    = (strategy == "CSP")
                              ? (cp.strike - g1.price) * 100.0 * contracts
                              : (d.price   - g1.price) * 100.0 * contracts;
            sig.max_reward  = g1.price * 100.0 * contracts;
            sig.breakeven   = (strategy == "CSP")
                              ? cp.strike - g1.price
                              : cp.strike + g1.price;
            sig.greeks      = g1;

        } else if (strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD") {
            OptionContract sell_leg = primary;
            sell_leg.strike = cp.strike2;
            OptionGreeks g2 = compute_greeks(sell_leg);

            double debit       = g1.price - g2.price;
            double spread_width = std::abs(cp.strike2 - cp.strike);
            sig.entry_price = debit;
            sig.max_risk    = debit * 100.0 * contracts;
            sig.max_reward  = (spread_width - debit) * 100.0 * contracts;
            sig.breakeven   = (strategy == "BULL_CALL_SPREAD")
                              ? cp.strike + debit
                              : cp.strike - debit;
            // Net Greeks
            sig.greeks.price = debit;
            sig.greeks.delta = g1.delta - g2.delta;
            sig.greeks.gamma = g1.gamma - g2.gamma;
            sig.greeks.theta = g1.theta - g2.theta;
            sig.greeks.vega  = g1.vega  - g2.vega;
            sig.greeks.implied_volatility = iv_sigma;

        } else if (strategy == "STRADDLE") {
            OptionContract put_leg = primary;
            put_leg.type = OptionType::Put;
            OptionGreeks gp = compute_greeks(put_leg);

            double total_debit = g1.price + gp.price;
            sig.entry_price = total_debit;
            sig.max_risk    = total_debit * 100.0 * contracts;
            sig.max_reward  = 999999.0;
            sig.breakeven   = cp.strike + total_debit; // upper breakeven (lower = strike - debit)
            sig.greeks.price = total_debit;
            sig.greeks.delta = g1.delta + gp.delta;
            sig.greeks.gamma = g1.gamma + gp.gamma;
            sig.greeks.theta = g1.theta + gp.theta;
            sig.greeks.vega  = g1.vega  + gp.vega;
            sig.greeks.implied_volatility = iv_sigma;

        } else if (strategy == "STRANGLE") {
            OptionContract put_leg = primary;
            put_leg.type   = OptionType::Put;
            put_leg.strike = cp.strike2;
            OptionGreeks gp = compute_greeks(put_leg);

            double total_debit = g1.price + gp.price;
            sig.entry_price = total_debit;
            sig.max_risk    = total_debit * 100.0 * contracts;
            sig.max_reward  = 999999.0;
            sig.breakeven   = cp.strike + total_debit;
            sig.greeks.price = total_debit;
            sig.greeks.delta = g1.delta + gp.delta;
            sig.greeks.gamma = g1.gamma + gp.gamma;
            sig.greeks.theta = g1.theta + gp.theta;
            sig.greeks.vega  = g1.vega  + gp.vega;
            sig.greeks.implied_volatility = iv_sigma;
        }

        // Rationale
        sig.rationale    = buildRationale(strategy, d, iv_rank, iv_sigma, hrv30);
        sig.profile_name = profile_.name;
        return sig;
    }

    static std::string buildRationale(const std::string& strategy,
                                      const UnderlyingData& d,
                                      double iv_rank, double iv_level, double hrv30)
    {
        std::string r = strategy + " on " +
                        (d.price > d.sma20 ? "bullish" : "bearish") + " bias. ";
        r += "RSI=" + fmt(d.rsi14) + ", ATR=" + fmt(d.atr14) + ". ";
        r += "IV=" + fmt(iv_level * 100.0, 1) + "% (rank " + fmt(iv_rank, 0) +
             "%) vs HRV=" + fmt(hrv30 * 100.0, 1) + "% — ";
        bool rich  = (hrv30 > 0.01) && (iv_level > hrv30 * 1.20);
        bool cheap = (hrv30 > 0.01) && (iv_level < hrv30 * 0.90);
        r += rich  ? "vol RICH — variance premium favours sellers."
           : cheap ? "vol CHEAP — mean-reversion favours buyers."
           : "vol near fair value.";
        return r;
    }

    // ── Telegram formatting ───────────────────────────────────────────────────

    std::string formatAlert(const OptionsSignal& s, double vix,
                            const AllocationStrategy& regime) const
    {
        std::string leg2_str = (s.strike2 > 0)
            ? " / $" + fmt(s.strike2, 0)
            : "";

        std::string strategy_label = s.strategy;
        // Human-readable strategy names
        if (s.strategy == "LONG_CALL")          strategy_label = "Long Call";
        else if (s.strategy == "LONG_PUT")       strategy_label = "Long Put";
        else if (s.strategy == "CSP")            strategy_label = "Cash-Secured Put";
        else if (s.strategy == "CC")             strategy_label = "Covered Call";
        else if (s.strategy == "BULL_CALL_SPREAD") strategy_label = "Bull Call Spread";
        else if (s.strategy == "BEAR_PUT_SPREAD")  strategy_label = "Bear Put Spread";
        else if (s.strategy == "STRADDLE")       strategy_label = "Long Straddle";
        else if (s.strategy == "STRANGLE")       strategy_label = "Long Strangle";

        bool unlimited_reward = (s.max_reward > 999990.0);
        std::string reward_str = unlimited_reward ? "Unlimited" : "$" + fmt(s.max_reward, 0);

        std::string rr_str;
        if (!unlimited_reward && s.max_risk > 0) {
            rr_str = "\n📊 *R:R Ratio:* " + fmt(s.max_reward / s.max_risk, 1) + ":1";
        }

        std::string regime_emoji = (regime.current_regime == Regime::RISK_ON)    ? "✅"
                                 : (regime.current_regime == Regime::RISK_OFF)   ? "🔴"
                                 : "🟡";

        std::string fc_footer = s.free_capital_mode
            ? "\n⚡ _Free Capital Mode — $" + fmt(s.allocated_capital, 0) + " allocated_"
            : "";

        int conf_pct = static_cast<int>(std::round(s.confidence * 100.0));

        std::string iv_zone = (s.iv_rank < 30) ? "LOW — buy premium zone ✅"
                            : (s.iv_rank > 50) ? "HIGH — sell premium zone ✅"
                            : "NEUTRAL";

        bool vol_rich  = (s.hrv30 > 0.01) && (s.iv_level > s.hrv30 * 1.20);
        bool vol_cheap = (s.hrv30 > 0.01) && (s.iv_level < s.hrv30 * 0.90);
        std::string hrv_tag = vol_rich  ? "RICH (sellers edge ✅)"
                            : vol_cheap ? "CHEAP (buyers edge ✅)"
                            : "FAIR";
        std::string hrv_line = "\n• IV: " + fmt(s.iv_level * 100.0, 1) +
                               "% | HRV-30: " + fmt(s.hrv30 * 100.0, 1) +
                               "% → " + hrv_tag;

        std::string alert =
            "📊 *OPTIONS SIGNAL — " + s.underlying + "* [" + s.profile_name + " · " + s.capital_tier + "]\n"
            "────────────────────────────────────\n"
            "🎯 *Strategy:* " + strategy_label + "\n"
            "📅 *Expiry:* " + s.expiry_date + "\n"
            "💵 *Strike(s):* $" + fmt(s.strike, 0) + leg2_str + "\n"
            "💰 *Entry:* $" + fmt(s.entry_price) +
                " | Max Risk: $" + fmt(s.max_risk, 0) +
                " | Max Gain: " + reward_str +
            rr_str + "\n"
            "⚖️ *Breakeven:* $" + fmt(s.breakeven) + "\n"
            "\n📐 *Greeks*\n"
            "• Delta: " + fmt(s.greeks.delta, 3) +
            " | Gamma: "  + fmt(s.greeks.gamma, 4) + "\n"
            "• Theta: $" + fmt(s.greeks.theta, 4) + "/day"
            " | Vega: "  + fmt(s.greeks.vega, 3) + "\n"
            "• IV Rank: " + fmt(s.iv_rank, 0) + "% ← " + iv_zone +
            hrv_line + "\n"
            "\n📈 *Technicals — " + s.underlying + "*\n"
            "• RSI(14): " + fmt(s.rsi) +
            " | ATR(14): $" + fmt(s.atr) + "\n"
            "\n🌐 *Macro Regime:* " + regime.log_message + " " + regime_emoji +
            " (VIX " + fmt(vix, 1) + ")\n"
            "🌡️ *Regime Gate:* " + (
                s.confidence >= 1.0 ? "✅ ON" :
                s.confidence <= 0.0 ? "🔴 OFF" :
                "⚠️ " + std::to_string(static_cast<int>(std::round(s.confidence * 100.0))) + "% (TRANSITION)"
            ) + "\n"
            "\n📋 _" + s.rationale + "_" +
            fc_footer + "\n"
            "⚠️ _Advisory only — manual execution required._";

        return alert;
    }

    // ── Telegram dispatch ─────────────────────────────────────────────────────

    void sendTelegram(const std::string& message) const {
        if (tgToken_.empty() || tgChatId_.empty()) return;
        try {
            httplib::Client cli("https://api.telegram.org");
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            // Send as a single message — options alerts are <1500 bytes, well within
            // Telegram's 4096-byte hard limit. Byte-boundary chunking would split
            // multi-byte UTF-8 emoji sequences and unclosed Markdown spans.
            json body = {
                {"chat_id",    tgChatId_},
                {"text",       message},
                {"parse_mode", "Markdown"}
            };
            std::string path = "/bot" + tgToken_ + "/sendMessage";
            cli.Post(path.c_str(), body.dump(), "application/json");
        } catch (...) {
            log("WARN", "[OPTIONS_SIGNAL] Telegram delivery failed.");
        }
    }

    // ── Per-ticker evaluator — returns scored signal or nullopt if no setup ───

    std::optional<ScoredSignal> evaluateTicker(
        const std::string& ticker,
        double effective_capital, const std::string& tier, bool fc_mode,
        double vix, const SpySnapshot& spy,
        const AllocationStrategy& regime,
        const std::unordered_map<std::string, SectorSnapshot>& sector_cache = {})
    {
        log("INFO", "[OPTIONS_SCAN] Scanning " + ticker + "...");

        UnderlyingData d = fetchUnderlyingBars(ticker);
        if (!d.valid) {
            log("WARN", "[OPTIONS_SCAN] No bar data for " + ticker + " — skipping.");
            return std::nullopt;
        }

        IVData iv_data  = fetchIVData(ticker, vix, d.hrv30);
        double iv_rank  = iv_data.iv_rank;
        double iv_sigma = iv_data.iv_level;
        double rfr      = 0.05;

        DirectionalBias bias     = computeBias(d);
        std::string     strategy = selectStrategy(bias, iv_rank, iv_sigma, d.hrv30, tier);

        double regime_clearance = regimeConfidence(regime.current_regime, strategy);
        if (regime_clearance < 1e-6) {
            log("INFO", "[OPTIONS_SCAN] " + ticker + " / " + strategy +
                " suppressed by RISK_OFF regime.");
            return std::nullopt;
        }

        // Setup quality gate — requires at least one of:
        //   A) Price ≥1.0×ATR from SMA20 (clear trend conviction)
        //   B) IV deviates ≥30% from HRV (strong vol signal)
        //   C) RSI ≤35 or ≥68 (clear momentum extreme)
        {
            double sma_atrs  = (d.atr14 > 0) ? std::abs(d.price - d.sma20) / d.atr14 : 0.0;
            bool strong_trend = sma_atrs >= 1.0;
            bool strong_vol   = (d.hrv30 > 0.01) &&
                                (iv_sigma > d.hrv30 * 1.30 || iv_sigma < d.hrv30 * 0.80);
            bool rsi_extreme  = d.rsi14 <= 35.0 || d.rsi14 >= 68.0;
            bool clear_bias   = (bias != DirectionalBias::Neutral);

            if (!strong_trend && !strong_vol && !rsi_extreme && !clear_bias) {
                log("INFO", "[OPTIONS_SCAN] " + ticker +
                    " — no qualifying setup (SMA=" + fmt(sma_atrs, 2) +
                    "xATR RSI=" + fmt(d.rsi14, 1) + "). Skipped.");
                return std::nullopt;
            }
        }

        // Computed once, early — feeds both quality-driven DTE selection and
        // quality-driven sizing below, plus the ranking score at dispatch time.
        double quality_score = computeQualityScore(d, iv_sigma, d.rsi14).quality_score;

        ContractParams cp = buildContractParams(strategy, d.price, d.atr14, rfr, iv_sigma, quality_score);
        if (cp.strike <= 0.0) {
            log("WARN", "[OPTIONS_SCAN] Could not determine valid strike for " + ticker);
            return std::nullopt;
        }

        // Skeptic intelligence (WS2 alt-macro + WS3 insider + China lag). Maps
        // the trade's directional bias to a size multiplier and, on a hard
        // opposition (fresh unpriced China move against us, or a headline that
        // contradicts the physical supply data), suppresses the entry outright.
        // Non-directional vol plays pass Neutral, so those can only be nudged
        // by the neutral-safe inputs, never suppressed.
        nox::skeptic::Dir skeptic_dir =
            (bias == DirectionalBias::Bullish) ? nox::skeptic::Dir::Bullish :
            (bias == DirectionalBias::Bearish) ? nox::skeptic::Dir::Bearish :
                                                 nox::skeptic::Dir::Neutral;
        nox::skeptic::Decision skeptic = fetchSkepticIntelligence(ticker, skeptic_dir);
        if (skeptic.suppress) {
            log("WARN", "[OPTIONS_SCAN][SKEPTIC] " + ticker + " / " + strategy +
                " suppressed — " + skeptic.detail);
            return std::nullopt;
        }

        double sized_capital = effective_capital * skeptic.size_mult;

        OptionsSignal sig = assembleSignal(ticker, d, strategy, cp,
                                           iv_rank, iv_sigma, rfr, regime_clearance,
                                           tier, fc_mode, sized_capital, d.hrv30, quality_score);

        // Sector/trend gate verdict — computed here (once per ticker) from the
        // per-scan sector_cache, carried on the signal so run_scan()'s dispatch
        // loop can check it without recomputing.
        auto sector_it = sectorEtfMap().find(ticker);
        if (sector_it != sectorEtfMap().end()) {
            sig.sector_etf = sector_it->second;
            auto cache_it = sector_cache.find(sector_it->second);
            if (cache_it != sector_cache.end()) {
                sig.sector_conflict = sectorConflicts(bias, cache_it->second);
            }
        }

        std::string alert = formatAlert(sig, vix, regime);
        return scoreSignal(sig, alert, d);
    }

    // ── Live order execution via OptionsOrderRouter ───────────────────────────

    void executeSignal(const OptionsSignal& sig) const {
        nox::options_router::OptionsOrderRouter router(alpacaUrl_, apiKey_, apiSec_);

        // Covered calls require 100 shares per contract as collateral.
        // Without them Alpaca would receive a naked short call — uncapped downside.
        // Abort and notify rather than let an unintended naked position through.
        if (sig.strategy == "CC") {
            if (!router.validateCCPosition(sig.underlying, profile_.qty_contracts)) {
                log("WARN", "[OPTIONS_EXEC] CC aborted — need " +
                    std::to_string(profile_.qty_contracts * 100) +
                    " shares of " + sig.underlying + ", none found.");
                sendTelegram(
                    "⚠️ *CC ORDER SKIPPED — " + sig.underlying + "*\n"
                    "────────────────────────\n"
                    "Need " + std::to_string(profile_.qty_contracts * 100) +
                    " shares for a covered call. No position found.\n"
                    "_Advisory signal still valid — execute manually if you hold the shares._"
                );
                return;
            }
        }

        auto result = router.route(sig, profile_.qty_contracts);

        if (result.success) {
            log("INFO", "[OPTIONS_EXEC] Order placed — " + result.message);
            // Persist the fill so it's tracked for exits and shows up in reports.
            // Without this the position lives only at the broker and is never sold.
            if (execution_recorder_) {
                try {
                    execution_recorder_(sig, profile_.qty_contracts);
                } catch (const std::exception& e) {
                    log("WARN", std::string("[OPTIONS_EXEC] execution_recorder failed: ") + e.what());
                }
            }
            sendTelegram(
                "✅ *OPTIONS ORDER PLACED*\n"
                "────────────────────────\n"
                "• *Ticker:* " + sig.underlying + "\n"
                "• *Strategy:* " + sig.strategy + "\n"
                "• *Contracts:* " + std::to_string(profile_.qty_contracts) + "\n"
                "• *Expiry:* " + sig.expiry_date + "\n"
                "• *Order ID:* `" + result.order_id + "`"
            );
        } else {
            log("WARN", "[OPTIONS_EXEC] Order failed — " + result.message);
            sendTelegram(
                "🚨 *OPTIONS ORDER FAILED*\n"
                "────────────────────────\n"
                "• *Ticker:* " + sig.underlying + "\n"
                "• *Strategy:* " + sig.strategy + "\n"
                "• *Reason:* `" + result.message + "`"
            );
        }
    }
};

} // namespace nox::options_signal
