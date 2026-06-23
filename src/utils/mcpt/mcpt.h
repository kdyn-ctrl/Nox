#ifndef MCPT_H
#define MCPT_H

#include <vector>
#include <random>
#include <functional>

namespace mcpt {

/**
 * @brief Performs a Monte Carlo Permutation Test (MCPT) on historical daily log returns.
 *
 * Scrambles input returns 1,000 times using std::shuffle with std::mt19937, breaking
 * chronological patterns while strictly preserving mean and variance.
 *
 * @param returns Const reference to input vector of daily log returns.
 * @param seed Optional RNG seed for reproducibility. If 0, uses std::random_device.
 * @return Single permuted vector after 1,000 shuffles.
 */
std::vector<double> permute_returns(const std::vector<double>& returns, uint32_t seed = 0);

/**
 * @brief Batch MCPT: generates all 1,000 permutations.
 *
 * For workflows requiring multiple independent permutations (e.g., computing
 * permutation-based p-values). Each permutation is returned sequentially.
 *
 * @param returns Const reference to input vector of daily log returns.
 * @param callback Function invoked for each of 1,000 permutations with (permutation_index, permuted_vector).
 * @param seed Optional RNG seed for reproducibility.
 */
void permute_returns_batch(
    const std::vector<double>& returns,
    std::function<void(int, const std::vector<double>&)> callback,
    uint32_t seed = 0
);

/**
 * @brief Multi-threaded batch MCPT (optional, high-performance path).
 *
 * Generates 1,000 permutations in parallel using thread-local RNGs.
 * Useful when callback involves expensive computations (e.g., test statistics).
 *
 * @param returns Const reference to input vector of daily log returns.
 * @param callback Function invoked for each permutation with (permutation_index, permuted_vector).
 * @param seed Optional RNG seed for reproducibility.
 * @param num_threads Number of worker threads. 0 = auto-detect.
 */
void permute_returns_parallel(
    const std::vector<double>& returns,
    std::function<void(int, const std::vector<double>&)> callback,
    uint32_t seed = 0,
    int num_threads = 0
);

} // namespace mcpt

#endif // MCPT_H
