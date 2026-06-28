# IBKR Execution Migration (PRIVATE)

> Private forward development. Lives on the `nocturnal` branch only — **never**
> merge into the public `main` showcase. Replaces the Alpaca REST execution path
> with a native Interactive Brokers TWS C++ API integration.

## Why migrate

| Concern         | Alpaca (current, paper)         | IBKR (target, live)                      |
|-----------------|----------------------------------|------------------------------------------|
| Transport       | REST polling                     | Persistent TCP socket, async callbacks   |
| Fills           | Market-order only, no improvement| SmartRouting, price improvement, mid fills|
| Options routing | Market orders, basic mleg        | Combo contracts, adaptive algo orders    |
| Market data     | REST snapshot/quote              | Streaming ticks (`tickPrice`/`tickSize`) |
| Venues          | US equity options only           | Futures, index options, non-US venues    |
| Exec reporting  | Order status polling             | `execDetails` + `orderStatus` callbacks  |

## Current status

**Active venue: Alpaca paper** (`ALPACA_BASE_URL=https://paper-api.alpaca.markets`)

IBKR is fully wired into the codebase but not active. Flip to live when paper
trading results justify it.

## Components

### Done ✅

- **`IBKRClient.hpp` / `IBKRClient.cpp`** — async socket layer
  - `IBKRWrapper : DefaultEWrapper` — overrides 7 callbacks; ~150 unused get no-op defaults
  - `IBKRConnection : EClientSocket` — full lifecycle: `eConnect` → EReader thread → message-pump thread; idempotent `disconnect()`
  - `SpscRingBuffer<MarketTick, 4096>` — lock-free producer/consumer ring buffer for tick data
  - `ExecutionLogger` — thread-safe sqlite3 logger for fills, order status, errors

- **`IBKROrderRouter.hpp`** — maps `OptionsSignal` → IBKR `Contract` + `Order` for all 8 strategies
  - Single-leg: `LONG_CALL`, `LONG_PUT`, `CSP`, `CC`
  - Multi-leg: `BULL_CALL_SPREAD`, `BEAR_PUT_SPREAD`, `STRADDLE`, `STRANGLE`

- **`main.cpp` venue flag** — compiled in under `#ifdef IBKR_ENABLED`
  - Reads `EXECUTION_VENUE=alpaca|ibkr` at startup
  - On IBKR: connects to IB Gateway, waits for `nextValidId` handshake
  - BUY webhook signals route to `placeOrder(stock_contract)` instead of Alpaca REST
  - SELL signals alert via Telegram (position query needed to know qty — see remaining work)

- **`setup_ibkr_vendor.sh`** — downloads and unpacks IBKR TWS API 9.81 C++ source into `third_party/`

## Threading model

| Path          | Mechanism                             | Producer → Consumer            |
|---------------|---------------------------------------|--------------------------------|
| Market ticks  | lock-free SPSC ring buffer (4096)     | pump thread → main exec thread |
| Order/conn    | `std::mutex` + `std::atomic<OrderId>` | pump thread ↔ main exec thread |
| DB logging    | `ExecutionLogger` mutex               | any thread                     |

## Target API version

TWS API stable **9.81**: `filled`/`remaining` are `double`, `error` is the
3-argument overload. For ≥10.10 switch to `Decimal` and use the 5-argument
`error` (adds `time_t` + `advancedOrderRejectJson`). See build note in `IBKRClient.hpp`.

## Remaining work

1. **SELL position query** — IBKR has no "close all" REST endpoint. Need to call `reqPositions()` before routing a SELL to know the exact quantity held. Currently Telegram-alerts for manual action.
2. **OptionsSignalGenerator → IBKR** — when `auto_execute=true` and venue is IBKR, `executeSignal()` currently creates a new `OptionsOrderRouter` (Alpaca). Wire it to `IBKROrderRouter` instead.
3. **PositionManager quotes** — `monitor_positions()` fetches quotes via Alpaca REST. Replace with IBKR streaming ticks from the ring buffer when the IBKR path is active.
4. **Vendor TWS API** — run `setup_ibkr_vendor.sh` once before building with `-DIBKR_ENABLED`.

## Activation sequence (when ready for live money)

```bash
# 1. Vendor TWS API source
./execution/setup_ibkr_vendor.sh

# 2. Compile IBKR-enabled binary
g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -DIBKR_ENABLED \
    -I execution -I shared \
    -I execution/third_party/twsapi/source/cppclient/client \
    execution/main.cpp execution/IBKRClient.cpp execution/PositionManager.cpp \
    execution/third_party/twsapi/source/cppclient/client/*.cpp \
    -lssl -lcrypto -lpthread -lsqlite3 -o execution/nox_engine

# 3. Add to .env
# IB_GATEWAY_USER=your_paper_username
# IB_GATEWAY_PASSWORD=your_paper_password
# EXECUTION_VENUE=ibkr
# IBKR_GATEWAY_HOST=ib-gateway
# IBKR_GATEWAY_PORT=4002   # paper=4002, live=4001

# 4. Uncomment ib-gateway service in docker-compose.yml, then:
docker compose up -d
```
