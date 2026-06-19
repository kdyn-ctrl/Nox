// =================================================================================
// main.cpp — Nox Trading System Backtester
// =================================================================================
// This program simulates the Nox trading strategy over a historical dataset.
//
// Key Responsibilities:
//   1. Parse command-line arguments for tunable parameters and date ranges.
//   2. Load and pre-process historical data using CSVParser.
//   3. Run the main simulation loop, applying all strategy rules day-by-day.
//   4. Log every trade to `trades.csv` for detailed review.
//   5. Print final summary statistics to the console.
//   6. In headless mode, print a single CSV line for the optimization script.
//
// Build:
//   ./build.sh
//
// Usage:
//   ./backtester <path_to_data.csv> [options]
//
// Example (Full Run):
//   ./backtester ./data/spy_vix_daily.csv
//
// Example (Optimization Run):
//   ./backtester ./data/spy_vix_daily.csv --headless --vix 30 --buffer 0.97 --rsi 40
// =================================================================================

#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <map>
#include <cmath>
#include <fstream>
#include <algorithm>
#include <numeric>

// Project-specific headers
#include "csv_parser.hpp"
#include "../shared/RegimeStateMachine.hpp" // Direct import of live logic

// =================================================================================
// Data Structures
// =================================================================================

struct Trade {
    std::string entry_date;
    std::string exit_date;
    double entry_price;
    double exit_price;
    int shares;
    double pnl;
    double pnl_pct;
    double portfolio_balance_at_exit;
};

struct Position {
    std::string entry_date;
    double entry_price;
    int shares;
    double stop_loss_price;
    size_t entry_day_index = 0; // Trading-day index at entry — used to compute holding period
};

struct BacktestParams {
    std::string start_date    = "1900-01-01";
    std::string end_date      = "2100-01-01";
    double initial_balance    = 100000.0;
    double vix_threshold      = 30.0;
    double sma_buffer_pct     = 0.98;
    // rsi_gate: minimum RSI required for a new entry (floor gate).
    // Default of 0.0 disables the filter — all RSI values pass.
    // Pass --rsi <value> to enable (e.g. --rsi 40 blocks entries when RSI < 40).
    double rsi_gate           = 0.0;
    double sl_atr_multiplier  = 2.0;
    // cooldown_days: number of trading days to wait after a stop-loss exit
    // before allowing a new entry. Prevents re-entering into the same choppy
    // volatility spike that just stopped us out.
    // Default of 5 days. Pass --cooldown 0 to disable.
    int    cooldown_days      = 5;
    // vol_gate: multiplier applied to the 50-day volume MA to set a minimum
    // volume threshold for new entries. A value of 1.0 requires at least
    // average volume. 0.0 disables the gate entirely (default).
    // Example: --vol 1.0 blocks entries on below-average-volume days.
    double vol_gate           = 0.0;
    bool headless             = false;
};

// =================================================================================
// Helper Functions
// =================================================================================

// Simple command-line argument parser
void parse_args(int argc, char* argv[], BacktestParams& params) {
    for (int i = 2; i < argc; ++i) { // Start from 2 to skip program name and filepath
        std::string arg = argv[i];
        if (arg == "--headless") {
            params.headless = true;
        } else if (arg == "--vix" && i + 1 < argc) {
            params.vix_threshold = std::stod(argv[++i]);
        } else if (arg == "--buffer" && i + 1 < argc) {
            params.sma_buffer_pct = std::stod(argv[++i]);
        } else if (arg == "--rsi" && i + 1 < argc) {
            params.rsi_gate = std::stod(argv[++i]);
        } else if (arg == "--cooldown" && i + 1 < argc) {
            params.cooldown_days = std::stoi(argv[++i]);
        } else if (arg == "--vol" && i + 1 < argc) {
            params.vol_gate = std::stod(argv[++i]);
        } else if (arg == "--start" && i + 1 < argc) {
            params.start_date = argv[++i];
        } else if (arg == "--end" && i + 1 < argc) {
            params.end_date = argv[++i];
        }
    }
}

// =================================================================================
// Main Simulation
// =================================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_data.csv> [options]" << std::endl;
        return 1;
    }

    // --- 1. Initialization ---
    std::string filepath = argv[1];
    BacktestParams params;
    parse_args(argc, argv, params);

    std::vector<DailyDataPoint> historical_data;
    try {
        historical_data = CSVParser::load(filepath);
    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }

    // --- Simulation State ---
    double portfolio_balance    = params.initial_balance;
    double peak_balance         = params.initial_balance;
    double max_drawdown         = 0.0;
    Position* current_position  = nullptr;
    std::vector<Trade> trade_log;
    RegimeStateMachine state_machine;
    int cooldown_remaining = 0; // Trading days remaining before next entry is allowed

    // --- Performance Metric Accumulators ---
    int    win_count                  = 0;
    int    loss_count                 = 0;
    double total_win_pnl              = 0.0;
    double total_loss_pnl             = 0.0;
    int    max_consecutive_losses     = 0;
    int    current_consecutive_losses = 0;
    int    total_holding_days         = 0;
    size_t day_index                  = 0;   // Counts every valid post-warmup trading day
    std::vector<double> daily_equity_curve;  // Full equity curve for Sharpe computation

    // --- File for Trade Log ---
    std::ofstream trades_file("trades.csv");
    trades_file << "EntryDate,ExitDate,EntryPrice,ExitPrice,Shares,PNL,PNL_Pct,PortfolioBalance\n";

    // --- 2. Main Simulation Loop ---
    for (const auto& day : historical_data) {
        // --- Warm-up & Filtering Guards ---
        if (day.sma200 == 0.0) continue; // SMA-200 not mature yet
        if (!day.valid) continue;        // Skip corrupted data rows
        if (day.date < params.start_date || day.date > params.end_date) continue;

        // --- A. CHECK EXITS FIRST ---
        if (current_position != nullptr) {
            bool exit_triggered = false;
            double exit_price = 0.0;

            // Trigger 1: Stop Loss
            if (day.low <= current_position->stop_loss_price) {
                exit_triggered = true;
                exit_price = current_position->stop_loss_price; // Assume worst case fill
            }

            // Trigger 2: Regime Flip to RISK_OFF
            AllocationStrategy current_strategy = state_machine.evaluate(day.vix_close, day.close, day.sma200, params.vix_threshold, params.sma_buffer_pct);
            if (current_strategy.current_regime == Regime::RISK_OFF) {
                exit_triggered = true;
                // Exit on the day's close since regime is evaluated on close
                exit_price = day.close * 0.9995; // Apply slippage
            }

            if (exit_triggered) {
                double pnl = (exit_price - current_position->entry_price) * current_position->shares;
                portfolio_balance += (current_position->shares * exit_price); // Add sale proceeds

                Trade t;
                t.entry_date = current_position->entry_date;
                t.exit_date = day.date;
                t.entry_price = current_position->entry_price;
                t.exit_price = exit_price;
                t.shares = current_position->shares;
                t.pnl = pnl;
                t.pnl_pct = (pnl / (t.entry_price * t.shares)) * 100.0;
                t.portfolio_balance_at_exit = portfolio_balance;
                trade_log.push_back(t);

                trades_file << t.entry_date << "," << t.exit_date << "," << t.entry_price << ","
                            << t.exit_price << "," << t.shares << "," << t.pnl << ","
                            << t.pnl_pct << "," << t.portfolio_balance_at_exit << "\n";

                // --- Update win/loss and holding period accumulators ---
                if (pnl >= 0.0) {
                    ++win_count;
                    total_win_pnl += pnl;
                    current_consecutive_losses = 0;
                    // No cooldown after a profitable exit — re-enter freely
                    cooldown_remaining = 0;
                } else {
                    ++loss_count;
                    total_loss_pnl += std::abs(pnl);
                    ++current_consecutive_losses;
                    if (current_consecutive_losses > max_consecutive_losses)
                        max_consecutive_losses = current_consecutive_losses;
                    // Enforce cooldown only after a stop-loss exit
                    cooldown_remaining = params.cooldown_days;
                }
                total_holding_days += static_cast<int>(day_index - current_position->entry_day_index);

                delete current_position;
                current_position = nullptr;
            }
        }

        // --- B. Evaluate Regime & Entry Gates ---
        AllocationStrategy strategy = state_machine.evaluate(day.vix_close, day.close, day.sma200, params.vix_threshold, params.sma_buffer_pct);

        if (current_position == nullptr) { // Only check for new entries if not in a position
            // Decrement cooldown counter each trading day we are out of the market.
            if (cooldown_remaining > 0) --cooldown_remaining;

            // Gate 1: Regime Gate — only enter on RISK_ON (full capital deployment).
            //   TRANSITION is intentionally excluded: it signals a choppy/uncertain
            //   market and should result in holding cash, not new entries at half size.
            //   RISK_OFF (capital_multiplier == 0.0) is already excluded by this check.
            // Gate 2: Cooldown Gate — block re-entry for N days after a stop-loss exit
            //   to avoid whipsawing back into the same volatile market conditions.
            // Gate 3: RSI Gate   — block entries when RSI is BELOW the gate (market
            //                     still recovering / not yet confirmed). A gate of 0
            //                     disables the filter entirely (all RSI values pass).
            //                     NOTE: This is a FLOOR gate (need RSI >= threshold).
            //                     The live execution engine uses a CEILING gate (RSI <= 70).
            // Gate 4: Volume Gate — require above-average volume on entry day to confirm
            //                     the breakout/regime transition has real participation.
            //                     Disabled (vol_gate == 0.0) or when vol_ma50 == 0.0
            //                     (no volume data in CSV).
            bool vol_gate_pass = (params.vol_gate == 0.0)
                                 || (day.vol_ma50 == 0.0)  // gate auto-disabled if no vol data
                                 || (day.volume >= day.vol_ma50 * params.vol_gate);

            if (strategy.current_regime == Regime::RISK_ON && cooldown_remaining == 0
                && (params.rsi_gate == 0.0 || day.rsi14 >= params.rsi_gate)
                && vol_gate_pass) {

                // --- C. All Gates Passed: Enter Position ---
                double position_size_usd = portfolio_balance * strategy.capital_multiplier;
                double entry_price = day.close * 1.0005; // Apply slippage
                int shares_to_buy = static_cast<int>(position_size_usd / entry_price);

                if (shares_to_buy > 0) {
                    portfolio_balance -= (shares_to_buy * entry_price); // Deduct cost

                    current_position = new Position();
                    current_position->entry_date       = day.date;
                    current_position->entry_price      = entry_price;
                    current_position->shares           = shares_to_buy;
                    current_position->stop_loss_price  = entry_price - (day.atr14 * params.sl_atr_multiplier);
                    current_position->entry_day_index  = day_index;
                }
            }
        }
        
        // --- D. Update Portfolio Metrics ---
        double current_equity = portfolio_balance;
        if(current_position != nullptr) {
            current_equity += (current_position->shares * day.close);
        }
        if (current_equity > peak_balance) {
            peak_balance = current_equity;
        }
        double drawdown = (peak_balance - current_equity) / peak_balance;
        if (drawdown > max_drawdown) {
            max_drawdown = drawdown;
        }

        // Record equity for Sharpe computation and advance the trading day counter.
        daily_equity_curve.push_back(current_equity);
        ++day_index;
    }

    trades_file.close();

    // --- 3. Performance Calculation ---

    // If the simulation ends with an open position, synthetically close it at
    // the last valid day's close for reporting purposes. This ensures all
    // performance metrics (trade count, win rate, Sharpe, etc.) correctly
    // reflect the full period — including any still-open position at period end.
    // This is especially important for OOS runs where the strategy may enter
    // early and hold through the entire out-of-sample window.
    if (current_position != nullptr) {
        // Find the last valid data point within the date range for mark-to-market.
        double mtm_price = 0.0;
        std::string mtm_date;
        for (int i = static_cast<int>(historical_data.size()) - 1; i >= 0; --i) {
            const auto& d = historical_data[i];
            if (d.valid && d.sma200 != 0.0 && d.date <= params.end_date && d.date >= params.start_date) {
                mtm_price = d.close;
                mtm_date  = d.date;
                break;
            }
        }

        if (mtm_price > 0.0) {
            double pnl = (mtm_price - current_position->entry_price) * current_position->shares;
            double balance_at_close = portfolio_balance + (current_position->shares * mtm_price);

            Trade t;
            t.entry_date              = current_position->entry_date;
            t.exit_date               = mtm_date + "*"; // '*' marks this as a synthetic MTM close
            t.entry_price             = current_position->entry_price;
            t.exit_price              = mtm_price;
            t.shares                  = current_position->shares;
            t.pnl                     = pnl;
            t.pnl_pct                 = (pnl / (t.entry_price * t.shares)) * 100.0;
            t.portfolio_balance_at_exit = balance_at_close;
            trade_log.push_back(t);

            // Write to trades file so the open position is visible in the log.
            std::ofstream trades_append("trades.csv", std::ios::app);
            trades_append << t.entry_date << "," << t.exit_date << "," << t.entry_price << ","
                          << t.exit_price << "," << t.shares << "," << t.pnl << ","
                          << t.pnl_pct << "," << t.portfolio_balance_at_exit << "\n";

            if (pnl >= 0.0) {
                ++win_count;
                total_win_pnl += pnl;
            } else {
                ++loss_count;
                total_loss_pnl += std::abs(pnl);
                ++current_consecutive_losses;
                if (current_consecutive_losses > max_consecutive_losses)
                    max_consecutive_losses = current_consecutive_losses;
            }
            total_holding_days += static_cast<int>(day_index - current_position->entry_day_index);

            portfolio_balance = balance_at_close;
        }

        delete current_position;
        current_position = nullptr;
    }

    double final_balance = !trade_log.empty()
        ? trade_log.back().portfolio_balance_at_exit
        : portfolio_balance;

    double total_return_pct = (final_balance / params.initial_balance - 1.0) * 100.0;
    
    // --- B&H Calculation ---
    // Find the first row within the filtered date range to use as the B&H entry price.
    // Using a hardcoded index[200] is wrong for OOS/date-filtered runs — it always
    // anchors to the first post-warmup row in the full dataset, not the run's start date.
    double bh_entry_price = 0.0;
    double bh_exit_price  = 0.0;
    for (const auto& d : historical_data) {
        if (d.valid && d.sma200 != 0.0 && d.date >= params.start_date) {
            if (bh_entry_price == 0.0) bh_entry_price = d.close; // first valid day in window
            if (d.date <= params.end_date) bh_exit_price = d.close; // last valid day in window
        }
    }
    double buy_and_hold_return_pct = (bh_entry_price > 0.0 && bh_exit_price > 0.0)
        ? (bh_exit_price / bh_entry_price - 1.0) * 100.0
        : 0.0;

    // --- Derived Trade Metrics ---
    double win_rate       = (!trade_log.empty()) ? (win_count  / static_cast<double>(trade_log.size())) * 100.0 : 0.0;
    double avg_win        = (win_count  > 0)     ?  total_win_pnl  / win_count  : 0.0;
    double avg_loss       = (loss_count > 0)     ?  total_loss_pnl / loss_count : 0.0;
    double win_loss_ratio = (avg_loss   > 0.0)   ?  avg_win / avg_loss          : 0.0;
    double avg_hold_days  = (!trade_log.empty()) ?  static_cast<double>(total_holding_days) / trade_log.size() : 0.0;

    // --- Sharpe Ratio (annualised, risk-free rate = 0) ---
    double sharpe_ratio = 0.0;
    if (daily_equity_curve.size() > 1) {
        std::vector<double> daily_returns;
        daily_returns.reserve(daily_equity_curve.size() - 1);
        for (size_t i = 1; i < daily_equity_curve.size(); ++i) {
            if (daily_equity_curve[i - 1] > 0.0)
                daily_returns.push_back((daily_equity_curve[i] - daily_equity_curve[i - 1]) / daily_equity_curve[i - 1]);
        }
        if (!daily_returns.empty()) {
            double mean_r = std::accumulate(daily_returns.begin(), daily_returns.end(), 0.0) / daily_returns.size();
            double sq_sum = 0.0;
            for (double r : daily_returns) sq_sum += (r - mean_r) * (r - mean_r);
            double std_r  = std::sqrt(sq_sum / daily_returns.size());
            if (std_r > 0.0)
                sharpe_ratio = (mean_r / std_r) * std::sqrt(252.0);
        }
    }


    // --- 4. Output ---
    if (params.headless) {
        std::cout << params.vix_threshold << "," << params.sma_buffer_pct << "," << params.rsi_gate << ","
                  << params.cooldown_days << "," << params.vol_gate << ","
                  << final_balance << "," << total_return_pct << "," << max_drawdown * 100.0 << ","
                  << trade_log.size() << "," << buy_and_hold_return_pct << ","
                  << win_rate << "," << avg_win << "," << avg_loss << ","
                  << win_loss_ratio << "," << sharpe_ratio << ","
                  << max_consecutive_losses << "," << avg_hold_days << std::endl;
    } else {
        std::cout << "\n--- Backtest Results ---\n";
        std::cout << "Parameters:\n";
        std::cout << "  VIX Threshold: " << params.vix_threshold << "\n";
        std::cout << "  SMA Buffer:    " << params.sma_buffer_pct << "\n";
        std::cout << "  RSI Gate:      " << params.rsi_gate << "\n";
        std::cout << "  Cooldown Days: " << params.cooldown_days << "\n";
        std::cout << "  Volume Gate:   " << params.vol_gate << "x MA50\n";
        std::cout << "------------------------\n";
        std::cout << "Initial Balance:       $" << std::fixed << std::setprecision(2) << params.initial_balance << "\n";
        std::cout << "Final Balance:         $" << final_balance << "\n";
        std::cout << "Total Return:           " << total_return_pct << "%\n";
        std::cout << "Max Drawdown:           " << max_drawdown * 100.0 << "%\n";
        std::cout << "Sharpe Ratio:           " << sharpe_ratio << "\n";
        std::cout << "------------------------\n";
        std::cout << "Total Trades:           " << trade_log.size() << "\n";
        std::cout << "Win Rate:               " << win_rate << "%\n";
        std::cout << "Avg Win:               $" << avg_win << "\n";
        std::cout << "Avg Loss:              $" << avg_loss << "\n";
        std::cout << "Win/Loss Ratio:         " << win_loss_ratio << "\n";
        std::cout << "Max Consecutive Losses: " << max_consecutive_losses << "\n";
        std::cout << "Avg Holding Period:     " << avg_hold_days << " days\n";
        std::cout << "------------------------\n";
        std::cout << "Buy & Hold Return:      " << buy_and_hold_return_pct << "%\n";
        std::cout << "------------------------\n";
        std::cout << "Trade log saved to trades.csv\n";
    }

    return 0;
}
