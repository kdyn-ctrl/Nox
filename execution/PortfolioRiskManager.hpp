#pragma once

// Phase 4, item 2: portfolio-level circuit breaker.
//
// Pure aggregation/decision logic — no I/O, no sqlite, no network. Callers
// (PositionManager.cpp) are responsible for fetching live underlying prices,
// solving live Greeks per position via OptionEngine.hpp, and building the
// PositionGreekContribution vector this header consumes. Kept dependency-free
// so it can be unit-tested against synthetic contributions the same way
// AlphaDecayStore/IvRankStore separate math from I/O.
//
// Scope (per explicit direction): only options carry Greeks in this codebase
// (equity delta is trivially 1/share and isn't modeled here). Breach handling
// is: block new orders while breached (via the options pre-order gate), and
// force-close ONLY the single position whose own contribution is largest in
// the breaching direction — a position that's within its share of target
// stays untouched and keeps running through the normal 50%/stop/21-DTE rules.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace nox::risk {

// One position's contribution to portfolio-level exposure, already sized by
// quantity/100-share-multiplier and sign-adjusted for short vs long.
struct PositionGreekContribution {
    long        position_id;
    std::string ticker;
    double      delta_contribution;   // underlying-share-equivalents
    double      vega_contribution;    // $ per 1% vol move
    double      notional;             // |underlying * qty * 100|, always >= 0
};

struct PortfolioGreeks {
    double net_delta        = 0.0;
    double net_vega         = 0.0;
    double options_notional = 0.0;
};

struct RiskTargets {
    double max_abs_delta;
    double max_abs_vega;
    double max_options_notional;
    double max_equity_notional;

    // env-tunable, with fake-safe defaults (see CLAUDE.md's
    // "hardcode nothing tunable" rule) — a bot running with no env
    // configuration still gets a sane, conservative cap rather than an
    // unbounded one.
    static RiskTargets fromEnv() {
        RiskTargets t;
        t.max_abs_delta        = 500.0;     // MAX_PORTFOLIO_DELTA (share-equivalents)
        t.max_abs_vega         = 2000.0;     // MAX_PORTFOLIO_VEGA ($ per 1% vol move)
        t.max_options_notional = 100000.0;   // MAX_OPTIONS_NOTIONAL ($)
        t.max_equity_notional  = 200000.0;   // MAX_EQUITY_NOTIONAL ($)

        auto readPositiveEnv = [](const char* name, double& target) {
            if (const char* v = std::getenv(name)) {
                try {
                    double parsed = std::stod(v);
                    if (parsed > 0.0) target = parsed;
                } catch (...) {}
            }
        };
        readPositiveEnv("MAX_PORTFOLIO_DELTA", t.max_abs_delta);
        readPositiveEnv("MAX_PORTFOLIO_VEGA", t.max_abs_vega);
        readPositiveEnv("MAX_OPTIONS_NOTIONAL", t.max_options_notional);
        readPositiveEnv("MAX_EQUITY_NOTIONAL", t.max_equity_notional);
        return t;
    }
};

struct RiskBreach {
    bool        breached = false;
    std::string reason;
    long        position_to_close = -1; // -1: no single position identified
};

inline PortfolioGreeks aggregate(const std::vector<PositionGreekContribution>& positions) {
    PortfolioGreeks g;
    for (const auto& p : positions) {
        g.net_delta        += p.delta_contribution;
        g.net_vega         += p.vega_contribution;
        g.options_notional += p.notional;
    }
    return g;
}

// Among `positions`, finds the one with the largest contribution in the
// given metric, restricted to the breaching direction for signed metrics
// (delta/vega) or largest magnitude for an unsigned one (notional).
// Returns -1 if `positions` is empty.
inline long largestContributor(const std::vector<PositionGreekContribution>& positions,
                               double (*metric)(const PositionGreekContribution&),
                               double sign) {
    long   best_id  = -1;
    double best_val = 0.0;
    for (const auto& p : positions) {
        double v = metric(p) * sign;
        if (best_id == -1 || v > best_val) {
            best_val = v;
            best_id  = p.position_id;
        }
    }
    return best_id;
}

inline RiskBreach evaluate(const std::vector<PositionGreekContribution>& positions,
                           const RiskTargets& targets) {
    RiskBreach breach;
    if (positions.empty()) return breach;

    PortfolioGreeks g = aggregate(positions);

    if (std::abs(g.net_delta) > targets.max_abs_delta) {
        breach.breached = true;
        breach.reason = "portfolio delta " + std::to_string(g.net_delta) +
                         " exceeds target " + std::to_string(targets.max_abs_delta);
        double sign = (g.net_delta > 0.0) ? 1.0 : -1.0;
        breach.position_to_close = largestContributor(
            positions, [](const PositionGreekContribution& p) { return p.delta_contribution; }, sign);
    } else if (std::abs(g.net_vega) > targets.max_abs_vega) {
        breach.breached = true;
        breach.reason = "portfolio vega " + std::to_string(g.net_vega) +
                         " exceeds target " + std::to_string(targets.max_abs_vega);
        double sign = (g.net_vega > 0.0) ? 1.0 : -1.0;
        breach.position_to_close = largestContributor(
            positions, [](const PositionGreekContribution& p) { return p.vega_contribution; }, sign);
    } else if (g.options_notional > targets.max_options_notional) {
        breach.breached = true;
        breach.reason = "options notional " + std::to_string(g.options_notional) +
                         " exceeds target " + std::to_string(targets.max_options_notional);
        breach.position_to_close = largestContributor(
            positions, [](const PositionGreekContribution& p) { return p.notional; }, 1.0);
    }
    return breach;
}

} // namespace nox::risk
