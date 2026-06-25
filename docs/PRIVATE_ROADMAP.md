# Nox Private Development Roadmap

This document is for the private development version of Nox. The public repo
(Nox) is the resume/showcase version — frozen at the state described in the
guides. All forward development lives here.

---

## Where things stand right now

### What's solid (don't break these)
- Black-Scholes engine with all 5 Greeks + Newton-Raphson IV solver
- Wilder's RSI, standard ATR, 30-day HRV computation
- Regime state machine (VIX + SPY 200-SMA → RISK_ON/TRANSITION/RISK_OFF)
- 8 options strategies across 3 directional biases
- Alpaca multi-leg order routing with `position_effect: "open"`
- CC auto-execution pre-check (validates share position before placing)
- Dual-profile architecture (BOT conservative + PERSONAL aggressive)
- Standalone backtester (`nox_backtest`) with BS re-pricing simulation

### What the backtester already told you
- With a flat IV proxy (HRV × 1.15), the variance premium signal never fires
- SPY straddles underperformed because realized vol < implied vol consistently
- `BEAR_PUT_SPREAD` showed 100% win rate on 3 trades (too few to trust)
- Stop losses at 2× debit never triggered — most losses came from holding to expiry

---

## Priority 1 — Things you can do without buying data

### 1a. Earnings avoidance filter — ✅ IMPLEMENTED

> Live: `OptionsSignalGenerator` fetches an earnings calendar from the
> america-data-engine each scan cycle and skips any ticker with earnings within
> 5 days (`[EARNINGS_GATE]`). Gate fails open (disabled) if the feed is
> unavailable.

The single biggest cause of unexpected large moves that destroy spreads and CSPs.
Alpaca provides earnings dates via:

```
GET /v2/corporate_actions/announcements?ca_types=Dividend&since=YYYY-MM-DD&until=YYYY-MM-DD
```

For earnings specifically, use the `Earnings` ca_type. Add this to `scanTicker`:

```cpp
bool nearEarnings(const std::string& symbol, int days_threshold = 3) {
    // Call Alpaca announcements endpoint
    // If earnings date is within days_threshold calendar days, return true
    // scanTicker: if (nearEarnings(ticker)) { log("skip — earnings"); return; }
}
```

**Expected impact:** Removes the most common source of gap-through-strikes events.
Even a simple filter (skip if earnings within 5 days) substantially improves
the win rate on spreads and income strategies.

### 1b. Proper exit rules in the live engine — ✅ IMPLEMENTED

> Live: `PositionManager` persists open positions to SQLite and runs a monitoring
> thread (30-min cycle) that re-prices each position off the Alpaca options quote
> feed and applies the 50% profit rule, 21-DTE rule (income trades), and stop
> loss, closing via `OptionsOrderRouter::closePosition` with a Telegram alert.
> (See "Suggested next polish" at the end for the remaining shutdown-latency item.)

Currently the live engine has no exit logic after entry. The backtester showed
that holding to expiry is the main source of losses. Add:

```cpp
// In a new PositionMonitor class or extend the options scanner:
struct OpenPosition {
    std::string ticker, strategy, occ_symbol;
    std::string expiry_date;
    double entry_price, max_risk;
    std::time_t entry_time;
};
```

Exit logic to implement:
- **50% profit rule**: close when current value ≥ entry × 1.50 (long) or ≤ entry × 0.50 (short)
- **21 DTE rule**: close income trades (CSP/CC/strangle) when 21 days remain, regardless of P&L
- **Stop loss**: close when current value ≤ entry × 0.50 (long) or ≥ entry × 2.0 (short)

You need to persist open positions (JSON file or SQLite) and add a monitoring
thread that checks them every 30 minutes alongside the signal scanner.

### 1c. Backtest parameter sweep

The backtester accepts CLI args — use this to find which parameters actually work:

```bash
# Does a tighter stop help?
./nox_backtest stop=1.5 range=2y watchlist=SPY,QQQ,AAPL,TSLA,NVDA

# Does weekly scanning outperform daily scanning?
./nox_backtest scan=1 range=2y
./nox_backtest scan=5 range=2y
./nox_backtest scan=10 range=2y

# Is the personal profile (shorter DTE, higher delta) better?
./nox_backtest profile=personal range=2y

# Which tickers actually have edge?
./nox_backtest watchlist=TSLA range=2y
./nox_backtest watchlist=NVDA range=2y
./nox_backtest watchlist=SPY  range=2y

# Does 5-year data tell a different story?
./nox_backtest range=5y watchlist=SPY,QQQ
```

When you find a parameter combination with >55% win rate across ≥50 trades,
that's worth paper-trading live. Anything under 50 trades is noise.

### 1d. Tighten the stop loss in the backtester

The current 2× stop (lose 100% of debit) is loose. Run:
```bash
./nox_backtest stop=1.0  # lose 0% before stopping — exit at break-even or worse
./nox_backtest stop=1.5  # lose 50% of debit before stopping
./nox_backtest stop=2.0  # current default
```

The goal is to find the stop that maximizes:
`win_rate × avg_win - loss_rate × avg_loss`

A tighter stop usually hurts win rate but improves expected value by capping losses.

---

## Priority 2 — Requires external data (low cost or free)

### 2a. True IV Rank (52-week percentile) — ✅ IMPLEMENTED

> Live: the Historical IV Dataset System collects EOD IV snapshots and computes a
> rolling IV Rank, consumed by the signal generator's vol-richness gates
> (`iv_rank_sell_min` / `iv_rank_buy_max` per profile). NOTE: confirm the
> generator reads the historical rank rather than the legacy snapshot-relative
> value in every path — see "Suggested next polish".

The single most important data upgrade. Real IV rank compares today's IV to
the full 52-week range of daily IV, not just the current snapshot.

**Free option**: CBOE publishes historical VIX data (for SPX vol). For individual
equities, your best free option is to accumulate Alpaca snapshot data over time
(save the daily IV fetch to a file, build up your own 52-week history).

**Cheap option**: Polygon.io (~$29/month) provides historical options data.

Implementation once you have historical IV:
```cpp
// Store daily IV snapshots:
// {date, ticker, iv_avg} → JSON file or SQLite

// Compute IV rank at signal time:
double computeIVRank(const std::vector<double>& iv_history, double iv_current) {
    double iv_min = *std::min_element(iv_history.begin(), iv_history.end());
    double iv_max = *std::max_element(iv_history.begin(), iv_history.end());
    if (iv_max - iv_min < 1e-6) return 50.0;
    return (iv_current - iv_min) / (iv_max - iv_min) * 100.0;
}
```

When you have real IV rank, the variance premium signal becomes:
- IV Rank > 50% AND IV > HRV × 1.15 → strong sell premium
- IV Rank < 30% AND IV < HRV × 0.90 → buy premium

### 2b. Accumulate your own IV history — ✅ IMPLEMENTED

> Live: EOD collection persists daily IV snapshots (part of the Historical IV
> Dataset System), which is what feeds 2a.

Start now: every scan cycle, save the IV snapshot to a file.

```cpp
// In OptionsSignalGenerator::scanTicker, after fetchIVData:
void saveIVSnapshot(const std::string& ticker, double iv_level, const std::string& date) {
    std::string path = "/data/iv_history/" + ticker + ".csv";
    std::ofstream f(path, std::ios::app);
    f << date << "," << iv_level << "\n";
}
```

After 30 days you have a month of history. After 252 trading days you have
a full IV rank series. This costs nothing but time.

### 2c. Earnings calendar integration

Alpaca's corporate actions endpoint is available on the free tier. Build a
simple cache that fetches the next 30 days of earnings once per day.

---

## Priority 3 — Significant architecture work

### 3a. Position manager and portfolio Greeks

Currently the engine generates entry signals but has no memory of open positions.
A real options trading system tracks:

- Net portfolio delta (sum of all position deltas × notional)
- Net theta (daily time decay across all positions)
- Net vega (portfolio sensitivity to vol moves)
- Current P&L per position vs. entry price

This requires:
1. Persisting open positions to disk (SQLite recommended)
2. A monitoring loop that re-prices all open positions every 15 minutes
3. An alert when a position hits its profit target or stop loss
4. A REST endpoint to view the portfolio (`GET /positions/options`)

### 3b. Delta hedging (delta-neutral strategies)

Once you have portfolio Greeks, you can hedge directional risk:
- If net portfolio delta > 0.20 (long-biased), sell a small SPY position or
  buy a put spread to reduce delta
- Practical for straddles/strangles where you want pure vol exposure,
  not directional exposure

This is advanced and only worth doing once 3a is working.

### 3c. Volatility surface and skew

Currently the engine uses a single IV for each underlying (the snapshot average).
Real options pricing uses a full surface: different IVs for different strikes
(skew) and expiries (term structure).

The practical impact:
- OTM puts on equities have higher IV than ATM (put skew / smirk)
- This means CSPs are more expensive than BS at ATM vol implies
- Your strike selection is slightly optimistic — the real delta is lower than calculated

To implement this properly you need a historical options chain with per-strike IVs,
which requires Polygon.io or similar. For now, acknowledge this limitation in
your writeups rather than trying to correct for it with heuristics.

### 3d. Backtester with real options chain data

The current backtester uses BS re-pricing with an HRV proxy for IV. Once you
have Polygon.io or another historical chain, replace `valuePosition()` with
actual historical option mid-prices. This removes the IV proxy bias entirely
and gives you real P&L estimates including bid/ask spread effects.

### 3e. Native execution-venue migration (Alpaca → IBKR)

Alpaca's options support is REST-only with market-order-biased fills and a single
US-equity-options venue. The forward path is a native socket integration with the
Interactive Brokers TWS API (headless IB Gateway, paper 4002 / live 4001):

- Persistent TCP connection with asynchronous `EWrapper` callbacks
- Lock-free tick ingestion + mutex-guarded order/exec state (thread-safe by design)
- Direct multi-leg/limit order routing and richer execution reporting
- Path to futures/index options and non-US venues

Status: prototype lives in the private development branch; not wired into the
public showcase engine.

### Suggested next polish (small, high-signal)

- **PositionManager shutdown latency:** `stop_monitoring()` joins a thread that may
  be mid-`sleep_for(30 min)`, so shutdown can hang up to 30 minutes. Replace the
  bare sleep with a `std::condition_variable::wait_for` on the stop flag for an
  interruptible wait. (The flag itself is now `std::atomic<bool>` — the data race
  is fixed.)
- **IV Rank wiring audit:** confirm `selectStrategy`/signal assembly consume the
  historical IV Rank everywhere; the legacy snapshot-relative `iv_rank`
  (`display only`) comment in `OptionsSignalGenerator` suggests at least one path
  may still use the snapshot value.

---

## Priority 4 — For the Tsinghua application / quant interviews

### What interviewers will test

1. **Can you derive BS from first principles?**
   Practice: start from the replicating portfolio argument, derive the PDE, solve it.
   Know the boundary conditions and why log-normal is used.

2. **What are the weaknesses of BS?**
   - Assumes constant vol (real markets have vol surface and jumps)
   - European exercise only (most equity options are American)
   - No dividends (adjustable but most implementations ignore it)
   - Log-normal returns (fat tails and skewness are empirically real)

3. **What is the variance risk premium and why does it exist?**
   - IV > RV on average because sellers demand compensation for the risk of
     being short gamma during a spike
   - It's not free money: it comes with negative skewness (rare large losses)
   - The carry is positive but the distribution is left-skewed

4. **What would you change about this engine for live trading?**
   Good answer: limit orders instead of market, real IV surface, position
   management with 21 DTE close rule, earnings avoidance, transaction costs
   in the backtester, American exercise adjustment for short-dated options.

5. **How do you validate a backtest?**
   - Walk-forward validation: train on years 1-3, test on year 4 (out-of-sample)
   - Parameter stability: results should not change dramatically if you shift
     the training window by 30 days
   - Look for overfitting: a strategy that requires >5 parameters to define
     is almost certainly overfit to historical noise

### Reading list before the interview

**Essential:**
- Hull, *Options, Futures, and Other Derivatives* (Ch. 19-21 for Greeks, Ch. 22 for smiles)
- Natenberg, *Option Volatility and Pricing* (practitioner perspective, very readable)

**For the quant angle:**
- Gatheral, *The Volatility Surface* (advanced, but shows you understand the field)
- Wilmott, *Paul Wilmott on Quantitative Finance* (comprehensive reference)

**Papers worth knowing:**
- Black & Scholes (1973) — original paper, read it once
- Carr & Wu, "Variance Risk Premiums" (2009) — theoretical backing for your edge signal

---

## What to tell Tsinghua / interviewers about this project

**One-paragraph framing:**

> "I built an algorithmic options trading engine from scratch in C++ that implements
> Black-Scholes pricing with all five Greeks, a Newton-Raphson IV solver, and a
> macro regime state machine gating eight options strategies across three capital
> tiers. The engine integrates with Alpaca's institutional API for live options
> order routing, including multi-leg spread execution. I added a variance risk
> premium signal (sell when IV > 30-day HRV × 1.20) as the core edge mechanism,
> and built a standalone backtester to validate signal quality on historical
> OHLCV data. The system is live on paper trading."

**What NOT to oversell:**
- Don't claim it's profitable — it's been paper traded, not validated with real capital
- Don't claim the backtester uses real options prices — it uses BS re-pricing with an IV proxy
- Don't claim the IV rank is a true 52-week percentile — it's a snapshot-relative measure

**What IS defensible:**
- The BS implementation is correct (formulas, Greeks, IV solver)
- The architecture decisions are principled (dual profiles, regime gating, delta-targeting)
- The variance premium signal is theoretically motivated (cite Carr & Wu)
- You understand the limitations of the current approach and know what to fix

---

## Immediate next actions (ordered by impact)

1. Run `./nox_backtest range=2y watchlist=SPY,QQQ,AAPL,TSLA,NVDA` — get the full picture
2. Run the parameter sweep (stop=1.5, scan=1 vs 5 vs 10) — find what moves the needle
3. Add earnings avoidance to `scanTicker` — highest-impact code change
4. Start saving daily IV snapshots to build your own history
5. Implement the 21 DTE / 50% profit close rule in the live engine
6. Paper trade for 60 days — real signal quality only shows in real market data
