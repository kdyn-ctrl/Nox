#include <iostream>
#include <cassert>
#include "../RegimeStateMachine.hpp"

// A simple helper for floating point comparison
void assert_equal(double actual, double expected, const std::string& test_name) {
    // Using a small epsilon for float comparison
    if (std::abs(actual - expected) > 1e-9) {
        std::cerr << "Test failed: " << test_name << std::endl;
        std::cerr << "  Expected: " << expected << ", Got: " << actual << std::endl;
        assert(false);
    }
}

int main() {
    std::cout << "Running tests for RegimeStateMachine::evaluate()..." << std::endl;

    RegimeStateMachine rsm;

    // Group 1: RISK_ON (1.0)
    assert_equal(rsm.evaluate(450.0, 440.0, 15.0), 1.0, "RISK_ON: Core Case");
    assert_equal(rsm.evaluate(440.01, 440.0, 15.0), 1.0, "RISK_ON: Edge - SPY just above SMA");
    assert_equal(rsm.evaluate(450.0, 440.0, 19.9), 1.0, "RISK_ON: Edge - VIX just below threshold");

    // Group 2: RISK_OFF (0.0)
    assert_equal(rsm.evaluate(430.0, 440.0, 35.0), 0.0, "RISK_OFF: Core Case");
    assert_equal(rsm.evaluate(439.99, 440.0, 35.0), 0.0, "RISK_OFF: Edge - SPY just below SMA");
    assert_equal(rsm.evaluate(430.0, 440.0, 30.1), 0.0, "RISK_OFF: Edge - VIX just above threshold");

    // Group 3: TRANSITION (0.5)
    assert_equal(rsm.evaluate(450.0, 440.0, 35.0), 0.5, "TRANSITION: Conflict - Bullish SPY, High VIX");
    assert_equal(rsm.evaluate(430.0, 440.0, 15.0), 0.5, "TRANSITION: Conflict - Bearish SPY, Low VIX");
    assert_equal(rsm.evaluate(440.0, 440.0, 25.0), 0.5, "TRANSITION: Boundary - SPY equals SMA");
    assert_equal(rsm.evaluate(450.0, 440.0, 20.0), 0.5, "TRANSITION: Boundary - VIX at lower threshold");
    assert_equal(rsm.evaluate(430.0, 440.0, 30.0), 0.5, "TRANSITION: Boundary - VIX at upper threshold");
    
    // Group 4: EXTREME CASES
    assert_equal(rsm.evaluate(0.0, 0.0, 0.0), 0.5, "EXTREME: Zero Inputs");
    assert_equal(rsm.evaluate(1e9, 1e8, 1e3), 0.5, "EXTREME: Large Numbers");
    assert_equal(rsm.evaluate(500, 510, 999), 0.0, "EXTREME: Very High VIX");
    assert_equal(rsm.evaluate(500, 490, 1), 1.0, "EXTREME: Very Low VIX");


    std::cout << "All tests passed!" << std::endl;

    return 0;
}
