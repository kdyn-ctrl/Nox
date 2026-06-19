#!/bin/bash
# =============================================================================
# walk_forward.sh - Walk-Forward / Out-of-Sample Validation
# =============================================================================
# PURPOSE
#   Validates that the best parameters found during optimisation are not
#   curve-fitted to the full dataset. A strategy that only works on data it
#   was optimised against is useless in live trading.
#
# METHOD
#   1. IN-SAMPLE  (IS)  : Run the full parameter grid on 1998-01-01 → 2015-12-31.
#   2. FIND WINNER      : Select the single best parameter set by Sharpe ratio,
#                         subject to a minimum trade count (avoids lucky 1-trade runs).
#   3. OUT-OF-SAMPLE (OOS): Run ONLY the winning params on 2016-01-01 → present.
#                           No further optimisation is performed at this stage.
#   4. REPORT           : Print a clear side-by-side comparison.
#
# INTERPRETATION
#   - OOS performance ≥ ~50% of IS performance  →  Strategy is robust. Proceed
#                                                   to Step 4 (bake in defaults).
#   - OOS performance collapses (e.g. negative return, Sharpe < 0)  →  Curve-
#                                                   fitted. Re-examine strategy
#                                                   logic before going live.
#
# Pre-requisites:
#   1. The backtester must be compiled  (./build.sh).
#   2. The historical data CSV must be present at the specified path.
#
# Usage:
#   ./walk_forward.sh <path_to_data.csv>
#
# Output files:
#   is_results.csv   — Full in-sample grid search results
#   oos_results.csv  — Single out-of-sample run with the winning params
# =============================================================================

set -euo pipefail

# --- Configuration ---
DATA_FILE="${1:-}"
IS_RESULTS_FILE="is_results.csv"
OOS_RESULTS_FILE="oos_results.csv"
BACKTESTER_EXE="./backtester"

# Walk-forward split dates
IS_START="1998-01-01"
IS_END="2015-12-31"
OOS_START="2016-01-01"
OOS_END="2100-01-01"   # Effectively "present and beyond"

# Minimum trades required for a param set to be eligible as winner.
# Guards against lucky single-trade results dominating the IS ranking.
MIN_TRADES=3

# Number of top IS param sets to validate OOS.
# Running the top N (not just the #1) guards against selecting a winner
# whose IS Sharpe edge over #2 is noise (e.g. a 0.01 Sharpe difference).
TOP_N=3

# CSV column indices (0-based) matching the headless output format:
# vix_threshold,sma_buffer_pct,rsi_gate,cooldown_days,vol_gate,final_balance,
# total_return_pct,max_drawdown_pct,total_trades,buy_and_hold_pct,win_rate_pct,
# avg_win,avg_loss,win_loss_ratio,sharpe_ratio,max_consec_losses,avg_hold_days
COL_VIX=0
COL_BUFFER=1
COL_RSI=2
COL_COOLDOWN=3
COL_VOL=4
COL_RETURN=6
COL_DRAWDOWN=7
COL_TRADES=8
COL_WINRATE=10
COL_WIN_LOSS=13
COL_SHARPE=14

# --- Parameter Grid (mirror of optimization_loop.sh) ---
VIX_THRESHOLDS=(28.0 30.0 32.0 35.0)
SMA_BUFFERS=(0.98 0.97 0.96)
RSI_GATES=(0 40 45 50)
COOLDOWN_DAYS=(0 3 5 10)
VOL_GATES=(0 0.8 1.0 1.2)

# =============================================================================
# Validation
# =============================================================================
if [ -z "$DATA_FILE" ]; then
    echo "Usage: $0 <path_to_data.csv>"
    exit 1
fi

if [ ! -f "$DATA_FILE" ]; then
    echo "Error: Data file not found: $DATA_FILE"
    exit 1
fi

if [ ! -f "$BACKTESTER_EXE" ]; then
    echo "Error: Backtester executable not found. Please run ./build.sh first."
    exit 1
fi

# =============================================================================
# PHASE 1 — IN-SAMPLE GRID SEARCH (1998 → 2015)
# =============================================================================
echo ""
echo "============================================================"
echo "  PHASE 1: IN-SAMPLE GRID SEARCH"
echo "  Period : $IS_START  →  $IS_END"
echo "============================================================"
echo ""

CSV_HEADER="vix_threshold,sma_buffer_pct,rsi_gate,cooldown_days,vol_gate,final_balance,total_return_pct,max_drawdown_pct,total_trades,buy_and_hold_pct,win_rate_pct,avg_win,avg_loss,win_loss_ratio,sharpe_ratio,max_consec_losses,avg_hold_days"
echo "$CSV_HEADER" > "$IS_RESULTS_FILE"

TOTAL_RUNS=$(( ${#VIX_THRESHOLDS[@]} * ${#SMA_BUFFERS[@]} * ${#RSI_GATES[@]} * ${#COOLDOWN_DAYS[@]} * ${#VOL_GATES[@]} ))
RUN_COUNT=0

for vix in "${VIX_THRESHOLDS[@]}"; do
    for buffer in "${SMA_BUFFERS[@]}"; do
        for rsi in "${RSI_GATES[@]}"; do
            for cooldown in "${COOLDOWN_DAYS[@]}"; do
                for vol in "${VOL_GATES[@]}"; do
                    RUN_COUNT=$(( RUN_COUNT + 1 ))
                    printf "  [%2d/%d]  VIX=%-5s  Buffer=%-5s  RSI=%-3s  Cooldown=%-3s  Vol=%-4s\n" \
                           "$RUN_COUNT" "$TOTAL_RUNS" "$vix" "$buffer" "$rsi" "$cooldown" "$vol"

                    $BACKTESTER_EXE "$DATA_FILE" \
                        --headless \
                        --vix "$vix" --buffer "$buffer" --rsi "$rsi" --cooldown "$cooldown" --vol "$vol" \
                        --start "$IS_START" --end "$IS_END" \
                        >> "$IS_RESULTS_FILE" 2>/dev/null
                done
            done
        done
    done
done

echo ""
echo "  In-sample results saved to: $IS_RESULTS_FILE"

# =============================================================================
# PHASE 2 — FIND TOP N IN-SAMPLE PARAMS (by Sharpe, min $MIN_TRADES trades)
# =============================================================================
echo ""
echo "============================================================"
echo "  PHASE 2: SELECTING TOP $TOP_N IN-SAMPLE PARAMETER SETS"
echo "  Ranked by: Sharpe Ratio (min $MIN_TRADES trades required)"
echo "  NOTE: Running top $TOP_N (not just #1) guards against a winner"
echo "  whose Sharpe edge is noise (e.g. a 0.01 difference)."
echo "============================================================"
echo ""

# Use awk to:
#   1. Skip header (NR==1)
#   2. Filter rows by MIN_TRADES
#   3. Sort by Sharpe descending and print the top TOP_N lines
# awk field indices are 1-based, so add 1 to our 0-based COL_* constants.
TOP_LINES=$(awk -F',' \
    -v min_trades="$MIN_TRADES" \
    -v col_trades="$(( COL_TRADES + 1 ))" \
    -v col_sharpe="$(( COL_SHARPE + 1 ))" \
    -v top_n="$TOP_N" \
    'NR > 1 && $col_trades >= min_trades { print $col_sharpe"|"$0 }' \
    "$IS_RESULTS_FILE" \
    | sort -t'|' -k1 -rn \
    | head -n "$TOP_N" \
    | cut -d'|' -f2-)

if [ -z "$TOP_LINES" ]; then
    echo "  ERROR: No parameter set met the minimum trade count of $MIN_TRADES."
    echo "  Consider lowering MIN_TRADES or widening the parameter grid."
    exit 1
fi

echo "  Rank  VIX     Buffer  RSI   Cool  Vol   Return%   Drawdown%  Sharpe  Trades  WinRate%"
echo "  ----  ------  ------  ----  ----  ----  --------  ---------  ------  ------  --------"
rank=0
while IFS= read -r line; do
  rank=$(( rank + 1 ))
  v=$(  echo "$line" | cut -d',' -f$(( COL_VIX      + 1 )) )
  b=$(  echo "$line" | cut -d',' -f$(( COL_BUFFER   + 1 )) )
  r=$(  echo "$line" | cut -d',' -f$(( COL_RSI      + 1 )) )
  c=$(  echo "$line" | cut -d',' -f$(( COL_COOLDOWN + 1 )) )
  vg=$( echo "$line" | cut -d',' -f$(( COL_VOL      + 1 )) )
  ret=$(echo "$line" | cut -d',' -f$(( COL_RETURN   + 1 )) )
  dd=$( echo "$line" | cut -d',' -f$(( COL_DRAWDOWN + 1 )) )
  sh=$( echo "$line" | cut -d',' -f$(( COL_SHARPE   + 1 )) )
  tr=$( echo "$line" | cut -d',' -f$(( COL_TRADES   + 1 )) )
  wr=$( echo "$line" | cut -d',' -f$(( COL_WINRATE  + 1 )) )
  printf "  #%-3d  %-6s  %-6s  %-4s  %-4s  %-4s  %-8s  %-9s  %-6s  %-6s  %-8s\n" \
         "$rank" "$v" "$b" "$r" "$c" "$vg" "$ret" "$dd" "$sh" "$tr" "$wr"
done <<< "$TOP_LINES"

# The primary winner is still rank #1 (highest IS Sharpe).
BEST_LINE=$(echo "$TOP_LINES" | head -1)
BEST_VIX=$(      echo "$BEST_LINE" | cut -d',' -f$(( COL_VIX      + 1 )) )
BEST_BUFFER=$(   echo "$BEST_LINE" | cut -d',' -f$(( COL_BUFFER   + 1 )) )
BEST_RSI=$(      echo "$BEST_LINE" | cut -d',' -f$(( COL_RSI      + 1 )) )
BEST_COOLDOWN=$( echo "$BEST_LINE" | cut -d',' -f$(( COL_COOLDOWN + 1 )) )
BEST_VOL=$(      echo "$BEST_LINE" | cut -d',' -f$(( COL_VOL      + 1 )) )

# =============================================================================
# PHASE 3 — OUT-OF-SAMPLE RUN (2016 → present, top N params)
# =============================================================================
echo ""
echo "============================================================"
echo "  PHASE 3: OUT-OF-SAMPLE VALIDATION (top $TOP_N param sets)"
echo "  Period : $OOS_START  →  present"
echo "============================================================"
echo ""

echo "$CSV_HEADER" > "$OOS_RESULTS_FILE"

# Store all OOS lines in an array for the final report.
declare -a OOS_LINES_ARR

rank=0
while IFS= read -r is_line; do
    rank=$(( rank + 1 ))
    v=$( echo "$is_line" | cut -d',' -f$(( COL_VIX      + 1 )) )
    b=$( echo "$is_line" | cut -d',' -f$(( COL_BUFFER   + 1 )) )
    r=$( echo "$is_line" | cut -d',' -f$(( COL_RSI      + 1 )) )
    c=$( echo "$is_line" | cut -d',' -f$(( COL_COOLDOWN + 1 )) )
    vg=$(echo "$is_line" | cut -d',' -f$(( COL_VOL      + 1 )) )

    printf "  [%d/%d]  VIX=%-5s  Buffer=%-5s  RSI=%-3s  Cooldown=%-3s  Vol=%-4s\n" \
           "$rank" "$TOP_N" "$v" "$b" "$r" "$c" "$vg"

    oos_line=$( $BACKTESTER_EXE "$DATA_FILE" \
        --headless \
        --vix "$v" --buffer "$b" --rsi "$r" --cooldown "$c" --vol "$vg" \
        --start "$OOS_START" --end "$OOS_END" \
        2>/dev/null )

    echo "$oos_line" >> "$OOS_RESULTS_FILE"
    OOS_LINES_ARR+=("$oos_line")
done <<< "$TOP_LINES"

echo ""
echo "  Out-of-sample results saved to: $OOS_RESULTS_FILE"

# =============================================================================
# PHASE 4 — SIDE-BY-SIDE REPORT (one block per top-N candidate)
# =============================================================================
echo ""
echo "============================================================"
echo "  PHASE 4: WALK-FORWARD VALIDATION REPORT"
echo "============================================================"

rank=0
while IFS= read -r is_line; do
    rank=$(( rank + 1 ))
    oos_line="${OOS_LINES_ARR[$(( rank - 1 ))]}"

    IS_VIX=$(      echo "$is_line"  | cut -d',' -f$(( COL_VIX      + 1 )) )
    IS_BUFFER=$(   echo "$is_line"  | cut -d',' -f$(( COL_BUFFER   + 1 )) )
    IS_RSI=$(      echo "$is_line"  | cut -d',' -f$(( COL_RSI      + 1 )) )
    IS_COOLDOWN=$( echo "$is_line"  | cut -d',' -f$(( COL_COOLDOWN + 1 )) )
    IS_VOL=$(      echo "$is_line"  | cut -d',' -f$(( COL_VOL      + 1 )) )
    IS_RETURN=$(   echo "$is_line"  | cut -d',' -f$(( COL_RETURN   + 1 )) )
    IS_DRAWDOWN=$( echo "$is_line"  | cut -d',' -f$(( COL_DRAWDOWN + 1 )) )
    IS_TRADES=$(   echo "$is_line"  | cut -d',' -f$(( COL_TRADES   + 1 )) )
    IS_WINRATE=$(  echo "$is_line"  | cut -d',' -f$(( COL_WINRATE  + 1 )) )
    IS_WINLOSS=$(  echo "$is_line"  | cut -d',' -f$(( COL_WIN_LOSS + 1 )) )
    IS_SHARPE=$(   echo "$is_line"  | cut -d',' -f$(( COL_SHARPE   + 1 )) )

    OOS_RETURN=$(   echo "$oos_line" | cut -d',' -f$(( COL_RETURN   + 1 )) )
    OOS_DRAWDOWN=$( echo "$oos_line" | cut -d',' -f$(( COL_DRAWDOWN + 1 )) )
    OOS_TRADES=$(   echo "$oos_line" | cut -d',' -f$(( COL_TRADES   + 1 )) )
    OOS_WINRATE=$(  echo "$oos_line" | cut -d',' -f$(( COL_WINRATE  + 1 )) )
    OOS_WINLOSS=$(  echo "$oos_line" | cut -d',' -f$(( COL_WIN_LOSS + 1 )) )
    OOS_SHARPE=$(   echo "$oos_line" | cut -d',' -f$(( COL_SHARPE   + 1 )) )

    echo ""
    echo "  --- Candidate #$rank  (VIX=$IS_VIX  Buffer=$IS_BUFFER  RSI=$IS_RSI  Cooldown=$IS_COOLDOWN  Vol=$IS_VOL) ---"
    printf "  %-22s  %-22s  %-22s\n" "Metric" "In-Sample (IS)" "Out-of-Sample (OOS)"
    printf "  %-22s  %-22s  %-22s\n" "----------------------" "----------------------" "----------------------"
    printf "  %-22s  %-22s  %-22s\n" "Period" "$IS_START → $IS_END" "$OOS_START → present"
    printf "  %-22s  %-21s%%  %-21s%%\n" "Total Return"  "$IS_RETURN"   "$OOS_RETURN"
    printf "  %-22s  %-21s%%  %-21s%%\n" "Max Drawdown"  "$IS_DRAWDOWN" "$OOS_DRAWDOWN"
    printf "  %-22s  %-22s  %-22s\n"   "Sharpe Ratio"  "$IS_SHARPE"   "$OOS_SHARPE"
    printf "  %-22s  %-22s  %-22s\n"   "Total Trades"  "$IS_TRADES"   "$OOS_TRADES"
    printf "  %-22s  %-21s%%  %-21s%%\n" "Win Rate"      "$IS_WINRATE"  "$OOS_WINRATE"
    printf "  %-22s  %-22s  %-22s\n"   "Win/Loss Ratio" "$IS_WINLOSS" "$OOS_WINLOSS"
done <<< "$TOP_LINES"

echo ""
echo "------------------------------------------------------------"
echo "  INTERPRETATION"
echo "------------------------------------------------------------"
echo "  OOS Sharpe > 0 and trades fired  →  Strategy is ROBUST."
echo "  OOS performance ~50%+ of IS      →  Strong robustness."
echo "  OOS trades = 0                   →  No regime trigger in"
echo "                                      this period, OR gate"
echo "                                      too tight. Check RSI."
echo "  OOS performance collapses        →  Likely CURVE-FITTED."
echo "  Consistent across top-N params   →  Higher confidence."
echo "============================================================"
echo ""
