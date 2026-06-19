#!/bin/bash
# =============================================================================
# optimization_loop.sh - Grid Search Optimization Runner
# =============================================================================
# This script automates the process of finding the optimal parameters for the
# Nox trading strategy by performing a grid search over a predefined set of
# tunable values.
#
# It iterates through all combinations of:
#   - VIX thresholds
#   - SMA buffers
#   - RSI gates
#
# For each combination, it invokes the compiled C++ backtester in "headless"
# mode. The backtester is responsible for running the simulation and printing
# a single CSV-formatted line of summary statistics to standard output.
#
# This script captures that output and appends it to a master `results.csv`
# file, creating a comprehensive log of all parameter permutations and their
# corresponding performance metrics.
#
# Pre-requisites:
#   1. The backtester must be compiled (`./build.sh`).
#   2. The historical data CSV must be present at the specified path.
#
# Usage:
#   ./optimization_loop.sh <path_to_data.csv>
# =============================================================================

# --- Configuration ---
DATA_FILE=$1
RESULTS_FILE="results.csv"
BACKTESTER_EXE="./backtester"

# Check for data file argument
if [ -z "$DATA_FILE" ]; then
    echo "Usage: $0 <path_to_data.csv>"
    exit 1
fi

# Check if backtester executable exists
if [ ! -f "$BACKTESTER_EXE" ]; then
    echo "Error: Backtester executable not found. Please run ./build.sh first."
    exit 1
fi


# --- Parameter Grid ---
# VIX Thresholds: The VIX level that triggers a RISK_OFF state.
VIX_THRESHOLDS=(28.0 30.0 32.0 35.0)

# SMA Buffers: The percentage below the 200 SMA to trigger RISK_OFF
# (e.g., 0.98 means price is 2% below the SMA).
SMA_BUFFERS=(0.98 0.97 0.96)

# RSI Gates: The minimum RSI required to allow a new entry.
# Blocks entries when RSI is below this level (market still weak/falling).
# A value of 0 effectively disables the gate.
RSI_GATES=(0 40 45 50)

# Cooldown Days: Trading days to wait after a stop-loss exit before re-entering.
# Prevents whipsawing back into the same choppy market that just stopped us out.
# A value of 0 disables the cooldown entirely.
COOLDOWN_DAYS=(0 3 5 10)

# Volume Gate: minimum volume as a multiple of the 50-day volume MA.
# 0 = disabled. 1.0 = require at least average volume. 1.2 = 20% above average.
VOL_GATES=(0 0.8 1.0 1.2)


# --- Execution ---

# 1. Initialize the results file with a header row.
echo "vix_threshold,sma_buffer_pct,rsi_gate,cooldown_days,vol_gate,final_balance,total_return_pct,max_drawdown_pct,total_trades,buy_and_hold_pct,win_rate_pct,avg_win,avg_loss,win_loss_ratio,sharpe_ratio,max_consec_losses,avg_hold_days" > $RESULTS_FILE

echo "Starting optimization grid search..."

# 2. Loop through every combination of parameters.
for vix in "${VIX_THRESHOLDS[@]}"; do
    for buffer in "${SMA_BUFFERS[@]}"; do
        for rsi in "${RSI_GATES[@]}"; do
            for cooldown in "${COOLDOWN_DAYS[@]}"; do
                for vol in "${VOL_GATES[@]}"; do

                    echo "Running: VIX=$vix, Buffer=$buffer, RSI=$rsi, Cooldown=$cooldown, Vol=$vol"

                    # 3. Execute the backtester in headless mode with the current params.
                    # stdout (CSV line) is appended to results. stderr (parser logs) is suppressed.
                    $BACKTESTER_EXE $DATA_FILE --headless --vix $vix --buffer $buffer --rsi $rsi --cooldown $cooldown --vol $vol >> $RESULTS_FILE 2>/dev/null

                done
            done
        done
    done
done

echo "--------------------------------------------------"
echo "Optimization complete."
echo "Results saved to: $RESULTS_FILE"
echo "--------------------------------------------------"
