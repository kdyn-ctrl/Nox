#include "mcpt.h"
#include <algorithm>
#include <thread>
#include <mutex>

namespace mcpt {

std::vector<double> permute_returns(const std::vector<double>& returns, uint32_t seed) {
    if (returns.empty()) return std::vector<double>();

    std::vector<double> permuted = returns;

    std::mt19937 rng(seed != 0 ? seed : std::mt19937::default_seed);

    for (int i = 0; i < 1000; ++i) {
        std::shuffle(permuted.begin(), permuted.end(), rng);
    }

    return permuted;
}

void permute_returns_batch(
    const std::vector<double>& returns,
    std::function<void(int, const std::vector<double>&)> callback,
    uint32_t seed) {

    if (returns.empty()) return;

    std::vector<double> permuted = returns;
    std::mt19937 rng(seed != 0 ? seed : std::mt19937::default_seed);

    for (int i = 0; i < 1000; ++i) {
        std::shuffle(permuted.begin(), permuted.end(), rng);
        callback(i, permuted);
    }
}

void permute_returns_parallel(
    const std::vector<double>& returns,
    std::function<void(int, const std::vector<double>&)> callback,
    uint32_t seed,
    int num_threads) {

    if (returns.empty()) return;

    if (num_threads <= 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads <= 0) num_threads = 4;
    }

    std::mutex callback_mutex;
    std::vector<std::thread> threads;
    constexpr int PERMS_PER_THREAD = 10;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&returns, &callback, &callback_mutex, seed, t]() {
            std::vector<double> permuted = returns;
            std::mt19937 rng(seed != 0 ? seed ^ (t << 16) : std::mt19937::default_seed ^ (t << 16));

            int start = t * PERMS_PER_THREAD;
            int end = (t + 1) * PERMS_PER_THREAD;
            if (t == std::thread::hardware_concurrency() - 1) {
                end = 1000;
            }

            for (int i = start; i < end && i < 1000; ++i) {
                std::shuffle(permuted.begin(), permuted.end(), rng);
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    callback(i, permuted);
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
}

} // namespace mcpt
