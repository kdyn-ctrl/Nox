#include "mcpt.h"
#include <iostream>
#include <vector>
#include <numeric>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <algorithm>

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

// Path-dependent test statistic: maximum drawdown of the cumulative equity curve.
//
// Unlike mean / variance / Sharpe (which are functions of the returns multiset and
// therefore invariant under permutation), maximum drawdown depends on the ORDER of
// returns. This makes it a valid statistic for a permutation test: shuffling the
// returns produces a genuinely different value, so the resulting p-value is non-degenerate.
//
// Equity is built as a cumulative product of (1 + r). Max drawdown is the largest
// peak-to-trough decline along that curve, returned as a positive fraction (0 = no drawdown).
double calculate_max_drawdown(const std::vector<double>& returns) {
    if (returns.empty()) return 0.0;
    double equity = 1.0;
    double peak = 1.0;
    double max_dd = 0.0;
    for (double r : returns) {
        equity *= (1.0 + r);
        if (equity > peak) peak = equity;
        double dd = (peak - equity) / peak;
        if (dd > max_dd) max_dd = dd;
    }
    return max_dd;
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
    double original_max_dd = calculate_max_drawdown(historical_returns);

    std::cout << "=== MCPT Performance Analysis ===" << std::endl;
    std::cout << "Statistic: Maximum Drawdown (path-dependent)" << std::endl;
    std::cout << "Original Mean:       " << std::fixed << std::setprecision(6) << original_mean << std::endl;
    std::cout << "Original Variance:   " << original_variance << std::endl;
    std::cout << "Original Max DD:     " << original_max_dd << std::endl << std::endl;

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
    std::vector<double> dd_distribution;
    int perm_count = 0;

    mcpt::permute_returns_batch(
        historical_returns,
        [&](int idx, const std::vector<double>& perm) {
            dd_distribution.push_back(calculate_max_drawdown(perm));
            perm_count++;
        },
        12345
    );
    end = std::chrono::high_resolution_clock::now();
    double batch_duration = std::chrono::duration<double>(end - start).count();

    // p-value: fraction of permuted statistics at least as extreme as the original.
    int ge_count = std::count_if(dd_distribution.begin(), dd_distribution.end(),
                                 [original_max_dd](double x) { return x >= original_max_dd; });
    double p_value = static_cast<double>(ge_count) / perm_count;

    std::cout << "Time: " << batch_duration * 1000 << " ms" << std::endl;
    std::cout << "Permutations processed: " << perm_count << std::endl;
    std::cout << "Mean Max DD (permuted): " << calculate_mean(dd_distribution) << std::endl;
    std::cout << "Original Max DD rank:   " << ge_count << " / " << perm_count << std::endl;
    std::cout << "p-value:                " << p_value << std::endl << std::endl;

    std::cout << "--- Parallel Mode ---" << std::endl;
    start = std::chrono::high_resolution_clock::now();
    dd_distribution.clear();

    mcpt::permute_returns_parallel(
        historical_returns,
        [&](int idx, const std::vector<double>& perm) {
            dd_distribution.push_back(calculate_max_drawdown(perm));
        },
        12345,
        0
    );
    end = std::chrono::high_resolution_clock::now();
    double parallel_duration = std::chrono::duration<double>(end - start).count();

    std::cout << "Time: " << parallel_duration * 1000 << " ms" << std::endl;
    std::cout << "Permutations processed: " << dd_distribution.size() << std::endl;
    std::cout << "Speedup vs. batch: " << batch_duration / parallel_duration << "x" << std::endl;

    return 0;
}
