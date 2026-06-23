# Nox — Autonomous Quantitative Trading System

Nox is a containerized, multi-agent quantitative trading framework built for automated macro regime classification, risk-managed position sizing, and low-latency order execution. The system is written in C++ and Python, deployed across three isolated Docker containers that communicate exclusively over a private internal network.

Designed and maintained by a self-directed quantitative developer targeting systematic, rules-based exposure to US equity, crypto, and **Chinese A-share markets**.

---

## Table of Contents
1. [System Overview](#system-overview)
2. [Full Pipeline — Step by Step](#full-pipeline--step-by-step)
3. [Container Architecture](#container-architecture)
4. [Data Flow Diagram](#data-flow-diagram)
5. [Three-Gate Execution Model](#three-gate-execution-model)
6. [Kelly Position Sizing](#kelly-position-sizing)
7. [Regime State Machine](#regime-state-machine)
8. [Chinese A-Share Constraints](#chinese-a-share-constraints)
9. [Observability & Memory Bank](#observability--memory-bank)
10. [Safety Rules & Design Contracts](#safety-rules--design-contracts)
11. [Deployment](#deployment)

---

## System Overview

Nox separates concerns across three autonomous agents. No single container has full system authority — the analyst cannot execute trades, the execution engine cannot generate signals, and the heartbeat monitor cannot touch orders. Each agent has one job and fails loudly if it cannot do it.

| Container | Language | Role |
|---|---|---|
| `analyst-brain` | C++ | Macro data ingestion, regime classification, signal generation |
| `execution-engine` | C++ | Signal validation, risk sizing, order routing |
| `heartbeat-monitor` | Python | Intelligence reporting, SEC radar, conversational interface |

---

## Full Pipeline — Step by Step

This is the exact sequence of operations from raw market data to a live order, in order:

**Step 1 — Market Data Ingestion** `analyst/main.cpp`
- Fetches the current VIX index from Yahoo Finance
- Fetches SPY daily closes (1 year of bars) from Yahoo Finance
- Computes the 200-day Simple Moving Average from the raw bar data
- If either fetch fails, the cycle is marked invalid and retried in 5 minutes — no stale data is ever forwarded

**Step 2 — Regime Classification** `shared/RegimeStateMachine.hpp`
- Feeds VIX and SPY/SMA values into the Regime State Machine
- Classifies current market conditions based on optimized thresholds (see `RegimeStateMachine.hpp` for details)
- Assigns a capital multiplier (1.0 for RISK_ON, 0.5 for TRANSITION, 0.0 for RISK_OFF) to the strategy output

**Step 3 — Signal Serialization & Transmission** `analyst/main.cpp`
- Serializes the full regime result into a validated JSON payload
- Transmits via HTTP POST to `execution-engine:8080/webhook` over the Docker bridge network
- Retries up to 5 times with exponential backoff (2s → 4s → 8s → 16s → 32s) on failure
- Logs `[CRITICAL]` and halts if all retries are exhausted

**Step 4 — Schema Gate** `execution/main.cpp`
- Execution engine receives the payload
- Parses strictly using `nlohmann::json` — any malformed input is rejected with HTTP 400 before any business logic runs
- Array and single-object payloads are both supported

**Step 5 — Auth Gate** `execution/main.cpp`
- Validates `secret_key` field against `WEBHOOK_SECRET_TOKEN` environment variable
- Mismatched payloads are silently dropped and logged as `[WARN]` — no HTTP error is returned to avoid fingerprinting the auth boundary

**Step 6 — Logic Gate (RSI Momentum Filter)** `execution/main.cpp`
- `BUY` signals with RSI > 70 are blocked (overbought)
- `SELL` signals with RSI < 30 are blocked (oversold)
- Blocked signals fire a Telegram alert and halt — they do not proceed to sizing

**Step 7 — Regime Gate** `execution/main.cpp` + `execution/RegimeStateMachine.hpp`
- Re-evaluates regime state at order time using the signal's embedded VIX and SPY data
- `RISK_OFF` regime immediately halts the order with a Telegram alert
- `TRANSITION` regime scales equity down by 50% before sizing

**Step 8 — Live Equity Fetch** `execution/main.cpp`
- Fetches live portfolio value from Alpaca in real time
- Retries up to 3 times with exponential backoff (2s → 4s → 8s)
- If equity cannot be confirmed, the order is aborted — no estimated or cached equity value is ever used

**Step 9 — Kelly Position Sizing** `execution/main.cpp`
- Applies the Kelly Criterion formula: `K% = W - ((1 - W) / R)`
- Scales result by the dynamic `KELLY_FRACTION` safety multiplier (injected via env var)
- Hard caps: maximum 10% portfolio risk per trade, minimum 1%
- If Kelly output is zero or negative (no statistical edge), the order is aborted with a `[CRITICAL]` log
- Zero-share allocations are blocked — a forced 1-share purchase that exceeds the 10% cap is never permitted

**Step 9a — Chinese A-Share Board-Lot Truncation** `execution/main.cpp` *(CN-RULE-001)*
- After Kelly sizing, the raw share quantity is truncated down to the nearest multiple of `CN_BOARD_LOT_SIZE` (default 100)
- A-share exchanges reject any order not in a whole lot (一手, 100 shares minimum)
- If truncation reduces the quantity to zero the trade is aborted with `[CRITICAL]` log and Telegram alert

**Step 9b — Chinese A-Share T+1 Settlement Gate** `execution/main.cpp` *(CN-RULE-002)*
- On every confirmed BUY, the entry date is recorded in memory and persisted to `CN_POSITIONS_PATH`
- On every SELL signal, the engine verifies the sell date is strictly later than the entry date
- If `sell_date == entry_date` the signal is discarded — same-day round-trips are prohibited on all A-share boards
- State survives engine restarts; stale entries (entry_date before today) are pruned at load time

**Step 10 — Order Routing** `execution/main.cpp`
- Constructs a validated Alpaca order payload with the Kelly-calculated share quantity
- Routes to Alpaca paper or live API (controlled by `ALPACA_BASE_URL` environment variable)
- Order confirmation or rejection is logged and sent to Telegram in both cases

---

## Container Architecture

```text
Nox/
├── analyst/          # C++ — Macro analyst agent
│   ├── main.cpp
│   ├── RegimeStateMachine.hpp
│   ├── httplib.h
│   ├── nlohmann/json.hpp
│   └── Dockerfile
│
├── execution/        # C++ — Execution & risk engine
│   ├── main.cpp
│   ├── RegimeStateMachine.hpp
│   └── Dockerfile
│
├── heartbeat/        # Python — Intelligence & monitoring agent
│   ├── monitor.py
│   └── Dockerfile
│
├── data/             # Persistent Docker volume
│   └── memory_bank.db  # SQLite — audit reports, trade history, SEC filings
│
├── docker-compose.yml
└── .env              # Runtime credentials (never committed)
```

---

## Data Flow Diagram

```
╔══════════════════════════════════════════════════════════════╗
║                     RAW DATA SOURCES                         ║
║  Yahoo Finance (VIX, SPY)        Alpaca News API             ║
║  SEC EDGAR (8-K filings)         Anthropic Claude API        ║
╚══════════════╤═══════════════════════════╤═══════════════════╝
               │                           │
               ▼                           ▼
╔══════════════════════╗     ╔═════════════════════════════════╗
║   analyst-brain      ║     ║      heartbeat-monitor          ║
║   (C++)              ║     ║      (Python / Nox)             ║
║                      ║     ║                                 ║
║  VIX + SPY fetch     ║     ║  SEC Radar (30s poll)           ║
║  200-day SMA calc    ║     ║  Daily Scout (10am ET)          ║
║  Regime evaluation   ║     ║  Claude NLP analysis            ║
║  JSON serialization  ║     ║  SQLite memory bank             ║
╚══════════╤═══════════╝     ║  Telegram bot interface         ║
           │  HTTP POST      ╚═════════════════════════════════╝
           │  :8080/webhook
           ▼
╔══════════════════════╗
║   execution-engine   ║
║   (C++)              ║
║                      ║
║  [1] Schema Gate     ║
║  [2] Auth Gate       ║
║  [3] RSI Gate        ║
║  [4] Regime Gate     ║
║  [5] Equity Fetch    ║
║  [6] Kelly Sizing    ║
║  [7] Order Routing   ║
╚══════════╤═══════════╝
           │
           ▼
     Alpaca Markets
     (Paper / Live)
```

---

## Three-Gate Execution Model

Every incoming signal passes through three sequential gates before any capital is touched. A failure at any gate aborts the order immediately.

| Gate | Check | Failure Response |
|---|---|---|
| **Schema Gate** | Is the JSON payload structurally valid? | HTTP 400, logged as `[ERROR]` |
| **Auth Gate** | Does `secret_key` match `WEBHOOK_SECRET_TOKEN`? | Silent drop, logged as `[WARN]` |
| **Logic Gate** | Does RSI confirm the trade direction? | Order blocked, Telegram alert fired |

---

## Kelly Position Sizing

All position sizes are computed deterministically. No AI model influences share quantity.

```
Raw Kelly  =  W - ((1 - W) / R)

Where:
  W  =  KELLY_WIN_RATE          (injected at runtime)
  R  =  KELLY_WIN_LOSS_RATIO    (injected at runtime)

Adjusted Kelly  =  Raw Kelly × KELLY_FRACTION   (configurable safety multiplier)
Dollar Risk     =  Live Equity × Adjusted Kelly
Share Quantity  =  floor(Dollar Risk / Current Price)

Hard caps:
  Maximum risk per trade:  10% of live portfolio equity
  Minimum risk per trade:   1% of live portfolio equity
  Zero-share result:        order aborted, [CRITICAL] logged
```

---

## Regime State Machine

The `RegimeStateMachine` (defined in `shared/RegimeStateMachine.hpp`) is the single source of truth for market regime classification. It runs in both the analyst and execution containers. The analyst generates the initial regime signal, and the execution engine re-validates it at order time to ensure conditions have not changed.

The machine uses a combination of the VIX index and the S&P 500's price relative to its 200-day moving average to classify the market into one of three states. The specific VIX thresholds and SMA buffer percentages are determined through walk-forward optimization and are documented within the `RegimeStateMachine.hpp` file itself. This ensures that the system's behavior is consistently governed by back-tested parameters.

| State | Capital Multiplier | Effect |
|---|---|---|
| `RISK_ON` | 1.0 | Full capital deployment |
| `TRANSITION` | 0.5 | Reduced (50%) capital deployment |
| `RISK_OFF` | 0.0 | All new entries halted |

---

## Chinese A-Share Constraints

The execution engine enforces two rules that are unique to Chinese A-share (沪深 A股) markets and have no equivalent in US equity execution.

### CN-RULE-001 — Board-Lot Truncation (手, shǒu)

A-share exchanges accept orders only in multiples of 100 shares (one lot / 一手). After the Kelly calculator (or fixed-tier sizing) produces a raw share quantity, the engine truncates to the nearest lower multiple before constructing the order payload.

```
Raw Kelly qty:  345 shares
Board-lot size: 100
Submitted qty:  300 shares   ← 300 = (345 ÷ 100) × 100
```

If truncation reduces the quantity to zero (e.g., Kelly allocates 60 shares), the trade is **aborted** — the engine will not submit a sub-lot order or round up. A `[CRITICAL]` log and Telegram alert are fired.

The lot size is configurable via the `CN_BOARD_LOT_SIZE` environment variable (default: `100`). This lets the engine adapt if future board rules change without requiring a code rebuild.

### CN-RULE-002 — T+1 Settlement (T+1交割制度)

Chinese A-share markets prohibit same-day round-trips. A position purchased on trading day **T** cannot be sold until day **T+1** or later. The engine enforces this at the signal level:

1. On a confirmed BUY, the entry date is recorded in an in-memory map and persisted to disk at `CN_POSITIONS_PATH` (default: `/tmp/china_positions.json`).
2. On every SELL signal, the engine looks up the entry date for that ticker. If `sell_date == entry_date`, the signal is **silently discarded** — no order is placed, a `[CRITICAL]` log is emitted, and a Telegram alert is sent.
3. On a confirmed SELL, the entry record is evicted from both memory and disk.
4. On engine restart, the persistence file is reloaded. Records with an entry date before today are automatically pruned (they are at minimum T+1 old and the restriction has lifted).

**Backtester mode:** The signal payload may include a `trade_date` field (`"YYYY-MM-DD"`) to inject the simulation clock date. If absent, the engine uses the current system date. All backtester-generated signals **must** include this field or the T+1 gate will compare against today's wall-clock date, which is almost always T+N and will never block correctly.

**Important:** The gate has no override flag. To manually close a same-day position (e.g., a stop-loss triggered by the broker), use the broker dashboard directly — this is correct behavior, as the exchange itself enforces T+1 and will reject a same-day sell.

### Environment Variables (China-Specific)

| Variable | Default | Description |
|---|---|---|
| `CN_BOARD_LOT_SIZE` | `100` | Lot size for board-lot truncation (CN-RULE-001) |
| `CN_POSITIONS_PATH` | `/tmp/china_positions.json` | Persistence path for T+1 position state (CN-RULE-002) |

---

## Observability & Memory Bank

Every order, regime transition, and SEC alert produces both a structured stdout log and a Telegram notification. Silent execution is not permitted by design.

**Telegram Commands (via Nox)**
| Command | Action |
|---|---|
| `/status` | Live system health — container state, audit count, Kelly engine status |
| `/history [n]` | Last N daily audit reports from the memory bank (default 5, max 20) |
| Free text | Conversational query against portfolio state, market conditions, or system status |

**SQLite Memory Bank** — `/app/data/memory_bank.db`
| Table | Contents |
|---|---|
| `daily_audits` | Every daily Scout report — timestamp, tickers scanned, full Claude analysis |
| `trade_history` | Every executed order — ticker, action, price, RSI, Kelly ratio, P&L |
| `processed_filings` | SEC filing IDs already processed — prevents duplicate alerts |

**Log Format:** `[LEVEL] [COMPONENT] Message`
Levels: `INFO` / `WARN` / `ERROR` / `FATAL` / `CRITICAL`

---

## Safety Rules & Design Contracts

All rules are documented in full in the `Rules` file. Summary of non-negotiable constraints:

| Rule | Constraint |
|---|---|
| RULE-001 | Analyst cycle interval comes from `ANALYST_CYCLE_HOURS` env var — never hardcoded |
| RULE-005 | Kelly sizing enforces 10% hard cap — zero-share results abort the trade, never round up |
| RULE-007 | Failed payloads retry with exponential backoff — no silent cycle skips |
| RULE-008 | Every HTTP call has explicit connect + read timeouts — no indefinite blocks |
| RULE-009 | All credentials come from environment variables — no secrets in source code |
| RULE-013 | Every action produces a stdout log AND a Telegram notification — no silent execution |
| RULE-014 | All trading validated on Alpaca Paper first — `ALPACA_BASE_URL` controls paper vs live |
| RULE-015 | AI models never determine trade size, side, or price — only deterministic C++ does |
| RULE-018 | Zero-share Kelly results are hard-blocked — no forced minimum purchase |
| CN-RULE-001 | A-share orders truncated to nearest lot (`CN_BOARD_LOT_SIZE`); sub-lot results abort the trade |
| CN-RULE-002 | T+1 gate blocks same-day sells; entry dates persisted across restarts via `CN_POSITIONS_PATH` |

---

## Deployment

**Prerequisites:** Docker, Docker Compose, a `.env` file with the following variables:

```env
ALPACA_API_KEY=
ALPACA_SECRET_KEY=
ALPACA_BASE_URL=https://paper-api.alpaca.markets
WEBHOOK_SECRET_TOKEN=
KELLY_WIN_RATE=
KELLY_WIN_LOSS_RATIO=
KELLY_FRACTION=0.15
TELEGRAM_BOT_TOKEN=
TELEGRAM_CHAT_ID=
ANTHROPIC_API_KEY=
ANALYST_CYCLE_HOURS=6

# Chinese A-share constraints (optional — defaults shown)
CN_BOARD_LOT_SIZE=100
CN_POSITIONS_PATH=/tmp/china_positions.json
```

**Start the system:**
```bash
docker compose up -d --build
```

**Rebuild a single container after a code change:**
```bash
docker compose up -d --build heartbeat-monitor
docker compose up -d --build execution-engine
docker compose up -d --build analyst-brain
```

**View live logs:**
```bash
docker logs nox_heartbeat -f
docker logs nox_execution -f
docker logs nox_analyst -f
```

**Query the memory bank directly:**
```bash
python3 -c "
import sqlite3
conn = sqlite3.connect('/root/Nox/data/memory_bank.db')
for row in conn.execute('SELECT id, timestamp, claude_analysis FROM daily_audits ORDER BY timestamp DESC LIMIT 5'):
    print(f'--- {row[0]} | {row[1]} ---')
    print(row[2])
conn.close()
"
```
