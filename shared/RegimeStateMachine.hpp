#pragma once

#include <iostream>
#include <string>

// 1. Strictly define the possible market states
enum class Regime {
    RISK_ON,
    TRANSITION,
    RISK_OFF
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

public:
    // The main function your Analyst Agent calls every morning.
    // Defaults below are illustrative placeholders — production thresholds are
    // derived from private walk-forward validation, not committed to source.
    AllocationStrategy evaluate(double current_vix, double spy_price, double spy_200_sma) {
        return evaluate(current_vix, spy_price, spy_200_sma, 30.0, 0.95);
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
