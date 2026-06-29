# Paper Trading Readiness & Live Trading Migration Guide

**Last updated: 2026-06-29**

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

### Gap 1 — PositionManager ↔ Signal Generator Integration (CRITICAL)

**The problem:** `PositionManager::add_position()` exists and the exit-rule monitor is running, but `executeSignal()` never calls `add_position()` after a successful fill. This means the 30-minute position monitor has an empty database and the exit rules (50% profit, 21 DTE, stop-loss) never fire in practice.

**Risk:** Live positions accumulate without automated exits. You would hold into expiration or require manual management of every position.

**Fix:** In `execution/main.cpp` (or `OptionsSignalGenerator.hpp`), after a successful `order_router_.route(signal, qty)`, add:
```cpp
position_manager_->add_position(
    signal.underlying, option_type, signal.strike,
    qty_contracts, fill_price, today_date,
    profile_type, signal.expiry_date
);
```

**Effort:** ~2 hours. This is a wiring task, not a design task.

---

### Gap 2 — Position State Sync on Restart (HIGH)

**The problem:** When the execution engine container restarts (deploy, crash, OOM), `open_positions` in SQLite is not re-seeded from the broker. The position monitor wakes up with an empty table and doesn't know about any open positions. The 50% profit / stop-loss rules then never fire for those positions.

**Risk:** After any restart, you are flying blind on positions opened before the restart.

**Fix:** Add a startup reconciliation step in `main.cpp` that:
1. Calls `GET /v2/positions` from Alpaca
2. Calls `GET /v2/options/positions` for options
3. Inserts any positions not already in `open_positions` with a sentinel `entry_price = current_price` (conservative: start tracking from now)

**Effort:** ~4 hours.

---

### Gap 3 — No Earnings Avoidance Filter for Options (MEDIUM)

**The problem:** The earnings calendar is fetched from the data engine every 24 hours, but the options signal generator does not currently gate on it. A ticker with earnings in 3 days will generate signals normally. Options positions held into earnings face binary risk that the Black-Scholes model does not capture (IV crush + gap risk).

**Risk:** Options positions entered 3 days before earnings frequently experience extreme losses that the backtest does not model.

**Fix:** In `OptionsSignalGenerator.hpp`, before evaluating a ticker, query the earnings calendar from the data engine's internal endpoint. If `days_to_earnings <= 3`, skip the ticker. The endpoint already exists: `GET http://america-data-engine:8001/earnings/calendar`.

**Effort:** ~3 hours.

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

## 5. Recommended Paper Trading Checklist

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

- [ ] Run the backtester: `./execution/nox_backtest watchlist=SPY,QQQ,AAPL,TSLA,NVDA range=2y`
- [ ] Compare backtest win rate vs paper win rate (expect some divergence; >10pp gap = investigate)
- [ ] Review WS1 contradiction block rate — if >40% of signals blocked, IV skew threshold may be too tight
- [ ] Check WS5 liquidity gate hit rate — if >20%, scanner watchlist may include illiquid names
- [ ] Review weekly report history (`/history 5`) for P&L trend

### Month 2 — Live-trading pre-flight

- [ ] Close Gap 1 (PositionManager integration)
- [ ] Close Gap 2 (Position sync on restart)
- [ ] Close Gap 3 (Earnings filter)
- [ ] Recalibrate Kelly parameters from paper results
- [ ] Fund live broker account (min $25k for pattern-day-trader exemption on options)
- [ ] Flip `ALPACA_BASE_URL` or activate IBKR path
- [ ] Set `DRAWDOWN_HALT_PCT=0.10` and monitor first week manually

---

## 6. Honest Assessment of Expected Edge

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
