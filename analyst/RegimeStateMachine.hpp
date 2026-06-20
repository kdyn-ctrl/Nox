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
    // Hardcoded defaults must match the walk-forward OOS winning parameters.
    // Update these when walk_forward.sh produces a new validated winner.
    AllocationStrategy evaluate(double current_vix, double spy_price, double spy_200_sma) {
        return evaluate(current_vix, spy_price, spy_200_sma, 35.0, 0.98);
    }

    // Overloaded evaluate function for the backtester to use configurable parameters.
    AllocationStrategy evaluate(double current_vix, double spy_price, double spy_200_sma, double vix_threshold, double sma_buffer_pct) {

        // Rule 1: The Panic Valve (Risk-Off)
        // If VIX is spiking over the threshold, OR the market has broken heavily below the 200 SMA
        if (current_vix >= vix_threshold || spy_price < (spy_200_sma * sma_buffer_pct)) {
            current_state = Regime::RISK_OFF;
        } 
        // Rule 2: The Green Light (Risk-On)
        // RISK_ON: market is trending above the 200 SMA.
        // VIX is NOT checked here — the RISK_OFF gate above already handles
        // elevated VIX. Checking VIX again here would create a dead zone
        // (VIX 20–threshold) where the regime is always TRANSITION even in
        // clear uptrends, causing the backtester and live system to diverge.
        else if (spy_price > spy_200_sma) {
            current_state = Regime::RISK_ON;
        } 
        // Rule 3: The Choppy Middle (Transition)
        // Price is between SMA*buffer and SMA — market is right at the trendline.
        else {
            current_state = Regime::TRANSITION;
        }

        return get_strategy(current_state);
    }

private:
    // This maps the state to your actual risk management math
    AllocationStrategy get_strategy(Regime state) {
        switch (state) {
            case Regime::RISK_ON:
                // Normal conditions: 2x ATR stop loss
                return {state, 1.0, 2.0, "STATUS: RISK-ON. Volatility low. Deploying full capital."};
            
            case Regime::TRANSITION:
                // Choppy conditions: Halt buys, tighten stops to 1.5x ATR
                return {state, 0.0, 1.5, "STATUS: TRANSITION. Geopolitical chop detected. Halting new entries."};
            
            case Regime::RISK_OFF:
                // Bear market/Crash: Halt buys, accumulate cash reserves, tight 1.0x ATR trailing stop
                return {state, 0.0, 1.0, "STATUS: RISK-OFF. Extreme volatility. Halting new entries."};
        }
        
        // Failsafe catch
        return {Regime::TRANSITION, 0.0, 1.0, "ERROR: State resolution failed. Defaulting to safe mode."};
    }
};
