#!/bin/bash
# Build the Nox execution engine
set -euo pipefail

echo "[BUILD] Compiling execution engine..."

g++ -std=c++17 -O3 -Wall -Wextra \
    -I.. \
    -I../shared \
    main.cpp \
    PositionManager.cpp \
    -o nox_engine \
    -lssl -lcrypto -lpthread -lsqlite3

echo "[BUILD] Done. Binary: ./nox_engine"
