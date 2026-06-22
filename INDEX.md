# Nox Project Structure

## 📁 Quick Navigation

### Core Configuration
- **[README.md](README.md)** — Main project overview
- **[docker-compose.yml](docker-compose.yml)** — Service orchestration (C++ execution engine, Python data engines, heartbeat monitor)
- **[CMakeLists.txt](CMakeLists.txt)** — C++ build configuration
- **[.env.example](.env.example)** — Environment variables template

---

## 🚀 Core Services

### `/execution` — Execution Engine (C++)
Webhook-enabled order execution and risk management
- Listens on **port 8080** (exposed to host)
- Receives JSON webhooks from TradingView
- Implements Kelly sizing and trailing stops
- Communicates with Alpaca broker API

### `/analyst` — Analyst Brain (C++)
Market analysis and regime classification
- Runs on 6-hour cycles (configurable)
- Outputs trade signals and alerts to Telegram
- Depends on data engines for market context

### `/china_data_engine` — China Market Data (Python)
Scrapes Chinese financial data via APScheduler
- East Money, Cailian Press, NBS PMI, PBOC LPR
- 15-minute refresh cycle
- Internal-only (port 8000 not exposed)

### `/america_data_engine` — US Market Data (Python)
Alpaca API integration and equity data caching
- 15-minute refresh cycle
- Internal-only (port 8000 not exposed)

### `/heartbeat` — Monitoring Service (Python)
System health checks and Telegram notifications
- Monitors all data engines
- Sends alerts on failures
- Runs independently

### `/backtest-engine` — Backtesting Engine (C++)
Historical strategy testing and out-of-sample validation
- Generates performance metrics
- Helps validate regime classification logic

---

## 📊 Data & Logs

### `/data`
Historical market data, backtest results, regime classifications
- Downloaded via `scripts/download_data.py`

### `/logs`
Runtime logs from all services
- Execution engine actions
- Analyst decisions
- Error tracking

### `/shared`
Shared C++ utilities and headers
- JSON parsing (nlohmann)
- Common data structures
- Utility functions

---

## 🧪 Testing

Location: **`/docs/testing/`**

| Guide | Purpose |
|-------|---------|
| [TEST_GUIDE.md](docs/testing/TEST_GUIDE.md) | Complete testing overview |
| [EXECUTION_TEST_GUIDE.md](docs/testing/EXECUTION_TEST_GUIDE.md) | Test execution engine webhooks |
| [ANALYST_TEST_GUIDE.md](docs/testing/ANALYST_TEST_GUIDE.md) | Test analyst brain signals |
| [HEARTBEAT_TEST_GUIDE.md](docs/testing/HEARTBEAT_TEST_GUIDE.md) | Test monitoring service |
| [DATA_ENGINE_TEST_GUIDE.md](docs/testing/DATA_ENGINE_TEST_GUIDE.md) | Test data scraping |
| [BACKTEST_TEST_GUIDE.md](docs/testing/BACKTEST_TEST_GUIDE.md) | Test backtesting engine |
| [TESTING_PHILOSOPHY.md](docs/testing/TESTING_PHILOSOPHY.md) | Testing principles |
| [TEST_MAINTENANCE_GUIDE.md](docs/testing/TEST_MAINTENANCE_GUIDE.md) | Maintaining test suites |
| [TEST_UPDATE_CHECKLIST.md](docs/testing/TEST_UPDATE_CHECKLIST.md) | Checklist for new features |

**Run all tests:**
```bash
./scripts/run_tests.sh
```

**Test source files:** `/tests/`
- `test_kelly_sizing.cpp` — Position sizing logic
- `test_regime.cpp` — Market regime classification

---

## 📚 Documentation

Location: **`/docs/`**

| Document | Purpose |
|----------|---------|
| [NOX_USER_GUIDE.md](docs/NOX_USER_GUIDE.md) | System setup and operation |
| [DOCUMENTATION_OVERVIEW.md](docs/DOCUMENTATION_OVERVIEW.md) | Architecture and design |
| [DOCUMENTATION_SUMMARY.txt](docs/DOCUMENTATION_SUMMARY.txt) | Quick reference |

---

## 📈 Trading

Location: **`/trading/`**

### `openclaw_weighted_alpha.pinescript`
**OpenClaw Weighted Alpha** — 4H Momentum + Volatility Strategy

**Assets:** SPY, XOM, NVDA, BTCUSD  
**Signals:** EMA(9) x EMA(21) crossover + volume spike filter  
**Risk Tiers:** 
- **Tier 3** (Grade A Gamble): Entry with volume spike
- **Tier 1** (Standard): Entry without spike

**Webhook Target:** `http://<YOUR_VPS_IP>:8080/webhook`

---

## 🔧 Utilities & Scripts

Location: **`/scripts/`**

| Script | Purpose |
|--------|---------|
| `download_data.py` | Fetch historical market data |
| `refresh_data.sh` | Update all data engines |
| `refresh_cron.sh` | Scheduled background refresh |
| `run_tests.sh` | Execute full test suite |

**Usage:**
```bash
# Download data
python3 scripts/download_data.py

# Refresh all data
./scripts/refresh_data.sh

# Run tests
./scripts/run_tests.sh
```

---

## 🏗️ Build & Development

### Compilation
```bash
mkdir -p build && cd build
cmake ..
make
```

### Source Files
- **`main.cpp`** — Main application entry point
- **`mcpt.cpp`** / **`mcpt.h`** — Monte Carlo Path Tracing utilities
- **`mcpt_example.cpp`** — MCPT usage examples
- **/tests/** — Unit test implementations

### Build Artifacts
- **/build/** — Compiled executables and test binaries

---

## 🌐 Network Topology

```
TradingView Webhooks
        ↓
Execution Engine (localhost:8080) ← Direct IP: <YOUR_VPS_IP>:8080
        ↓
Analyst Brain ← Data Engines (Internal Network)
        ↓
Alpaca API + Telegram Notifications
```

**Key Points:**
- Execution engine exposed directly (no Traefik proxy)
- Data engines on internal `nox_net` network
- All services in Docker via `docker-compose.yml`

---

## ⚡ Quick Start

```bash
# Start system
docker-compose up -d

# Check logs
docker-compose logs -f execution-engine

# Stop system
docker-compose down
```

---

## 📋 File Organization

```
Nox/
├── docker-compose.yml          # Service orchestration
├── CMakeLists.txt              # C++ build config
├── README.md                   # Main overview
├── INDEX.md                    # This file
│
├── execution/                  # Order execution (C++)
├── analyst/                    # Regime analysis (C++)
├── backtest-engine/            # Backtesting (C++)
├── china_data_engine/          # China data (Python)
├── america_data_engine/        # US data (Python)
├── heartbeat/                  # Monitoring (Python)
│
├── docs/                       # Documentation
│   ├── NOX_USER_GUIDE.md
│   ├── DOCUMENTATION_*.md
│   └── testing/                # Test guides
│
├── trading/                    # Strategies
│   └── openclaw_weighted_alpha.pinescript
│
├── scripts/                    # Utility scripts
│   ├── download_data.py
│   ├── refresh_*.sh
│   └── run_tests.sh
│
├── shared/                     # C++ utilities
├── data/                       # Market data
├── logs/                       # Runtime logs
├── tests/                      # Unit tests
└── build/                      # Build artifacts
```

---

**Last Updated:** 2026-06-22  
**Version:** 1.0
