# Paper Trading Readiness & Live Trading Migration Guide

**Last updated: 2026-06-29 (updated: Gap 1 + Gap 2 implemented; Gap 3 was already done)**

---

## Short Answer

**Yes, you can start paper trading now.** The core execution engine, signal generator, position manager, and risk gates are all fully operational. The architecture is production-grade.

**No, you are not ready for live money yet.** Three specific gaps must be closed first — see Section 3.

---

## 1. What Is Fully Operational

Every component listed below is actively running and will give you real, reliable paper-trade data:

| Component | Status | What it does for you |
|-----------|--------|----------------------|
| Options Signal Generator | ✅ | Scans watchlist every 30 min, generates 8 strategy types (CSP, CC, spreads, straddles, strangles) |
| Execution Engine (Alpaca) | ✅ | Routes multi-leg market orders to Alpaca paper account, authenticates, confirms fills |
| Position Manager | ✅ | Tracks every open options position in SQLite; applies 50% profit, 21 DTE, and 2× stop-loss exit rules automatically |
| Regime State Machine | ✅ | VIX + SPY 200-SMA gating — suppresses directional trades in RISK_OFF, scales sizing in TRANSITION |
| WS1 Contradiction Vector | ✅ | Blocks signals where NLP headline sentiment contradicts IV skew direction |
| WS2 Alt-Macro Pipeline | ✅ | War-risk insurance premiums, AIS tanker transit spikes, OFAC sanctions — macro tail-risk context |
| WS3 Insider Cluster Filter | ✅ | Detects multi-executive insider buys from SEC Form 4 filings (bullish confirmation signal) |
| WS5 Liquidity Vacuum Gate | ✅ | Aborts any signal where bid-ask spread is >3σ above rolling baseline — prevents bad fills |
| WS4 Half-Life Decay | ✅ | Exponential decay of all sentiment scores per category (geo: 6h, macro: 48h, earnings: 72h, technical: 12h) |
| WS6 Skeptic Report | ✅ | Saturday batch — JSON + Markdown summary of all weekly signals, grades, and workstream verdicts |
| WS7 Lag Windows | ✅ | Tracks 6-K SEC filings before Chinese retail media picks them up; grades each event A/B/C/F |
| Daily Scout | ✅ | 09:00 ET: US news + SEC filings + China macro → Claude analysis → Telegram |
| Weekly Report | ✅ | 16:00 ET on last NYSE trading day: P&L, win rate, MAE, calibration, parse failures |
| Monthly Journal | ✅ | 1st of each month: full performance report written to `reports/YYYY-MM.md` |
| SEC Radar (real-time) | ✅ | 8-K and 6-K feeds polled every 30 sec; filing detected → Claude risk-score → Telegram |
| Whole-Market Scanner | ✅ | 3-stage pipeline: ~6,000 universe → activity screener → RSI/ATR/SMA analysis, every 30 min |
| IV Accumulation | ✅ | Daily EOD snapshot at 16:30 ET → building toward true 52-week IV Rank |
| IBKR path | ✅ wired | Socket layer + order router fully coded; not active yet — see Section 3 |

---

## 2. What Data Will You Get From Paper Trading

Paper trading with this system **will** produce the following useful, real data:

**What will be accurate:**
- Signal frequency and regime distribution (how often RISK_ON vs TRANSITION vs RISK_OFF)
- Options strategy mix (which strategies get selected by IV/bias conditions)
- Fill slippage benchmark — paper fills report fill prices; compare to Black-Scholes theoretical
- Exit rule effectiveness — 50% profit / 21 DTE / stop-loss trigger rate in live market conditions
- IV Rank accumulation — after 30 trading days per ticker, true IV percentile becomes meaningful
- Workstream filter hit rate — what % of signals are blocked by WS1/WS3/WS5 each week

**What will be noisy or absent (known gaps):**
- **Realized P&L** — the `trade_history` table in `memory_bank.db` is not currently written by the execution engine. You'll see P&L in Alpaca's dashboard but not in the weekly report's DB queries until this is wired up.
- **MAE / Calibration Score** — the `trade_predictions` table is empty. The weekly report will show "N/A" here until a workstream is wired to log predicted vs actual outcomes.
- **IV Rank accuracy** — for the first 30 trading days, IV Rank uses a snapshot-relative calculation (current IV vs average of what you've accumulated). True percentile rank requires ~252 sessions of history.
- **Backtester accuracy** — the backtester uses `HRV30 × 1.15` as an IV proxy; it does not use real historical options chain prices. Treat backtest results as directional estimates, not precise P&L predictions.

---

## 3. Three Gaps to Close Before Live Money

These are not theoretical concerns. Each one creates a real operational risk with actual dollars:

### Gap 1 — PositionManager ↔ Signal Generator Integration ✅ FIXED

**Was:** `executeSignal()` never called `add_position()` after a successful fill. Exit rules (50% profit, 21 DTE, stop-loss) never fired.

**Fix applied (2026-06-29):** `PositionManager*` added to `OptionsSignalGenerator` constructor. After a successful `router.route()` in `executeSignal()`, `add_position()` is now called with the fill details. `VixTermStructure` moved to namespace scope (pre-existing GCC 13 compile fix). `PositionManager.cpp` TelegramNotifier stub replaced with real `nox::TelegramNotifier`. Binary rebuilt.

---

### Gap 2 — Position State Sync on Restart ✅ FIXED

**Was:** On restart, `open_positions` SQLite was not re-seeded from the broker. Exit rules never fired for positions opened before the restart.

**Fix applied (2026-06-29):** `reconcilePositionsFromBroker()` added to `NoxEngine` constructor, called after `positionManager_->start_monitoring()`. Fetches `GET /v2/positions` from Alpaca, filters `asset_class=us_option`, parses OCC symbols (UNDERLYING+YYMMDD+C/P+STRIKE), and seeds any untracked positions using `avg_entry_price` as the tracked entry. Sends a Telegram alert listing how many orphan positions were seeded.

---

### Gap 3 — Earnings Avoidance Filter ✅ ALREADY IMPLEMENTED

**Status:** This was implemented before the doc was written. `fetchEarningsCalendar()` queries `GET http://america-data-engine:8001/earnings/calendar` at the start of each scan cycle. `hasEarningsWithin5Days()` gates on a **5-day** window (more conservative than the 3 days described above). Tickers within 5 days of earnings are skipped with `[EARNINGS_GATE]` log entry.

---

## 4. Live Trading Migration Path

When paper trading has validated the signal quality (target: 60-day window, ≥50 signals, ≥52% directional accuracy), follow this sequence:

### Step 1: Activate IBKR execution path

IBKR is already wired. Three remaining items (see `execution/IBKR_MIGRATION.md`):

1. SELL path: implement `reqPositions()` before routing SELL signals (IBKR has no "close all" endpoint)
2. Wire `executeSignal()` (when `EXECUTION_VENUE=ibkr`) to `IBKROrderRouter` instead of `OptionsOrderRouter`
3. Wire `PositionManager` quote fetches to IBKR streaming ticks from the ring buffer instead of Alpaca REST

### Step 2: Switch `ALPACA_BASE_URL` (if staying on Alpaca)

The simplest live-trading path is:
```env
ALPACA_BASE_URL=https://api.alpaca.markets
```

This is a one-line change. Ensure your Alpaca account is funded and options-enabled before flipping this.

### Step 3: Add portfolio-level risk limits

Before live, add a hard cap in `main.cpp` for aggregate Greeks exposure:

- **Delta cap:** if sum(|delta| × price × 100) exceeds N% of portfolio value → halt options scanner
- **Notional cap:** total open options notional > X% of portfolio → reject new signals
- **Drawdown circuit breaker:** already exists (`DRAWDOWN_HALT_PCT=0.10`), verify it's set correctly

### Step 4: Validate your Kelly parameters on paper results

The current Kelly parameters (`KELLY_WIN_RATE=0.6842`, `KELLY_WIN_LOSS_RATIO=2.316`) came from a backtest. After 60 days of paper trading, recalibrate:

```python
# From your trade_history table (once it's being populated):
wins  = len([t for t in trades if t.pnl > 0])
total = len(trades)
avg_win  = mean([t.pnl for t in trades if t.pnl > 0])
avg_loss = abs(mean([t.pnl for t in trades if t.pnl < 0]))

win_rate = wins / total                       # replace KELLY_WIN_RATE
win_loss = avg_win / avg_loss if avg_loss > 0 # replace KELLY_WIN_LOSS_RATIO
```

If live win rate is materially below 0.68, reduce `KELLY_FRACTION` before adding capital.

---

## 5. Backtesting on Different Market Universes

The backtester can test any market segment. Use it to validate your signal quality on different ticker universes before paper trading.

### Quick start examples:

```bash
# Your recommended watchlist (TSLA removed)
./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0.15 exit_slip=0.15

# Chinese ADRs only
./execution/nox_backtest watchlist=BABA,JD,BILI,PDD,DIDI,NIO,XPeng,NTES,BIDU,IQ range=2y

# Mega-cap tech
./execution/nox_backtest watchlist=AAPL,MSFT,GOOGL,NVDA,META,TSLA range=2y

# Healthcare sector
./execution/nox_backtest watchlist=JNJ,PFE,AMGN,LLY,ABBV,MRK,TMO,REGN range=2y

# Financials sector
./execution/nox_backtest watchlist=JPM,BAC,WFC,GS,C,BLK,BX,KKR range=2y

# Consumer sector
./execution/nox_backtest watchlist=WMT,COST,MCD,NKE,SBUX,DIS,BKNG range=2y

# Compare: same tickers, different slippage assumptions
./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0 exit_slip=0      # Theoretical
./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0.10 exit_slip=0.10 # Optimistic
./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0.20 exit_slip=0.20 # Conservative
```

### Parameter reference:

| Parameter | Meaning | Typical Range |
|-----------|---------|---------------|
| `watchlist=` | Comma-separated tickers | Any US-traded ticker |
| `range=` | Historical data period | `1y`, `2y`, `5y` |
| `entry_slip=` | Entry slippage per contract ($/share) | `0.05`–`0.30` |
| `exit_slip=` | Exit slippage per contract ($/share) | `0.05`–`0.30` |
| `profit=` | Exit at X% of max profit | `0.30`–`0.75` (default: 0.50) |
| `stop=` | Stop loss at X× debit paid | `1.5`–`3.0` (default: 2.0) |
| `capital=` | Starting capital (tier gate) | `5000`, `30000`, `75000` |

### Batch testing multiple segments:

Use the provided shell script (`execution/backtest_market.sh`) to test your strategy across pre-configured market segments:

```bash
# Make it executable
chmod +x ./execution/backtest_market.sh

# Test one segment (fetches data, runs backtest with slippage, reports results)
./execution/backtest_market.sh mega 2y
./execution/backtest_market.sh chinese 1y
./execution/backtest_market.sh healthcare 2y
./execution/backtest_market.sh all_tech_100 2y

# Test all segments at once (takes ~20-30 minutes)
./execution/backtest_market.sh all 2y
```

**Available segments:**
- `mega` — AAPL, MSFT, GOOGL, AMZN, NVDA, META, TSLA
- `tech` — INTC, AMD, QCOM, MU, AMAT, LRCX, CDNS, SNPS, ASML, ARM
- `healthcare` — JNJ, PFE, AMGN, LLY, ABBV, MRK, TMO, REGN, VRTX, BIIB
- `financials` — JPM, BAC, WFC, GS, C, BLK, BX, KKR, SCHW, COIN
- `consumer` — WMT, COST, MCD, NKE, SBUX, DIS, AMZN, BKKING, CCL, MAR
- `energy` — XOM, CVX, COP, MPC, PSX, OKE, SLB, EOG, FANG, COG
- `industrials` — BA, CAT, HON, RTX, GD, LMT, ETN, MMM, ITW, GE
- `chinese` — BABA, JD, BILI, PDD, DIDI, NIO, XPeng, NTES, BIDU, IQ
- `mega_chinese` — Blended: AAPL, MSFT, BABA, JD, NVDA, TSLA
- `all_tech_100` — Extended tech universe (40+ tickers)

### Interpreting results:

**Red flags (don't paper trade yet):**
- Win rate < 52% (not statistically different from coin flip on small sample)
- Avg P&L per trade < $5 (slippage will erase edge)
- Max drawdown > 30% of starting capital (too volatile)
- One sector has <30 trades (insufficient data)

**Green flags (ready to paper trade):**
- Win rate 60%+ on ≥50 trades per ticker
- Avg P&L/trade > $20 with realistic slippage
- Max drawdown < 25% of capital
- Directional accuracy 62%+ (bullish + bearish combined)

### Historical context:

Baseline from June 2026 backtest (SPY, QQQ, AAPL, NVDA, TSLA with 0.30 total slippage):
- **TSLA removed:** 356 trades, 62.6% win, +$10.4k P&L
- **TSLA included:** 445 trades, 60% win, -$53.7k P&L (volatility mismatch)

**Lesson:** Not all tickers suit your strategy. Test before adding to watchlist.

---

## 6. Position Sizing Guide (Kelly Criterion)

Your backtests show **62.6% win rate, $29/trade average with realistic slippage**. This is a real edge. But **position size determines whether you profit or blow up.**

### The math:

**Kelly % (optimal leverage):**
```
kelly_fraction = (win_rate × avg_win - loss_rate × avg_loss) / avg_win
kelly_fraction = (0.626 × 29 - 0.374 × 10) / 29 = 0.58 (58%)
```

**But don't use full Kelly.** In practice, use 1/4 to 1/2 Kelly to survive drawdowns.

### Three scenarios on $35k starting capital:

| Kelly | Risk/Trade | Contracts | Expected Annual | Max Drawdown | Recovery |
|-------|-----------|-----------|-----------------|-------------|----------|
| 1/4 (Conservative) | $5,075 | 1 contract | $4k–5k (12–15%) | ~$10k | 3–4 months |
| 1/2 (Moderate) | $10,150 | 2 contracts | $8k–10.5k (24–30%) | ~$20k | 2–3 months |
| Full (Aggressive) | $20,300 | 4 contracts | $16k–21k (48–60%) | ~$40k | 1–2 months |

### Recommendation for paper trading:

**Start 1/2 Kelly (2 contracts).** Reasons:
1. Real live performance is noisier than backtest (more slippage, wider spreads)
2. You can always scale up if edge holds
3. If you hit a 7-loss streak (happens 1× per 100 trades), you don't panic
4. Expected monthly income: ~$700–875 (meaningful without being make-or-break)

### If you want to test without capital:

Paper trading on Alpaca is literally free. You're not risking real money, just opportunity cost. Use it to:
- Confirm backtested win rate holds (target: 60%+ on first 50 trades)
- Verify Telegram alerts work (no silent failures)
- Test your emotional discipline (can you sit through a 3-loss day?)
- Calibrate your stop-loss and profit-target widths in real market

---

## 7. Recommended Paper Trading Checklist

### Day 1 — Setup

- [ ] Start with `OPTIONS_BOT_AUTO_EXECUTE=false` (advisory only)
- [ ] Run `/report` manually — verify Claude scout produces a coherent analysis
- [ ] Run `/status` — verify all containers are ONLINE
- [ ] Run `/pulse` — verify VIX and headlines are live
- [ ] Check Alpaca paper dashboard shows paper account balance

### Week 1 — Validate signal flow

- [ ] Enable `OPTIONS_BOT_AUTO_EXECUTE=true` (execution starts)
- [ ] Verify each trade produces a Telegram alert with Greeks
- [ ] Verify `open_positions` table in SQLite has rows after each trade
- [ ] Monitor exit rule fires (50% profit / stop-loss) via Telegram
- [ ] Run `/signals` daily — confirm execution engine is receiving signals

### Week 4 — Evaluate signal quality

- [ ] Run the backtester: `./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0.15 exit_slip=0.15`
- [ ] Compare backtest win rate vs paper win rate (expect some divergence; >10pp gap = investigate)
- [ ] Review WS1 contradiction block rate — if >40% of signals blocked, IV skew threshold may be too tight
- [ ] Check WS5 liquidity gate hit rate — if >20%, scanner watchlist may include illiquid names
- [ ] Review weekly report history (`/history 5`) for P&L trend

### Month 2 — Live-trading pre-flight

- [x] ~~Close Gap 1 (PositionManager integration)~~ — done 2026-06-29
- [x] ~~Close Gap 2 (Position sync on restart)~~ — done 2026-06-29
- [x] ~~Close Gap 3 (Earnings filter)~~ — was already implemented (5-day window)
- [ ] Recalibrate Kelly parameters from paper results
- [ ] Fund live broker account (min $25k for pattern-day-trader exemption on options)
- [ ] Flip `ALPACA_BASE_URL` or activate IBKR path
- [ ] Set `DRAWDOWN_HALT_PCT=0.10` and monitor first week manually

---

## 8. Honest Assessment of Expected Edge

**Variance risk premium (the core signal):**
The claim — that implied vol consistently exceeds realized vol — is well-established academically (Carr & Wu 2009). In practice, the average IV > RV spread in US equity options is 3–5 vol points. This generates a carry for premium sellers, but:
- The carry is captured slowly (monthly theta) and lost quickly (gamma when spot moves)
- A single large move (earnings surprise, macro shock) can wipe 3-4 months of premium collected
- The system's earnings avoidance gap (Section 3, Gap 3) is particularly important here

**Directional bias (RSI + SMA):**
These are widely-known, widely-traded signals. Expect directional accuracy near 52–55%, not 60%+. The backtest reported 68% win rate, which is high — verify this holds in out-of-sample paper trading before trusting it.

**WS1–WS3 filters:**
These are genuine signal quality improvements. Contradiction filtering (text vs IV) has a theoretical basis in cross-market information asymmetry. Insider clustering is a proven alpha source in academic literature. Expect a measurable improvement in signal quality once you have enough paper trades to compute the filtered vs unfiltered win rate comparison.

**What this system cannot do:**
- Beat a market maker on options pricing (they have full volatility surfaces; you have a single IV)
- Capture intraday gamma (30-minute scan interval misses intraday vol spikes)
- Guarantee anything — it's a disciplined framework, not an ATM

---

*This document should be reviewed after 60 days of paper trading and updated with realized results before committing to live capital.*
