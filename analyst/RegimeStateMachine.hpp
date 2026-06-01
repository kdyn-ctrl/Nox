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
    std::string log_message;         // For your Discord heartbeat webhook
};

// 3. The State Machine Class
class RegimeStateMachine {
private:
    // Default to cautious on startup until data proves otherwise
    Regime current_state = Regime::TRANSITION; 

public:
    // The main function your Analyst Agent calls every morning
    AllocationStrategy evaluate(double current_vix, double spy_price, double spy_200_sma) {
        
        // Rule 1: The Panic Valve (Risk-Off)
        // If VIX is spiking over 30, OR the market has broken heavily below the 200 SMA (2% buffer)
        if (current_vix >= 30.0 || spy_price < (spy_200_sma * 0.98)) {
            current_state = Regime::RISK_OFF;
        } 
        // Rule 2: The Green Light (Risk-On)
        // Volatility is low AND the broader market is trending up
        else if (current_vix < 20.0 && spy_price > spy_200_sma) {
            current_state = Regime::RISK_ON;
        } 
        // Rule 3: The Choppy Middle (Transition)
        // Geopolitical uncertainty, VIX between 20-30, or price chopping around the SMA
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
                // Choppy conditions: Deploy half capital, tighten stops to 1.5x ATR
                return {state, 0.5, 1.5, "STATUS: TRANSITION. Geopolitical chop detected. Capital cut by 50%."};
            
            case Regime::RISK_OFF:
                // Bear market/Crash: Halt buys, accumulate cash reserves, tight 1.0x ATR trailing stop
                return {state, 0.0, 1.0, "STATUS: RISK-OFF. Extreme volatility. Halting new entries."};
        }
        
        // Failsafe catch
        return {Regime::TRANSITION, 0.0, 1.0, "ERROR: State resolution failed. Defaulting to safe mode."};
    }
};
