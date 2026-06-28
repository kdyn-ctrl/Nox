#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# setup_ibkr_vendor.sh — Download and unpack IBKR TWS API 9.81 C++ source.
#
# Run once from the repo root before building with -DIBKR_ENABLED:
#   chmod +x execution/setup_ibkr_vendor.sh
#   ./execution/setup_ibkr_vendor.sh
#
# After this script completes, build the IBKR-enabled engine with:
#   cd execution
#   g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -DIBKR_ENABLED \
#       -I. -I../shared \
#       -I third_party/twsapi/source/cppclient/client \
#       main.cpp IBKRClient.cpp \
#       third_party/twsapi/source/cppclient/client/*.cpp \
#       -lssl -lcrypto -lpthread -lsqlite3 \
#       -o nox_engine_ibkr
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR_DIR="$SCRIPT_DIR/third_party/twsapi"

echo ">>> Setting up IBKR TWS API 9.81 under $VENDOR_DIR"
mkdir -p "$VENDOR_DIR"
cd "$VENDOR_DIR"

ZIP="twsapi_macunix.981.01.zip"
URL="https://interactivebrokers.github.io/downloads/$ZIP"

if [ -d "source/cppclient/client" ]; then
    echo ">>> TWS API already unpacked. Skipping download."
else
    if command -v curl &>/dev/null; then
        curl -L "$URL" -o "$ZIP"
    elif command -v wget &>/dev/null; then
        wget "$URL" -O "$ZIP"
    else
        echo "ERROR: Neither curl nor wget is available. Download manually:"
        echo "  $URL"
        echo "Place the zip at: $VENDOR_DIR/$ZIP"
        exit 1
    fi

    unzip -q "$ZIP"
    rm -f "$ZIP"
    echo ">>> TWS API unpacked."
fi

CLIENT_DIR="$VENDOR_DIR/source/cppclient/client"
if [ ! -f "$CLIENT_DIR/EWrapper.h" ]; then
    echo "ERROR: Expected $CLIENT_DIR/EWrapper.h not found."
    echo "Check the zip structure — IBKR may have changed their archive layout."
    exit 1
fi

echo ""
echo "✅ TWS API 9.81 ready at: $CLIENT_DIR"
echo ""
echo "Build command:"
echo "  cd $SCRIPT_DIR"
echo "  g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -DIBKR_ENABLED \\"
echo "      -I. -I../shared \\"
echo "      -I $CLIENT_DIR \\"
echo "      main.cpp IBKRClient.cpp \\"
echo "      $CLIENT_DIR/*.cpp \\"
echo "      -lssl -lcrypto -lpthread -lsqlite3 \\"
echo "      -o nox_engine_ibkr"
echo ""
echo "Then set EXECUTION_VENUE=ibkr and IBKR_GATEWAY_PORT=4002 (paper) in .env"
echo "and restart the execution-engine container."
