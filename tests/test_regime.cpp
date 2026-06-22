#include <iostream>
#include <cassert>
#include <string>
#include "../shared/RegimeStateMachine.hpp"

void assert_regime(Regime actual, Regime expected, const std::string& test_name) {
    if (actual != expected) {
        std::string actual_str = (actual == Regime::RISK_ON) ? "RISK_ON" :
                                 (actual == Regime::RISK_OFF) ? "RISK_OFF" : "TRANSITION";
        std::string expected_str = (expected == Regime::RISK_ON) ? "RISK_ON" :
                                   (expected == Regime::RISK_OFF) ? "RISK_OFF" : "TRANSITION";
        std::cerr << "Test failed: " << test_name << std::endl;
        std::cerr << "  Expected: " << expected_str << ", Got: " << actual_str << std::endl;
        assert(false);
    }
}

int main() {
    std::cout << "Running tests for RegimeStateMachine::evaluate()..." << std::endl;

    RegimeStateMachine rsm;

    // Test parameters: vix, spy_price, spy_200_sma
    // Rules: RISK_OFF if VIX >= 35 OR SPY < SMA*0.98
    //        RISK_ON if SPY > SMA (and VIX < 35)
    //        TRANSITION otherwise

    // Group 1: RISK_ON (VIX low, SPY above SMA)
    assert_regime(rsm.evaluate(15.0, 450.0, 440.0).current_regime, Regime::RISK_ON, "RISK_ON: Core Case");
    assert_regime(rsm.evaluate(15.0, 440.01, 440.0).current_regime, Regime::RISK_ON, "RISK_ON: Edge - SPY just above SMA");
    assert_regime(rsm.evaluate(34.99, 450.0, 440.0).current_regime, Regime::RISK_ON, "RISK_ON: Edge - VIX just below threshold");

    // Group 2: RISK_OFF (VIX high OR SPY below SMA buffer)
    assert_regime(rsm.evaluate(35.0, 430.0, 440.0).current_regime, Regime::RISK_OFF, "RISK_OFF: Core Case");
    assert_regime(rsm.evaluate(15.0, 430.0, 440.0).current_regime, Regime::RISK_OFF, "RISK_OFF: SPY below SMA*0.98");
    assert_regime(rsm.evaluate(35.0, 450.0, 440.0).current_regime, Regime::RISK_OFF, "RISK_OFF: VIX at threshold");

    // Group 3: TRANSITION (SPY at or below SMA but above SMA*0.98, VIX low)
    assert_regime(rsm.evaluate(15.0, 440.0, 440.0).current_regime, Regime::TRANSITION, "TRANSITION: SPY equals SMA");
    assert_regime(rsm.evaluate(15.0, 439.99, 440.0).current_regime, Regime::TRANSITION, "TRANSITION: SPY between SMA and SMA*0.98");

    // Group 4: EXTREME CASES
    assert_regime(rsm.evaluate(0.0, 0.0, 0.0).current_regime, Regime::TRANSITION, "EXTREME: Zero Inputs - price == SMA");
    assert_regime(rsm.evaluate(1e3, 1e8, 1e9).current_regime, Regime::RISK_OFF, "EXTREME: Large Numbers - VIX very high");
    assert_regime(rsm.evaluate(999.0, 500.0, 510.0).current_regime, Regime::RISK_OFF, "EXTREME: Very High VIX triggers RISK_OFF");
    assert_regime(rsm.evaluate(1.0, 500.0, 490.0).current_regime, Regime::RISK_ON, "EXTREME: Very Low VIX, price above SMA");

    std::cout << "All tests passed!" << std::endl;

    return 0;
}
