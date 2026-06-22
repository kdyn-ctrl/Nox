#include <iostream>
#include <cmath>
#include <cassert>
#include <string>

// Mock logger for the test
void logMessage(const std::string& level, const std::string& message) {
    std::cerr << "[" << level << "] " << message << std::endl;
}

// Simplified Kelly calculation function for the purpose of this test
bool calculate_kelly_share_size(double portfolio_value, double stock_price, double kelly_multiplier, int& quantity) {
    if (stock_price <= 0) {
        logMessage("CRITICAL", "Stock price is non-positive.");
        quantity = 0;
        return false;
    }

    // This is the core logic check requested.
    // If we cannot even afford a single share, we should not proceed.
    if (portfolio_value < stock_price) {
        logMessage("CRITICAL", "Insufficient portfolio value to purchase a single share.");
        quantity = 0;
        return false;
    }

    // A simplified position sizing formula for demonstration.
    // A full Kelly criterion calculation would involve win probability and win/loss ratio.
    double desired_position_value = portfolio_value * kelly_multiplier;
    double fractional_shares = desired_position_value / stock_price;

    // We can only trade whole shares.
    quantity = static_cast<int>(std::floor(fractional_shares));

    // Ensure we don't have a negative quantity from erroneous inputs.
    if (quantity < 0) {
        quantity = 0;
    }
    
    // The function is successful if it results in a non-zero quantity of shares to trade.
    return quantity > 0;
}

int main() {
    // Setup the test case scenario
    double mock_portfolio_value = 10.0;
    double mock_target_stock_price = 500.0;
    double standard_kelly_multiplier = 1.0; 
    int resulting_share_quantity = -1; // Default to a sentinel value

    // Execute the function under test
    bool success = calculate_kelly_share_size(
        mock_portfolio_value, 
        mock_target_stock_price, 
        standard_kelly_multiplier, 
        resulting_share_quantity
    );

    // Assert the expected outcomes
    // 1. The function should return false, indicating a trade should not be placed.
    assert(!success);

    // 2. The resulting share quantity should be exactly 0.
    assert(resulting_share_quantity == 0);

    // If assertions pass, the test is successful.
    // The [CRITICAL] log is expected to be printed to stderr during execution.
    std::cout << "Test passed successfully." << std::endl;

    return 0;
}
