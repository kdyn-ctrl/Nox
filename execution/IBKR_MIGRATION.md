# IBKR Execution Migration (PRIVATE)

> Private forward development. Lives on the `nocturnal` branch only — **never**
> merge into the public `main` showcase. Replaces the Alpaca REST execution path
> with a native Interactive Brokers TWS C++ API integration.

## Why migrate

| Concern             | Alpaca (current, public)        | IBKR (this work)                         |
|---------------------|---------------------------------|------------------------------------------|
| Transport           | REST polling                    | Persistent TCP socket, async callbacks   |
| Fills               | Market-order biased             | Native limit / multi-leg routing         |
| Market data         | REST snapshot/quote             | Streaming ticks (`tickPrice`/`tickSize`) |
| Venues              | US equity options only          | Futures, index options, non-US venues    |
| Exec reporting      | Order status polling            | `execDetails` + `orderStatus` callbacks  |

## Components (`IBKRClient.hpp` / `IBKRClient.cpp`)

- **`IBKRWrapper : DefaultEWrapper`** — async socket callbacks. Overrides the five
  required by the migration brief (`nextValidId`, `orderStatus`, `openOrder`,
  `execDetails`, `error`) plus `tickPrice`/`tickSize`. Derives from
  `DefaultEWrapper` so the ~150 unused callbacks get no-op defaults.
- **`IBKRConnection : EClientSocket`** — owns the connection lifecycle for the
  headless IB Gateway (paper 4002 / live 4001): `eConnect` → `EReader` thread →
  single message-pump thread; idempotent `disconnect()`.
- **`ExecutionLogger`** — thread-safe sqlite3 logger (own mutex) for fills, order
  status, and errors. Decoupled from `PositionManager` so the unit is standalone.

## Threading model

| Path                | Mechanism                          | Producer → Consumer            |
|---------------------|------------------------------------|--------------------------------|
| Market ticks        | lock-free SPSC ring buffer (4096)  | pump thread → main exec thread |
| Order/conn state    | `std::mutex` + `std::atomic<OrderId>` | pump thread ↔ main exec thread |
| DB logging          | `ExecutionLogger` mutex            | any thread                     |

All `EWrapper` callbacks run on the single message-pump thread, which is the sole
producer for the tick ring buffer (SPSC invariant holds). Ticks are dropped on
overflow rather than blocking the socket path.

## Target API version

TWS API stable **9.81**: `filled`/`remaining` are `double`, `error` is the
3-argument overload. For ≥10.10 switch those to `Decimal` and use the 5-argument
`error` (adds `time_t` + `advancedOrderRejectJson`). See the build note at the
bottom of `IBKRClient.hpp`.

## Build

```bash
g++ -std=c++17 -pthread \
    -I third_party/twsapi/source/cppclient/client \
    execution/IBKRClient.cpp \
    third_party/twsapi/source/cppclient/client/*.cpp \
    -lsqlite3 -lssl -lcrypto -o nox_ibkr_test
```

Validated against stub TWS headers: compiles clean; the SPSC ring buffer passes a
500k-item concurrent producer/consumer test (no loss/duplication); sqlite logger
round-trips.

## Remaining to wire up (not done)

1. Order construction helpers (Contract/Order builders for the 8 strategies).
2. Map `OptionsSignal` → IBKR contract + order (mirror `OptionsOrderRouter`).
3. Replace `PositionManager`'s Alpaca quote fetch with IBKR streaming ticks.
4. Integrate into `main.cpp` behind a venue flag (Alpaca | IBKR).
5. Vendor the TWS API source under `third_party/` and add a CMake/Make target.
