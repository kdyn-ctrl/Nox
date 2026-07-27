#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "OptionEngine.hpp"
#include "OptionsSignalTypes.hpp"
#include "OptionsOrderRouter.hpp"
#include "IvRankStore.hpp"
#include "AlphaDecayStore.hpp"
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
#include <unordered_map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <functional>
#include <thread>
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
struct NoxUnitTestAccess;  // forward decl at global scope; defined in the test file
#endif

namespace nox::options_signal {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class DirectionalBias { Bullish, Bearish, Neutral };

// ─── QualityScore — setup conviction, computed from raw inputs only ───────────
// No OptionsSignal dependency, so it's computable as soon as strategy/bias are
// chosen — well before contract params or sizing are decided. This is what
// makes it usable to influence DTE selection and position sizing, not just
// ranking after the fact.
struct QualityScore {
    double quality_score     = 0.0;
    double sma_distance_atrs = 0.0; // how far price is from SMA20 in ATR units
    double vol_deviation     = 0.0; // abs(IV/HRV - 1.0)
    double rsi_extremity     = 0.0; // abs(RSI - 50) / 50
    double vol_ratio         = 1.0; // recent 5-day avg volume / 20-day avg
    double macd_alignment    = 0.0; // 1.0 if histogram confirms directional bias
    double macd_hist         = 0.0; // raw histogram value (for logging)
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
    double vol_ratio         = 1.0; // recent 5-day avg volume / 20-day avg
    double macd_alignment    = 0.0; // 1.0 if histogram confirms directional bias
    double macd_hist         = 0.0; // raw histogram value (for logging)
};

// ─── Structures ───────────────────────────────────────────────────────────────

struct UnderlyingData {
    double price       = 0.0;
    double sma20       = 0.0;
    double sma50       = 0.0;
    double rsi14       = 0.0;
    double atr14       = 0.0;
    double hrv30       = 0.20; // 30-day historical realized volatility (annualized)
    double vol20_avg   = 0.0;  // 20-day average daily volume
    double vol_ratio   = 1.0;  // recent 5-day avg volume / 20-day avg (>1.2 = expanding)
    double macd_line   = 0.0;  // MACD line (EMA12 - EMA26)
    double macd_signal = 0.0;  // 9-period EMA of macd_line
    double macd_hist   = 0.0;  // histogram (macd_line - macd_signal)
    bool   valid       = false;
};

// Returned by fetchIVData — actual IV level from Alpaca snapshot + display rank
struct IVData {
    double iv_level = 0.20; // annualized implied volatility (use in Black-Scholes)
    double iv_rank  = 50.0; // within-snapshot relative position (display only)
    bool   vol_rich = false; // iv_level > hrv30 * 1.20 — sell-premium environment
};

// VIX term structure snapshot — namespace scope so it can default-construct as a
// function parameter default without an incomplete-type issue.
// VIX3M/VIX > 1.05 → contango      → short-vol structurally favoured
// VIX3M/VIX < 0.95 → backwardation → front vol spiked → avoid selling premium
struct VixTermStructure {
    double spot  = -1.0; // ^VIX
    double vix3m = -1.0; // ^VIX3M (91-day CBOE index)
    double ratio = 1.0;  // vix3m / spot
    bool   valid = false;
    std::string label; // "CONTANGO" | "BACKWARDATION" | "FLAT"
};


// ─── OptionsSignalGenerator ───────────────────────────────────────────────────

class OptionsSignalGenerator {
#ifdef NOX_UNIT_TEST
    friend struct ::NoxUnitTestAccess;  // test-only access to private members; no-op in production builds
#endif
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

    // ── Phase 1 ghost-fill defensive hooks (engine-wired, header stays decoupled) ──
    //
    // Pre-order gate (items 3 & 4): consulted BEFORE the order fires. The engine
    // implementation checks the position-exists guard and the 60s duplicate
    // blocker, and — if allowed — writes the 'pending' ledger row. Returns Allow to
    // proceed, or a Blocked* reason to skip. `signature` and `client_oid` are
    // computed here and passed so the engine's ledger row matches this order.
    enum class OrderGate { Allow, BlockedDuplicate, BlockedPositionExists, BlockedError, BlockedRiskCap, BlockedKillSwitch };
    using PreOrderHook = std::function<OrderGate(const OptionsSignal& sig, int qty,
                                                 const std::string& client_oid,
                                                 const std::string& signature)>;
    void set_pre_order_hook(PreOrderHook cb) { pre_order_hook_ = std::move(cb); }

    // Post-order hook (items 1 & 2): called after route() returns to record the
    // order's disposition in the ledger. `ledger_status` is one of
    // pending/unknown/failed (never 'filled' here — only reconciliation confirms a
    // fill against broker truth).
    using PostOrderHook = std::function<void(const std::string& client_oid,
                                             const std::string& ledger_status,
                                             const std::string& broker_order_id)>;
    void set_post_order_hook(PostOrderHook cb) { post_order_hook_ = std::move(cb); }

    // Phase 2, item A (signal-regeneration audit): fired at every point run_scan
    // suppresses a candidate BEFORE it would reach the pre-order gate, so the
    // engine can persist a "why was this signature silent this cycle" trail
    // alongside the pre-order-gate outcomes it already logs. Header stays
    // DB-agnostic — same decoupling pattern as the hooks above.
    using SignalEventHook = std::function<void(const std::string& ticker,
                                                const std::string& strategy,
                                                const std::string& signature,
                                                double quality_score,
                                                const std::string& outcome,
                                                const std::string& reason)>;
    void set_signal_event_hook(SignalEventHook cb) { signal_event_hook_ = std::move(cb); }

    // Engine-wide prediction-quality logging (see CLAUDE.md's "Engine-Wide
    // Prediction Quality Scoring"): fired wherever a directional call gets a
    // confidence-like number but currently has nowhere to persist it — WS1
    // contradiction and the Skeptic WS2/WS3/WS8 feeds. Purely additive: this
    // never influences sizing/gating, only feeds the later resolver/rollup.
    // Header stays DB-agnostic — engine wires this to OrderLedger::logPrediction().
    using PredictionLogHook = std::function<void(const std::string& source_type,
                                                  const std::string& ticker,
                                                  const std::string& direction,
                                                  double confidence,
                                                  const std::string& detail)>;
    void set_prediction_log_hook(PredictionLogHook cb) { prediction_log_hook_ = std::move(cb); }

    // Full-detail signal store: fired for every fully-formed candidate — whether
    // it ends up submitted, gate-suppressed, earnings-skipped, or DTE-floor-
    // skipped — with its contract/regime context, so past signals are queryable
    // for analysis instead of only what actually reached the broker. Same
    // DB-agnostic decoupling as SignalEventHook; engine wires this to
    // OrderLedger::logGeneratedSignal().
    struct GeneratedSignalInfo {
        std::string ticker;
        std::string strategy;
        std::string signature;
        std::string direction;
        double      strike            = 0.0;
        double      strike2           = 0.0;
        double      strike3           = 0.0; // 4-leg strategies only (e.g. REVERSE_IRON_CONDOR)
        double      strike4           = 0.0;
        std::string expiration_date;
        int         dte               = 0;
        bool        macro_override_used = false;
        double      iv_rank           = 0.0;
        double      hrv30             = 0.0;
        double      quality_score     = 0.0;
        std::string regime;
        std::string vix_term_label;
        bool        earnings_checked  = true;
        std::string outcome;
        std::string reason;
    };
    using GeneratedSignalHook = std::function<void(const GeneratedSignalInfo&)>;
    void set_generated_signal_hook(GeneratedSignalHook cb) { generated_signal_hook_ = std::move(cb); }

    // ── Post-earnings drift research (passive — never gates or sizes a trade) ──
    //
    // The earnings gate above always SKIPS a ticker within 5 days of earnings;
    // this is a separate, independent observation trail so that skip decision
    // can be studied later instead of just trusted. When a ticker is skipped
    // for a *confirmed* earnings date (not the fail-closed "unconfirmed" case,
    // where the actual date isn't known), record the pre-earnings price and
    // technicals once per (ticker, earnings_date); the engine resolves the
    // realized move 1 and 5 calendar days after that date once enough time has
    // passed. Nothing here reads back into strategy selection or sizing — it
    // exists purely so the accumulated options_signals-adjacent table can be
    // read for patterns after a few months, per explicit direction: track,
    // don't trade on it (yet).
    struct EarningsDriftInfo {
        std::string ticker;
        std::string earnings_date; // YYYY-MM-DD, only fired when confirmed (not fail-closed)
        double      price  = 0.0;
        double      rsi    = 0.0;
        double      sma20  = 0.0;
        double      sma50  = 0.0;
        double      atr    = 0.0;
        std::string direction; // bullish/bearish/neutral going into earnings
    };
    using EarningsDriftHook = std::function<void(const EarningsDriftInfo&)>;
    void set_earnings_drift_hook(EarningsDriftHook cb) { earnings_drift_hook_ = std::move(cb); }

    // One row awaiting its T+1 or T+5 realized-move fill (day_offset is 1 or 5).
    struct EarningsDriftPendingItem {
        long        id         = 0;
        std::string ticker;
        double      pre_price  = 0.0;
        int         day_offset = 0;
    };
    using EarningsDriftPendingQuery = std::function<std::vector<EarningsDriftPendingItem>()>;
    void set_earnings_drift_pending_query(EarningsDriftPendingQuery q) {
        earnings_drift_pending_query_ = std::move(q);
    }
    using EarningsDriftResolveHook =
        std::function<void(long id, int day_offset, double price, double move_pct)>;
    void set_earnings_drift_resolve_hook(EarningsDriftResolveHook cb) {
        earnings_drift_resolve_hook_ = std::move(cb);
    }

    // Phase 2, item C: true 52-week historical IV Rank. Optional — nullptr keeps
    // fetchIVData()'s same-snapshot proxy (the historical table may not have
    // 30+ days yet, or the engine may not have wired this at all). Raw pointer,
    // not owned: the engine's IvRankStore outlives this generator.
    void set_iv_rank_store(nox::execution::IvRankStore* store) { iv_rank_store_ = store; }

    // Phase 3: alpha-decay tier-down. heartbeat/alpha_decay_monitor.py writes a
    // 0-1 multiplier when the rolling 30-day Sharpe has degraded >20% against
    // a positive 12-month baseline; this scales contract sizing down without
    // touching the strategy/entry logic itself. nullptr or no data → 1.0 (no
    // change). Raw pointer, not owned: outlives this generator.
    void set_alpha_decay_store(nox::execution::AlphaDecayStore* store) { alpha_decay_store_ = store; }

    // Phase 3: IBKR venue support. When set, executeSignal() routes through
    // this instead of constructing an Alpaca OptionsOrderRouter — main.cpp
    // wires this to IBKROrderRouter::route() when EXECUTION_VENUE=ibkr. Note:
    // the CC covered-call collateral check below queries Alpaca positions
    // specifically and is skipped (with a warning) when this override is set,
    // since IBKR's position query is a separate, not-yet-built path — CC
    // signals on IBKR are advisory-only until that gap is closed.
    using OrderExecutionOverride = std::function<nox::options_router::OrderResult(
        const OptionsSignal&, int qty_contracts, const std::string& client_oid)>;
    void set_order_execution_override(OrderExecutionOverride cb) {
        order_execution_override_ = std::move(cb);
    }

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

        // §6 C2: reset the Skeptic activity counters for this scan so the
        // end-of-scan summary reflects only this cycle, not a running total.
        skeptic_calls_this_scan_   = 0;
        skeptic_neutral_this_scan_ = 0;

        const auto& watchlist = profile_.watchlist;
        double effective_capital = resolveCapital(live_equity);
        std::string tier         = computeCapitalTier(effective_capital);
        bool  fc_mode            = (profile_.free_capital_amount > 0.0);

        log("INFO", "[OPTIONS_SCAN][" + profile_.name + "] Tier=" + tier +
            " | Capital=$" + fmt(effective_capital, 0) +
            " | Tickers=" + std::to_string(watchlist.size()) +
            " | MaxSignals=" + std::to_string(profile_.max_signals_per_scan));

        // Fetch VIX term structure first — it includes spot VIX so we can reuse it.
        VixTermStructure vts = fetchVixTermStructure();
        double vix = (vts.valid && vts.spot > 0.0) ? vts.spot : fetchVix();

        if (vts.valid) {
            log("INFO", "[OPTIONS_SCAN] VIX term structure: " + vts.label +
                " (VIX=" + fmt(vts.spot, 1) +
                " VIX3M=" + fmt(vts.vix3m, 1) +
                " ratio=" + fmt(vts.ratio, 3) + ")");
        } else {
            log("WARN", "[OPTIONS_SCAN] Could not fetch VIX term structure — spot VIX only.");
        }

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

        // Passive research trail (never gates/sizes anything) — resolve any
        // earlier observations now old enough for their T+1/T+5 realized move.
        resolveEarningsDrift();

        // Compute macro-catalyst DTE override: active if today's date matches
        // MACRO_DTE_OVERRIDE_DATES or VIX backwardation is extreme.
        bool use_macro_dte_override = shouldUseMacroDTEOverride(vts);

        // Post-earnings buffer window (Phase 3.3): days AFTER a report during
        // which we refuse fresh directional entries (IV-crush / mean-reversion
        // trap). Default 2 (~48h); 0 disables. Read once per scan.
        long post_earnings_buffer_days = 2;
        if (const char* v = std::getenv("OPTIONS_POST_EARNINGS_BUFFER_DAYS")) {
            try { post_earnings_buffer_days = std::stol(v); } catch (...) {}
        }

        // ── Stage 1: evaluate all tickers, collect scored candidates ──────────
        std::vector<ScoredSignal> candidates;
        for (const auto& ticker : watchlist) {
            try {
                bool pre_earnings  = hasEarningsWithin5Days(ticker, earnings_calendar);
                bool post_earnings = hasRecentEarnings(ticker, earnings_calendar,
                                                       post_earnings_buffer_days);
                if (pre_earnings || post_earnings) {
                    std::string reason, outcome;
                    if (!earnings_calendar.valid) {
                        // Calendar fetch failed, fail-closed
                        reason = "earnings calendar fetch failed, failing closed";
                        outcome = "skipped_earnings_unconfirmed";
                    } else if (post_earnings && !pre_earnings) {
                        // Reported within the last N days — IV-crush / mean-
                        // reversion trap (buying into the exhausted post-print move).
                        reason = "post-earnings buffer (reported within last " +
                                 std::to_string(post_earnings_buffer_days) +
                                 "d — IV crush / mean-reversion trap)";
                        outcome = "skipped_post_earnings_buffer";
                    } else {
                        // Confirmed earnings ahead
                        reason = "earnings confirmed within 5 days";
                        outcome = "skipped_earnings_confirmed";
                    }
                    log("INFO", "[OPTIONS_SCAN][EARNINGS_GATE] " + ticker +
                        " — " + reason + ".");
                    if (signal_event_hook_) {
                        signal_event_hook_(ticker, "", "", 0.0, outcome, reason);
                    }
                    fireGeneratedSignal(ticker, "", outcome, reason, nullptr, 0.0,
                                         "", "", &vts, use_macro_dte_override,
                                         /*earnings_checked=*/earnings_calendar.valid);
                    // Record a drift observation only for a CONFIRMED UPCOMING
                    // report — the post-earnings and fail-closed branches have no
                    // upcoming date to anchor an observation to.
                    if (earnings_calendar.valid && pre_earnings) {
                        recordEarningsDriftObservation(ticker, earnings_calendar);
                    }
                    continue;
                }
                auto result = evaluateTicker(ticker, effective_capital, tier,
                                             fc_mode, vix, spy, regime,
                                             use_macro_dte_override, vts, sector_cache);
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

            // VIX term structure gate: suppress SELL-premium auto-execution during
            // backwardation (front vol > longer-dated vol = tail-risk regime).
            // Advisory alerts still go out; only live execution is blocked.
            bool is_sell_premium = (sc.signal.strategy == "CSP" || sc.signal.strategy == "CC" ||
                                    sc.signal.strategy == "STRANGLE");
            if (vts.valid && vts.label == "BACKWARDATION" && is_sell_premium) {
                log("WARN", "[OPTIONS_SCAN][VIX_TERM] " + sc.signal.underlying +
                    " SELL-premium execution suppressed — VIX backwardation (" +
                    fmt(vts.ratio, 3) + "). Advisory alert sent.");
                sendTelegram(
                    "⚠️ *VIX TERM GATE — " + sc.signal.underlying + "*\n"
                    "────────────────────────\n"
                    "Strategy: " + sc.signal.strategy + "\n"
                    "VIX3M/VIX = " + fmt(vts.ratio, 3) + " → *BACKWARDATION*\n"
                    "Auto-execution suppressed — front vol > back vol signals tail risk.\n"
                    "_Advisory signal still valid._"
                );
                if (signal_event_hook_) {
                    signal_event_hook_(sc.signal.underlying, sc.signal.strategy,
                                       makeSignature(sc.signal), sc.quality_score,
                                       "suppressed_vix_term_gate",
                                       "VIX3M/VIX backwardation ratio=" + fmt(vts.ratio, 3));
                }
                fireGeneratedSignal(sc.signal.underlying, sc.signal.strategy,
                                     "suppressed_vix_term_gate",
                                     "VIX3M/VIX backwardation ratio=" + fmt(vts.ratio, 3),
                                     &sc.signal, sc.quality_score, "", "", &vts);
                // Do NOT consume a dispatch slot for a suppressed candidate
                // (audit §2 H4 / Core Philosophy): a persistent gate must not
                // starve executable signals ranked below it. The gate's own
                // advisory alert already went out.
                continue;
            }

            // Sector/trend gate: suppress auto-execution when the signal's bias
            // opposes its own sector ETF's EMA trend, even if the broad SPY/VIX
            // regime looks fine (a sector-wide rotation, not a market-wide one).
            // Advisory alerts still go out; only live execution is blocked.
            double sector_gate_enabled = 1.0;
            if (const char* v = std::getenv("SECTOR_TREND_GATE_ENABLED")) { try { sector_gate_enabled = std::stod(v); } catch (...) {} }
            if (sc.signal.sector_conflict && sector_gate_enabled > 0.5) {
                log("WARN", "[OPTIONS_SCAN][SECTOR_GATE] " + sc.signal.underlying +
                    " (" + sc.signal.sector_etf + ") execution suppressed — bias opposes sector trend.");
                sendTelegram(
                    "⚠️ *SECTOR TREND GATE — " + sc.signal.underlying + "*\n"
                    "────────────────────────\n"
                    "Strategy: " + sc.signal.strategy + "\n"
                    "Sector ETF " + sc.signal.sector_etf + " trend opposes this signal's bias.\n"
                    "Auto-execution suppressed — sector-wide headwind.\n"
                    "_Advisory signal still valid._"
                );
                if (signal_event_hook_) {
                    signal_event_hook_(sc.signal.underlying, sc.signal.strategy,
                                       makeSignature(sc.signal), sc.quality_score,
                                       "suppressed_sector_trend_gate",
                                       sc.signal.sector_etf + " trend conflict");
                }
                fireGeneratedSignal(sc.signal.underlying, sc.signal.strategy,
                                     "suppressed_sector_trend_gate",
                                     sc.signal.sector_etf + " trend conflict",
                                     &sc.signal, sc.quality_score, "", "", &vts);
                // Suppressed candidate does not consume a dispatch slot (§2 H4).
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
                    if (signal_event_hook_) {
                        signal_event_hook_(sc.signal.underlying, sc.signal.strategy,
                                           makeSignature(sc.signal), sc.quality_score,
                                           "suppressed_liquidity_gate", gate.reason);
                    }
                    fireGeneratedSignal(sc.signal.underlying, sc.signal.strategy,
                                         "suppressed_liquidity_gate", gate.reason,
                                         &sc.signal, sc.quality_score, "", "", &vts);
                    // Suppressed candidate does not consume a dispatch slot (§2 H4).
                    continue;
                }
                executeSignal(sc.signal);
            }
            fireGeneratedSignal(sc.signal.underlying, sc.signal.strategy, "submitted", "",
                                 &sc.signal, sc.quality_score, "", "", &vts);
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
            // `dispatched` also counts the loop's earlier iterations (including
            // gate-suppressed ones), so candidates[dispatched:] is exactly the
            // tail never reached because the cap was hit first.
            for (std::size_t i = static_cast<std::size_t>(dispatched); i < candidates.size(); ++i) {
                const auto& sc = candidates[i];
                if (signal_event_hook_) {
                    signal_event_hook_(sc.signal.underlying, sc.signal.strategy,
                                       makeSignature(sc.signal), sc.quality_score,
                                       "suppressed_cap",
                                       "max_signals_per_scan=" + std::to_string(limit));
                }
                fireGeneratedSignal(sc.signal.underlying, sc.signal.strategy,
                                     "suppressed_cap",
                                     "max_signals_per_scan=" + std::to_string(limit),
                                     &sc.signal, sc.quality_score, "", "", &vts);
            }
        }

        // §6 C2: report Skeptic activity every scan, even when everything was
        // neutral — a fully dark Skeptic (all N/N calls neutral) is now
        // distinguishable in the logs/alerts from "no signals today", instead
        // of being a byte-identical silent no-op.
        log("INFO", "[OPTIONS_SCAN][" + profile_.name + "] Skeptic neutral " +
            std::to_string(skeptic_neutral_this_scan_) + "/" +
            std::to_string(skeptic_calls_this_scan_) + " this scan.");
        if (skeptic_calls_this_scan_ > 0 && skeptic_neutral_this_scan_ == skeptic_calls_this_scan_) {
            log("WARN", "[OPTIONS_SCAN][" + profile_.name +
                "] Skeptic returned 100% neutral this scan (" +
                std::to_string(skeptic_calls_this_scan_) +
                " ticker(s)) — check for a dead insider/altmacro/china feed.");
            sendTelegram(
                "⚠️ *Skeptic all-neutral this scan* (" + profile_.name + ")\n"
                "────────────────────────\n" +
                std::to_string(skeptic_calls_this_scan_) + " ticker(s) checked, 0 with an "
                "actionable Skeptic signal. Could be a genuinely quiet day, or a dead "
                "insider/altmacro/china feed — check `/insider/clusters`, `/macro/alt`, "
                "`/lag/macro`."
            );
        }
    }

private:
    // ── Config ────────────────────────────────────────────────────────────────
    std::string alpacaUrl_;
    std::string apiKey_;
    std::string apiSec_;
    std::string tgToken_;
    std::string tgChatId_;
    RiskProfile profile_;
    ExecutionRecorder execution_recorder_;         // optional fill-persistence hook
    PreOrderHook       pre_order_hook_;             // Phase 1: pre-order gate (dedup + position check)
    PostOrderHook      post_order_hook_;            // Phase 1: ledger status update
    SignalEventHook    signal_event_hook_;          // Phase 2: gate/cap suppression audit trail
    PredictionLogHook  prediction_log_hook_;         // engine-wide prediction-quality logging
    GeneratedSignalHook generated_signal_hook_;      // full-detail signal store (every candidate)
    EarningsDriftHook          earnings_drift_hook_;         // post-earnings drift research (passive)
    EarningsDriftPendingQuery  earnings_drift_pending_query_;
    EarningsDriftResolveHook   earnings_drift_resolve_hook_;
    std::set<std::string> earnings_drift_recorded_; // (ticker|date) already recorded this process lifetime
    nox::execution::IvRankStore* iv_rank_store_ = nullptr; // Phase 2, item C: true historical IV rank
    nox::execution::AlphaDecayStore* alpha_decay_store_ = nullptr; // Phase 3: alpha-decay tier-down
    // §6 C2 (audit burndown Track 3): a totally dead Skeptic feed set (every
    // decide() call returning skeptic_neutral because every input parse
    // failed) used to be byte-identical in the logs to "no signals today" —
    // skeptic_neutral was deliberately unlogged. These count every
    // fetchSkepticIntelligence() call this scan and are reported once at the
    // end of run_scan() (RULE-D3: "Skeptic neutral 47/47 today" is
    // information; silence is not).
    mutable int skeptic_calls_this_scan_   = 0;
    mutable int skeptic_neutral_this_scan_ = 0;
    OrderExecutionOverride order_execution_override_; // Phase 3: IBKR venue routing
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
        if (!profile_.enforce_tier_gates) return true; // personal/breakout: all strategies open

        // LEAP strategies are directional longs — same tier rules as LONG_CALL/LONG_PUT
        if (strategy == "LEAP_CALL" || strategy == "LEAP_PUT") {
            return true; // allowed at all tiers; capital requirements checked at sizing
        }
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
                                strategy == "LEAP_CALL"  || strategy == "LEAP_PUT" ||
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

        // Approximate ET offset: Apr(3)–Oct(9) = UTC-4 (EDT), else UTC-5 (EST).
        // NOTE (audit §2 M4): month-bucketing is wrong on the DST-transition
        // margins (early March, early November) by 1h; proper handling needs the
        // 2nd-Sun-Mar / 1st-Sun-Nov transition dates — left as a minor follow-up.
        int offset_h = (utc.tm_mon >= 3 && utc.tm_mon <= 9) ? 4 : 5;
        int et_mins  = ((utc.tm_hour - offset_h + 24) % 24) * 60 + utc.tm_min;

        // Regular session is 09:30–16:00 ET. Opening at 09:00 (the prior bug)
        // queued market orders 30 min early, against the overnight gap (§2 M4).
        return et_mins >= 9 * 60 + 30 && et_mins < 16 * 60;
    }

    // ── Signal quality score ──────────────────────────────────────────────────
    // Combines independent conviction signals into a single rank value. Higher
    // = stronger setup. Computed from raw underlying/strategy inputs only (no
    // OptionsSignal dependency) so it's available BEFORE DTE selection and
    // sizing decide anything — not just after the fact for ranking.
    static QualityScore computeQualityScore(const UnderlyingData& d, double iv_sigma,
                                            double rsi, const std::string& strategy) {
        QualityScore q;
        q.sma_distance_atrs = (d.atr14 > 0)
            ? std::abs(d.price - d.sma20) / d.atr14 : 0.0;
        q.vol_deviation   = (d.hrv30 > 0.01)
            ? std::abs(iv_sigma / d.hrv30 - 1.0) : 0.0;
        q.rsi_extremity   = std::abs(rsi - 50.0) / 50.0;
        q.vol_ratio       = d.vol_ratio;
        // Volume boost: above-average volume strengthens the setup.
        // Capped at 2× so a single-day spike doesn't dominate.
        // At 1.0× (average) boost = 0.0; at 2.0× boost = 1.0.
        double vol_boost   = std::max(0.0, std::min(d.vol_ratio, 2.0) - 1.0);
        // MACD alignment: histogram must confirm directional bias.
        // Bullish strategies want macd_hist > 0; bearish want macd_hist < 0.
        bool is_bullish = (strategy == "LONG_CALL"  || strategy == "LEAP_CALL" ||
                           strategy == "BULL_CALL_SPREAD" || strategy == "CC");
        bool is_bearish = (strategy == "LONG_PUT"   || strategy == "LEAP_PUT" ||
                           strategy == "BEAR_PUT_SPREAD");
        q.macd_hist = d.macd_hist;
        if ((is_bullish && d.macd_hist > 0.0) || (is_bearish && d.macd_hist < 0.0))
            q.macd_alignment = 1.0;
        // Weights: trend (40%), vol signal (20%), RSI (15%), volume (15%), MACD (10%).
        // sma_distance_atrs and vol_deviation are unbounded raw magnitudes (kept
        // that way on the QualityScore/ScoredSignal struct for logging — see
        // run_scan()'s "SMA=...xATR" line) but must be normalized to [0,1] here,
        // same as the other three components, before weighting. Otherwise a
        // ticker mid a multi-week trend (5-6 raw ATRs from its SMA20) swamps the
        // per-scan ranking sort regardless of RSI/MACD/volume confirmation on
        // other candidates — the rest of the codebase already assumes a [0,1]
        // ceiling here (q_high=0.55 "elite setup" threshold in DTE/sizing logic).
        double sma_component = std::min(q.sma_distance_atrs / 3.0, 1.0); // 3+ ATRs = max
        double vol_component  = std::min(q.vol_deviation, 1.0);          // 100%+ IV/HRV divergence = max
        q.quality_score   = sma_component * 0.40
                           + vol_component  * 0.20
                           + q.rsi_extremity      * 0.15
                           + vol_boost             * 0.15
                           + q.macd_alignment     * 0.10;
        return q;
    }

    // Thin wrapper for the ranking/logging path in run_scan() — computes the
    // same score from an already-assembled OptionsSignal.
    static ScoredSignal scoreSignal(const OptionsSignal& sig,
                                    const UnderlyingData& d) {
        QualityScore q = computeQualityScore(d, sig.iv_level, sig.rsi, sig.strategy);
        ScoredSignal sc;
        sc.signal            = sig;
        sc.quality_score     = q.quality_score;
        sc.sma_distance_atrs = q.sma_distance_atrs;
        sc.vol_deviation     = q.vol_deviation;
        sc.rsi_extremity     = q.rsi_extremity;
        sc.vol_ratio         = q.vol_ratio;
        sc.macd_alignment    = q.macd_alignment;
        sc.macd_hist         = q.macd_hist;
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

    // ── Market data: VIX term structure (spot vs 3-month) ─────────────────────
    // VIX3M/VIX > 1.05 → contango (normal) → longer-dated vol pricier, favours
    // sell-premium strategies. < 0.95 → backwardation → front-month fear spike,
    // a tail-risk regime where selling premium gets run over. run_scan() uses
    // this to suppress SELL-premium auto-execution (advisory alert still fires).
    VixTermStructure fetchVixTermStructure() const {
        VixTermStructure ts;
        auto fetchSymbol = [](const std::string& symbol) -> double {
            try {
                httplib::Client cli("https://query1.finance.yahoo.com");
                cli.set_connection_timeout(std::chrono::seconds(8));
                cli.set_read_timeout(std::chrono::seconds(12));
                std::string path = "/v8/finance/chart/" + symbol + "?interval=1d&range=2d";
                auto res = cli.Get(path.c_str());
                if (!res || res->status != 200) return -1.0;
                auto body = json::parse(res->body);
                const auto& closes = body.at("chart").at("result").at(0)
                                          .at("indicators").at("quote").at(0).at("close");
                for (int i = static_cast<int>(closes.size()) - 1; i >= 0; --i)
                    if (!closes[i].is_null()) return closes[i].get<double>();
            } catch (...) {}
            return -1.0;
        };

        ts.spot  = fetchSymbol("%5EVIX");
        ts.vix3m = fetchSymbol("%5EVIX3M");

        if (ts.spot > 0.0 && ts.vix3m > 0.0) {
            ts.ratio = ts.vix3m / ts.spot;
            ts.valid = true;
            if (ts.ratio > 1.05)      ts.label = "CONTANGO";
            else if (ts.ratio < 0.95) ts.label = "BACKWARDATION";
            else                      ts.label = "FLAT";
        }
        return ts;
    }

    // ── WS1 Contradiction Vector — sentiment-vs-skew Kelly cut ────────────────
    // Queries the america-data-engine for this ticker's latest contradiction
    // verdict. A CONTRADICT* verdict (headline sentiment disagrees with IV skew)
    // halves the Kelly-sized risk budget. Fails open (1.0x, normal sizing) on any
    // network/parse error — a dead intelligence feed must never block execution.
    double fetchWS1KellyMultiplier(const std::string& ticker) const {
        try {
            const char* secret = std::getenv("WEBHOOK_SECRET_TOKEN");
            if (!secret) return 1.0;

            httplib::Client cli("http://america-data-engine:8001");
            cli.set_connection_timeout(std::chrono::seconds(3));
            cli.set_read_timeout(std::chrono::seconds(5));

            httplib::Headers headers = {{"X-Nox-Token", secret}};
            auto res = cli.Get("/contradiction/us", headers);
            if (!res || res->status != 200) return 1.0;

            auto body = json::parse(res->body);
            const auto& results = body.at("results");

            for (const auto& entry : results) {
                if (entry.value("ticker", "") != ticker) continue;
                std::string verdict = entry.value("verdict", "");
                double sentiment = entry.value("sentiment_score", 0.0);
                if (prediction_log_hook_ && verdict != "NO_DATA") {
                    std::string dir = sentiment > 0 ? "BULLISH" : sentiment < 0 ? "BEARISH" : "NEUTRAL";
                    prediction_log_hook_("ws1_contradiction", ticker, dir,
                                         std::min(1.0, std::abs(sentiment)), verdict);
                }
                if (verdict.rfind("CONTRADICT", 0) == 0) {
                    log("WARN", "[WS1][KELLY_CUT] " + ticker +
                        " — contradiction verdict: " + verdict +
                        " — kelly_fraction halved (0.5x).");
                    return 0.5;
                }
                return 1.0;
            }
        } catch (...) {}
        return 1.0; // fail-safe: intelligence feed unreachable → normal sizing
    }

    // ── Skeptic intelligence — WS2 alt-macro + WS3 insider + China lag ────────
    // The piece that finally makes the non-contradiction Skeptic workstreams
    // move real trades. Queries the three data-engine feeds that were exposed
    // but never consumed, folds them (plus the new /china/lag information-lag
    // feed) into one size-multiplier / suppress verdict via the pure
    // SkepticIntelligence decision layer. WS1 (contradiction) stays separate in
    // fetchWS1KellyMultiplier; this composes multiplicatively on top of it.
    //
    // Fail-open exactly like WS1: any unreachable/malformed feed contributes an
    // empty input, so decide() returns a 1.0x no-op — a dead Skeptic never
    // blocks or distorts execution (RULE-008).
    nox::skeptic::Decision fetchSkepticIntelligence(const std::string& ticker,
                                                    nox::skeptic::Dir dir) const {
        using namespace nox::skeptic;
        Inputs in;
        try {
            const char* secret = std::getenv("WEBHOOK_SECRET_TOKEN");
            if (!secret) {
                // §6 C2: this bypasses decide() entirely, so it must count and
                // log itself — otherwise a misconfigured token looks exactly
                // like a quiet day with nothing to say.
                ++skeptic_calls_this_scan_;
                ++skeptic_neutral_this_scan_;
                log("WARN", "[SKEPTIC][" + ticker + "] WEBHOOK_SECRET_TOKEN unset — "
                             "Skeptic feed unreachable, no-op (1.0x)");
                return Decision{}; // no auth → treat as no-op (1.0x)
            }

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
            auto dirStr = [](Dir d) -> std::string {
                return d == Dir::Bullish ? "BULLISH" : d == Dir::Bearish ? "BEARISH" : "NEUTRAL";
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
            // A genuine buy-cluster is inherently a bullish directional call —
            // insiders don't cluster-sell through this feed (see InsiderInput).
            if (prediction_log_hook_ && in.insider.has_cluster) {
                prediction_log_hook_("skeptic_insider", ticker, "BULLISH",
                                     std::min(1.0, in.insider.insider_count / 5.0),
                                     "insider_cluster(" + std::to_string(in.insider.insider_count) + ")");
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
            if (prediction_log_hook_ && in.alt_macro.applies && in.alt_macro.bias != Dir::Neutral) {
                prediction_log_hook_("skeptic_altmacro", ticker, dirStr(in.alt_macro.bias),
                                     std::min(1.0, in.alt_macro.strength),
                                     in.alt_macro.text_contradicts_physical ? "text_contradicts_physical" : "physical_supply");
            }

            // China macro information-lag feed (WS8). Lives on the separate
            // china-data-engine (which owns the PMI + retail-media lag data),
            // not the america-data-engine the WS1/WS2/WS3 feeds come from.
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
            if (prediction_log_hook_ && in.china.applies && in.china.bias != Dir::Neutral) {
                prediction_log_hook_("china_lag_ws8", ticker, dirStr(in.china.bias),
                                     std::min(1.0, in.china.strength),
                                     in.china.release + (in.china.fresh ? ",fresh" : ",stale"));
            }
        } catch (...) {
            ++skeptic_calls_this_scan_;
            ++skeptic_neutral_this_scan_;
            log("WARN", "[SKEPTIC][" + ticker + "] unexpected exception fetching Skeptic feeds — no-op (1.0x)");
            return Decision{}; // any unexpected failure → no-op
        }

        Decision d = decide(dir, in, Knobs::fromEnv());
        ++skeptic_calls_this_scan_;
        if (d.reason == "skeptic_neutral") {
            ++skeptic_neutral_this_scan_;
            // §6 C2: log at DEBUG, not silently — a neutral verdict must be
            // traceable in the logs, distinct from a call that never happened.
            log("DEBUG", "[SKEPTIC][" + ticker + "] skeptic_neutral — " + d.detail);
        } else {
            log(d.suppress ? "WARN" : "INFO",
                "[SKEPTIC][" + ticker + "] " + d.reason +
                " size_mult=" + fmt(d.size_mult, 2) + " — " + d.detail);
        }
        return d;
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
    // Broad VIX/SPY regime and VIX term structure don't catch a sector-wide
    // move against a single-name signal (e.g. a bullish tech name firing
    // straight into a tech rotation while SPY itself is fine). This computes
    // a sector ETF's EMA-fast-vs-EMA-slow trend, fetched once per distinct
    // ETF per scan cycle (see run_scan()'s sector_cache), and is checked at
    // dispatch time the same way the VIX term-structure gate is.

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
            {"XOM", "XLE"},
            {"F", "XLY"},
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

            auto res = cli.Get("/v8/finance/chart/" + etf_symbol + "?interval=1d&range=6mo");
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

    // ── Market data: Earnings Calendar (america-data-engine) ──────────────────

    struct EarningsEvent {
        std::string date;
        std::string description;
    };

    struct EarningsCalendarResult {
        std::map<std::string, std::vector<EarningsEvent>> data;
        bool valid = false;  // true only if fetch succeeded
    };

    EarningsCalendarResult fetchEarningsCalendar() const {
        EarningsCalendarResult result;
        try {
            httplib::Client cli("http://america-data-engine:8001");
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            // Construct the authorization header (WEBHOOK_SECRET_TOKEN)
            const char* webhook_secret = std::getenv("WEBHOOK_SECRET_TOKEN");
            if (!webhook_secret) {
                log("WARN", "[EARNINGS_FETCH] WEBHOOK_SECRET_TOKEN not set; failing closed (will skip all tickers).");
                return result;  // valid=false
            }

            httplib::Headers headers;
            headers.emplace("X-Nox-Token", webhook_secret);

            auto res = cli.Get("/earnings/calendar", headers);
            if (!res || res->status != 200) {
                log("WARN", "[EARNINGS_FETCH] america-data-engine returned status " +
                    std::to_string(res ? res->status : 0) + "; failing closed (will skip all tickers).");
                return result;  // valid=false
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
                result.data[ticker] = ticker_events;
            }

            int total_events = 0;
            for (const auto& pair : result.data) {
                total_events += pair.second.size();
            }
            log("INFO", "[EARNINGS_FETCH] Loaded earnings calendar: " +
                std::to_string(total_events) + " event(s).");
            result.valid = true;

        } catch (const std::exception& e) {
            log("WARN", "[EARNINGS_FETCH] Exception fetching earnings calendar: " +
                std::string(e.what()) + "; failing closed (will skip all tickers).");
        }
        return result;  // valid=false on exception
    }

    // Returns true if: (1) calendar is valid AND earnings found within 5 days, OR
    //                  (2) calendar is invalid (fetch failed) → fail-closed, assume earnings risk.
    // Caller can check calendar.valid to distinguish "confirmed no earnings" from
    // "could not confirm, skipping out of caution."
    // Signed day count from today (UTC) to an event date "YYYY-MM-DD", via the
    // days-from-civil (Hinnant) serial day number — correct across year
    // boundaries (the old ±1000 hack, audit §2 M2, skipped the gate for any
    // event in a different calendar year). Returns a large out-of-window
    // sentinel (999999) if the event date can't be parsed, so neither the
    // pre- nor post-earnings window matches it. Single source of the calendar
    // math for both hasEarningsWithin5Days and hasRecentEarnings (RULE-D6).
    static long daysFromTodayTo(const std::string& event_ymd) {
        auto to_days = [](int y, int m, int d) -> long {
            y -= m <= 2;
            long era = (y >= 0 ? y : y - 399) / 400;
            long yoe = static_cast<long>(y - era * 400);
            long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + doe - 719468;
        };
        int ey, em, ed;
        if (std::sscanf(event_ymd.c_str(), "%d-%d-%d", &ey, &em, &ed) != 3) {
            return 999999; // unparseable → out of every window
        }
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm* gm = std::gmtime(&tt);
        return to_days(ey, em, ed) -
               to_days(gm->tm_year + 1900, gm->tm_mon + 1, gm->tm_mday);
    }

    bool hasEarningsWithin5Days(const std::string& ticker,
                                 const EarningsCalendarResult& calendar) const {
        // Fail-closed: if the calendar fetch failed, treat ALL tickers as earnings-risky.
        if (!calendar.valid) {
            return true;
        }

        auto it = calendar.data.find(ticker);
        if (it == calendar.data.end()) {
            return false; // No earnings found for this ticker
        }

        // Earnings within 5 days ahead (inclusive of today).
        for (const auto& event : it->second) {
            long days_diff = daysFromTodayTo(event.date);
            if (days_diff >= 0 && days_diff <= 5) {
                return true;
            }
        }

        return false;
    }

    // Post-earnings buffer (Phase 3.3): true if a confirmed report for `ticker`
    // fell within the last `lookback_days` (a negative day-diff — already
    // reported). Blocks opening a fresh directional bet into the post-print
    // IV-crush / mean-reversion window (e.g. buying puts at the exact structural
    // bottom of a one-day gap-down). The premise is a KNOWN fact — the earnings
    // date — not a fabricated win probability, so it is allowed to gate
    // (RULE-D5). Returns false on an invalid calendar: hasEarningsWithin5Days
    // already fails closed there, so this must not double-count / mislabel it.
    bool hasRecentEarnings(const std::string& ticker,
                            const EarningsCalendarResult& calendar,
                            long lookback_days) const {
        if (!calendar.valid || lookback_days <= 0) {
            return false;
        }
        auto it = calendar.data.find(ticker);
        if (it == calendar.data.end()) {
            return false;
        }
        for (const auto& event : it->second) {
            long days_diff = daysFromTodayTo(event.date);
            if (days_diff < 0 && days_diff >= -lookback_days) {
                return true;
            }
        }
        return false;
    }

    // Structural suppression gates on a fully-assembled signal (Phase 3.A —
    // "wire the trade-math veto that was display-only into an actual gate").
    // Returns a non-empty suppression reason if the signal should be vetoed, or
    // "" to pass. Judges only MEASURED structure (realized R:R from the strikes,
    // long-leg delta) — never a fabricated win probability, so it complies with
    // RULE-D5 (contrast the EV gate we deliberately did NOT build: its P_win is
    // unmeasurable here — see research/direction_test.py). Both knobs env-tunable.
    std::string structuralSuppressionReason(const OptionsSignal& sig) const {
        // (A) Poor risk:reward on a defined-risk-AND-reward structure. The same
        // veto is already shown in the Telegram RISK ASSESSMENT section, but was
        // display-only — a sub-floor spread was still dispatched. Unlimited-
        // reward longs (calls/puts/LEAPs) and undefined-risk naked legs have no
        // meaningful R:R and are skipped. Default 1.0 = reject only when you'd
        // risk MORE than the max reward on a defined trade (a structural
        // absurdity, not an edge bet); set OPTIONS_MIN_RR_RATIO=1.5 for the
        // stricter floor, or <=0 to disable.
        double min_rr = 1.0;
        if (const char* v = std::getenv("OPTIONS_MIN_RR_RATIO")) { try { min_rr = std::stod(v); } catch (...) {} }
        bool unlimited_reward = (sig.max_reward > 999990.0);
        bool unlimited_risk   = (sig.max_risk   > 999990.0);
        if (min_rr > 0.0 && !unlimited_reward && !unlimited_risk && sig.max_risk > 0.0) {
            double rr = sig.max_reward / sig.max_risk;
            if (rr < min_rr) {
                return "poor R:R " + fmt(rr, 2) + ":1 (< " + fmt(min_rr, 2) +
                       " floor) — risk exceeds reward on a defined structure";
            }
        }
        // (B) Lottery-ticket veto: long-leg delta below a floor = a low-
        // probability OTM bet. OFF by default (0.0) — delta is probability of
        // expiring ITM, a crude proxy the live direction test undercuts, so it
        // gates only when a human opts in via OPTIONS_MIN_LONG_DELTA (RULE-D5).
        double min_delta = 0.0;
        if (const char* v = std::getenv("OPTIONS_MIN_LONG_DELTA")) { try { min_delta = std::stod(v); } catch (...) {} }
        if (min_delta > 0.0) {
            bool directional_long = (sig.strategy == "LONG_CALL" || sig.strategy == "LONG_PUT" ||
                                     sig.strategy == "LEAP_CALL" || sig.strategy == "LEAP_PUT" ||
                                     sig.strategy == "BULL_CALL_SPREAD" || sig.strategy == "BEAR_PUT_SPREAD");
            if (directional_long && std::abs(sig.greeks.delta) < min_delta) {
                return "lottery-ticket delta " + fmt(std::abs(sig.greeks.delta), 2) +
                       " (< " + fmt(min_delta, 2) + " floor)";
            }
        }
        return "";
    }

    // Phase 3.2: short-dated directional longs must be IN-THE-MONEY. Below the
    // DTE threshold, theta annihilates an OTM long leg before the underlying
    // can travel far enough to cross the strike, so a sub-14-DTE OTM debit
    // spread is a structural theta trap. When the resolved DTE is under
    // OPTIONS_SHORT_DTE_ITM_THRESHOLD (default 14), floor the long-leg target
    // delta at OPTIONS_SHORT_DTE_MIN_LONG_DELTA (default 0.60 → ITM). Both the
    // DTE and the delta are measured quantities, so this may gate (RULE-D5).
    // Applies to directional long legs only (calls/puts and the long leg of a
    // debit spread); LEAPs are already long-dated, income/wing legs unaffected.
    double effectiveLongDelta(int resolved_dte, double base_delta) const {
        int threshold = 14;
        if (const char* v = std::getenv("OPTIONS_SHORT_DTE_ITM_THRESHOLD")) { try { threshold = std::stoi(v); } catch (...) {} }
        double itm_floor = 0.60;
        if (const char* v = std::getenv("OPTIONS_SHORT_DTE_MIN_LONG_DELTA")) { try { itm_floor = std::stod(v); } catch (...) {} }
        if (threshold > 0 && resolved_dte < threshold) {
            return std::max(base_delta, itm_floor);
        }
        return base_delta;
    }

    // Returns the closest confirmed earnings date within the next 5 days for
    // `ticker`, or "" if none / calendar invalid. Used only for the passive
    // earnings-drift research trail below — reuses parseDateToEpochDays'
    // exact Gregorian day count instead of hasEarningsWithin5Days' simplified
    // same-year day-of-year diff, so it's safe across year boundaries.
    static std::string nextEarningsDateWithin5Days(const std::string& ticker,
                                                     const EarningsCalendarResult& calendar) {
        if (!calendar.valid) return "";
        auto it = calendar.data.find(ticker);
        if (it == calendar.data.end()) return "";

        auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm_buf{};
        gmtime_r(&now_t, &tm_buf);
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d");
        long today_days = parseDateToEpochDays(oss.str());

        std::string best;
        long best_diff = -1;
        for (const auto& event : it->second) {
            long diff = parseDateToEpochDays(event.date) - today_days;
            if (diff >= 0 && diff <= 5 && (best.empty() || diff < best_diff)) {
                best = event.date;
                best_diff = diff;
            }
        }
        return best;
    }

    // Fires once per (ticker, earnings_date) per process lifetime (the DB's
    // UNIQUE constraint is the backstop across restarts) — an extra bars fetch
    // solely for research, never gating or sizing anything.
    void recordEarningsDriftObservation(const std::string& ticker,
                                         const EarningsCalendarResult& earnings_calendar) {
        if (!earnings_drift_hook_) return;
        std::string edate = nextEarningsDateWithin5Days(ticker, earnings_calendar);
        if (edate.empty()) return;
        std::string key = ticker + "|" + edate;
        if (earnings_drift_recorded_.count(key)) return;
        earnings_drift_recorded_.insert(key);

        UnderlyingData ed = fetchUnderlyingBars(ticker);
        if (!ed.valid) return;
        EarningsDriftInfo info;
        info.ticker        = ticker;
        info.earnings_date = edate;
        info.price         = ed.price;
        info.rsi           = ed.rsi14;
        info.sma20         = ed.sma20;
        info.sma50         = ed.sma50;
        info.atr           = ed.atr14;
        info.direction     = biasLabel(computeBias(ed));
        earnings_drift_hook_(info);
    }

    // Called once per run_scan(). Asks the engine (via the pending-query hook)
    // which observations are now old enough to have their T+1/T+5 realized
    // move filled in, fetches the current price for each, and writes the
    // result back through the resolve hook. Fails silently/fails open — a
    // missed resolution just gets retried next scan.
    void resolveEarningsDrift() const {
        if (!earnings_drift_pending_query_ || !earnings_drift_resolve_hook_) return;
        auto pending = earnings_drift_pending_query_();
        for (const auto& item : pending) {
            if (item.pre_price <= 0.0) continue;
            UnderlyingData d = fetchUnderlyingBars(item.ticker);
            if (!d.valid) continue;
            double move_pct = (d.price - item.pre_price) / item.pre_price * 100.0;
            earnings_drift_resolve_hook_(item.id, item.day_offset, d.price, move_pct);
        }
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

            const auto& raw_closes  = ohlcv.at("close");
            const auto& raw_highs   = ohlcv.at("high");
            const auto& raw_lows    = ohlcv.at("low");
            const auto& raw_volumes = ohlcv.at("volume");

            std::vector<double> closes, highs, lows, volumes;
            for (size_t i = 0; i < raw_closes.size(); ++i) {
                if (!raw_closes[i].is_null() && !raw_highs[i].is_null() && !raw_lows[i].is_null()) {
                    closes.push_back(raw_closes[i].get<double>());
                    highs.push_back(raw_highs[i].get<double>());
                    lows.push_back(raw_lows[i].get<double>());
                    double v = (i < raw_volumes.size() && !raw_volumes[i].is_null())
                               ? raw_volumes[i].get<double>() : 0.0;
                    volumes.push_back(v);
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

            // Volume: 20-day average and recent 5-day vs baseline ratio.
            // vol_ratio > 1.2 = expanding volume (confirms breakouts and trend moves).
            // Fails neutral (1.0) when volume data is unavailable.
            double vol20_avg = 0.0, vol5_avg = 0.0;
            if (volumes.size() >= 20) {
                for (size_t i = volumes.size() - 20; i < volumes.size(); ++i) vol20_avg += volumes[i];
                vol20_avg /= 20.0;
            }
            if (volumes.size() >= 5) {
                for (size_t i = volumes.size() - 5; i < volumes.size(); ++i) vol5_avg += volumes[i];
                vol5_avg /= 5.0;
            }
            double vol_ratio = (vol20_avg > 1000.0) ? vol5_avg / vol20_avg : 1.0;

            // MACD(12,26,9): seed each EMA on the simple average of its first n
            // bars, then run the remaining bars forward. Only the final MACD-line,
            // signal-line, and histogram values are needed.
            double macd_line = 0.0, macd_signal_val = 0.0, macd_hist_val = 0.0;
            if (closes.size() >= 35) { // 26 seed + 9 signal seed minimum
                double seed12 = 0.0, seed26 = 0.0;
                for (size_t i = 0; i < 12; ++i) seed12 += closes[i];
                seed12 /= 12.0;
                for (size_t i = 0; i < 26; ++i) seed26 += closes[i];
                seed26 /= 26.0;
                // Build the MACD-line series (starting after the 26-bar seed) so
                // the 9-period signal EMA can be seeded and run over it.
                std::vector<double> macd_series;
                macd_series.reserve(closes.size() - 26);
                double e12 = seed12, e26 = seed26;
                double k12 = 2.0 / 13.0, k26 = 2.0 / 27.0;
                for (size_t i = 26; i < closes.size(); ++i) {
                    e12 = closes[i] * k12 + e12 * (1.0 - k12);
                    e26 = closes[i] * k26 + e26 * (1.0 - k26);
                    macd_series.push_back(e12 - e26);
                }
                macd_line = macd_series.back();
                if (macd_series.size() >= 9) {
                    double sig_seed = 0.0;
                    for (size_t i = 0; i < 9; ++i) sig_seed += macd_series[i];
                    sig_seed /= 9.0;
                    double k9 = 2.0 / 10.0;
                    double sig_ema = sig_seed;
                    for (size_t i = 9; i < macd_series.size(); ++i)
                        sig_ema = macd_series[i] * k9 + sig_ema * (1.0 - k9);
                    macd_signal_val = sig_ema;
                }
                macd_hist_val = macd_line - macd_signal_val;
            }

            UnderlyingData d;
            d.price       = closes.back();
            d.sma20       = sma20;
            d.sma50       = sma50;
            d.rsi14       = rsi;
            d.atr14       = atr_sum / 14.0;
            d.hrv30       = hrv30;
            d.vol20_avg   = vol20_avg;
            d.vol_ratio   = vol_ratio;
            d.macd_line   = macd_line;
            d.macd_signal = macd_signal_val;
            d.macd_hist   = macd_hist_val;
            d.valid       = true;
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

        // Phase 2, item C: override the same-snapshot proxy above with the true
        // 52-week historical percentile once enough history exists — this is
        // what the iv_rank_buy_max/iv_rank_sell_min thresholds were always meant
        // to gate on. Applies regardless of which branch above set iv_level
        // (live snapshot or VIX-proxy fallback); silently keeps the proxy if the
        // store isn't wired or hasn't accumulated 30+ distinct days yet.
        if (iv_rank_store_) {
            auto hist = iv_rank_store_->computeRank(symbol, result.iv_level);
            if (hist.full_history) result.iv_rank = hist.iv_rank;
        }

        return result;
    }

    // ── Directional bias from technicals ─────────────────────────────────────

    static DirectionalBias computeBias(const UnderlyingData& d) {
        bool above_20  = d.price > d.sma20;
        bool above_50  = d.price > d.sma50;
        // Symmetric bias detection: both bullish and bearish require the same trend
        // structure (price above/below both SMAs) + symmetric RSI ranges.
        // Bullish: price above SMA20 and SMA50, RSI > 50 but not overbought (< 70)
        // Bearish: price below SMA20 and SMA50, RSI < 50 but not oversold (> 30)
        // Note: RSI extremes (<30 oversold, >70 overbought) are mean-reversion zones,
        // not directional signals — stay Neutral there to avoid fighting the reversal.
        bool rsi_bull  = d.rsi14 > 50.0 && d.rsi14 < 70.0;
        bool rsi_bear  = d.rsi14 < 50.0 && d.rsi14 > 30.0;  // mirror of bullish
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
                               double hrv, const std::string& tier,
                               double sma_atrs = 0.0, double rsi = 50.0,
                               bool above_sma50 = true) const {
        // Breakout profile: strong displacement from SMA20 + SMA50 alignment + RSI momentum zone.
        // SMA50 check prevents LEAP signals on counter-trend bounces — price must be on the right
        // side of the medium-term trend before committing to a 6-month directional contract.
        if (profile_.is_breakout_profile && sma_atrs >= profile_.breakout_sma_atrs_min) {
            if (bias == DirectionalBias::Bullish && above_sma50 && rsi >= 55.0 && rsi <= 78.0)
                return "LEAP_CALL";
            if (bias == DirectionalBias::Bearish && !above_sma50 && rsi >= 22.0 && rsi <= 45.0)
                return "LEAP_PUT";
            // Breakout profile but SMA50 or RSI not aligned — fall through to normal selection
        }

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
            if (strategyAllowed("STRANGLE", tier)) return "STRANGLE"; // naked short income — undefined risk, see assembleSignal
            if (strategyAllowed("CSP", tier))      return "CSP";
        }
        if (prefer_buy) {
            // Cheap IV + no directional bias + breakout expected: prefer the
            // defined-risk debit structure (capped loss = net debit, capped
            // gain = spread width - debit) over an unlimited-reward straddle
            // when the tier allows it — same "expect a big move, don't know
            // which way" bet, less capital at risk.
            if (strategyAllowed("REVERSE_IRON_CONDOR", tier)) return "REVERSE_IRON_CONDOR";
            if (strategyAllowed("STRADDLE", tier)) return "STRADDLE"; // vol expansion play
        }
        if (strategyAllowed("CSP", tier)) return "CSP";
        return "LONG_CALL";
    }

    // ── Contract parameter construction ───────────────────────────────────────
    //
    // ── Contract liquidity filter ─────────────────────────────────────────────
    // Verifies the actual options chain before committing to a signal: a
    // technically-valid setup on an illiquid contract just bleeds to the
    // bid-ask spread. Fails open (liquid=true) when Alpaca's snapshot data is
    // unavailable — a quote-feed hiccup must never block an otherwise-good signal.

    struct ContractLiquidity {
        bool   valid         = false;
        double bid           = 0.0;
        double ask           = 0.0;
        double mid           = 0.0;
        double spread_pct    = 0.0;  // (ask-bid)/mid
        int    open_interest = 0;
        bool   liquid        = false; // true if spread_pct < 0.15 and OI > 50
    };

    ContractLiquidity checkContractLiquidity(const std::string& symbol,
                                             const std::string& expiry_date,
                                             double strike,
                                             const std::string& option_type) const {
        ContractLiquidity result;
        result.liquid = true; // fail open by default

        try {
            httplib::Client cli(alpacaUrl_);
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey_},
                {"APCA-API-SECRET-KEY", apiSec_}
            };

            // Narrow band around the target strike (±0.5) so we get only the
            // intended contract or its immediate neighbours.
            std::ostringstream path_ss;
            path_ss << "/v1beta1/options/snapshots/" << symbol
                    << "?expiration_date=" << expiry_date
                    << "&strike_price_gte=" << fmt(strike - 0.5, 2)
                    << "&strike_price_lte=" << fmt(strike + 0.5, 2)
                    << "&type=" << option_type
                    << "&feed=indicative";

            auto res = cli.Get(path_ss.str().c_str(), headers);
            if (!res || res->status != 200) return result;

            auto body = json::parse(res->body);
            if (!body.contains("snapshots") || body["snapshots"].empty()) return result;

            const auto& snapshots = body["snapshots"];

            // Closest-strike match: parse the OCC symbol's embedded strike field
            // (last 8 digits / 1000). E.g. AAPL250117C00150000 → $150.000.
            std::string best_key;
            double      best_diff = 1e9;
            for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
                const std::string& occ = it.key();
                if (occ.size() >= 8) {
                    try {
                        double s    = std::stod(occ.substr(occ.size() - 8)) / 1000.0;
                        double diff = std::abs(s - strike);
                        if (diff < best_diff) { best_diff = diff; best_key = occ; }
                    } catch (...) {}
                }
            }
            if (best_key.empty()) best_key = snapshots.begin().key();

            const auto& snap = snapshots.at(best_key);

            // Bid/ask: prefer greeks.bid_price / ask_price, then latestQuote.bp/ap
            double bid = 0.0, ask = 0.0;
            if (snap.contains("greeks")) {
                bid = snap["greeks"].value("bid_price", 0.0);
                ask = snap["greeks"].value("ask_price", 0.0);
            }
            if ((bid <= 0.0 || ask <= 0.0) && snap.contains("latestQuote")) {
                bid = snap["latestQuote"].value("bp", 0.0);
                ask = snap["latestQuote"].value("ap", 0.0);
            }
            if (bid <= 0.0 || ask <= 0.0 || ask < bid) return result;

            // Open interest: try both field name variants, then use |delta|*10000
            // as a proxy when the snapshot omits the OI field.
            int oi = 0;
            if (snap.contains("openInterest") && !snap["openInterest"].is_null())
                oi = snap["openInterest"].get<int>();
            else if (snap.contains("open_interest") && !snap["open_interest"].is_null())
                oi = snap["open_interest"].get<int>();
            else if (snap.contains("greeks") && snap["greeks"].contains("delta") &&
                     !snap["greeks"]["delta"].is_null())
                oi = static_cast<int>(std::abs(snap["greeks"]["delta"].get<double>()) * 10000.0);

            double mid        = (bid + ask) / 2.0;
            double spread_pct = (mid > 0.0) ? (ask - bid) / mid : 1.0;

            result.valid         = true;
            result.bid           = bid;
            result.ask           = ask;
            result.mid           = mid;
            result.spread_pct    = spread_pct;
            result.open_interest = oi;
            result.liquid        = (spread_pct < 0.15 && oi > 50);
        } catch (...) {}

        return result;
    }

    // Selects strike(s) and DTE based on strategy type.
    // Uses delta targets computed via Black-Scholes.
    // Returns false if no viable contract found.

    struct ContractParams {
        double strike  = 0.0;
        double strike2 = 0.0; // 0 = single-leg
        double strike3 = 0.0; // 4-leg strategies only
        double strike4 = 0.0; // 4-leg strategies only
        double expiry  = 0.0; // years to expiration
        std::string expiry_date;
    };

    ContractParams buildContractParams(const std::string& strategy,
                                       double spot, double atr,
                                       double rfr, double iv_sigma,
                                       double hrv30 = 0.20,
                                       double quality_score = 0.5) const {
        ContractParams p;

        // Minimum DTE floor (env-tunable, default 7 days to avoid 0DTE landmines)
        int min_dte = 7;
        if (const char* v = std::getenv("MIN_DTE_FLOOR")) {
            try { min_dte = std::max(1, std::stoi(v)); } catch (...) {}
        }

        // Target DTE from profile
        int target_dte = profile_.dte_long;
        if (strategy == "LEAP_CALL" || strategy == "LEAP_PUT")
            target_dte = profile_.dte_leap;
        else if (strategy == "CSP" || strategy == "CC")
            target_dte = profile_.dte_income;
        else if (strategy == "STRADDLE" || strategy == "STRANGLE" ||
                 strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD" ||
                 strategy == "REVERSE_IRON_CONDOR")
            target_dte = profile_.dte_spread;

        // Adaptive DTE for directional longs: high realized vol → faster moves →
        // halve the DTE window so we don't overpay for time in a whipsaw regime.
        // But never go below the minimum floor.
        if ((strategy == "LONG_CALL" || strategy == "LONG_PUT") && hrv30 > 0.30)
            target_dte = std::max(min_dte, target_dte / 2);

        // Quality-driven DTE — theta-sensitive long-premium strategies only.
        // A weak setup (low momentum/conviction) needs more time for the thesis
        // to play out or theta kills it before it can; a strong setup doesn't
        // need to overpay for time it won't use. Short-premium strategies
        // (CSP/CC) are excluded — decay is their edge, not their enemy.
        bool is_theta_sensitive_long = (strategy == "LONG_CALL" || strategy == "LONG_PUT" ||
                                        strategy == "LEAP_CALL" || strategy == "LEAP_PUT" ||
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
                target_dte = std::max(min_dte, relaxed); // never below the global/macro floor
            } else if (quality_score <= q_low) {
                target_dte = std::max(target_dte, low_floor_days); // push weak setups out, never shrink strong ones
            }
            // mid-band: profile default stands unchanged
        }

        // Enforce minimum DTE on all strategies to prevent 0DTE/1DTE landmines
        target_dte = std::max(min_dte, target_dte);

        p.expiry = target_dte / 365.0;

        // Roll forward to the next valid options expiration date (Friday).
        // Start with the target_dte date and advance until we find a Friday.
        // This ensures the broker can actually fill the order, not a fake date.
        auto now    = std::chrono::system_clock::now();
        auto target_tp = now + std::chrono::hours(24 * target_dte);

        // Find the next Friday (tm_wday: 0=Sun, 1=Mon, ..., 5=Fri, 6=Sat)
        auto exp_t = std::chrono::system_clock::to_time_t(target_tp);
        std::tm tm_buf{};
        gmtime_r(&exp_t, &tm_buf);

        int days_until_friday = (5 - tm_buf.tm_wday + 7) % 7;  // 0 if already Friday, else days to Friday
        if (days_until_friday == 0 && tm_buf.tm_wday == 5) {
            // Already Friday, use it
        } else if (days_until_friday > 0) {
            // Advance to next Friday
            exp_t += 86400 * days_until_friday;
            gmtime_r(&exp_t, &tm_buf);
        } else {
            // Roll to next Friday
            exp_t += 86400 * ((5 - tm_buf.tm_wday + 7) % 7);
            gmtime_r(&exp_t, &tm_buf);
        }

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

        // Phase 3.2: floor the long-leg delta to ITM for short-dated plays
        // (target_dte < threshold). Only the directional long leg is bumped;
        // the OTM short wing (d_wing) and income legs (d_inc) are untouched, so
        // a short-DTE debit spread becomes an ITM-long / OTM-short structure
        // instead of an all-OTM theta trap.
        const double d_long = effectiveLongDelta(target_dte, profile_.delta_long);
        const double d_inc  = profile_.delta_income;
        const double d_wing = profile_.delta_spread_wing;
        const double d_leap = profile_.delta_leap;

        if (strategy == "LEAP_CALL") {
            p.strike = findStrikeForDelta(d_leap, nox::options::OptionType::Call);
        } else if (strategy == "LEAP_PUT") {
            p.strike = findStrikeForDelta(d_leap, nox::options::OptionType::Put);
        } else if (strategy == "LONG_CALL") {
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
        } else if (strategy == "REVERSE_IRON_CONDOR") {
            // Long debit iron condor: bull call spread + bear put spread, same
            // deltas as those two strategies individually (near-ATM long leg,
            // far-OTM short wing on each side).
            p.strike  = findStrikeForDelta(d_long, nox::options::OptionType::Call); // long call
            p.strike2 = findStrikeForDelta(d_wing, nox::options::OptionType::Call); // short call (far OTM)
            if (p.strike2 <= p.strike) p.strike2 = p.strike + atr;
            p.strike3 = findStrikeForDelta(d_long, nox::options::OptionType::Put);  // long put
            p.strike4 = findStrikeForDelta(d_wing, nox::options::OptionType::Put);  // short put (far OTM)
            if (p.strike4 >= p.strike3) p.strike4 = p.strike3 - atr;
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
        sig.strike3           = cp.strike3;
        sig.strike4           = cp.strike4;
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

        bool is_call = (strategy == "LONG_CALL" || strategy == "LEAP_CALL" ||
                        strategy == "CC" || strategy == "BULL_CALL_SPREAD" ||
                        strategy == "STRADDLE" || strategy == "REVERSE_IRON_CONDOR");
        primary.type   = is_call ? OptionType::Call : OptionType::Put;
        sig.option_type = primary.type;

        OptionGreeks g1 = compute_greeks(primary);

        double max_risk   = computeMaxRisk(allocated_capital, tier);
        // Phase 3: alpha-decay tier-down scales the risk budget itself (not the
        // floored contract count) so a degraded regime shrinks sizing smoothly
        // instead of only mattering once it crosses a whole-contract boundary.
        double decay_scale = alpha_decay_store_ ? alpha_decay_store_->getTierMultiplier() : 1.0;

        // Quality-driven sizing multiplier — composes with decay_scale the same
        // way ws1_mult does elsewhere: a low-conviction setup gets a smaller
        // slice of the risk budget, a high-conviction one a slightly larger one.
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
        int resolved_dte = static_cast<int>(std::round(cp.expiry * 365.0));
        {
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

        // Contracts must be sized off the strategy's ACTUAL per-contract max
        // loss, not the single primary leg's premium — g1.price alone is only
        // the true cost basis for outright long premium (LONG_CALL/PUT/LEAP).
        // For CSP/CC the real capital at risk is the strike/spot minus premium
        // (orders of magnitude larger than the premium itself); for spreads/
        // straddles/strangles/the reverse iron condor it's the net debit,
        // which differs from a single leg's price. Sizing off g1.price alone
        // for those previously let the actual dollar risk balloon far past
        // the intended risk_pct budget (CSP/CC could be 50-100x over).
        double per_contract_risk = g1.price * 100.0; // default: outright long premium
        if (strategy == "CSP") {
            per_contract_risk = std::max(0.01, (cp.strike - g1.price)) * 100.0;
        } else if (strategy == "CC") {
            per_contract_risk = std::max(0.01, (d.price - g1.price)) * 100.0;
        } else if (strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD") {
            OptionContract sizing_leg = primary;
            sizing_leg.strike = cp.strike2;
            double debit = g1.price - compute_greeks(sizing_leg).price;
            per_contract_risk = std::max(0.01, debit) * 100.0;
        } else if (strategy == "STRADDLE") {
            OptionContract sizing_put = primary;
            sizing_put.type = OptionType::Put;
            per_contract_risk = (g1.price + compute_greeks(sizing_put).price) * 100.0;
        } else if (strategy == "STRANGLE") {
            // Naked short strangle (selectStrategy only picks STRANGLE under
            // prefer_sell — this is always the income/credit thesis, never a
            // long strangle). Real risk is NOT the premium collected, that's
            // the max reward — it's the margin the position ties up, which
            // can run many multiples of the premium. Sizing off premium (the
            // old behaviour) let a small risk budget buy far more naked short
            // vol than the account could actually margin.
            // Proxy formula (invented, not broker-verified — RULE-D5): the
            // standard Reg-T "greater of 20% underlying notional minus OTM
            // amount, or 10% of strike, plus premium" per leg, margined at
            // the worse of the two legs (both can't move against you at once).
            OptionContract sizing_put = primary;
            sizing_put.type   = OptionType::Put;
            sizing_put.strike = cp.strike2;
            double put_price   = compute_greeks(sizing_put).price;
            double call_otm    = std::max(0.0, cp.strike  - d.price);
            double put_otm     = std::max(0.0, d.price - cp.strike2);
            double call_margin = std::max(0.20 * d.price * 100.0 - call_otm * 100.0,
                                           0.10 * cp.strike * 100.0) + g1.price * 100.0;
            double put_margin  = std::max(0.20 * d.price * 100.0 - put_otm * 100.0,
                                           0.10 * cp.strike2 * 100.0) + put_price * 100.0;
            per_contract_risk  = std::max(call_margin, put_margin);
        } else if (strategy == "REVERSE_IRON_CONDOR") {
            OptionContract sizing_short_call = primary;
            sizing_short_call.strike = cp.strike2;
            OptionContract sizing_long_put = primary;
            sizing_long_put.type   = OptionType::Put;
            sizing_long_put.strike = cp.strike3;
            OptionContract sizing_short_put = primary;
            sizing_short_put.type   = OptionType::Put;
            sizing_short_put.strike = cp.strike4;
            double call_debit = g1.price - compute_greeks(sizing_short_call).price;
            double put_debit  = compute_greeks(sizing_long_put).price - compute_greeks(sizing_short_put).price;
            per_contract_risk = std::max(0.01, call_debit + put_debit) * 100.0;
        }
        double contracts  = std::floor((max_risk * decay_scale * quality_size_mult) / per_contract_risk);
        sig.contracts      = static_cast<int>(contracts);

        // Per-contract P&L geometry
        if (strategy == "LONG_CALL" || strategy == "LONG_PUT" ||
            strategy == "LEAP_CALL" || strategy == "LEAP_PUT") {
            sig.entry_price = g1.price;
            sig.max_risk    = g1.price * 100.0 * contracts;
            sig.max_reward  = 999999.0; // theoretically unlimited — sentinel
            bool is_call_side = (strategy == "LONG_CALL" || strategy == "LEAP_CALL");
            sig.breakeven   = is_call_side
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
            // Short strangle — sells both legs for a credit. max_reward is
            // capped at the premium collected; max_risk is genuinely uncapped
            // on the call side (and large-but-bounded on the put side), so it
            // gets the same 999999 sentinel long premium strategies use for
            // unlimited reward — here it means unlimited RISK instead. Do not
            // read this as "no risk"; it is the opposite.
            OptionContract put_leg = primary;
            put_leg.type   = OptionType::Put;
            put_leg.strike = cp.strike2;
            OptionGreeks gp = compute_greeks(put_leg);

            double total_credit = g1.price + gp.price;
            sig.entry_price = total_credit;
            sig.max_risk    = 999999.0;
            sig.max_reward  = total_credit * 100.0 * contracts;
            sig.breakeven   = cp.strike + total_credit;
            sig.greeks.price = total_credit;
            // Short both legs → position greeks are the negative of the raw
            // per-share long greeks (short gamma/vega, positive theta from
            // collecting decay).
            sig.greeks.delta = -(g1.delta + gp.delta);
            sig.greeks.gamma = -(g1.gamma + gp.gamma);
            sig.greeks.theta = -(g1.theta + gp.theta);
            sig.greeks.vega  = -(g1.vega  + gp.vega);
            sig.greeks.implied_volatility = iv_sigma;

        } else if (strategy == "REVERSE_IRON_CONDOR") {
            // Long debit iron condor: bull call spread (strike/strike2) netted
            // with a bear put spread (strike3/strike4). Unlike STRADDLE/STRANGLE,
            // reward here is CAPPED at the narrower spread's width minus the
            // total debit — the tradeoff for a much smaller max_risk.
            OptionContract short_call = primary;
            short_call.strike = cp.strike2;
            short_call.type   = OptionType::Call;
            OptionGreeks g_sc = compute_greeks(short_call);

            OptionContract long_put = primary;
            long_put.strike = cp.strike3;
            long_put.type   = OptionType::Put;
            OptionGreeks g_lp = compute_greeks(long_put);

            OptionContract short_put = primary;
            short_put.strike = cp.strike4;
            short_put.type   = OptionType::Put;
            OptionGreeks g_sp = compute_greeks(short_put);

            double call_debit  = g1.price - g_sc.price;  // bull call spread
            double put_debit   = g_lp.price - g_sp.price; // bear put spread
            double total_debit = call_debit + put_debit;
            double call_width  = std::abs(cp.strike2 - cp.strike);
            double put_width   = std::abs(cp.strike3 - cp.strike4);

            sig.entry_price = total_debit;
            sig.max_risk    = total_debit * 100.0 * contracts;
            sig.max_reward  = std::max(0.0, std::min(call_width, put_width) - total_debit) * 100.0 * contracts;
            // Two breakevens exist (upper & lower); OptionsSignal only stores
            // one, same limitation STRADDLE/STRANGLE already have. Store the
            // upper one; the lower is cp.strike3 - put_debit.
            sig.breakeven   = cp.strike + total_debit;
            sig.greeks.price = total_debit;
            sig.greeks.delta = g1.delta - g_sc.delta + g_lp.delta - g_sp.delta;
            sig.greeks.gamma = g1.gamma - g_sc.gamma + g_lp.gamma - g_sp.gamma;
            sig.greeks.theta = g1.theta - g_sc.theta + g_lp.theta - g_sp.theta;
            sig.greeks.vega  = g1.vega  - g_sc.vega  + g_lp.vega  - g_sp.vega;
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
        bool is_leap = (strategy == "LEAP_CALL" || strategy == "LEAP_PUT");
        std::string r = strategy + " on " +
                        (d.price > d.sma20 ? "bullish" : "bearish") + " bias. ";
        if (is_leap) {
            double sma_atrs = (d.atr14 > 0) ? std::abs(d.price - d.sma20) / d.atr14 : 0.0;
            r += "Breakout setup: price " + fmt(sma_atrs, 1) +
                 "x ATR from SMA20 with RSI=" + fmt(d.rsi14, 1) +
                 " in momentum zone. 180-day expiry gives time to develop. ";
        } else {
            r += "RSI=" + fmt(d.rsi14) + ", ATR=" + fmt(d.atr14) + ". ";
        }
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
                            const AllocationStrategy& regime,
                            const VixTermStructure& vts = VixTermStructure{},
                            double quality_score = -1.0) const
    {
        std::string leg2_str = (s.strike2 > 0)
            ? " / $" + fmt(s.strike2, 0)
            : "";
        if (s.strategy == "REVERSE_IRON_CONDOR") {
            // 4 legs: long call / short call / long put / short put
            leg2_str = " / $" + fmt(s.strike2, 0) + " (calls), $" +
                       fmt(s.strike3, 0) + " / $" + fmt(s.strike4, 0) + " (puts)";
        }

        std::string strategy_label = s.strategy;
        // Human-readable strategy names
        if (s.strategy == "LONG_CALL")             strategy_label = "Long Call";
        else if (s.strategy == "LONG_PUT")          strategy_label = "Long Put";
        else if (s.strategy == "LEAP_CALL")         strategy_label = "LEAP Call (180d+)";
        else if (s.strategy == "LEAP_PUT")          strategy_label = "LEAP Put (180d+)";
        else if (s.strategy == "CSP")               strategy_label = "Cash-Secured Put";
        else if (s.strategy == "CC")                strategy_label = "Covered Call";
        else if (s.strategy == "BULL_CALL_SPREAD")  strategy_label = "Bull Call Spread";
        else if (s.strategy == "BEAR_PUT_SPREAD")   strategy_label = "Bear Put Spread";
        else if (s.strategy == "STRADDLE")          strategy_label = "Long Straddle";
        else if (s.strategy == "STRANGLE")          strategy_label = "Short Strangle";
        else if (s.strategy == "REVERSE_IRON_CONDOR") strategy_label = "Reverse Iron Condor";

        bool unlimited_reward = (s.max_reward > 999990.0);
        std::string reward_str = unlimited_reward ? "Unlimited" : "$" + fmt(s.max_reward, 0);

        // Short strategies (e.g. short strangle) carry the 999999 sentinel in
        // max_risk to mean "undefined/naked risk" — the mirror of the max_reward
        // sentinel above. Without this the human saw a literal "Max Risk: $999999"
        // and a bogus "0.0:1 POOR R:R" flag (audit 2nd-pass A5, RULE-D2 honesty).
        bool unlimited_risk = (s.max_risk > 999990.0);
        std::string risk_str = unlimited_risk ? "Undefined (naked)" : "$" + fmt(s.max_risk, 0);

        std::string rr_str;
        if (!unlimited_reward && !unlimited_risk && s.max_risk > 0) {
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

        std::string vts_line;
        if (vts.valid) {
            std::string vts_emoji = (vts.label == "BACKWARDATION") ? "⚠️"
                                  : (vts.label == "CONTANGO")      ? "✅" : "➖";
            vts_line = "\n📉 *VIX Term Structure:* " + vts.label + " " + vts_emoji +
                       " (VIX=" + fmt(vts.spot, 1) + " · VIX3M=" + fmt(vts.vix3m, 1) +
                       " · ratio=" + fmt(vts.ratio, 3) + ")";
        }

        // ── Risk Assessment section (probability, R:R quality, decay warnings) ──
        double win_prob = std::abs(s.greeks.delta) * 100.0;
        std::string prob_str = "📈 *Win Probability:* ~" + fmt(win_prob, 0) + "%";

        std::vector<std::string> risk_flags;

        // Poor R:R warning (skip for naked/undefined-risk structures — the
        // sentinel would otherwise render every short strangle as "0.0:1 POOR R:R")
        if (!unlimited_reward && !unlimited_risk && s.max_risk > 0) {
            double rr_ratio = s.max_reward / s.max_risk;
            if (rr_ratio < 1.5) {
                risk_flags.push_back("⚠️ POOR R:R (" + fmt(rr_ratio, 1) + ":1 — consider skipping)");
            }
        }

        // Low probability warning
        if (win_prob < 30.0) {
            risk_flags.push_back("⚠️ LOW PROBABILITY (delta < 0.30 — lottery ticket)");
        }

        // High theta decay warning
        if (s.greeks.theta < -0.50) {
            risk_flags.push_back("⚠️ HIGH THETA DECAY ($" + fmt(std::abs(s.greeks.theta), 2) + "/day — watch intraday)");
        }

        // IV extremes warning
        if (s.iv_rank < 20.0) {
            risk_flags.push_back("⚠️ LOW IV RANK (" + fmt(s.iv_rank, 0) + "% — cheap premium, high risk)");
        } else if (s.iv_rank > 80.0) {
            risk_flags.push_back("⚠️ HIGH IV RANK (" + fmt(s.iv_rank, 0) + "% — expensive, vol crush risk)");
        }

        std::string risk_section = "\n⚠️ *RISK ASSESSMENT*\n" + prob_str + "\n";
        if (!risk_flags.empty()) {
            for (const auto& flag : risk_flags) {
                risk_section += "• " + flag + "\n";
            }
        } else {
            risk_section += "• ✅ No major red flags\n";
        }

        std::string quality_line;
        if (quality_score >= 0.0) {
            quality_line = "🏆 *Quality Score:* " + fmt(quality_score, 2) +
                " _(higher = stronger setup; ranks this signal vs. others this scan)_\n";
        }

        std::string alert =
            "📊 *OPTIONS SIGNAL — " + s.underlying + "* [" + s.profile_name + " · " + s.capital_tier + "]\n"
            "────────────────────────────────────\n"
            + quality_line +
            "🎯 *Strategy:* " + strategy_label + "\n"
            "📅 *Expiry:* " + s.expiry_date + "\n"
            "💵 *Strike(s):* $" + fmt(s.strike, 0) + leg2_str + "\n"
            "💰 *Entry:* $" + fmt(s.entry_price) +
                " | Max Risk: " + risk_str +
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
            + risk_section +
            "\n📈 *Technicals — " + s.underlying + "*\n"
            "• RSI(14): " + fmt(s.rsi) +
            " | ATR(14): $" + fmt(s.atr) + "\n"
            "\n🌐 *Macro Regime:* " + regime.log_message + " " + regime_emoji +
            " (VIX " + fmt(vix, 1) + ")\n"
            "🌡️ *Regime Gate:* " + (
                s.confidence >= 1.0 ? "✅ ON" :
                s.confidence <= 0.0 ? "🔴 OFF" :
                "⚠️ " + std::to_string(static_cast<int>(std::round(s.confidence * 100.0))) + "% (TRANSITION)"
            ) + "\n" +
            vts_line +
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

    // ── Calendar-day helpers (portable, no timezone dependency) ──────────────
    // Howard Hinnant's civil_from_days / days_from_civil algorithms: exact
    // Gregorian-calendar day counts without touching mktime/localtime (which are
    // TZ- and DST-sensitive and not what we want for a pure date diff).

    static long daysFromCivil(int y, unsigned m, unsigned d) {
        y -= (m <= 2);
        const long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<long>(doe) - 719468;
    }

    static std::string civilFromDays(long z) {
        z += 719468;
        const long era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const long y = static_cast<long>(yoe) + era * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned d = doy - (153 * mp + 2) / 5 + 1;
        const unsigned m = mp + (mp < 10 ? 3 : -9);
        int year = static_cast<int>(y + (m <= 2));
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(4) << year << "-"
            << std::setw(2) << m << "-" << std::setw(2) << d;
        return oss.str();
    }

    static long parseDateToEpochDays(const std::string& ymd) {
        int y = 0; unsigned m = 0, d = 0;
        std::sscanf(ymd.c_str(), "%d-%u-%u", &y, &m, &d);
        return daysFromCivil(y, m, d);
    }

    static std::string shiftDate(const std::string& ymd, int delta_days) {
        return civilFromDays(parseDateToEpochDays(ymd) + delta_days);
    }

    // ── Macro-catalyst DTE override check ─────────────────────────────────────
    // Returns true if the DTE floor should be lowered from MIN_DTE_FLOOR to
    // MACRO_MIN_DTE_FLOOR (default 2 days) — triggered by:
    //   1. Today's date in MACRO_DTE_OVERRIDE_DATES (comma-separated ISO dates)
    //   2. VIX backwardation beyond MACRO_DTE_OVERRIDE_VIX_RATIO (default 0.90)
    bool shouldUseMacroDTEOverride(const VixTermStructure& vts) const {
        // Check for explicit macro calendar dates
        const char* macro_dates_str = std::getenv("MACRO_DTE_OVERRIDE_DATES");
        if (macro_dates_str && std::strlen(macro_dates_str) > 0) {
            auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm_buf{};
            gmtime_r(&now_t, &tm_buf);
            std::ostringstream today_oss;
            today_oss << std::put_time(&tm_buf, "%Y-%m-%d");
            std::string today_str = today_oss.str();

            std::string dates_str(macro_dates_str);
            size_t pos = 0;
            while (pos < dates_str.length()) {
                size_t comma = dates_str.find(',', pos);
                std::string date_candidate = dates_str.substr(pos,
                    comma == std::string::npos ? std::string::npos : comma - pos);
                // Trim whitespace
                date_candidate.erase(0, date_candidate.find_first_not_of(" \t\n\r"));
                date_candidate.erase(date_candidate.find_last_not_of(" \t\n\r") + 1);
                if (date_candidate == today_str) {
                    return true;
                }
                pos = (comma == std::string::npos) ? dates_str.length() : comma + 1;
            }
        }

        // Check for extreme VIX backwardation
        double override_vix_ratio = 0.90;
        if (const char* v = std::getenv("MACRO_DTE_OVERRIDE_VIX_RATIO")) {
            try { override_vix_ratio = std::stod(v); } catch (...) {}
        }
        if (vts.valid && vts.ratio > 0.0 && vts.ratio < override_vix_ratio) {
            log("INFO", "[OPTIONS_SCAN] Macro DTE override active: VIX backwardation " +
                fmt(vts.ratio, 3) + " < " + fmt(override_vix_ratio, 3));
            return true;
        }
        return false;
    }

    // ── Contract validation — snap target expiry to nearest available ────────
    // Queries Alpaca for contracts near the TARGET expiry (not just "any expiry
    // in the strike band") and returns whichever available expiry is closest to
    // it — preserving the profile's DTE intent (e.g. 45d directional, 30d income)
    // instead of silently collapsing every signal to the nearest-dated contract.
    std::string validateAndSnapExpiryDate(const std::string& underlying,
                                          double strike1,
                                          const std::string& target_expiry_date,
                                          bool use_macro_override = false) const
    {
        try {
            httplib::Client cli(alpacaUrl_);
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey_},
                {"APCA-API-SECRET-KEY", apiSec_}
            };

            double strike_lo = strike1 * 0.85;
            double strike_hi = strike1 * 1.15;

            // Determine effective minimum DTE based on macro-catalyst override.
            // MACRO_MIN_DTE_FLOOR (default 2 days) when override active,
            // else MIN_DTE_FLOOR (default 7 days).
            int macro_min_dte = 2;
            if (const char* v = std::getenv("MACRO_MIN_DTE_FLOOR")) {
                try { macro_min_dte = std::max(1, std::stoi(v)); } catch (...) {}
            }
            int base_min_dte = 7;
            if (const char* v = std::getenv("MIN_DTE_FLOOR")) {
                try { base_min_dte = std::max(1, std::stoi(v)); } catch (...) {}
            }
            int effective_min_dte = use_macro_override ? macro_min_dte : base_min_dte;

            // Compute today's date, then the minimum allowed expiration date.
            // If effective_min_dte is 7 and today is 2026-07-10, we accept
            // expirations >= 2026-07-17; anything nearer is rejected regardless
            // of target proximity.
            auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm_buf{};
            gmtime_r(&now_t, &tm_buf);
            std::ostringstream today_oss;
            today_oss << std::put_time(&tm_buf, "%Y-%m-%d");
            std::string today_str = today_oss.str();
            std::string min_allowed_str = shiftDate(today_str, effective_min_dte);
            long min_allowed_epoch = parseDateToEpochDays(min_allowed_str);

            // Window the query around the target DTE (±21 days) so a liquid
            // weekly-optionable name's near-term expiries don't exhaust `limit`
            // before the API ever returns a contract near what we actually want.
            std::string window_lo = shiftDate(target_expiry_date, -21);
            std::string window_hi = shiftDate(target_expiry_date, 21);

            auto queryContracts = [&](bool bounded) -> json {
                std::ostringstream path;
                path << "/v2/options/contracts?underlying_symbols=" << underlying
                     << "&type=call"
                     << "&strike_price_gte=" << std::fixed << std::setprecision(2) << strike_lo
                     << "&strike_price_lte=" << std::fixed << std::setprecision(2) << strike_hi
                     << "&expiration_date_gte=" << (bounded ? window_lo : target_expiry_date);
                if (bounded) path << "&expiration_date_lte=" << window_hi;
                path << "&limit=50";
                auto res = cli.Get(path.str().c_str(), headers);
                if (!res || res->status != 200) return json::array();
                auto body = json::parse(res->body);
                return body.value("option_contracts", json::array());
            };

            auto contracts = queryContracts(/*bounded=*/true);
            if (contracts.empty()) {
                // Nothing in the ±21d window (e.g. underlying only lists monthlies
                // further out) — widen to "on or after target", same as lookupContractImpl.
                contracts = queryContracts(/*bounded=*/false);
            }
            if (contracts.empty()) {
                return ""; // No contracts at all
            }

            // Filter candidates: reject any expiration nearer than effective_min_dte
            // from today, then pick the closest to the target from what remains.
            std::string best_expiry;
            long best_diff = -1;
            long target_days = parseDateToEpochDays(target_expiry_date);
            for (const auto& c : contracts) {
                std::string ex = c.value("expiration_date", "");
                if (ex.empty()) continue;
                long ex_epoch = parseDateToEpochDays(ex);
                // Reject if too near (before min_allowed_epoch)
                if (ex_epoch < min_allowed_epoch) {
                    continue;
                }
                long diff = std::labs(ex_epoch - target_days);
                if (best_diff < 0 || diff < best_diff) {
                    best_diff   = diff;
                    best_expiry = ex;
                }
            }

            if (best_expiry.empty()) {
                // No candidates survived the DTE floor check. Fall back to the
                // next Friday >= effective_min_dte from today.
                std::string fallback_date = shiftDate(today_str, effective_min_dte);
                // Round up to next Friday
                auto fb_t = std::chrono::system_clock::now() +
                           std::chrono::hours(24 * effective_min_dte);
                auto fb_time_t = std::chrono::system_clock::to_time_t(fb_t);
                std::tm fb_buf{};
                gmtime_r(&fb_time_t, &fb_buf);
                int days_until_friday = (5 - fb_buf.tm_wday + 7) % 7;
                if (days_until_friday > 0) {
                    fb_time_t += 86400 * days_until_friday;
                    gmtime_r(&fb_time_t, &fb_buf);
                }
                std::ostringstream fb_oss;
                fb_oss << std::put_time(&fb_buf, "%Y-%m-%d");
                best_expiry = fb_oss.str();
                log("WARN", "[OPTIONS_SCAN] " + underlying +
                    " — no listed expiration >= " + std::to_string(effective_min_dte) +
                    " DTE. Falling back to next Friday: " + best_expiry);
            } else if (best_diff > 21) {
                log("WARN", "[OPTIONS_SCAN] " + underlying + " nearest available expiry " +
                    best_expiry + " is " + std::to_string(best_diff) +
                    " days from the intended target " + target_expiry_date + ".");
            }
            return best_expiry;
        } catch (const std::exception& e) {
            log("WARN", "[OPTIONS_SCAN] Contract validation failed: " + std::string(e.what()));
            return ""; // On error, skip this signal to avoid bad expiry dates
        }
    }

    // ── Per-ticker evaluator — returns scored signal or nullopt if no setup ───

    std::optional<ScoredSignal> evaluateTicker(
        const std::string& ticker,
        double effective_capital, const std::string& tier, bool fc_mode,
        double vix, const SpySnapshot& spy,
        const AllocationStrategy& regime,
        bool use_macro_dte_override = false,
        const VixTermStructure& vts = VixTermStructure{},
        const std::unordered_map<std::string, SectorSnapshot>& sector_cache = {})
    {
        log("INFO", "[OPTIONS_SCAN] Scanning " + ticker + "...");

        UnderlyingData d = fetchUnderlyingBars(ticker);
        if (!d.valid) {
            log("WARN", "[OPTIONS_SCAN] No bar data for " + ticker + " — skipping.");
            fireGeneratedSignal(ticker, "", "skipped_no_bar_data", "no bar data available");
            return std::nullopt;
        }

        IVData iv_data  = fetchIVData(ticker, vix, d.hrv30);
        double iv_rank  = iv_data.iv_rank;
        double iv_sigma = iv_data.iv_level;
        double rfr      = 0.05;

        double sma_atrs  = (d.atr14 > 0) ? std::abs(d.price - d.sma20) / d.atr14 : 0.0;
        bool   above_50  = (d.price > d.sma50);
        DirectionalBias bias     = computeBias(d);
        std::string     strategy = selectStrategy(bias, iv_rank, iv_sigma, d.hrv30, tier,
                                                  sma_atrs, d.rsi14, above_50);

        // Computed once, early — feeds both quality-driven DTE selection and
        // quality-driven sizing below, plus the ranking score at dispatch time.
        double quality_score = computeQualityScore(d, iv_sigma, d.rsi14, strategy).quality_score;

        std::string regime_label = (regime.current_regime == Regime::RISK_ON)  ? "RISK_ON"
                                  : (regime.current_regime == Regime::RISK_OFF) ? "RISK_OFF"
                                  : "TRANSITION";

        double regime_clearance = regimeConfidence(regime.current_regime, strategy);
        if (regime_clearance < 1e-6) {
            log("INFO", "[OPTIONS_SCAN] " + ticker + " / " + strategy +
                " suppressed by RISK_OFF regime.");
            fireGeneratedSignal(ticker, strategy, "suppressed_regime_gate",
                                 "regime=" + regime_label, nullptr, 0.0,
                                 biasLabel(bias), regime_label, &vts);
            return std::nullopt;
        }

        // Setup quality gate — requires at least one of:
        //   A) Price ≥1.0×ATR from SMA20 (clear trend conviction)
        //   B) IV deviates ≥30% from HRV (strong vol signal)
        //   C) RSI ≤35 or ≥68 (clear momentum extreme)
        {
            bool strong_trend = sma_atrs >= 1.0;
            bool strong_vol   = (d.hrv30 > 0.01) &&
                                (iv_sigma > d.hrv30 * 1.30 || iv_sigma < d.hrv30 * 0.80);
            bool rsi_extreme  = d.rsi14 <= 35.0 || d.rsi14 >= 68.0;
            bool clear_bias   = (bias != DirectionalBias::Neutral);

            if (!strong_trend && !strong_vol && !rsi_extreme && !clear_bias) {
                log("INFO", "[OPTIONS_SCAN] " + ticker +
                    " — no qualifying setup (SMA=" + fmt(sma_atrs, 2) +
                    "xATR RSI=" + fmt(d.rsi14, 1) + "). Skipped.");
                fireGeneratedSignal(ticker, strategy, "skipped_no_qualifying_setup",
                                     "SMA=" + fmt(sma_atrs, 2) + "xATR RSI=" + fmt(d.rsi14, 1),
                                     nullptr, 0.0, biasLabel(bias), regime_label, &vts);
                return std::nullopt;
            }
        }

        ContractParams cp = buildContractParams(strategy, d.price, d.atr14, rfr, iv_sigma, d.hrv30, quality_score);
        if (cp.strike <= 0.0) {
            log("WARN", "[OPTIONS_SCAN] Could not determine valid strike for " + ticker);
            fireGeneratedSignal(ticker, strategy, "skipped_invalid_strike",
                                 "could not determine valid strike", nullptr, 0.0,
                                 biasLabel(bias), regime_label, &vts);
            return std::nullopt;
        }

        // Validate that the target expiry date actually has available contracts.
        // If not, snap to the nearest available expiry before firing the signal.
        // This prevents signals from advertising non-existent contract dates.
        // Pass use_macro_dte_override so the DTE floor check respects the override.
        std::string actual_expiry = validateAndSnapExpiryDate(ticker, cp.strike, cp.expiry_date,
                                                               use_macro_dte_override);
        if (actual_expiry.empty()) {
            log("WARN", "[OPTIONS_SCAN] " + ticker + " — no available contracts near " + cp.expiry_date);
            fireGeneratedSignal(ticker, strategy, "skipped_no_contracts_within_floor",
                                 "no listed expiration near target " + cp.expiry_date,
                                 nullptr, 0.0, biasLabel(bias), regime_label, &vts,
                                 use_macro_dte_override);
            return std::nullopt;
        }
        cp.expiry_date = actual_expiry;

        // Premium affordability gate disabled for live trading — let Kelly sizing
        // handle position sizing, not a separate premium check. Gate was too strict
        // in high-IV environments and blocked valid technical setups.
        // {
        //     double prem_estimate = cp.strike * iv_sigma * std::sqrt(cp.expiry) * 0.4;
        //     double prem_cost     = prem_estimate * 100.0; // one contract
        //     double budget        = computeMaxRisk(effective_capital, tier);
        //     if (prem_cost > budget * 0.08) {
        //         log("INFO", "[OPTIONS_SCAN][PREM_GATE] " + ticker +
        //             " skipped — est. premium $" + fmt(prem_cost, 0) +
        //             " > 8% of budget ($" + fmt(budget * 0.08, 0) + ").");
        //         return std::nullopt;
        //     }
        // }

        // Contract liquidity filter — verify the actual options chain before
        // committing to this signal. Fails open when Alpaca's data is unavailable.
        {
            bool is_call_strat = (strategy == "LONG_CALL" || strategy == "LEAP_CALL" ||
                                  strategy == "CC" || strategy == "BULL_CALL_SPREAD" ||
                                  strategy == "STRADDLE" || strategy == "STRANGLE" ||
                                  strategy == "REVERSE_IRON_CONDOR");
            std::string opt_type_str = is_call_strat ? "call" : "put";
            ContractLiquidity liq = checkContractLiquidity(ticker, cp.expiry_date,
                                                            cp.strike, opt_type_str);
            if (liq.valid && !liq.liquid) {
                log("INFO", "[OPTIONS_SCAN][LIQ_FILTER] " + ticker +
                    " SKIP — spread=" + fmt(liq.spread_pct * 100.0, 1) +
                    "% OI=" + std::to_string(liq.open_interest) +
                    " (need spread<15% and OI>50).");
                fireGeneratedSignal(ticker, strategy, "skipped_illiquid",
                                     "spread=" + fmt(liq.spread_pct * 100.0, 1) +
                                     "% OI=" + std::to_string(liq.open_interest),
                                     nullptr, 0.0, biasLabel(bias), regime_label, &vts,
                                     use_macro_dte_override);
                return std::nullopt;
            }
        }

        // WS1 — Contradiction Vector check. Query the data engine for this
        // ticker's latest sentiment-vs-skew verdict immediately before finalizing
        // the Kelly allocation. CONTRADICT_* verdict → halve the risk budget.
        // Strict timeout + fail-open so a dead intelligence feed never blocks
        // execution (per RULE-008).
        double ws1_mult      = fetchWS1KellyMultiplier(ticker);

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
        bool is_directional = (strategy == "LONG_CALL" || strategy == "LONG_PUT" ||
                               strategy == "LEAP_CALL" || strategy == "LEAP_PUT" ||
                               strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD" ||
                               strategy == "CSP" || strategy == "CC");
        nox::skeptic::Decision skeptic =
            fetchSkepticIntelligence(ticker, is_directional ? skeptic_dir : nox::skeptic::Dir::Neutral);
        if (skeptic.suppress) {
            fireGeneratedSignal(ticker, strategy, "suppressed_skeptic", skeptic.detail,
                                nullptr, quality_score, biasLabel(bias), regime_label, &vts,
                                use_macro_dte_override);
            if (signal_event_hook_)
                signal_event_hook_(ticker, strategy, "", quality_score,
                                   "suppressed_skeptic", skeptic.detail);
            return std::nullopt;
        }

        double kelly_capital = effective_capital * ws1_mult * skeptic.size_mult;

        OptionsSignal sig = assembleSignal(ticker, d, strategy, cp,
                                           iv_rank, iv_sigma, rfr, regime_clearance,
                                           tier, fc_mode, kelly_capital, d.hrv30, quality_score);

        if (sig.contracts <= 0) {
            fireGeneratedSignal(ticker, strategy, "aborted_zero_contracts",
                                "sizing floored to 0 — risk budget too small for one contract",
                                &sig, quality_score, biasLabel(bias), regime_label, &vts,
                                use_macro_dte_override);
            if (signal_event_hook_)
                signal_event_hook_(ticker, strategy, "", quality_score,
                                   "aborted_zero_contracts",
                                   "sizing floored to 0 — risk budget too small for one contract");
            return std::nullopt;
        }

        // Structural suppression (Phase 3.A): veto a poor-R:R (or, opt-in, a
        // lottery-delta) structure now that the signal is fully priced. Uses
        // only measured structure, not a fabricated win probability (RULE-D5).
        std::string structure_veto = structuralSuppressionReason(sig);
        if (!structure_veto.empty()) {
            fireGeneratedSignal(ticker, strategy, "suppressed_structure", structure_veto,
                                &sig, quality_score, biasLabel(bias), regime_label, &vts,
                                use_macro_dte_override);
            if (signal_event_hook_)
                signal_event_hook_(ticker, strategy, "", quality_score,
                                   "suppressed_structure", structure_veto);
            return std::nullopt;
        }

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

        ScoredSignal sc = scoreSignal(sig, d);
        sc.formatted_alert = formatAlert(sig, vix, regime, vts, sc.quality_score);
        return sc;
    }

    // ── Phase 1 helpers: order identity ───────────────────────────────────────
    //
    // signature — the dedup key: the same logical order always produces the same
    //   string, so the ledger's 60s blocker can spot an accidental retry.
    // client_oid — the broker client_order_id: signature + a 60-second time bucket.
    //   A network retry within the same minute reproduces the identical oid (Alpaca
    //   rejects the duplicate → belt-and-suspenders under the ledger blocker); a
    //   genuine re-entry in a later scan lands in a new bucket and gets a fresh oid.

    static std::string orderSide(const OptionsSignal& sig) {
        if (sig.strategy == "CSP" || sig.strategy == "CC") return "sell";
        if (sig.strategy == "LONG_CALL" || sig.strategy == "LONG_PUT") return "buy";
        return "multi"; // spreads/straddles/strangles
    }

    static std::string makeSignature(const OptionsSignal& sig) {
        std::ostringstream oss;
        oss << sig.underlying << "|" << sig.strategy << "|"
            << fmt(sig.strike, 2) << "|" << fmt(sig.strike2, 2) << "|"
            << fmt(sig.strike3, 2) << "|" << fmt(sig.strike4, 2) << "|"
            << sig.expiry_date << "|" << orderSide(sig);
        return oss.str();
    }

    static std::string biasLabel(DirectionalBias bias) {
        switch (bias) {
            case DirectionalBias::Bullish: return "bullish";
            case DirectionalBias::Bearish: return "bearish";
            default:                       return "neutral";
        }
    }

    // Fires generated_signal_hook_ (full-detail audit trail — every candidate,
    // not just what reached the broker) if the engine wired one. `sig`, when
    // supplied, backfills strikes/expiry/dte/iv_rank/hrv30/signature; call
    // sites earlier in evaluateTicker (before a signal is assembled) pass
    // nullptr and only ticker/strategy/outcome/reason are recorded.
    void fireGeneratedSignal(const std::string& ticker, const std::string& strategy,
                              const std::string& outcome, const std::string& reason,
                              const OptionsSignal* sig = nullptr,
                              double quality_score = 0.0,
                              const std::string& direction = "",
                              const std::string& regime_label = "",
                              const VixTermStructure* vts = nullptr,
                              bool macro_override_used = false,
                              bool earnings_checked = true) const {
        if (!generated_signal_hook_) return;
        GeneratedSignalInfo info;
        info.ticker              = ticker;
        info.strategy            = strategy;
        info.outcome             = outcome;
        info.reason              = reason;
        info.direction           = direction;
        info.macro_override_used = macro_override_used;
        info.earnings_checked    = earnings_checked;
        info.regime              = regime_label;
        if (vts && vts->valid) info.vix_term_label = vts->label;
        if (sig) {
            info.signature       = makeSignature(*sig);
            info.strike          = sig->strike;
            info.strike2         = sig->strike2;
            info.strike3         = sig->strike3;
            info.strike4         = sig->strike4;
            info.expiration_date = sig->expiry_date;
            if (!sig->expiry_date.empty()) {
                auto today_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                std::tm today_buf{};
                gmtime_r(&today_t, &today_buf);
                std::ostringstream today_oss;
                today_oss << std::put_time(&today_buf, "%Y-%m-%d");
                info.dte = static_cast<int>(parseDateToEpochDays(sig->expiry_date) -
                                             parseDateToEpochDays(today_oss.str()));
            }
            info.iv_rank = sig->iv_rank;
            info.hrv30   = sig->hrv30;
        }
        info.quality_score = quality_score;
        generated_signal_hook_(info);
    }

    static std::string makeClientOid(const OptionsSignal& sig) {
        long epoch = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        long bucket = epoch / 60;
        // Fold EVERY leg strike + expiry into the id, mirroring makeSignature's
        // full key. The old form used only the primary strike + a 60s bucket, so
        // two distinct multi-leg orders on the same underlying+strategy+primary
        // strike but differing in a wing strike or expiry, generated in the same
        // minute, collided: the ledger's INSERT OR IGNORE silently dropped the
        // second (never tracked/reconciled) and Alpaca rejected the duplicate
        // client_order_id, killing a legitimate order. Strikes are quantised to
        // cents (×100) so a strike of 0.0 for an absent leg stays a stable "0".
        auto k = [](double v) { return static_cast<long>(std::llround(v * 100.0)); };
        std::ostringstream oss;
        oss << "nox-o-" << sig.underlying << "-" << sig.strategy << "-"
            << k(sig.strike) << "-" << k(sig.strike2) << "-" << k(sig.strike3)
            << "-" << k(sig.strike4) << "-" << sig.expiry_date << "-" << bucket;
        return oss.str();
    }

    // ── Live order execution via OptionsOrderRouter ───────────────────────────

    void executeSignal(const OptionsSignal& sig) const {
        if (sig.contracts <= 0) {
            log("WARN", "[OPTIONS_EXEC] " + sig.underlying + " / " + sig.strategy +
                " — refusing to execute a signal with contracts <= 0.");
            return;
        }
        nox::options_router::OptionsOrderRouter router(alpacaUrl_, apiKey_, apiSec_);

        // Covered calls require 100 shares per contract as collateral.
        // Without them Alpaca would receive a naked short call — uncapped downside.
        // Abort and notify rather than let an unintended naked position through.
        // This check is Alpaca-specific; when routing through an override
        // (e.g. IBKR) there is no equivalent position query yet, so CC stays
        // advisory-only on that venue rather than skipping the safety check
        // silently.
        if (sig.strategy == "CC") {
            if (order_execution_override_) {
                log("WARN", "[OPTIONS_EXEC] CC collateral check unavailable on this venue — "
                            "signal is advisory-only, not auto-executed.");
                return;
            }
            if (!router.validateCCPosition(sig.underlying, sig.contracts)) {
                std::string msg = "CC aborted — need " +
                    std::to_string(sig.contracts * 100) +
                    " shares of " + sig.underlying + ", none found.";
                log("WARN", "[OPTIONS_EXEC] " + msg);
                sendTelegram(
                    "⚠️ *CC ORDER SKIPPED — " + sig.underlying + "*\n"
                    "────────────────────────\n"
                    "Need " + std::to_string(sig.contracts * 100) +
                    " shares for a covered call. No position found.\n"
                    "_Advisory signal still valid — execute manually if you hold the shares._"
                );
                return;
            }
        }

        // STRANGLE is a naked short both directions (income thesis — see
        // selectStrategy's prefer_sell branch) — uncapped upside risk, large
        // downside risk to the put strike. Requires Alpaca options level 3
        // ("uncovered"). Abort with a clear message rather than let the
        // account discover the shortfall via an opaque 403 mid-execution, or
        // worse, silently route as a long strangle (the bug this guards).
        // This check is Alpaca-specific; no equivalent exists on an override
        // venue yet, so STRANGLE stays advisory-only there.
        if (sig.strategy == "STRANGLE") {
            if (order_execution_override_) {
                log("WARN", "[OPTIONS_EXEC] Naked-options approval check unavailable on this venue — "
                            "signal is advisory-only, not auto-executed.");
                return;
            }
            if (!router.validateNakedOptionsApproval()) {
                std::string msg = "STRANGLE aborted — account is not approved for naked/uncovered "
                                  "options (needs Alpaca options trading level 3).";
                log("WARN", "[OPTIONS_EXEC] " + msg);
                sendTelegram(
                    "⚠️ *STRANGLE ORDER SKIPPED — " + sig.underlying + "*\n"
                    "────────────────────────\n"
                    "Account is not approved for naked/uncovered options trading (level 3).\n"
                    "_Advisory signal still valid — this is a short strangle: sells both an "
                    "OTM call and an OTM put for a credit, uncapped risk both directions._"
                );
                return;
            }
        }

        const std::string signature  = makeSignature(sig);
        const std::string client_oid = makeClientOid(sig);

        // ── Pre-order gate (items 3 & 4): position-exists + 60s duplicate blocker.
        // The engine hook writes the 'pending' ledger row when it returns Allow.
        if (pre_order_hook_) {
            OrderGate gate = OrderGate::Allow;
            try {
                gate = pre_order_hook_(sig, sig.contracts, client_oid, signature);
            } catch (const std::exception& e) {
                log("WARN", std::string("[OPTIONS_EXEC] pre-order hook threw: ") + e.what());
                gate = OrderGate::BlockedError;
            }
            if (gate != OrderGate::Allow) {
                std::string why =
                    gate == OrderGate::BlockedDuplicate      ? "duplicate within 60s (network retry, not a new signal)" :
                    gate == OrderGate::BlockedPositionExists ? "position already open for this contract" :
                    gate == OrderGate::BlockedRiskCap        ? "portfolio Greeks/notional cap breached — new entries paused" :
                    gate == OrderGate::BlockedKillSwitch     ? "global kill switch active — all new entries halted" :
                                                               "pre-order safety check error";
                log("INFO", "[OPTIONS_EXEC][GATE] " + sig.underlying + " / " +
                    sig.strategy + " skipped — " + why);
                return; // signal-driven philosophy: next scan re-fires if still valid
            }
        }

        auto result = order_execution_override_
            ? order_execution_override_(sig, sig.contracts, client_oid)
            : router.route(sig, sig.contracts, client_oid);

        // ── Map the router disposition to a ledger status (items 1 & 2). A timeout
        // or unparseable 2xx becomes 'unknown' (NOT 'failed') — reconciliation, not
        // a guess, decides. This is what closes the ghost-fill hole.
        if (post_order_hook_) {
            std::string ledger_status;
            using D = nox::options_router::OrderDisposition;
            switch (result.disposition) {
                case D::Accepted:   ledger_status = "pending"; break; // accepted ≠ filled
                case D::Timeout:    ledger_status = "unknown"; break;
                case D::ParseError: ledger_status = "unknown"; break;
                case D::Rejected:   ledger_status = "failed";  break;
            }
            try {
                post_order_hook_(client_oid, ledger_status, result.order_id);
            } catch (const std::exception& e) {
                log("WARN", std::string("[OPTIONS_EXEC] post-order hook threw: ") + e.what());
            }
        }

        if (result.success) {
            log("INFO", "[OPTIONS_EXEC] ✅ Order placed — " + result.message);
            // Persist the fill so it's tracked for exits and shows up in reports.
            // Without this the position lives only at the broker and is never sold.
            if (execution_recorder_) {
                try {
                    execution_recorder_(sig, sig.contracts);
                } catch (const std::exception& e) {
                    std::string rec_err = "[OPTIONS_EXEC] ⚠️ execution_recorder failed: " + std::string(e.what());
                    log("WARN", rec_err);
                    sendTelegram(
                        "⚠️ *Position Recording Failed*\n"
                        "────────────────────────\n"
                        "Order was placed but position could not be recorded:\n"
                        "• *Ticker:* " + sig.underlying + "\n"
                        "• *Strategy:* " + sig.strategy + "\n"
                        "• *Order ID:* " + result.order_id + "\n"
                        "• *Error:* `" + e.what() + "`\n"
                        "The position won't be auto-exited. Verify in your broker."
                    );
                }
            }
            std::stringstream ss;
            ss << std::fixed << std::setprecision(2);
            ss << "✅ *OPTIONS ORDER PLACED*\n"
               << "────────────────────────\n"
               << "• *Ticker:* " << sig.underlying << "\n"
               << "• *Strategy:* " << sig.strategy << "\n"
               << "• *Contracts:* " << sig.contracts << "\n"
               << "• *Strikes:* ";
            if (sig.strike2 > 0) {
                ss << "$" << sig.strike << " / $" << sig.strike2 << " (spread)\n";
            } else {
                ss << "$" << sig.strike << "\n";
            }
            ss << "• *Expiry:* " << sig.expiry_date << "\n"
               << "• *Entry Price:* $" << sig.entry_price << "\n"
               << "• *Max Risk:* " << (sig.max_risk > 999990.0
                       ? std::string("Undefined (naked)") : "$" + fmt(sig.max_risk, 0)) << "\n"
               << "• *Max Reward:* $" << sig.max_reward << "\n"
               << "• *Order ID:* `" << result.order_id << "`";
            sendTelegram(ss.str());
        } else {
            log("WARN", "[OPTIONS_EXEC] ❌ Order FAILED — " + result.message);

            std::stringstream ss;
            ss << std::fixed << std::setprecision(2);
            ss << "🚨 *OPTIONS ORDER FAILED*\n"
               << "────────────────────────\n"
               << "• *Profile:* " << profile_.name << "\n"
               << "• *Ticker:* " << sig.underlying << "\n"
               << "• *Strategy:* " << sig.strategy << "\n"
               << "• *Contracts:* " << sig.contracts << "\n"
               << "• *Strikes:* ";
            if (sig.strike2 > 0) {
                ss << "$" << sig.strike << " / $" << sig.strike2 << " (spread)\n";
            } else {
                ss << "$" << sig.strike << "\n";
            }
            ss << "• *Expiry:* " << sig.expiry_date << "\n"
               << "• *Entry Price:* $" << sig.entry_price << "\n"
               << "• *Max Risk:* " << (sig.max_risk > 999990.0
                       ? std::string("Undefined (naked)") : "$" + fmt(sig.max_risk, 0)) << "\n"
               << "• *Error:*\n`" << result.message << "`";
            sendTelegram(ss.str());
        }
    }
};

} // namespace nox::options_signal
