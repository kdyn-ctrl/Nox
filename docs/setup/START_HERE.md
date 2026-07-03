# Nox Trading Engine — Live System Guide

**Current Status:** Live paper + small-account ($5k) trading ready. Equity + options architecture with rule-based exits, position persistence, trade ledger.

---

## What Nox Is Now (July 2026)

Nox is a **dual-asset C++ trading engine** that:

1. **Equity trading** (primary focus)
   - Scans 5–10 tickers every 5 minutes for technical signals (RSI, SMA, ATR)
   - Rule-based exits: take-profit (%), stop-loss (%), RSI exhaustion, trend-break, time-stop
   - Kelly Criterion position sizing (hard cap 10% per trade)
   - T+1 settlement gate for Chinese A-shares (dormant; ready when IBKR is added)
   - Persistent position tracking (survives container restarts)

2. **Options trading** (secondary, conservative)
   - Black-Scholes pricing + Greeks computation
   - Variance risk premium signal (sell premium when IV > 30-day RV by 20%)
   - Regime-gated strategy selection (RISK_ON/TRANSITION/RISK_OFF)
   - Multi-leg order routing to Alpaca institutional API
   - Optional auto-execution (OFF by default; recommended for paper only)

3. **Real-time analysis** (WS1 — Contradiction Vector)
   - Detects when equity + options signals diverge (bullish equity, bearish options = contradiction)
   - NLP sentiment analysis on news feeds + SEC filings
   - Alerts on regime changes, insider clusters, microstructure anomalies

4. **End-of-day reporting** (WS4 + WS6)
   - Daily Telegram reports: market regime, positions, realized P&L
   - Weekly research: correlation matrix, insider activity, liquidity regime
   - Persistent trade ledger (survives engine restarts)

---

## Architecture at a Glance

```
┌─────────────────────────────────────────────────────────────┐
│                  NOXTRADING ENGINE (C++)                     │
│  execution-engine pod; 1 CPU, 2GB RAM, no volume mounts     │
├─────────────────────────────────────────────────────────────┤
│ • Equity Scanner (5min) → /webhook SELL ← Rule exits        │
│ • Position Manager (persistent) → trade ledger              │
│ • T+1 Settlement Gate (CN-RULE-002, dormant)                │
│ • Telegram dispatch (all signals)                           │
└────────────────┬────────────────────────────────────────────┘
                 │
        ┌────────┴──────────┐
        ↓                   ↓
   ┌─────────────┐  ┌──────────────────┐
   │ HEARTBEAT   │  │ DATA ENGINES     │
   │ (Monitor)   │  │ (China + America)│
   │ Python 3.10 │  │ Separate         │
   ├─────────────┤  ├──────────────────┤
   │ • IV skew   │  │ • Market data    │
   │ • Reports   │  │ • News feeds     │
   │ • Telegram  │  │ • SEC filings    │
   │   commands  │  │ • VIX/macro      │
   └─────────────┘  └──────────────────┘
```

**Single source of truth:** Trade ledger in SQLite at `/app/data/memory_bank.db` (shared via volume mount).

---

## What Changed Recently (July 2026)

### ✅ Fixed / Shipped

**Equity order execution (critical bug fixed)**
- Rule-based exits were closing positions directly, bypassing the T+1 gate and avoiding ledger recording
- **Now:** All three sell paths (webhook, rule-based, trailing-stop) route through the same pipeline with consistent tagging
- **Visibility:** `/signals` and `/details` commands now show `[webhook]` / `[rule:Take-profit]` / `[trailing_stop_close]` for each trade

**Paper/live URL hardcoding (critical for future live trading)**
- heartbeat/monitor.py had 6 hardcoded `paper-api.alpaca.markets` URLs
- **Now:** All broker API calls read `ALPACA_BASE_URL` from env, same as the C++ engine
- **Impact:** When you flip `.env` ALPACA_BASE_URL to live, reports/portfolio commands actually show your live account (not paper)

**Chinese A-share correctness bugs (fixed 3 real issues + 1 non-bug clarification)**
- `get_today_date_string()` was thread-unsafe (shared static buffer in `std::localtime()`)
- Container timezone was UTC, but T+1 gate compared against ET — could silently fail in evening windows
- IBKR BUY path missed the `cnBoardLotSize > 1` guard (currently dead code)
- **Now:** All date computations thread-safe, anchored to US Eastern (via gmtime_r + DST offset)
- **New diagnostic:** `/cn_status` command shows gate state + tracked positions + cleared status

**Persistent logging & silent failures**
- Containers could crash silently (June 28 incident); analyst + execution both lost logs on restart
- **Now:** All containers write to a persistent logging volume; crash logs survive restarts
- **Trade ledger:** Fully persistent and queryable (`/trades` command shows all history)

---

## How to Use It Right Now

### Daily Operations

**1. Start paper trading** (learn the system)
```bash
# .env is already set to paper
docker compose up -d execution-engine heartbeat-monitor
docker compose logs -f execution-engine  # watch for [EXECUTION] logs
```

**2. Monitor via Telegram**
```
/signals      — last 10 trades (recent signal history)
/details 5    — deep breakdown of last 5 signals
/trades 10    — persistent ledger, last 10 executed trades
/cn_status    — T+1 settlement gate status (shows dormant)
/eod          — end-of-day report (equity + options P&L)
/eow          — end-of-week report (Fri @ 16:10 ET)
```

**3. Check rule-exit fires**
```bash
docker compose logs execution-engine | grep "\[EQUITY_EXIT\]"
# Example output:
# [2026-07-01 16:02:15] [EQUITY_EXIT] SPY → Take-profit (+12.5%) — liquidating.
```

When a rule fires, you'll get a Telegram alert:
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [rule:Take-profit (+12.5%)]
• Entry: $450, Exit: $505
• P&L: $5,500 (100 shares)
```

---

## System Configuration

### Watchlists (Recommended for $5k Account)

**Current (.env default):**
```env
NOX_WATCHLIST_US=AAPL,TSLA,NVDA,MSFT        # expensive, bad for $5k sizing
NOX_WATCHLIST_CN=BABA,JD,PDD,BIDU,NIO       # US-listed ADRs (no real A-share access)
```

**Recommended change (cheaper, better sizing on small accounts):**
```env
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,COIN,SOFI,F
NOX_WATCHLIST_CN=                            # leave empty; no IBKR yet
```

**Why:**
- SPY/QQQ: 1 share per trade is still meaningful ($450–360 position)
- PLTR/SOFI/F: $25–30 range → 15–20 shares per Kelly trade (good sizing)
- AMD/COIN: $120–130 range → 3–4 shares per trade (diversification)

See **TICKER_STRATEGY.md** for detailed watchlist design and backtesting recommendations.

---

### Rule-Based Equity Exits

Current parameters (in `.env`):

```env
EQUITY_RULE_EXITS_ENABLED=true              # master switch
EQUITY_EXIT_TAKE_PROFIT_PCT=0.15             # 15% profit target
EQUITY_EXIT_STOP_LOSS_PCT=0.10               # 10% stop loss
EQUITY_EXIT_RSI_CEILING=78                   # exit when RSI > 78 (exhaustion)
EQUITY_EXIT_SMA_BREAK=true                   # exit on close below 20-SMA
EQUITY_EXIT_MAX_HOLD_DAYS=0                  # 0 = off (no time-stop)
```

**How they work:**
- Every 5 minutes, all open equity positions are checked
- If **any** rule fires, the position closes via the T+1-gated pipeline
- Close reason tagged as `[rule:Take-profit (...)]` in Telegram + `/signals` / `/details`
- Trade recorded to ledger automatically

**To test on paper:** Set `EQUITY_EXIT_TAKE_PROFIT_PCT=0.01` (1% = aggressive), add a small position, confirm the close fires within 5 min.

---

### Kelly Position Sizing

**Configured (.env):**
```env
KELLY_WIN_RATE=0.6842         # from backtest (e.g., 60%+ accuracy)
KELLY_WIN_LOSS_RATIO=2.316    # avg win ÷ avg loss
KELLY_FRACTION=0.15           # fraction of raw Kelly (risk management)
```

**Formula:**
```
Raw Kelly = (p × W — (1-p) × L) / W
  where p = win_rate, W = avg_win_size, L = avg_loss_size

Position Size = min(Raw Kelly × Account, 10%)  # hard cap
```

**Example ($10k account, 60% win rate, 2:1 win/loss ratio):**
```
Raw Kelly = (0.6 × 2 — 0.4 × 1) / 2 = 0.8 ÷ 2 = 0.40 (40%)
KELLY_FRACTION=0.15 → Effective sizing = 0.40 × 0.15 = 6%
Hard cap min(6%, 10%) = 6%
Per-trade allocation = $10,000 × 6% = $600
```

**Start conservatively:** KELLY_FRACTION=0.10 on live accounts until you have 50+ trades of data.

---

## Validation Before Live Deployment

### Backtest on Your Watchlist
```bash
cd execution
./nox_backtest watchlist=SPY,QQQ,PLTR,AMD range=2y

# Output: Win rate, Sharpe, max drawdown, P&L by strategy
# Target: ≥55% win rate on ≥50 signals, Sharpe > 0.3
```

### Paper Trade for 30–90 Days
```
• 20+ signals (Buy + rule exits)
• 55%+ win rate
• 0 unexpected fills or slippage > 1%
• Telegram alerts firing correctly
• Regime gating working (RISK_OFF suppresses trades)
```

### Live Deployment Checklist
- [ ] 30+ paper trades, 55%+ win rate
- [ ] Backtest agrees (2y historical match)
- [ ] Telegram alerts tested + working
- [ ] `/cn_status` shows dormant gate (expected for no-IBKR setup)
- [ ] `/eod` reports show correct account summary
- [ ] Alpaca account has $5k minimum (PDT rules)
- [ ] ALPACA_BASE_URL flipped to live API
- [ ] KELLY_FRACTION set to 0.10 (conservative)
- [ ] Drawdown stop-loss configured (e.g., close all at -8%)

---

## Signal Messages (What You'll See)

### Equity Buy Signal
```
✅ BUY ORDER SUBMITTED
────────────────────────
• Ticker: SPY
• Quantity: 10 shares
• Entry Price: $450.00
• Order Status: Filled

Reason: RSI(14)=35 + Bullish 20-SMA + VIX<20
Market Regime: RISK_ON
Position Size: Kelly 6% of $5,000 = ~$300
```

### Rule-Based Exit (Take-Profit)
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [rule:Take-profit (+15.2%)]
• Entry: $450 @ 10 shares
• Exit: $518.40 @ 10 shares
• P&L: +$682 (realized)

Reason: Take-profit threshold (15% configured)
Close Price: $518.40
Shares Liquidated: 10
```

### T+1 Gate Block (Example, Dormant Today)
```
🚫 CN T+1 GATE BLOCKED
────────────────────────
• Ticker: TEST_CN
• Trigger: [rule:Take-profit (+12%)]
• Entry Date: 2026-07-01
• Sell Date: 2026-07-01

⛔ Same-day sell prohibited (T+1 rule). Signal discarded.
(This only activates when CN_BOARD_LOT_SIZE=100 in .env)
```

### Regime Change Alert
```
📊 REGIME CLASSIFICATION
────────────────────────
• Current Regime: RISK_OFF (red)
• VIX: 28.5 (threshold: >25)
• SPY vs 200-SMA: Below
• Effect: Suppressing directional trades, allowing income only

Position Sizing: 50% reduction during transition
```

---

## How to Read the Logs

**Key patterns to watch:**

```bash
# Successful equity buy
[EXECUTION] BUY signal for SPY. Filling 10 shares @ $450.

# Rule exits firing (check every 5 min during market hours)
[EQUITY_EXIT] SPY → Take-profit (+15.2%) — liquidating.

# Record to ledger (should follow each trade)
[POSITION_MANAGER] Recorded trade: SPY BUY 10 shares @ $450

# Regime changes
[ANALYST] Regime changed from RISK_ON to TRANSITION

# Telegram dispatch (confirms alert sent)
[TELEGRAM] Message queued: 📊 REGIME CLASSIFICATION
```

**If you see stuck messages:**
- `[EQUITY_EXIT] ... — liquidating` but no `[EXECUTION] SELL` → rule fire succeeded but close() failed
- Trade ledger empty but `/signals` shows trades → recording broke (usually a DB lock timeout)

---

## When Things Go Wrong

### Container Crashes (Silent Failure)
**Now fixed:** Persistent logging, check `/root/Nox/data/docker-logs/` for survivor logs.

### T+1 Gate Falsely Blocks (Timezone Mismatch)
**Now fixed:** Date computations are thread-safe and ET-anchored (via gmtime_r + DST offset).
**To verify:** Run `/cn_status` → "Today (ET)" should match your wall-clock date (e.g., 2026-07-01).

### Reports Show Paper Data Even After Live Flip
**Now fixed:** All broker URLs read from `ALPACA_BASE_URL` env. Flipping `.env` + restart = both engine + reports change venue.

### Rule Exits Not Firing
**Check:**
```bash
# Confirm EQUITY_RULE_EXITS_ENABLED=true in .env
grep EQUITY_RULE_EXITS_ENABLED /root/Nox/.env

# Check if positions exist
curl http://localhost:8080/positions  # execution engine REST API

# Check logs for [EQUITY_EXIT] lines
docker compose logs execution-engine | grep EQUITY_EXIT
```

If no logs: Either (a) no positions open, or (b) rules not firing on data (check threshold settings vs. actual prices).

---

## Next Steps to Live Trading

1. **Update .env to cheaper tickers** (TICKER_STRATEGY.md)
   ```bash
   cp .env .env.backup
   # Edit NOX_WATCHLIST_US to SPY,QQQ,PLTR,AMD,SOFI,COIN,F
   docker compose restart execution-engine heartbeat-monitor
   ```

2. **Backtest the new watchlist**
   ```bash
   cd /root/Nox/execution && ./nox_backtest watchlist=SPY,QQQ,PLTR,AMD,SOFI,COIN,F range=2y
   ```

3. **Paper trade for 30–60 days**
   - Target: 30+ signals, 55%+ win rate
   - Track P&L daily in `/trades` command

4. **Flip to live $5k** (when ready)
   ```bash
   # ALPACA_BASE_URL=https://api.alpaca.markets
   # KELLY_FRACTION=0.10  (conservative)
   # Check /cn_status to confirm dormant (no surprises)
   docker compose up -d execution-engine heartbeat-monitor
   ```

5. **Scale to $20k+** (only after 90+ live trades, positive P&L, <8% max drawdown)

---

## System Architecture Docs

For deeper understanding, see:
- **TICKER_STRATEGY.md** — Watchlist design, position sizing examples, migration path
- **docs/DESIGN_THINKING.md** — Historical design decisions, why things are the way they are
- **execution/IMPLEMENTATION_CHECKLIST.md** — C++ system components, threading model
- **TRAILING_STOP_MONITOR_README.md** — Alpaca trailing stop integration (broker-side exits)
- **Trade Ledger Docs** (feature_trade_ledger_and_exits.md in memory) — Persistence, reconciliation

---

## Known Limitations

❌ **No real options chain data:** Backtester uses Black-Scholes re-pricing (introduces bias)
❌ **No partial sells:** All exits are full liquidation (no lot-level tracking)
❌ **No earnings avoidance:** Can trade earnings surprises (high volatility risk)
❌ **No limit orders:** Market orders only (slippage on fast moves)
❌ **No dividend-adjustment:** Options Greeks assume zero dividends
❌ **IBKR not live:** T+1 gate is dormant (no real A-share access via Alpaca)

These are acceptable for a $5k paper/initial live account. Upgrade path documented in TICKER_STRATEGY.md.

---

**Last Updated:** July 1, 2026  
**Status:** Live-ready, tested, battle-hardened from June 28 incident  
**Questions?** Check `/help` in Telegram or read the source (execution/main.cpp is well-commented).

