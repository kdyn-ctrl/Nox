# Nox — Options Trading Engine

A production-grade C++ options trading system implementing Black-Scholes pricing, Greeks computation, regime-based strategy selection, and live execution via Alpaca. Built from first principles with a focus on quantitative rigor, thread safety, and signal validation.

**Status:** Paper trading / Educational. Not for live capital without extensive validation.

---

## What This Is

This is a **quantitative research project** demonstrating:

- Correct implementation of the Black-Scholes model with all five Greeks (Δ, Γ, Θ, Ν, ρ)
- A Newton-Raphson implied volatility solver
- 8 options strategies (long/short calls/puts, spreads, straddles, strangles)
- A regime state machine (RISK_ON/TRANSITION/RISK_OFF) gating strategy type
- A variance risk premium signal: sell options when implied vol > realized vol by 20%+
- Multi-leg order routing to Alpaca's institutional API
- Standalone backtester replaying historical signals to validate win rates
- Dual-profile architecture (BOT conservative vs PERSONAL aggressive)

It's the kind of system you'd build to understand how a quant shop actually prices options and sizes risk. The code is readable, the math is correct, and the architecture scales.

---

## Core Components

### 1. Black-Scholes Engine (`execution/OptionEngine.hpp`)

Implements the complete Black-Scholes model for European options:

```cpp
// All inputs validated (no negative prices, vol, or time)
OptionGreeks compute_greeks(const OptionContract& c, bool solve_iv = false) {
    double price   = bs_price(c, sigma);
    double delta   = bs_delta(c, sigma);       // ∂V/∂S
    double gamma   = bs_gamma(c, sigma);       // ∂²V/∂S²
    double theta   = bs_theta(c, sigma);       // daily decay
    double vega    = bs_vega(c, sigma);        // per 1% vol move
    double rho     = bs_rho(c, sigma);         // per 1% rate move
    double iv      = implied_volatility(c, market_price);  // Newton-Raphson
}
```

**Key implementation details:**
- CDF approximation: Abramowitz & Stegun 26.2.17 (max error ~7.5e-8)
- IV solver: 100 iterations, convergence tolerance 1e-7
- Daily theta: annual theta ÷ 365
- Vega & rho: scaled to 1% moves (the standard)

**Validated against:** Standard financial calculator outputs. Not validated against market prices (would need historical options chain).

### 2. Signal Generator (`execution/OptionsSignalGenerator.hpp`)

Scans a watchlist every 30 minutes (configurable) and generates options signals:

**Market data pipeline:**
- VIX from Yahoo Finance (macro regime)
- SPY daily closes for 200-day SMA (macro regime)
- Underlying OHLCV for technical indicators: RSI-14 (Wilder's), ATR-14, SMA-20, SMA-50
- 30-day historical realized volatility (close-to-close log-returns)
- IV from Alpaca options snapshot (or VIX proxy fallback)

**Regime state machine:**
```
VIX < 15 && SPY > SMA200        → RISK_ON (full position sizing)
VIX 15-25 OR SPY near SMA200    → TRANSITION (50% sizing)
VIX > 25 && SPY < SMA200        → RISK_OFF (income only, suppress directional)
```

**Variance risk premium signal:**
```
vol_rich = (IV > HRV30 × 1.20)   → sell premium (CSP, CC, strangle)
vol_cheap = (IV < HRV30 × 0.90)  → buy premium (spreads, straddles)
```

**Strategy selection (bias × vol regime):**

| Bias | Vol Rich | Vol Fair | Vol Cheap |
|---|---|---|---|
| Bullish | CSP | Bull Call Spread | Long Call |
| Bearish | CC | Bear Put Spread | Long Put |
| Neutral | Strangle | CSP | Straddle |

**Position sizing:** Risk tier-based (STARTER/STANDARD/ADVANCED) with configurable % of capital per trade.

**Execution control:** Auto-execute is OFF by default. When enabled, CC positions are pre-validated against Alpaca holdings (prevents naked call execution).

### 3. Backtester (`execution/backtest_main.cpp`)

Replays the signal logic on 1-5 years of historical OHLCV and reports:

- Win rate and avg P&L per trade
- Breakdown by strategy and ticker
- Directional accuracy (did the bias predict the move?)
- Variance premium signal validation (P&L in vol-rich vs vol-fair environments)
- Exit type distribution (profit target, stop loss, held to expiry)

**Methodology:**
- Fetches OHLCV from Yahoo Finance
- Replays `scanTicker` logic at configurable intervals (daily, weekly, etc.)
- Values positions using Black-Scholes daily (no real options chain data)
- IV proxy: HRV30 × 1.15 (represents variance premium assumption)
- Runs on paper — no slippage, commissions, or bid/ask modelled

**Usage:**
```bash
./nox_backtest watchlist=SPY,QQQ range=2y scan=5 profit=0.50 stop=2.0 capital=35000
```

### 4. Order Router (`execution/OptionsOrderRouter.hpp`)

Routes validated signals to Alpaca:

```cpp
// Contract lookup: find real contracts matching the delta target + expiry
AlpacaContract contract = lookupContract(underlying, target_strike, expiry_date);

// Single-leg: LONG_CALL / LONG_PUT / CSP / CC
// Multi-leg: BULL_CALL_SPREAD / BEAR_PUT_SPREAD / STRADDLE / STRANGLE
OrderResult result = router.route(signal, qty_contracts);
```

**Features:**
- OCC symbol formatting (6-char root, YYMMDD, C/P, 8-digit strike×1000)
- Multi-leg order class (`mleg`) for spreads
- `position_effect: "open"` on all legs (required by Alpaca)
- Market order execution only (no limit order support yet)

**CC validation:**
```cpp
if (signal.strategy == "CC") {
    if (!router.validateCCPosition(underlying, qty_contracts)) {
        // Abort: would place a naked call without 100 shares per contract
        return;
    }
}
```

---

## What's Implemented

✅ **Correct quantitative math**
- Black-Scholes with all Greeks, Newton-Raphson IV solver
- Wilder's exponential-smoothed RSI (not simple average)
- 30-day annualized realized volatility from log-returns
- Thread-safe time functions (`gmtime_r`, not `std::gmtime`)

✅ **Real broker integration**
- Alpaca API for contract lookup, snapshot quotes, order submission
- Multi-leg order routing with `position_effect` field
- Live account equity fetch for position sizing
- Position pre-check for covered calls (no naked short calls)

✅ **Architecture**
- Dual-profile system (conservative BOT vs aggressive PERSONAL)
- Regime state machine gating strategy type
- Variance risk premium as the core edge signal
- Telegram dispatch with Greeks breakdown

✅ **Validation & backtesting**
- Standalone backtester for signal quality assessment
- Parameter sweep support (profit target, stop loss, scan interval)
- Win rate and directional accuracy reporting
- Volume richness breakdown (P&L when IV > RV vs IV ≈ RV)

✅ **Position Manager with exit rules** *(added June 25, 2026)*
- SQLite-backed open position tracking (`open_positions` table in `memory_bank.db`)
- Background monitoring thread (30-min cycle)
- Three exit rules: 50% profit target, 21 DTE close for income trades, 2× stop loss
- Telegram alerts on close; market order routing via `OptionsOrderRouter`

✅ **Whole-market scanner** *(3-stage pipeline)*
- Stage 1: fetch ~6,000–8,000 tradable US equities from Alpaca
- Stage 2: batch-snapshot screen by unusual activity (volume × % move)
- Stage 3: RSI / ATR / SMA bar analysis on top-80 candidates, signal → execution engine
- Runs every 30 minutes during market hours; regime-gated

✅ **Intelligence pipeline (Heartbeat Monitor)**
- Daily scout at 09:00 ET: US news + SEC filings + China macro → Claude analysis
- Real-time SEC radar: 8-K and 6-K feeds, watchlist matching, heavyweight filing routing
- IV Rank accumulation: daily EOD snapshots → 52-week percentile builds over time
- `/pulse` command: fast intraday VIX + headlines + contradiction verdicts (~10 sec)
- Weekly performance report: fires at 16:00 ET on last NYSE trading day (holiday-aware)
- Monthly trading journal: written to `reports/` on the 1st of each month

✅ **WS7 Information Lag Window tracker** *(Chinese ADR edge)*
- Opens a window when a material 6-K is filed before CN retail media picks it up
- Polls china-data-engine every 15 min for East Money / Cailian Press pickup
- Computes abnormal return vs MCHI over the lag period
- Grades each event A/B/C/F with Claude; persisted to `lag_windows` table

✅ **IBKR execution path (wired, not yet active)**
- `IBKRClient.hpp/cpp`: async socket layer with lock-free SPSC ring buffer for ticks
- `IBKROrderRouter.hpp`: maps all 8 options strategies to IBKR combo contracts
- Venue flag in `main.cpp`; flip `EXECUTION_VENUE=ibkr` to activate
- 3 items remaining before production-ready (see `execution/IBKR_MIGRATION.md`)

---

## What's NOT Implemented (and why)

❌ **Real options chain data** — The backtester uses Black-Scholes re-pricing with an IV proxy (HRV × 1.15). Real historical mid-prices require Polygon.io (~$29/month) or CBOE DataShop. Backtest results are estimates, not market-accurate.

❌ **True 52-week IV Rank** — Accumulation started (daily EOD snapshots via `historical_volatility` table). Full rank becomes meaningful after 30+ trading days of data per ticker. It is silently absent on first paper-trading day.

❌ **American exercise valuation** — Greeks assume European exercise. Equity options are American; early-exercise premium is usually <1% for short-dated contracts but material beyond 60 DTE.

❌ **Volatility surface & skew** — Uses an OI-weighted average IV per ticker (no strike-by-strike surface). Real OTM puts carry a volatility premium not captured here, making strike selection slightly optimistic.

❌ **Limit order support** — All orders are market-only. Fills may differ from theoretical prices, especially for less-liquid options.

❌ **Earnings avoidance** — No filter to suppress signals 3 days before earnings. This is the single highest-impact missing safety gate. The Alpaca corporate actions endpoint provides this data at no extra cost.

❌ **PositionManager ↔ OptionsSignalGenerator wiring** — `PositionManager::add_position()` exists and works but is never called after `executeSignal()`. Until this is connected, positions won't be tracked by the exit-rule monitor. **This is the most critical gap for paper trading.**

❌ **`trade_history` / `trade_predictions` population** — The heartbeat's `memory_bank.db` tables exist but the execution engine does not write to them. The weekly report P&L and MAE fields will show zero / N/A until this cross-container write path is implemented.

---

## Edge & Signal Validation

### The Variance Risk Premium Signal

**Claim:** Options are overpriced relative to realized volatility on average. Historically, implied vol > realized vol by 3-5 vol points, creating a carry for sellers.

**Implementation:** Sell premium when `IV > HRV30 × 1.20` (20% premium over recent realized).

**Validation needed:** Run the backtester with `range=2y` across your watchlist. The report will show:
```
Vol RICH (IV>HRV×1.20)  : N trades | avg $+X
Vol FAIR                : M trades | avg $+Y
Vol CHEAP (IV<HRV×0.90) : P trades | avg $+Z
```

If RICH avg > FAIR avg, the signal has real edge. If not, either:
1. The IV proxy (HRV × 1.15) is too noisy — upgrade to real historical IV
2. Your watchlist has unusual vol structure — test individual tickers
3. The variance premium is smaller than transaction costs — not tradeable

### Directional Bias Accuracy

The backtester reports directional hit rate:
```
Bullish signals : 55 / 100 (55%) correct
Bearish signals : 48 / 100 (48%) correct
```

For an edge you need >52% on ≥50 signals (coin flip is 50%, you need statistical significance). The RSI + SMA bias filters are standard and widely-known, so expect diminishing returns.

---

## How to Validate This Before Live Trading

1. **Run the full 2-year backtest** across your watchlist:
   ```bash
   ./nox_backtest watchlist=SPY,QQQ,AAPL,TSLA,NVDA range=2y
   ```
   Look for ≥52% directional accuracy on ≥50 signals.

2. **Test parameter sensitivity:**
   ```bash
   # Does a tighter stop improve expected value?
   ./nox_backtest stop=1.5 range=2y
   
   # Which strategies have edge?
   ./nox_backtest watchlist=TSLA range=2y
   ```

3. **Paper trade for 60-90 days.** Real market data (slippage, bid/ask, execution quality) will show whether backtest results are real or optimistic.

4. **Validate regime gating:** During RISK_OFF periods (VIX > 25), does the engine correctly suppress directional trades and favor income strategies?

---

## Code Quality & Safety

**Thread safety:**
- `gmtime_r` instead of `std::gmtime` (reentrant)
- Two concurrent scan threads (BOT + PERSONAL profiles) operate independently
- No global mutable state except the regime machine (read-only after initialization)

**Input validation:**
- All market data fetches have explicit timeouts (8s connect, 15s read)
- Payloads fail fast on schema errors (HTTP 400)
- Kelly sizing enforces hard caps (never risk >10% of equity, never zero-share)

**Transparency:**
- Every signal fires a Telegram alert with Greeks breakdown
- All market data failures log and retry with exponential backoff
- No silent failures — a missing market data fetch aborts the scan cycle

**Testing:**
- Standalone backtester validates signal quality on historical data
- All Greeks formulas verified against standard financial calculator outputs
- Multi-leg order routing tested on Alpaca paper trading

---

## How to Build

```bash
cd /root/Nox/execution

# Build the live engine
g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -I. \
    -o nox_engine main.cpp \
    -lssl -lcrypto -lpthread

# Build the backtester
g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -I. \
    -o nox_backtest backtest_main.cpp \
    -lssl -lcrypto -lpthread
```

**Dependencies:** OpenSSL, pthreads, C++17

**Environment variables (required for live trading):**
```bash
export ALPACA_API_KEY="your-key"
export ALPACA_SECRET_KEY="your-secret"
export ALPACA_BASE_URL="https://paper-api.alpaca.markets"  # or live
export TELEGRAM_BOT_TOKEN="your-bot-token"
export TELEGRAM_CHAT_ID="your-chat-id"

# Profile configs (env vars control default, code has constants)
export OPTIONS_BOT_WATCHLIST="SPY,QQQ,AAPL,TSLA,NVDA"
export OPTIONS_BOT_AUTO_EXECUTE="false"  # true = execute orders (dangerous!)
```

---

## What a Quant Interviewer Will Ask

1. **Derive Black-Scholes from first principles.**
   - Start from the replicating portfolio, write the PDE, solve it.
   - Know the boundary conditions (value of calls/puts at expiry).

2. **What are the weaknesses of BS?**
   - Assumes constant volatility (real markets have vol surface and skew).
   - Assumes log-normal returns (doesn't capture fat tails or jumps).
   - European exercise only (real equities are American).
   - No dividend yield adjustment (code treats equity options as paying no div).

3. **Why is the variance risk premium real?**
   - Sellers of premium demand compensation for being short gamma.
   - When spot moves fast, you lose money re-hedging a short option.
   - IV > RV on average, but realized-vol sellers get negative skewness.

4. **How do you validate the signal on real data?**
   - Walk-forward backtesting: train on years 1-3, test on year 4 (out-of-sample).
   - Parameter stability: results shouldn't change if you shift the window 30 days.
   - Look for >52% directional accuracy on ≥50 signals.

5. **What would you change for production?**
   - Real options chain (Polygon.io) instead of BS re-pricing.
   - True IV rank (52-week percentile) instead of snapshot-relative.
   - Limit order support instead of market-only.
   - Position manager thread with 21 DTE exit rule for income trades.
   - Earnings filter (skip signals 3 days before/after earnings).

---

## For Tsinghua / Quant Applications

**What you can honestly claim:**
- Built a correct Black-Scholes pricer with all Greeks from scratch
- Implemented a Newton-Raphson IV solver that converges
- Integrated with a real broker API (Alpaca) for multi-leg routing
- Designed a regime-based strategy gate (VIX + SPY 200-SMA)
- Created a backtester to validate signal quality on historical data
- Understand why the variance risk premium exists and how to test it

**What NOT to claim:**
- This generates alpha or is profitable (paper trading, unvalidated)
- The backtester uses real option prices (it uses Black-Scholes with an IV proxy)
- The IV rank is a true 52-week percentile (it's a snapshot-relative metric)
- The Greeks are perfect (American exercise adjustments missing, no skew model)

**Why this is a strong portfolio piece:**
- Shows you can code, understand derivatives math, and think about risk
- Demonstrates knowledge of broker APIs and real-world constraints
- The honesty about limitations is more impressive than overselling edge

---

## Next Steps (Private Development)

See [PRIVATE_ROADMAP.md](docs/PRIVATE_ROADMAP.md) for the full development plan including:
- Earnings avoidance filter (highest-impact improvement)
- True IV rank accumulation (free, takes time)
- Position manager with exit rules (required for live)
- Real options chain integration (small cost, huge accuracy gain)

---

## References

- Hull, *Options, Futures, and Other Derivatives* — standard reference
- Natenberg, *Option Volatility and Pricing* — practitioner perspective
- Black & Scholes (1973) — original paper
- Carr & Wu (2009), "Variance Risk Premiums" — theoretical backing for the signal
- Alpaca Trade API docs — broker integration
