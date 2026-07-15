#!/bin/bash

set -e

echo "🏗️  Building test executables..."
mkdir -p build

# Compile tests
g++ -std=c++17 -pthread -o build/test_regime tests/test_regime.cpp
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
g++ -std=c++17 -pthread -o build/test_mcpt_example mcpt_example.cpp mcpt.cpp
g++ -std=c++17 -pthread -o build/mcpt_main main.cpp mcpt.cpp

# Quality-driven DTE selection + DTE/quality-tiered sizing (needs OpenSSL for
# httplib's TU, exercised indirectly via buildContractParams()/assembleSignal()).
g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -Iexecution \
    execution/test/test_quality_dte_sizing.cpp \
    -o build/test_quality_dte_sizing -lssl -lcrypto -lpthread

# Sector/trend macro gate — EMA math + conflict-decision logic wired through
# OptionsSignalGenerator (needs OpenSSL for httplib's TU).
g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -Iexecution \
    execution/test/test_sector_trend_gate.cpp \
    -o build/test_sector_trend_gate -lssl -lcrypto -lpthread

# Skeptic intelligence decision layer — wires WS2 (alt-macro) + WS3 (insider) +
# China information-lag into sizing/gating. Pure aggregation math, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_skeptic_intelligence.cpp \
    -o build/test_skeptic_intelligence

# Backtester adverse-selection fill model — pure math, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_backtest_fill_model.cpp \
    -o build/test_backtest_fill_model

# Backtester error-injection resilience model — pure deterministic math, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_backtest_error_injection.cpp \
    -o build/test_backtest_error_injection

echo ""
echo "✅ Build complete!"
echo ""

# Run tests
echo "🧪 Running RegimeStateMachine tests..."
build/test_regime
echo ""

echo "🧪 Running Kelly sizing tests..."
build/test_kelly_sizing
echo ""

echo "🧪 Running MCPT example..."
build/test_mcpt_example
echo ""

echo "🏃 Running main MCPT demo..."
build/mcpt_main
echo ""

echo "🧪 Running quality-driven DTE + sizing tests..."
build/test_quality_dte_sizing
echo ""

echo "🧪 Running sector/trend gate tests..."
build/test_sector_trend_gate
echo ""

echo "🧪 Running Skeptic intelligence decision-layer tests..."
build/test_skeptic_intelligence
echo ""

echo "🧪 Running backtester adverse-selection fill model tests..."
build/test_backtest_fill_model
echo ""

echo "🧪 Running backtester error-injection resilience tests..."
build/test_backtest_error_injection
echo ""

echo "✨ All tests passed successfully!"
