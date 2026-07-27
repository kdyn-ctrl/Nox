# Nox — Quantitative Trading Engine

[![Language](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Python](https://img.shields.io/badge/Python-3.11%2B-blue.svg)](https://www.python.org/)
[![Docker](https://img.shields.io/badge/Docker-Enabled-blue.svg)](https://www.docker.com/)
[![Status](https://img.shields.io/badge/Status-Paper--Trading-green.svg)](#license--status)

A production-grade C++ options and equity trading system with Black-Scholes pricing, regime-based strategy selection, and live Alpaca execution.

**Status:** Paper trading / Educational. Not for live capital without extensive validation.

---

## 📖 Documentation

**Start here:** [docs/README.md](docs/README.md)

Quick links:
- [Getting Started](docs/setup/START_HERE.md) — System overview and daily operations
- [Ticker Strategy](docs/setup/TICKER_STRATEGY.md) — Position sizing and watchlist design
- [Complete Guide](docs/guides/NOX_COMPLETE_GUIDE.md) — Full system architecture
- [Architecture](docs/guides/DESIGN_THINKING.md) — Design decisions and rationale

### 中文文档 (Chinese)
[docs_zh/README.md](docs_zh/README.md)

---

## Quick Start

1. **Learn the system:** [docs/setup/START_HERE.md](docs/setup/START_HERE.md)
2. **Set up watchlist:** [docs/setup/TICKER_STRATEGY.md](docs/setup/TICKER_STRATEGY.md)
3. **Deploy & test:** [docs/guides/PAPER_TRADING_READINESS.md](docs/guides/PAPER_TRADING_READINESS.md)

---

## Key Features

- **Options Engine:** Black-Scholes pricing with all Greeks, 8+ strategies, IV-rank gating
- **Equity Engine:** Kelly-sized position management with rule-based exits (take-profit, stop-loss, RSI, trend-break, time-stop)
- **Multi-source Data:** Alpaca, NewsAPI, Polygon, SEC EDGAR, AkShare (CN), East Money (CN)
- **Execution:** Alpaca REST + WebSocket, rule-based order routing, position tracking
- **Regime Classifier:** VIX/SMA-based market regime (RISK_ON / TRANSITION / RISK_OFF)
- **Backtester:** Walk-forward research harness (model-vs-model Black-Scholes pricing on historical OHLCV — a directional-signal research tool, **not** a go-live P&L estimator; see note below)
- **Monitoring:** Telegram alerts, EOD/EOW reports, health checks

---

## Source Code

- `execution/` — C++17 order execution engine
- `analyst/` — Market analysis and signal generation (C++)
- `america_data_engine/` — US market data pipeline (Python)
- `china_data_engine/` — China market data pipeline (Python)
- `heartbeat/` — System monitoring and Telegram interface (Python)

See [docs/reference/PROJECT_STATUS.md](docs/reference/PROJECT_STATUS.md) for complete architecture.

---

## Repository Structure

```
.
├── README.md                    (you are here)
├── docs/                        (complete documentation)
│   ├── README.md               (documentation index)
│   ├── setup/                  (getting started)
│   ├── guides/                 (how-to & deep dives)
│   ├── reference/              (architecture & status)
│   ├── testing/                (test guides)
│   ├── Rules/                  (signal rules)
│   └── journal/                (development log)
├── docs_zh/                    (中文文档 - Chinese docs)
├── execution/                  (C++ execution engine)
├── analyst/                    (C++ analyst brain)
├── america_data_engine/        (US market data)
├── china_data_engine/          (CN market data)
├── heartbeat/                  (monitoring & alerts)
├── shared/                     (shared C++ headers)
├── docker-compose.yml          (service orchestration)
└── .env.example               (configuration template)
```

---

## Documentation Map

| Purpose | Document |
|---------|----------|
| **I want to understand what this system does** | [START_HERE.md](docs/setup/START_HERE.md) |
| **I want to set up paper trading** | [TICKER_STRATEGY.md](docs/setup/TICKER_STRATEGY.md) + [WATCHLIST_RECOMMENDATIONS.md](docs/setup/WATCHLIST_RECOMMENDATIONS.md) |
| **I want to deploy to live trading** | [PAPER_TRADING_READINESS.md](docs/guides/PAPER_TRADING_READINESS.md) |
| **I want to understand the architecture** | [DESIGN_THINKING.md](docs/guides/DESIGN_THINKING.md) + [PROJECT_STATUS.md](docs/reference/PROJECT_STATUS.md) |
| **I want to modify the code** | [NOX_COMPLETE_GUIDE.md](docs/guides/NOX_COMPLETE_GUIDE.md) + [Testing guides](docs/testing/) |
| **I want to develop a new strategy** | [STRATEGY_DEVELOPMENT_GUIDE.md](docs/guides/STRATEGY_DEVELOPMENT_GUIDE.md) |
| **I want to see what changed** | [CHANGES_JULY_2026.md](docs/reference/CHANGES_JULY_2026.md) |

---

## Configuration

Copy `.env.example` to `.env` and configure:

```bash
cp .env.example .env
# Edit .env with your settings
```

Key variables:
- `NOX_WATCHLIST_US` — US equity watchlist
- `NOX_WATCHLIST_CN` — China equity watchlist (for IBKR)
- `ALPACA_BASE_URL` — Paper (default) or live endpoint
- `ALPACA_API_KEY` / `ALPACA_API_SECRET` — API credentials
- `TELEGRAM_BOT_TOKEN` / `TELEGRAM_CHAT_ID` — Telegram alerts

See [`.env.example`](.env.example) for all options.

---

## Deployment

```bash
docker-compose up -d
```

See [docs/guides/PAPER_TRADING_READINESS.md](docs/guides/PAPER_TRADING_READINESS.md) for deployment checklist.

---

## Testing

```bash
# Run test suite
cd execution && ./build.sh

# Backtest a strategy
./nox_backtest --tickers SPY,QQQ --start 2024-01-01 --end 2024-12-31
```

See [docs/testing/](docs/testing/) for detailed guides.

### A note on backtest results

The backtester replays the live signal-generation logic on historical OHLCV and
re-prices options with a Black-Scholes model (IV proxied from realized
volatility). It does **not** use real historical option chains, real bid/ask
spreads, or event-volatility dynamics. Optional cost knobs (`haircutpct`,
`commissionpercontract`) can degrade fills toward realism, but the fill price
still starts from a model mid, not a quoted market. Treat every backtest number
as **directional-signal research**, not a live-P&L forecast — a strategy that
looks profitable here has cleared a necessary bar, not a sufficient one.

---

## License & Status

Educational / Research. Not intended for production trading without full validation and legal review.

For questions, see [docs/reference/DOCUMENTATION_OVERVIEW.md](docs/reference/DOCUMENTATION_OVERVIEW.md).

---

**Last updated:** July 26, 2026
