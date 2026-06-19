#!/bin/bash
# =============================================================================
# build.sh — Compile the Nox backtester
# =============================================================================
set -euo pipefail

echo "[BUILD] Compiling backtester..."

g++ -std=c++17 -O3 -Wall -Wextra \
    -I. \
    main.cpp \
    -o backtester

echo "[BUILD] Done. Binary: ./backtester"
