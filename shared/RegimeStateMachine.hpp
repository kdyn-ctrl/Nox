#pragma once

#include <iostream>
#include <string>
#include <cmath>
#include <map>

// 1. Strictly define the possible market states
enum class Regime {
    RISK_ON,
    TRANSITION,
    RISK_OFF
};

// ---------------------------------------------------------------------------
// WS4 — Information Half-Life / Regime Decay
// ---------------------------------------------------------------------------
// Sentiment is perishable. A headline that moved the tape three days ago is
// near-worthless today, while an earnings surprise lingers longer. We model
// this with exponential decay:
//
//     score(t) = score_0 * exp(-lambda * dt_hours)
//
// lambda (the decay constant) is tunable PER signal category because different
// information types stale at different rates. lambda relates to the half-life H
// by lambda = ln(2) / H, so callers can configure intuitive half-lives in hours.
enum class SignalCategory {
    GEOPOLITICAL,    // breaking conflict / sanctions headlines — decays fast
    MACRO_ECONOMIC,  // Fed, CPI, employment — multi-day relevance
    EARNINGS,        // single-name fundamentals — persists longest
    TECHNICAL,       // chart-driven signals
    GENERIC          // fallback bucket
};

class HalfLifeDecay {
public:
    HalfLifeDecay() {
        // Default half-lives (hours) → lambda = ln2 / H.
        set_half_life_hours(SignalCategory::GEOPOLITICAL,   6.0);
        set_half_life_hours(SignalCategory::MACRO_ECONOMIC, 48.0);
        set_half_life_hours(SignalCategory::EARNINGS,       72.0);
        set_half_life_hours(SignalCategory::TECHNICAL,      12.0);
        set_half_life_hours(SignalCategory::GENERIC,        24.0);
    }

    // Configure decay via an intuitive half-life in hours (lambda = ln2 / H).
    void set_half_life_hours(SignalCategory cat, double half_life_hours) {
        if (half_life_hours > 0.0) lambda_[cat] = ln2_ / half_life_hours;
    }

    // Direct lambda setter (per hour), for callers that prefer the raw constant.
    void set_lambda(SignalCategory cat, double lambda_per_hour) {
        if (lambda_per_hour >= 0.0) lambda_[cat] = lambda_per_hour;
    }

    // score_0 : original sentiment magnitude at emission time.
    // dt_hours: hours elapsed since emission (negative/zero → no decay).
    // When bypassed (backtest .env override) the raw score passes through.
    double decayed_score(SignalCategory cat, double score_0, double dt_hours) const {
        if (bypass_)        return score_0;
        if (dt_hours <= 0.0) return score_0;
        auto it = lambda_.find(cat);
        double lambda = (it != lambda_.end()) ? it->second : (ln2_ / 24.0);
        return score_0 * std::exp(-lambda * dt_hours);
    }

    // Backtest bypass — wired to HALFLIFE_DECAY_BYPASS in .env.
    void set_bypass(bool b) { bypass_ = b; }
    bool is_bypassed() const { return bypass_; }

private:
    static constexpr double ln2_ = 0.6931471805599453;
    std::map<SignalCategory, double> lambda_;
    bool bypass_ = false;
};

// 2. Define what the Risk Agent receives based on the state
struct AllocationStrategy {
    Regime current_regime;
    double capital_multiplier;       // 1.0 = Full $65, 0.5 = $32.50, 0.0 = $0
    double stop_loss_atr_multiplier; // How wide the stop loss should be
    std::string log_message;         // For your Telegram heartbeat notification
};

// 3. The State Machine Class
class RegimeStateMachine {
private:
    // Default to cautious on startup until data proves otherwise
    Regime current_state = Regime::TRANSITION;

    // WS4 — regime-reset state.
    // A major macro catalyst (Fed pivot, war escalation, vol shock) invalidates
    // all prior macro NLP sentiment: the world before the catalyst no longer
    // describes the world after it. We force a cautious TRANSITION and raise a
    // latch the caller consumes to zero its accumulated sentiment weights.
    bool   macro_reset_pending_ = false;
    double prev_vix_            = -1.0;   // last VIX seen, for spike detection
    bool   reset_bypass_        = false;  // wired to REGIME_RESET_BYPASS in .env

public:
    // Backtest override — disables automatic catalyst-driven resets.
    void set_reset_bypass(bool b) { reset_bypass_ = b; }

    // Explicitly emit a regime_reset event (e.g. NLP detects a Fed pivot or
    // war escalation in the headline stream). Forces TRANSITION and latches.
    void trigger_regime_reset(const std::string& reason) {
        if (reset_bypass_) return;
        current_state       = Regime::TRANSITION;
        macro_reset_pending_ = true;
        std::cout << "[REGIME] regime_reset triggered: " << reason
                  << " — zeroing prior macro NLP weights." << std::endl;
    }

    // Heuristic catalyst detector: a sudden VIX jump between cycles is a
    // model-free proxy for a macro shock. Returns true if a reset fired.
    // vix_jump_threshold is supplied by the caller (CATALYST_VIX_JUMP in .env).
    bool detect_volatility_catalyst(double current_vix, double vix_jump_threshold) {
        bool fired = false;
        if (prev_vix_ >= 0.0 && (current_vix - prev_vix_) >= vix_jump_threshold) {
            trigger_regime_reset("VIX spike +" +
                std::to_string(current_vix - prev_vix_) + " breached catalyst threshold");
            fired = macro_reset_pending_;  // false if reset is bypassed
        }
        prev_vix_ = current_vix;
        return fired;
    }

    // Check-and-clear the reset latch. True at most once per reset event,
    // so the caller zeros its sentiment weights exactly once.
    bool consume_regime_reset() {
        bool was = macro_reset_pending_;
        macro_reset_pending_ = false;
        return was;
    }

    bool regime_reset_pending() const { return macro_reset_pending_; }


    // The main function your Analyst Agent calls every morning.
    // Hardcoded defaults must match the walk-forward OOS winning parameters.
    // Update these when walk_forward.sh produces a new validated winner.
    AllocationStrategy evaluate(double current_vix, double spy_price, double spy_200_sma) {
        return evaluate(current_vix, spy_price, spy_200_sma, 35.0, 0.98);
    }

    // Overloaded evaluate function for the backtester to use configurable parameters.
    AllocationStrategy evaluate(double current_vix, double spy_price, double spy_200_sma,
                                double vix_threshold, double sma_buffer_pct) {

        // Rule 1: The Panic Valve (Risk-Off)
        // If VIX is spiking over the threshold, OR the market has broken heavily below the 200 SMA
        if (current_vix >= vix_threshold || spy_price < (spy_200_sma * sma_buffer_pct)) {
            current_state = Regime::RISK_OFF;
        } else if (spy_price > spy_200_sma) {
            // VIX is not re-checked here — the RISK_OFF gate above handles elevated VIX.
            // Requiring VIX < 20 here would create a dead zone (VIX 20–threshold)
            // where clear uptrends stay TRANSITION and live diverges from backtest.
            current_state = Regime::RISK_ON;
        } else {
            // Price is between SMA*buffer and SMA — at the trendline.
            current_state = Regime::TRANSITION;
        }

        return get_strategy(current_state);
    }

private:
    AllocationStrategy get_strategy(Regime state) {
        switch (state) {
            case Regime::RISK_ON:
                return {state, 1.0, 2.0, "STATUS: RISK-ON. Volatility low. Deploying full capital."};
            case Regime::TRANSITION:
                return {state, 0.5, 1.5, "STATUS: TRANSITION. Geopolitical chop detected. Capital cut by 50%."};
            case Regime::RISK_OFF:
                return {state, 0.0, 1.0, "STATUS: RISK-OFF. Extreme volatility. Halting new entries."};
        }

        return {Regime::TRANSITION, 0.0, 1.0, "ERROR: State resolution failed. Defaulting to safe mode."};
    }
};
