# Test Guide for Backtest Engine

The Backtest Engine simulates the Nox trading strategy over historical data, applying all live strategy rules day-by-day to measure historical performance.

## Components Tested

- **CSV Parser**: Historical data loading and validation
- **Regime Classification**: Rules applied to historical data
- **Trade Simulation**: Entry/exit logic, P&L calculation
- **Parameter Tuning**: Optimization mode for grid search
- **Output Reporting**: Trade logs and summary statistics

## Building the Backtester

```bash
cd /root/Nox/backtest-engine
./build.sh

# Or manually:
g++ -std=c++17 -o ../build/backtester main.cpp
```

## Unit Tests

### CSV Parser Test
```bash
g++ -std=c++17 -o build/test_csv_parser tests/test_csv_parser.cpp \
  backtest-engine/csv_parser.hpp
./build/test_csv_parser
```

**Tests**:
- Parses valid CSV rows without errors
- Rejects rows with missing fields
- Converts string dates to comparable format
- Handles edge case: empty files

**Expected Output**:
```
✓ CSV Parser: loads valid data
✓ CSV Parser: rejects malformed rows
✓ CSV Parser: computes SMA correctly
```

## Manual Integration Tests

### 1. Full Backtest Run
```bash
# Verify data file exists
ls -la ./data/spy_vix_daily.csv

# Run backtest with default parameters
./build/backtester ./data/spy_vix_daily.csv

# Expected output:
# === BACKTEST SUMMARY ===
# Total Trades: XX
# Win Rate: XX.X%
# Avg Win: $XXX.XX
# Avg Loss: -$XXX.XX
# Total P&L: $XXX.XX
# Sharpe Ratio: X.XX
```

### 2. Date Range Test
```bash
# Backtest a specific date range
./build/backtester ./data/spy_vix_daily.csv \
  --start 2023-01-01 --end 2023-12-31

# Verify output shows only trades within date range
# Check trades.csv has no trades before 2023-01-01
```

### 3. Parameter Variation Test
```bash
# Test with custom regime thresholds
./build/backtester ./data/spy_vix_daily.csv \
  --vix 40 \
  --buffer 0.95 \
  --kelly-fraction 0.20

# Verify new thresholds are applied:
# - VIX threshold changed to 40 (from default 35)
# - SMA buffer changed to 0.95 (from default 0.98)
# - Kelly fraction changed to 0.20

# Output should show different trade counts/P&L
```

### 4. Headless (Optimization) Mode
```bash
# Single-line CSV output for optimization script
./build/backtester ./data/spy_vix_daily.csv --headless \
  --vix 35 --buffer 0.98 --kelly-fraction 0.25

# Expected output:
# vix,buffer,kelly,trades,winrate,sharpe,pnl
# 35,0.98,0.25,45,0.531,1.23,15230.45

# This format is machine-readable for grid search
```

### 5. Trades Log Verification
```bash
# Run backtest
./build/backtester ./data/spy_vix_daily.csv

# Examine generated trades.csv
head -20 trades.csv

# Verify format:
# entry_date,entry_price,exit_date,exit_price,shares,pnl
# 2022-03-15,420.50,2022-03-18,425.30,100,480.00
# ...

# Check calculations:
# - PnL = (exit_price - entry_price) * shares
# - Entry/exit dates are in chronological order
# - No negative share quantities
```

### 6. Regime Classification Consistency
```bash
# Run backtest on period where regime classification is known
# (e.g., 2020-02 COVID crash: should be RISK_OFF)

./build/backtester ./data/spy_vix_daily.csv \
  --start 2020-02-15 --end 2020-03-15

# Examine logs for regime transitions
# Check that high-VIX period (Feb-Mar 2020) shows RISK_OFF regime
```

### 7. Portfolio Accounting Test
```bash
# Verify portfolio value calculations are correct

./build/backtester ./data/spy_vix_daily.csv

# Check summary:
# - Starting capital: $100,000.00
# - If total P&L = $+15,230.45
# - Ending capital should be: $115,230.45

# Verify in trades.csv that cumulative P&L makes sense
```

### 8. Edge Case: Very Restrictive Parameters
```bash
# Set extremely tight filters to reduce trades
./build/backtester ./data/spy_vix_daily.csv \
  --vix 20 \
  --buffer 0.99 \
  --kelly-fraction 0.10

# Expected: 0-5 trades (very few conditions met)
# Should not crash even with no trades
```

### 9. Edge Case: Very Loose Parameters
```bash
# Set very loose filters to generate many trades
./build/backtester ./data/spy_vix_daily.csv \
  --vix 50 \
  --buffer 0.80 \
  --kelly-fraction 0.50

# Expected: 200+ trades (very permissive)
# Verify no integer overflow in statistics
```

## Grid Search / Optimization

```bash
# Example optimization loop (bash)
for vix in 30 35 40 45; do
  for buffer in 0.95 0.96 0.97 0.98; do
    for kelly in 0.15 0.20 0.25 0.30; do
      ./build/backtester ./data/spy_vix_daily.csv --headless \
        --vix $vix --buffer $buffer --kelly-fraction $kelly
    done
  done
done > optimization_results.csv

# Parse CSV to find best parameters
head -1 optimization_results.csv > best.csv
sort -t',' -k5 -rn optimization_results.csv | head -1 >> best.csv
cat best.csv
```

## Data File Requirements

The backtest expects a CSV with these columns:
```
date,close,vix,sma_200
2022-01-03,480.50,18.2,465.30
2022-01-04,482.10,17.8,465.80
...
```

**Generate sample data**:
```bash
python3 download_data.py --output ./data/spy_vix_daily.csv
```

## Environment Variables

None required. Backtest is completely self-contained.

## When to Update This Guide

Update this guide when:
1. **CSV format changes** — new columns added/removed
2. **Command-line arguments change** — new flags or defaults
3. **Trade logic changes** — entry/exit conditions modified
4. **Kelly formula changes** — input parameters or calculation changes
5. **Output format changes** — new columns in trades.csv or summary
6. **Parameter ranges change** — new valid min/max values

## Common Failures & Diagnostics

| Symptom | Check | Fix |
|---------|-------|-----|
| `[ERROR] File not found` | CSV path is correct | Use absolute path or verify file exists |
| `[ERROR] CSV parse error on row 5` | Data format matches spec | Check date format (YYYY-MM-DD), numeric columns |
| `Sharpe Ratio: NaN` | Not enough trades for calculation | Use more permissive parameters |
| `Win Rate: 0%` | Strategy losing on all trades | Check if regime thresholds are too strict |
| Trades.csv shows duplicate entries | Index tracking bug | Report with date range that reproduces |
| High trades count but low P&L | Check win rate | Strategy might have high loss magnitude |
| Parameter sensitivity seems wrong | Re-run with single parameter change | Isolate which parameter causes change |

## Testing Checklist Before Deployment

- [ ] Backtest runs without crashing on full data
- [ ] Trades.csv is valid and sorted by date
- [ ] Summary statistics include all metrics (Sharpe, win rate, etc.)
- [ ] Parameter variations produce different results
- [ ] Headless mode produces valid CSV
- [ ] Optimization script can parse results
- [ ] Start/end date filters work correctly
- [ ] Edge cases handled (0 trades, 1000+ trades)
