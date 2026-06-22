#!/bin/bash

set -e

echo "🏗️  Building test executables..."
mkdir -p build

# Compile tests
g++ -std=c++17 -pthread -o build/test_regime tests/test_regime.cpp
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
g++ -std=c++17 -pthread -o build/test_mcpt_example mcpt_example.cpp mcpt.cpp
g++ -std=c++17 -pthread -o build/mcpt_main main.cpp mcpt.cpp

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

echo "✨ All tests passed successfully!"
