# Test Guide for Nox

## 📚 All Test Guides

This repository contains comprehensive guides for testing and understanding Nox:

### For Everyone
| Guide | Purpose |
|-------|---------|
| **[NOX_USER_GUIDE.md](NOX_USER_GUIDE.md)** | Non-technical users: What the bot does, how to read alerts, how to monitor |

### For Developers
| Component | Guide | Purpose |
|-----------|-------|---------|
| **Core Logic** | [TEST_GUIDE.md](TEST_GUIDE.md) | RegimeStateMachine, Kelly sizing, MCPT unit tests |
| **Analyst** | [ANALYST_TEST_GUIDE.md](ANALYST_TEST_GUIDE.md) | Data fetching, regime classification, signal transmission |
| **Execution** | [EXECUTION_TEST_GUIDE.md](EXECUTION_TEST_GUIDE.md) | Order validation, routing, position sizing |
| **Backtest** | [BACKTEST_TEST_GUIDE.md](BACKTEST_TEST_GUIDE.md) | Historical simulation, parameter optimization |
| **Data Engines** | [DATA_ENGINE_TEST_GUIDE.md](DATA_ENGINE_TEST_GUIDE.md) | News/macro data services (America & China) |
| **Heartbeat** | [HEARTBEAT_TEST_GUIDE.md](HEARTBEAT_TEST_GUIDE.md) | Monitoring, intelligence reports, SEC radar |
| **Maintenance** | [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) | When and how to update test guides |

**👉 Start here**: If you're adding code, read [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) to understand which guides to update.

---

## Quick Start

Run all tests with:
```bash
./run_tests.sh
```

## Individual Test Targets

### RegimeStateMachine Tests
```bash
g++ -std=c++17 -pthread -o build/test_regime tests/test_regime.cpp
./build/test_regime
```
**Tests:** Market regime classification (RISK_ON/RISK_OFF/TRANSITION) based on VIX and SPY price vs 200-day SMA.

- **RISK_ON**: VIX < 35 AND SPY > 200-SMA (bullish, low volatility)
- **RISK_OFF**: VIX >= 35 OR SPY < 200-SMA*0.98 (crisis mode, sell signal)
- **TRANSITION**: SPY between 200-SMA and 200-SMA*0.98, VIX < 35 (uncertainty)

### Kelly Sizing Tests
```bash
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
./build/test_kelly_sizing
```
**Tests:** Position sizing sanity checks. Ensures we don't trade when portfolio can't afford a single share.

### MCPT (Monte Carlo Permutation Test) Example
```bash
g++ -std=c++17 -pthread -o build/test_mcpt_example mcpt_example.cpp mcpt.cpp
./build/test_mcpt_example
```
**Tests:** MCPT library functionality with 1,000 permutations:
- Single permutation (serial)
- Batch mode with callback
- Parallel mode with thread-level RNG isolation

Verifies that permutations preserve mean and variance of original returns.

### MCPT Main Demo
```bash
g++ -std=c++17 -pthread -o build/mcpt_main main.cpp mcpt.cpp
./build/mcpt_main
```
**Tests:** Basic MCPT usage — shuffles historical returns 1,000 times, confirms statistical properties are preserved.

## What Each Test Verifies

| Test | Purpose | Status |
|------|---------|--------|
| `test_regime` | Regime classification rules | ✅ PASS |
| `test_kelly_sizing` | Position sizing safety | ✅ PASS |
| `test_mcpt_example` | MCPT performance & correctness | ✅ PASS |
| `mcpt_main` | Basic MCPT functionality | ✅ PASS |

## Building Without CMake

The project currently doesn't require CMake. Compile with:
```bash
g++ -std=c++17 -pthread [source files] -o executable_name
```

All tests compile standalone with the C++ standard library.

## Continuous Verification

To rebuild and test after making changes:
```bash
./run_tests.sh
```

The script:
1. Compiles all tests from source
2. Runs each test sequentially
3. Reports pass/fail status
