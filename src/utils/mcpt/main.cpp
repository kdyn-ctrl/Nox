#include "mcpt.h"
#include <iostream>
#include <vector>
#include <numeric>
#include <cmath>

// Helper function to calculate the mean of a vector
double calculate_mean(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

// Helper function to calculate the variance of a vector
double calculate_variance(const std::vector<double>& v, double mean) {
    double sum = 0.0;
    for (double x : v) {
        sum += (x - mean) * (x - mean);
    }
    return sum / (v.size() - 1);
}

int main() {
    // Example usage
    std::vector<double> historical_returns = {0.01, -0.02, 0.03, -0.01, 0.005, -0.002, 0.015, -0.01, 0.02, -0.03};

    double original_mean = calculate_mean(historical_returns);
    double original_variance = calculate_variance(historical_returns, original_mean);

    std::cout << "Original Mean: " << original_mean << std::endl;
    std::cout << "Original Variance: " << original_variance << std::endl;

    std::vector<double> permuted_returns = mcpt::permute_returns(historical_returns);

    double permuted_mean = calculate_mean(permuted_returns);
    double permuted_variance = calculate_variance(permuted_returns, permuted_mean);

    std::cout << "Permuted Mean: " << permuted_mean << std::endl;
    std::cout << "Permuted Variance: " << permuted_variance << std::endl;

    // Verify that the contents are permuted but the elements are the same
    std::cout << "\nOriginal Returns: ";
    for(double r : historical_returns) std::cout << r << " ";
    std::cout << std::endl;

    std::cout << "Permuted Returns: ";
    for(double r : permuted_returns) std::cout << r << " ";
    std::cout << std::endl;


    return 0;
}
