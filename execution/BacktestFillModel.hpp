#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Adverse-selection fill model for backtest_main.cpp.
// ─────────────────────────────────────────────────────────────────────────────
// backtest_main.cpp's naive simulation assumes every profit-target/stop-loss
// touch fills at 100%. In reality a resting limit order only fills for
// certain when price actually CROSSES it (aggressive flow ran through your
// level — the "loser-shaped" fill you can't avoid). When price only TOUCHES
// the level and reverses, you were sitting in a queue and only get filled
// with some probability tied to how much size traded through that level
// relative to the queue ahead of you. Always assuming the touch fills means
// the backtest "catches" every favorable reversal a real resting order would
// have missed, which systematically overstates win rate and P&L.
//
// This header is pure math — no network, no Black-Scholes dependency — so it
// is independently unit-testable (see execution/test/test_backtest_fill_model.cpp).
//
// Data-availability note: there is no real option-level bid/ask or volume
// anywhere in this codebase's data source (Yahoo Finance OHLCV, underlying
// only). day_liquidity_proxy/avg_liquidity_proxy are meant to be filled with
// underlying daily volume as an explicit approximation — NOT calibrated
// market depth. Callers must not silently pretend this is real option
// liquidity; see backtest_main.cpp's Methodology section for the disclosure.

#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>

namespace nox::backtest {

enum class FillMode { Naive, AdverseSelection };

struct FillModelConfig {
    FillMode mode       = FillMode::Naive; // NOX_BT_FILL_MODEL=adverse_selection
    double   gamma       = 0.2;             // NOX_BT_FILL_GAMMA
    double   queue_mult   = 3.0;             // NOX_BT_FILL_QUEUE_MULT
    unsigned rng_seed      = 0;              // NOX_BT_FILL_SEED (0 = time-seeded)

    static FillModelConfig fromEnv() {
        FillModelConfig cfg;
        if (const char* v = std::getenv("NOX_BT_FILL_MODEL")) {
            std::string s(v);
            if (s == "adverse_selection") cfg.mode = FillMode::AdverseSelection;
        }
        if (const char* v = std::getenv("NOX_BT_FILL_GAMMA")) {
            try { cfg.gamma = std::stod(v); } catch (...) {}
        }
        if (const char* v = std::getenv("NOX_BT_FILL_QUEUE_MULT")) {
            try { cfg.queue_mult = std::stod(v); } catch (...) {}
        }
        if (const char* v = std::getenv("NOX_BT_FILL_SEED")) {
            try { cfg.rng_seed = static_cast<unsigned>(std::stoul(v)); } catch (...) {}
        }
        return cfg;
    }
};

// A synthetic per-day range for whatever's being filled against (an option's
// modeled value band for a day, built by re-pricing at the underlying's
// high/low/close — see backtest_main.cpp's syntheticOptionBar()).
struct OptionBarRange {
    double low = 0.0, high = 0.0, close_value = 0.0;
};

struct FillOutcome {
    bool   filled  = false;
    bool   touched = false; // true if the bar's range reached the level at all
    bool   crossed = false; // true if the level was crossed, not just touched
    double p_fill  = 0.0;   // fill probability; 1.0 once touched+crossed/certain
};

// approached_from_above: the level is reached by the bar's value falling onto
// it (touch test: bar.low <= level; crossed: bar.close < level). false means
// the level is reached by the bar's value rising onto it (touch test:
// bar.high >= level; crossed: bar.close > level).
//
// force_certain: stop-loss orders are effectively stop-market once touched —
// always fill on touch regardless of mode/probability.
inline FillOutcome simulateFill(const OptionBarRange& bar, double limit_level,
                                 bool approached_from_above,
                                 double day_liquidity_proxy, double avg_liquidity_proxy,
                                 const FillModelConfig& cfg, std::mt19937& rng,
                                 bool force_certain = false) {
    FillOutcome out;

    bool touched = approached_from_above ? (bar.low <= limit_level)
                                          : (bar.high >= limit_level);
    if (!touched) return out;
    out.touched = true;

    bool crossed = approached_from_above ? (bar.close_value < limit_level)
                                          : (bar.close_value > limit_level);
    out.crossed = crossed;

    if (crossed || force_certain || cfg.mode == FillMode::Naive) {
        out.filled = true;
        out.p_fill = 1.0;
        return out;
    }

    // Touched-only, AdverseSelection mode: probabilistic fill scaled by how
    // much liquidity traded through this level relative to an estimated
    // queue ahead of us.
    double queue_estimate = cfg.queue_mult * avg_liquidity_proxy;
    double p = (queue_estimate > 1.0)
                   ? (day_liquidity_proxy * cfg.gamma) / queue_estimate
                   : 1.0;
    if (p > 1.0) p = 1.0;
    if (p < 0.0) p = 0.0;
    out.p_fill = p;

    std::uniform_real_distribution<double> draw(0.0, 1.0);
    out.filled = draw(rng) < p;
    return out;
}

inline double profitTargetLevel(double entry_price, double profit_target_pct, bool is_long) {
    double target_pnl = entry_price * profit_target_pct;
    return is_long ? (entry_price + target_pnl) : (entry_price - target_pnl);
}

inline double stopLossLevel(double entry_price, double stop_loss_mult, bool is_long) {
    double loss = entry_price * stop_loss_mult;
    if (is_long) {
        // A long option's value floors at 0 (worthless) — it can never lose
        // MORE than 100% of the premium paid, so a stop_loss_mult >= 1.0
        // asked for a level below zero, which is mathematically unreachable
        // (audit §3 C2). Clamp the loss distance to the entry price itself;
        // the stop then fires only once the position is fully wiped out,
        // which real time decay/expiry can actually reach.
        loss = std::min(loss, entry_price);
        return entry_price - loss;
    }
    // A short's risk is uncapped (naked premium sold can cost many multiples
    // of the credit collected to close) — no clamp needed here.
    return entry_price + loss;
}

// The option's modeled VALUE rises for a profit target on a long (bought
// premium gaining value) and falls for a short (sold premium cheapening) —
// so the level is approached from below for a long, from above for a short.
inline bool approachedFromAboveForTarget(bool is_long) { return !is_long; }

// Value falls for a losing long (premium decaying/against you) and rises for
// a losing short (premium getting more expensive to close) — level is
// approached from above for a long stop, from below for a short stop.
inline bool approachedFromAboveForStop(bool is_long) { return is_long; }

} // namespace nox::backtest
