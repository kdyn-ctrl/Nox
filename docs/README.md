# Nox Trading System — Documentation

Complete documentation for the Nox trading engine, organized by purpose.

---

## 🚀 Getting Started

**New to Nox?** Start here.

- [**START_HERE.md**](setup/START_HERE.md) — System overview, what's working, daily operations (15 min read)
- [**TICKER_STRATEGY.md**](setup/TICKER_STRATEGY.md) — Position sizing, recommended watchlists, migration path (paper → $5k → $20k+)
- [**WATCHLIST_RECOMMENDATIONS.md**](setup/WATCHLIST_RECOMMENDATIONS.md) — Specific ticker selection, cheap stocks table, backtesting examples

---

## 📚 Guides & Strategies

In-depth how-to guides and system design.

- [**STRATEGY_DEVELOPMENT_GUIDE.md**](guides/STRATEGY_DEVELOPMENT_GUIDE.md) — Building and validating trading strategies
- [**DESIGN_THINKING.md**](guides/DESIGN_THINKING.md) — Architecture rationale and design decisions
- [**NOX_COMPLETE_GUIDE.md**](guides/NOX_COMPLETE_GUIDE.md) — Comprehensive system guide with all features
- [**NOX_USER_GUIDE.md**](guides/NOX_USER_GUIDE.md) — User-focused operations guide
- [**PAPER_TRADING_READINESS.md**](guides/PAPER_TRADING_READINESS.md) — Pre-deployment validation checklist
- [**TRAILING_STOP_MONITOR.md**](guides/TRAILING_STOP_MONITOR.md) — Broker-side trailing stop configuration and monitoring

---

## 🔍 Reference

System documentation, architecture, and change history.

- [**PROJECT_STATUS.md**](reference/PROJECT_STATUS.md) — Current architecture, feature matrix, data feeds (live trading status)
- [**CHANGES_JULY_2026.md**](reference/CHANGES_JULY_2026.md) — Critical bugs fixed, release notes, signal message format
- [**DOCUMENTATION_UPDATES_JULY_2026.md**](reference/DOCUMENTATION_UPDATES_JULY_2026.md) — What's new in documentation, navigation guide
- [**DOCUMENTATION_OVERVIEW.md**](reference/DOCUMENTATION_OVERVIEW.md) — Document structure and purpose
- [**INDEX.md**](reference/INDEX.md) — Legacy index (see this file instead)
- [**FUTURE_WORK.md**](reference/FUTURE_WORK.md) — Roadmap and planned features

---

## 🧪 Testing

Test strategy, guides, and best practices.

- [**TESTING_README.md**](testing/TESTING_README.md) — Overview of testing philosophy and approach
- **By Component:**
  - [ANALYST_TEST_GUIDE.md](testing/ANALYST_TEST_GUIDE.md) — Testing market analysis engine
  - [BACKTEST_TEST_GUIDE.md](testing/BACKTEST_TEST_GUIDE.md) — Backtesting framework and validation
  - [DATA_ENGINE_TEST_GUIDE.md](testing/DATA_ENGINE_TEST_GUIDE.md) — Data pipeline testing
  - [EXECUTION_TEST_GUIDE.md](testing/EXECUTION_TEST_GUIDE.md) — Order execution testing
  - [HEARTBEAT_TEST_GUIDE.md](testing/HEARTBEAT_TEST_GUIDE.md) — Monitor and health check testing
  - [TEST_GUIDE.md](testing/TEST_GUIDE.md) — Quick test reference
  - [TEST_MAINTENANCE_GUIDE.md](testing/TEST_MAINTENANCE_GUIDE.md) — Maintaining test suites
  - [TEST_UPDATE_CHECKLIST.md](testing/TEST_UPDATE_CHECKLIST.md) — Test update procedures
- [**TESTING_PHILOSOPHY.md**](testing/TESTING_PHILOSOPHY.md) — Philosophy and principles behind testing approach

---

## 📋 Rules & Configuration

- [**Rules/**](Rules/) — Signal generation rules and constants

---

## 📖 Journal

- [**journal/**](journal/) — Development journal, decisions, and incident logs

---

## 🌐 中文文档 (Chinese Documentation)

All guides available in Simplified Chinese. Mirror structure:

```
docs_zh/
├── setup/        (TICKER_STRATEGY, WATCHLIST_RECOMMENDATIONS)
├── guides/       (Complete guides and strategy docs)
├── reference/    (Status, changes, reference material)
├── testing/      (All test guides)
└── Rules/        (Signal rules)
```

Start with: [`docs_zh/README.md`](../docs_zh/README.md)

---

## Quick Navigation

### By Role

**Trader (using the system)**
1. [START_HERE.md](setup/START_HERE.md)
2. [TICKER_STRATEGY.md](setup/TICKER_STRATEGY.md)
3. [NOX_USER_GUIDE.md](guides/NOX_USER_GUIDE.md)

**Developer (modifying the system)**
1. [DESIGN_THINKING.md](guides/DESIGN_THINKING.md)
2. [NOX_COMPLETE_GUIDE.md](guides/NOX_COMPLETE_GUIDE.md)
3. [Testing guides](testing/) (by component)

**Quant Researcher (strategy dev)**
1. [STRATEGY_DEVELOPMENT_GUIDE.md](guides/STRATEGY_DEVELOPMENT_GUIDE.md)
2. [BACKTEST_TEST_GUIDE.md](testing/BACKTEST_TEST_GUIDE.md)
3. [NOX_COMPLETE_GUIDE.md](guides/NOX_COMPLETE_GUIDE.md)

### By Question

- **"What is Nox and what does it do?"** → [START_HERE.md](setup/START_HERE.md)
- **"How do I set up my watchlist?"** → [TICKER_STRATEGY.md](setup/TICKER_STRATEGY.md) + [WATCHLIST_RECOMMENDATIONS.md](setup/WATCHLIST_RECOMMENDATIONS.md)
- **"How do I deploy it?"** → [PAPER_TRADING_READINESS.md](guides/PAPER_TRADING_READINESS.md)
- **"What changed recently?"** → [CHANGES_JULY_2026.md](reference/CHANGES_JULY_2026.md)
- **"How does the system architecture work?"** → [DESIGN_THINKING.md](guides/DESIGN_THINKING.md)
- **"How do I test a new feature?"** → [Testing guides](testing/)
- **"Where's the current status?"** → [PROJECT_STATUS.md](reference/PROJECT_STATUS.md)

---

## File Organization

```
docs/
├── README.md (you are here)
├── setup/               ← Getting started
│   ├── START_HERE.md
│   ├── TICKER_STRATEGY.md
│   └── WATCHLIST_RECOMMENDATIONS.md
├── guides/              ← How-to and deep dives
│   ├── STRATEGY_DEVELOPMENT_GUIDE.md
│   ├── DESIGN_THINKING.md
│   ├── NOX_COMPLETE_GUIDE.md
│   ├── NOX_USER_GUIDE.md
│   ├── PAPER_TRADING_READINESS.md
│   └── TRAILING_STOP_MONITOR.md
├── reference/           ← Documentation & status
│   ├── PROJECT_STATUS.md
│   ├── CHANGES_JULY_2026.md
│   ├── DOCUMENTATION_UPDATES_JULY_2026.md
│   ├── DOCUMENTATION_OVERVIEW.md
│   ├── INDEX.md
│   ├── DOCUMENTATION_SUMMARY.txt
│   └── FUTURE_WORK.md
├── testing/             ← Test strategy and guides
│   ├── TESTING_README.md
│   ├── TESTING_PHILOSOPHY.md
│   ├── ANALYST_TEST_GUIDE.md
│   ├── BACKTEST_TEST_GUIDE.md
│   ├── DATA_ENGINE_TEST_GUIDE.md
│   ├── EXECUTION_TEST_GUIDE.md
│   ├── HEARTBEAT_TEST_GUIDE.md
│   └── ...
├── Rules/               ← Configuration rules
└── journal/             ← Development journal
```

---

**Last updated:** July 2, 2026  
**Status:** Ready for live trading after 30-day paper validation
