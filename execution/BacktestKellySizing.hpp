#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Fractional-Kelly position sizing for the backtester's trade sequence.
// ─────────────────────────────────────────────────────────────────────────────
// Mirrors execution/main.cpp's calculate_kelly_size(): K% = W - (1-W)/R,
// scaled by a configurable fraction, hard-capped, and halted (not silently
// promoted to 1 contract) on a non-positive raw Kelly or a zero-contract
// allocation — same RULE-005 semantics as the live engine.
//
// The one thing the live engine doesn't need to worry about that a backtest
// does: win_rate/win_loss_ratio must be computed CAUSALLY. Using the full,
// already-known trade history (including trades that haven't "happened" yet
// at a given point in the backtest) to size an earlier trade is look-ahead
// bias baked directly into the sizing decision. RollingTradeStats below only
// ever reflects trades the caller has already fed it, in sequence order.
//
// Pure math — no network/SQLite/file I/O — so it's independently unit
// testable (see execution/test/test_backtest_kelly_sizing.cpp).

#include <cmath>
#include <cstdlib>
#include <deque>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nox::backtest {

struct KellySizingConfig {
    bool   enabled              = false; // NOX_BT_KELLY_ENABLED
    double hard_cap              = 0.10;  // NOX_BT_KELLY_CAP
    int    rolling_window         = 20;    // NOX_BT_KELLY_WINDOW
    int    min_trades_for_stats   = 10;    // below this, use the seed W/R
    double seed_win_rate          = 0.50;  // NOX_BT_KELLY_SEED_WR
    double seed_win_loss_ratio     = 1.5;   // NOX_BT_KELLY_SEED_WLR
    // Kelly fractions to sweep and compare side by side in one report —
    // this is the actual answer to "can I backtest different Kelly rules".
    std::vector<double> sweep_fractions = {0.10, 0.25, 0.50, 1.00}; // NOX_BT_KELLY_SWEEP

    static KellySizingConfig fromEnv() {
        KellySizingConfig cfg;
        if (const char* v = std::getenv("NOX_BT_KELLY_ENABLED")) {
            std::string s(v);
            cfg.enabled = (s == "1" || s == "true" || s == "yes");
        }
        if (const char* v = std::getenv("NOX_BT_KELLY_CAP")) {
            try { cfg.hard_cap = std::stod(v); } catch (...) {}
        }
        if (const char* v = std::getenv("NOX_BT_KELLY_WINDOW")) {
            try { cfg.rolling_window = std::stoi(v); } catch (...) {}
        }
        if (const char* v = std::getenv("NOX_BT_KELLY_SEED_WR")) {
            try { cfg.seed_win_rate = std::stod(v); } catch (...) {}
        }
        if (const char* v = std::getenv("NOX_BT_KELLY_SEED_WLR")) {
            try { cfg.seed_win_loss_ratio = std::stod(v); } catch (...) {}
        }
        if (const char* v = std::getenv("NOX_BT_KELLY_SWEEP")) {
            std::vector<double> fractions;
            std::istringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                if (tok.empty()) continue;
                try { fractions.push_back(std::stod(tok)); } catch (...) {}
            }
            if (!fractions.empty()) cfg.sweep_fractions = fractions;
        }
        return cfg;
    }
};

// Returns -1 for "no valid sizing" (non-positive raw Kelly or < 1 contract) —
// same halt semantics as the live calculate_kelly_size(): a halted trade is
// skipped entirely, never silently forced to 1 contract.
inline int calculateKellyContracts(double equity, double option_price_per_contract,
                                   double win_rate, double win_loss_ratio,
                                   double kelly_fraction, double hard_cap) {
    if (option_price_per_contract <= 0.0 || win_loss_ratio <= 0.0) return -1;

    double kelly_pct = win_rate - ((1.0 - win_rate) / win_loss_ratio);
    if (kelly_pct <= 0.0) return -1;

    double adjusted_risk = kelly_pct * kelly_fraction;
    if (adjusted_risk > hard_cap) adjusted_risk = hard_cap;

    double dollar_amount = equity * adjusted_risk;
    int contracts = static_cast<int>(std::floor(dollar_amount / option_price_per_contract));
    return contracts > 0 ? contracts : -1;
}

// Causal rolling win-rate / win-loss-ratio tracker. Only ever reflects trades
// already record()-ed, in the order the caller feeds them — the caller is
// responsible for feeding trades in chronological order and for recording a
// trade's outcome only AFTER using its pre-trade stats to size it.
class RollingTradeStats {
public:
    explicit RollingTradeStats(int window) : window_(window) {}

    // pnl_per_share: same sign convention as Trade::pnl — the scale (×100 or
    // not) is irrelevant to a win-rate/ratio calculation.
    void record(double pnl_per_share) {
        history_.push_back(pnl_per_share);
        if (static_cast<int>(history_.size()) > window_) history_.pop_front();
    }

    int count() const { return static_cast<int>(history_.size()); }

    // {win_rate, win_loss_ratio}. win_loss_ratio is 0.0 when there are no
    // losses yet in the window — callers must treat that as "insufficient
    // data" and fall back to a seed ratio, not divide by it.
    std::pair<double, double> stats() const {
        int wins = 0, losses = 0;
        double win_sum = 0.0, loss_sum = 0.0;
        for (double p : history_) {
            if (p > 0.0)      { ++wins;   win_sum  += p;  }
            else if (p < 0.0) { ++losses; loss_sum += -p; }
        }
        int n = static_cast<int>(history_.size());
        double win_rate = n > 0 ? static_cast<double>(wins) / n : 0.0;
        double avg_win  = wins   > 0 ? win_sum  / wins   : 0.0;
        double avg_loss = losses > 0 ? loss_sum / losses : 0.0;
        double wlr = avg_loss > 1e-9 ? avg_win / avg_loss : 0.0;
        return {win_rate, wlr};
    }

private:
    int window_;
    std::deque<double> history_;
};

} // namespace nox::backtest
