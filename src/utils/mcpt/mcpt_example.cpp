#include "mcpt.h"
#include <iostream>
#include <vector>
#include <numeric>
#include <cmath>
#include <chrono>
#include <iomanip>

double calculate_mean(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

double calculate_variance(const std::vector<double>& v, double mean) {
    double sum = 0.0;
    for (double x : v) {
        sum += (x - mean) * (x - mean);
    }
    return sum / (v.size() - 1);
}

double calculate_sharpe_ratio(const std::vector<double>& returns, double risk_free_rate = 0.0) {
    if (returns.empty()) return 0.0;
    double mean = calculate_mean(returns);
    double variance = calculate_variance(returns, mean);
    double std_dev = std::sqrt(variance);
    return std_dev > 0 ? (mean - risk_free_rate) / std_dev * std::sqrt(252) : 0.0;
}

int main() {
    std::vector<double> historical_returns = {
        0.0120, -0.0080, 0.0150, -0.0050, 0.0090,
        -0.0030, 0.0110, -0.0070, 0.0140, -0.0100,
        0.0085, -0.0045, 0.0125, -0.0065, 0.0095,
        -0.0025, 0.0105, -0.0055, 0.0135, -0.0090
    };

    double original_mean = calculate_mean(historical_returns);
    double original_variance = calculate_variance(historical_returns, original_mean);
    double original_sharpe = calculate_sharpe_ratio(historical_returns);

    std::cout << "=== MCPT Performance Analysis ===" << std::endl;
    std::cout << "Original Mean:     " << std::fixed << std::setprecision(6) << original_mean << std::endl;
    std::cout << "Original Variance: " << original_variance << std::endl;
    std::cout << "Original Sharpe:   " << original_sharpe << std::endl << std::endl;

    std::cout << "--- Single Permutation (Original API) ---" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<double> single_perm = mcpt::permute_returns(historical_returns, 12345);
    auto end = std::chrono::high_resolution_clock::now();
    double single_duration = std::chrono::duration<double>(end - start).count();

    std::cout << "Time: " << single_duration * 1000 << " ms" << std::endl;
    std::cout << "Mean preserved: " << (std::abs(calculate_mean(single_perm) - original_mean) < 1e-10 ? "YES" : "NO") << std::endl;
    std::cout << "Variance preserved: " << (std::abs(calculate_variance(single_perm, calculate_mean(single_perm)) - original_variance) < 1e-10 ? "YES" : "NO") << std::endl << std::endl;

    std::cout << "--- Batch Mode (Callback) ---" << std::endl;
    start = std::chrono::high_resolution_clock::now();
    std::vector<double> sharpe_distribution;
    int perm_count = 0;

    mcpt::permute_returns_batch(
        historical_returns,
        [&](int idx, const std::vector<double>& perm) {
            sharpe_distribution.push_back(calculate_sharpe_ratio(perm));
            perm_count++;
        },
        12345
    );
    end = std::chrono::high_resolution_clock::now();
    double batch_duration = std::chrono::duration<double>(end - start).count();

    std::cout << "Time: " << batch_duration * 1000 << " ms" << std::endl;
    std::cout << "Permutations processed: " << perm_count << std::endl;
    std::cout << "Mean Sharpe (permuted): " << calculate_mean(sharpe_distribution) << std::endl;
    std::cout << "Original Sharpe rank: " << std::count_if(sharpe_distribution.begin(), sharpe_distribution.end(),
                                                            [original_sharpe](double x) { return x >= original_sharpe; })
              << " / 1000" << std::endl << std::endl;

    std::cout << "--- Parallel Mode ---" << std::endl;
    start = std::chrono::high_resolution_clock::now();
    sharpe_distribution.clear();

    mcpt::permute_returns_parallel(
        historical_returns,
        [&](int idx, const std::vector<double>& perm) {
            sharpe_distribution.push_back(calculate_sharpe_ratio(perm));
        },
        12345,
        0
    );
    end = std::chrono::high_resolution_clock::now();
    double parallel_duration = std::chrono::duration<double>(end - start).count();

    std::cout << "Time: " << parallel_duration * 1000 << " ms" << std::endl;
    std::cout << "Speedup vs. batch: " << batch_duration / parallel_duration << "x" << std::endl;

    return 0;
}
