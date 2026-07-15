#ifndef BACKTEST_ERROR_MODEL_HPP
#define BACKTEST_ERROR_MODEL_HPP

// BacktestErrorModel — "does the strategy still profit through operational
// errors?" This is the explicit ask: not "is the alpha real on a clean run"
// but "when the bot is WRONG about the world — a ghost fill it never
// reconciled, an exit that never fired, a fill far worse than the mark — does
// the edge survive the drawdown those errors inflict?"
//
// The three error modes mirror the real failure surface Phase 1-4 hardened
// against, applied here as their UN-defended counterfactual so the backtest
// can price the damage if a defense ever silently lapses:
//   • ghost_fill   — an order the bot believed failed actually filled at the
//                    broker and went un-reconciled → a duplicate, unmanaged
//                    position. Modeled as DOUBLING that trade's P&L (the
//                    unintended second lot rides to the same exit). Doubles
//                    losers as readily as winners — that's the whole risk.
//   • missed_exit  — the 50%/stop exit never fired (monitor outage, quote
//                    gap) → the position rode to expiry instead. Modeled by
//                    swapping in pnl_if_held_to_expiry, the honest
//                    counterfactual the simulator recorded (NOT a guessed
//                    give-back). Turns locked-in winners into whatever expiry
//                    actually was.
//   • adverse_fill — entry or exit filled materially worse than the mark
//                    (slippage spike, wide spread crossed) → a fixed haircut,
//                    as a fraction of entry premium, subtracted from P&L.
//
// PURE + deterministic: seeded std::mt19937 so a given (config, trade set)
// always produces the same injected outcome — a backtest that changes verdict
// run-to-run is useless. No I/O; unit-tested in test/test_backtest_error_injection.cpp.

#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace nox::backtest {

struct ErrorConfig {
    // Per-trade independent probabilities (0..1). All default 0 → a no-op pass
    // that returns the baseline unchanged, so the model is inert unless asked.
    double ghost_fill_rate      = 0.0;
    double missed_exit_rate     = 0.0;
    double adverse_fill_rate    = 0.0;
    // Slippage haircut on an adverse fill, as a fraction of entry premium.
    double adverse_slippage_pct = 0.15;
    std::uint32_t seed          = 1u; // 0 is a valid, reproducible seed too

    bool active() const {
        return ghost_fill_rate > 0.0 || missed_exit_rate > 0.0 || adverse_fill_rate > 0.0;
    }

    static double envd(const char* n, double d) {
        if (const char* v = std::getenv(n)) { try { return std::stod(v); } catch (...) {} }
        return d;
    }
    static ErrorConfig fromEnv() {
        ErrorConfig c;
        c.ghost_fill_rate      = envd("BT_ERR_GHOST_FILL_RATE",   c.ghost_fill_rate);
        c.missed_exit_rate     = envd("BT_ERR_MISSED_EXIT_RATE",  c.missed_exit_rate);
        c.adverse_fill_rate    = envd("BT_ERR_ADVERSE_FILL_RATE", c.adverse_fill_rate);
        c.adverse_slippage_pct = envd("BT_ERR_ADVERSE_SLIP_PCT",  c.adverse_slippage_pct);
        if (const char* v = std::getenv("BT_ERR_SEED")) {
            try { c.seed = static_cast<std::uint32_t>(std::stoul(v)); } catch (...) {}
        }
        return c;
    }
};

// Minimal view of a Trade the model needs — decouples the header from
// backtest_main.cpp's full Trade struct so it is trivially testable.
struct TradeView {
    double      entry_price            = 0.0;
    double      pnl                    = 0.0;
    double      pnl_if_held_to_expiry  = 0.0;
    std::string exit_reason;                     // "PROFIT_TARGET"/"STOP_LOSS"/"EXPIRY"
};

// Tags describing what was injected into a single trade (for the report).
struct TradeErrorTags {
    bool ghost_fill  = false;
    bool missed_exit = false;
    bool adverse_fill = false;
    double pnl_before = 0.0;
    double pnl_after  = 0.0;
};

struct ErrorImpact {
    int    ghost_fills   = 0;
    int    missed_exits  = 0;
    int    adverse_fills = 0;
    std::vector<double> injected_pnls;      // per-trade P&L after injection
    std::vector<TradeErrorTags> tags;       // parallel to the input trades
};

// Apply the error model to a trade set, returning the injected P&L series +
// impact tally. Order of application per trade:
//   1. missed_exit  — swap in the held-to-expiry counterfactual (only meaningful
//                     when the trade actually exited early; an EXPIRY trade is
//                     unchanged since it already ran to expiry).
//   2. adverse_fill — subtract the slippage haircut.
//   3. ghost_fill   — double the (already-perturbed) P&L for the duplicate lot.
// This ordering composes the way the real failures would stack: the exit is
// decided first, the fill quality next, and a duplicated position scales
// whatever P&L that lot ended with.
inline ErrorImpact applyErrors(const std::vector<TradeView>& trades, const ErrorConfig& cfg) {
    ErrorImpact out;
    out.injected_pnls.reserve(trades.size());
    out.tags.reserve(trades.size());

    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);

    for (const auto& t : trades) {
        TradeErrorTags tag;
        tag.pnl_before = t.pnl;
        double pnl = t.pnl;

        // Draw all three rolls unconditionally so the RNG stream stays aligned
        // regardless of which branches a given trade takes (reproducibility).
        double roll_missed  = u(rng);
        double roll_adverse = u(rng);
        double roll_ghost   = u(rng);

        if (cfg.missed_exit_rate > 0.0 && roll_missed < cfg.missed_exit_rate &&
            t.exit_reason != "EXPIRY") {
            pnl = t.pnl_if_held_to_expiry;
            tag.missed_exit = true;
            ++out.missed_exits;
        }

        if (cfg.adverse_fill_rate > 0.0 && roll_adverse < cfg.adverse_fill_rate) {
            pnl -= cfg.adverse_slippage_pct * t.entry_price;
            tag.adverse_fill = true;
            ++out.adverse_fills;
        }

        if (cfg.ghost_fill_rate > 0.0 && roll_ghost < cfg.ghost_fill_rate) {
            pnl *= 2.0; // duplicate unmanaged lot rides to the same outcome
            tag.ghost_fill = true;
            ++out.ghost_fills;
        }

        tag.pnl_after = pnl;
        out.injected_pnls.push_back(pnl);
        out.tags.push_back(tag);
    }
    return out;
}

// Convenience roll-up used by the report + tests: totals and the survival
// verdict. `retention` is injected-total / baseline-total (guarded for a
// zero/negative baseline). still_profitable = injected total P&L > 0.
struct ErrorSummary {
    double baseline_total = 0.0;
    double injected_total = 0.0;
    double retention      = 0.0;   // fraction of baseline P&L retained
    bool   still_profitable = false;
    double max_drawdown_baseline = 0.0; // negative or zero
    double max_drawdown_injected = 0.0;
};

inline double maxDrawdown(const std::vector<double>& pnls) {
    double running = 0.0, peak = 0.0, max_dd = 0.0;
    for (double p : pnls) {
        running += p;
        if (running > peak) peak = running;
        if (running - peak < max_dd) max_dd = running - peak;
    }
    return max_dd;
}

inline ErrorSummary summarize(const std::vector<TradeView>& trades, const ErrorImpact& impact) {
    ErrorSummary s;
    std::vector<double> base;
    base.reserve(trades.size());
    for (const auto& t : trades) { s.baseline_total += t.pnl; base.push_back(t.pnl); }
    for (double p : impact.injected_pnls) s.injected_total += p;

    if (s.baseline_total > 1e-9)
        s.retention = s.injected_total / s.baseline_total;
    else
        s.retention = (s.injected_total >= 0.0) ? 1.0 : 0.0;

    s.still_profitable        = s.injected_total > 0.0;
    s.max_drawdown_baseline   = maxDrawdown(base);
    s.max_drawdown_injected   = maxDrawdown(impact.injected_pnls);
    return s;
}

} // namespace nox::backtest

#endif // BACKTEST_ERROR_MODEL_HPP
