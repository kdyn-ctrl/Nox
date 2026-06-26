# Nox — Strategy Development & Operations Guide (Beginner Edition)

> **Who this is for:** you, the operator/developer of Nox, when you want to go
> from *"I have an idea for a trade"* to *"it's running and being monitored"*
> without blowing up the account or fooling yourself with bad statistics.
>
> This is the **private** companion to the public `NOX_USER_GUIDE.md` (which
> explains operating the bot day-to-day) and `PRIVATE_ROADMAP.md` (forward
> development). Read those too — this doc focuses on the *method* of building
> strategies and the *mechanics* of driving the system you've built.

---

## Part 0 — The one rule that matters most

**Do not risk real money on a strategy you have not (1) backtested out-of-sample,
(2) checked for statistical significance, and (3) paper-traded.**

Almost every beginner mistake is a variation of skipping one of those three. The
rest of this guide is mostly about *how* to do them on this system.

---

## Part 1 — Concepts you need before writing any strategy

You don't need a finance degree, but you need these eight ideas. Each has a
one-line "why it matters here."

| Concept | Plain meaning | Why it matters in Nox |
|---|---|---|
| **Edge / expectancy** | Average $ you expect to make per trade over many trades | If expectancy ≤ 0, no risk management saves you. Kelly sizing *halts* on non-positive edge (RULE-005). |
| **Win rate (W)** | Fraction of trades that profit | Feeds Kelly: `KELLY_WIN_RATE`. |
| **Win/Loss ratio (R)** | Avg win size ÷ avg loss size | Feeds Kelly: `KELLY_WIN_LOSS_RATIO`. |
| **Kelly fraction** | How much of capital to risk for optimal long-run growth | The engine uses **fractional** Kelly (`KELLY_FRACTION`, default 0.15) capped at 10%. Full Kelly is too aggressive — see Part 4. |
| **Drawdown** | Peak-to-trough drop in equity | The number that makes you quit. A 50% drawdown needs a 100% gain to recover. |
| **Overfitting (curve-fitting)** | Tuning rules until they look perfect *on past data* | The #1 way backtests lie. Defeated by out-of-sample testing + MCPT. |
| **Look-ahead bias** | Using information the strategy couldn't have known yet | The backtesters here are written "strict no-lookahead" (`bars[0..end_idx]` only). Preserve that if you edit them. |
| **Regime** | Is the market risk-on or risk-off right now? | Nox gates entries on VIX + SPY-200SMA via the `RegimeStateMachine`. A strategy that ignores regime will trade into crashes. |

### Options-specific concepts (only if trading options)

- **The Greeks** — Delta (direction exposure), Theta (time decay, your friend when
  selling / enemy when buying), Vega (volatility exposure), Gamma (how fast delta
  moves). Nox computes all five via Black-Scholes (`OptionEngine.hpp`).
- **IV vs HRV** — *Implied* vol (what the option price predicts) vs *Historical
  Realized* vol (what the stock actually did). Nox's core options edge is the
  **variance risk premium**: when `IV > HRV × 1.20`, options are "rich" → prefer
  *selling* premium (CSP/CC/strangle). When `IV < HRV × 0.90`, "cheap" → *buy*
  premium (long calls/puts/straddle).
- **DTE** — Days to expiration. Short DTE = fast theta but more gamma risk.
- **The 8 strategies Nox knows:** LONG_CALL, LONG_PUT, CSP (cash-secured put),
  CC (covered call), BULL_CALL_SPREAD, BEAR_PUT_SPREAD, STRADDLE, STRANGLE —
  selected from directional bias × vol regime in `selectStrategy()`.

---

## Part 2 — The strategy development lifecycle

Think of it as a funnel. Most ideas should *die* in steps 2–4. That's the point.

```
1. Hypothesis        →  "Selling puts on oversold large-caps in RISK_ON has edge"
2. Define rules      →  exact entry/exit/sizing, no ambiguity
3. Backtest          →  nox_backtest / backtest-engine on historical data
4. Validate          →  out-of-sample + MCPT significance + walk-forward
5. Risk-size         →  set Kelly params / risk tier / regime gates
6. Paper trade       →  Alpaca paper account, watch for 2–4 weeks
7. Go live small     →  smallest size that's meaningful, scale slowly
8. Monitor & review  →  Telegram alerts + periodic performance review
```

### Step 1 — Hypothesis
Write it as one falsifiable sentence: *"When [condition], [instrument] tends to
[behavior], which I can capture with [strategy]."* If you can't state the
condition precisely, you can't code or test it.

### Step 2 — Define rules with zero ambiguity
A strategy is **entry rule + exit rule + position size**. For example:
- *Entry:* bias = Bullish (price > SMA20 > SMA50, RSI 40–65) **and** regime = RISK_ON.
- *Exit:* +50% of max profit, OR stop at 2× debit, OR 21 DTE (whichever first).
- *Size:* 1% of regime-adjusted equity (risk tier 1).

These map directly onto knobs that already exist in the code — you rarely need to
invent new logic, just change parameters.

### Step 3 — Backtest (see Part 3 for commands)
Run it. Look at: number of trades, win rate, avg win/loss, total P&L, max
drawdown. **Be suspicious of good results**, especially with few trades.

### Step 4 — Validate (the step beginners skip)
- **Out-of-sample (OOS):** Develop/tune on one period (e.g. 2015–2021), then test
  *once* on a period you never looked at (e.g. 2022–2025). If it falls apart OOS,
  it was overfit.
- **Statistical significance (MCPT):** A strategy can look good by luck. The
  Monte Carlo Permutation Test (`src/utils/mcpt/`) shuffles your returns many
  times and asks "could random ordering produce a result this good?" A **p-value
  < 0.05** means probably not luck. *Important:* MCPT only works on
  **path-dependent** statistics (max drawdown, equity-curve metrics) — not on
  mean/Sharpe, which are identical under shuffling. The bundled example uses max
  drawdown for exactly this reason.
- **Walk-forward (advanced):** Re-optimize on a rolling window and test on the
  next window, repeatedly. Closest thing to "how it would have actually run."
- **Sample size:** < 30 trades → don't trust it. "100% win rate on 3 trades" is
  noise (the roadmap literally caught this with `BEAR_PUT_SPREAD`).

### Step 5 — Risk-size (Part 4).
### Step 6 — Paper trade
Point `ALPACA_BASE_URL` at `https://paper-api.alpaca.markets`. Let it run on fake
money. You're checking that *live behavior matches the backtest* (slippage, fills,
data gaps, alert flow) — not chasing profit yet.

### Step 7 — Go live small, scale slowly.
### Step 8 — Monitor & review (Part 6).

---

## Part 3 — Using the backtesters

You have **two** backtesters. Use the one matching your strategy.

### A) Options strategy backtester — `nox_backtest`
Replays the live `OptionsSignalGenerator` logic on Yahoo OHLCV and re-prices
with Black-Scholes.

```bash
cd /root/Nox/execution
# Build (requires PositionManager.cpp + sqlite3 for the engine; backtester itself is lighter)
g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -I. \
    -o nox_backtest backtest_main.cpp -lssl -lcrypto -lpthread

# Run — all args are key=value
./nox_backtest watchlist=SPY,QQQ,AAPL range=2y capital=50000
./nox_backtest watchlist=NVDA range=5y scan=5 profit=0.50 stop=2.0 profile=personal
./nox_backtest --help        # full arg list
```
Key args: `watchlist`, `range` (1y/2y/5y), `scan` (every N trading days, must be ≥1),
`profit` (take-profit fraction), `stop` (stop as ×debit), `capital` (sets the tier
gate), `profile=personal` (aggressive profile).

### B) Equity/regime backtester — `backtest-engine`
For directional SPY-style strategies using the regime machine + Kelly.

```bash
cd /root/Nox/backtest-engine
g++ -std=c++17 -O2 -o backtester main.cpp
./backtester    # reads the data CSV; writes trades.csv
```
Refresh its data first with `scripts/download_data.py` (SPY + VIX history).

### C) Statistical significance — MCPT
```bash
cd /root/Nox
g++ -std=c++17 -pthread -o build/mcpt_main src/utils/mcpt/main.cpp src/utils/mcpt/mcpt.cpp
./build/mcpt_main
```
Feed it your strategy's per-trade returns and read the p-value.

> **Honesty check the backtest gives you (already learned, per roadmap):** with a
> flat IV proxy the variance-premium signal never fires, and SPY straddles
> underperform because realized vol stayed below implied. Real options data
> (paid feed) is needed to fully trust the income strategies. Treat current
> options backtests as *directional sanity checks*, not gospel.

---

## Part 4 — Position sizing & risk (the part that keeps you alive)

Nox sizes positions three ways, chosen by the `risk_tier` field on a signal:

- **Tier 1 ("Standard")** — risks **1%** of regime-adjusted equity, 2.0× ATR stop.
- **Tier 3 ("Let the knife cut")** — risks **5%**, 3.5× ATR stop. Aggressive.
- **Tier 0 / anything else** — **Kelly Criterion** sizing.

### Kelly, explained simply
Kelly says the growth-optimal risk fraction is `K = W − (1−W)/R`. With Nox's OOS
winner params (W≈0.68, R≈2.32) raw Kelly ≈ 55% — *insanely* aggressive. So Nox:
1. Multiplies by `KELLY_FRACTION` (default **0.15**) → ~8.2%.
2. Hard-caps at **10%** of equity (RULE-005).
3. **Halts the trade** if Kelly ≤ 0 (no edge) or if it can't afford one share —
   it does *not* silently fall back to a token position.

**Beginner guidance:** start with `KELLY_FRACTION` *lower* than you think (0.10–0.15).
Fractional Kelly massively reduces drawdown for only a small hit to growth. Full
Kelly will give you gut-wrenching swings.

### Hard safety gates that run regardless of your strategy
- **Regime gate** — VIX ≥ 30 or SPY < 200-SMA ⇒ new entries blocked (capital
  multiplier 0).
- **RSI floor** — equity BUYs below RSI 30 are blocked.
- **Notional ceiling** — order value can't exceed 10% of equity (catches price
  spikes between sizing and submission).
- **CN rules** — board-lot truncation (100-share lots) and T+1 same-day-sell block
  for A-shares.
- **Earnings gate** — options scans skip tickers with earnings within 5 days.

You get these for free. A new strategy mostly means choosing *parameters*, not
removing safety.

---

## Part 5 — Operating the system

### 5.1 The components (what's running)
| Service | Port | Role |
|---|---|---|
| `execution-engine` (`nox_engine`) | 8080 | Receives signals via `/webhook`, sizes & routes orders to Alpaca, runs the two options scanners + the position monitor. |
| `analyst-brain` (`analyst_agent`) | — | Every `ANALYST_CYCLE_HOURS`, fetches VIX+SPY, evaluates regime, posts a REPORT audit to the engine. |
| `america-data-engine` | 8001 | Alpaca news + earnings calendar (FastAPI). |
| `china-data-engine` | 8000 | East Money hot board, PMI, LPR, Cailian news (akshare). |
| `heartbeat/monitor.py` | — | Telegram bot: status command, SEC EDGAR filing radar, IV rank, portfolio. |

### 5.2 Configuration (`.env`)
Required by the engine (it hard-aborts if any are missing):
`ALPACA_API_KEY`, `ALPACA_SECRET_KEY`, `ALPACA_BASE_URL` (paper vs live!),
`WEBHOOK_SECRET_TOKEN`, `KELLY_WIN_RATE`, `KELLY_WIN_LOSS_RATIO`, `KELLY_FRACTION`,
`TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`.
Optional: `OPTIONS_BOT_*` / `OPTIONS_PERSONAL_*` (watchlists, scan interval,
auto-execute, contract qty), `CN_BOARD_LOT_SIZE`, `CN_POSITIONS_PATH`,
`ANALYST_CYCLE_HOURS`.

> **The single most important switch:** `ALPACA_BASE_URL`.
> `paper-api.alpaca.markets` = fake money. `api.alpaca.markets` = **real money**.
> Triple-check this before every change.

### 5.3 Build & run
```bash
# Build the live engine (note: PositionManager.cpp + -lsqlite3 are required)
cd /root/Nox/execution
g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -I. \
    -o nox_engine main.cpp PositionManager.cpp \
    -lssl -lcrypto -lpthread -lsqlite3

# Bring up the whole stack
cd /root/Nox
docker-compose up -d          # start all services
docker-compose ps             # check health
docker-compose logs -f execution-engine   # tail logs
docker-compose down           # stop
```

### 5.4 Sending a signal manually (the webhook contract)
The engine listens on `POST /webhook`. A signal is JSON (single object or array):
```json
{
  "secret_key": "<WEBHOOK_SECRET_TOKEN>",
  "ticker": "AAPL",
  "action": "BUY",                 // BUY | SELL | HOLD | REPORT
  "price": 195.50,
  "rsi": 48.0,
  "atr": 3.2,
  "risk_tier": 1,                  // 0/blank=Kelly, 1=1%, 3=5%
  "stop_loss_atr_multiplier": 2.0,
  "vix": 18.0,
  "spy_price": 530.0,
  "spy_200_sma": 510.0,
  "trade_date": "2026-06-26"       // optional; for backtester/T+1
}
```
Notes:
- A **wrong `secret_key` is silently dropped** (returns 200, logs a WARN) — by
  design, so attackers can't fingerprint the auth boundary. If your signal seems
  ignored, check the secret first.
- `action: "REPORT"` is an audit heartbeat (analyst uses it); it ignores
  price/sizing fields.
- The options scanners run on their own timers — you don't push those; they alert
  (and optionally auto-execute) on their own.

### 5.5 Bot vs Personal profiles
- **BOT** — conservative; enforces tier gates and regime gate; can `auto_execute`
  if you enable it (`OPTIONS_BOT_AUTO_EXECUTE`).
- **PERSONAL** — aggressive; all strategies open; **always advisory** (never
  auto-executes) — it just sends you Telegram signals to act on manually.

Start with auto-execute **off**. Watch the advisory signals for weeks. Only enable
auto-execute once you trust them and have paper-traded.

---

## Part 6 — Monitoring & reviewing

- **Telegram is your dashboard.** Green = normal, Yellow = caution/gate, Red =
  problem (see `NOX_USER_GUIDE.md` for the full alert taxonomy). Send the status
  command to the bot for a health snapshot.
- **Alpaca dashboard** — ground truth for positions/fills. Reconcile against
  Telegram periodically.
- **Review cadence:** weekly, look at realized win rate vs your backtest's win
  rate. Drift means the edge is decaying or the market regime changed.
- **Position monitor:** the engine's `PositionManager` thread auto-applies
  profit-target / stop / 21-DTE exits to *single-leg* options positions it
  recorded. Multi-leg positions are **not** auto-managed yet (you'll see a WARN) —
  manage those manually for now (see roadmap).

---

## Part 7 — Common beginner traps (memorize these)

1. **Overfitting** — if you tuned 6 parameters until the curve looked perfect,
   it's fit to noise. Fewer parameters, always check OOS.
2. **Tiny samples** — a strategy needs dozens of trades before its stats mean
   anything.
3. **Ignoring costs** — commissions, slippage, and the bid/ask spread (especially
   on options) eat thin edges alive.
4. **Trading into regime** — the gates protect you; don't disable them to "catch
   the bounce."
5. **Live before paper** — never. Paper first, every time.
6. **Confusing luck with skill** — run MCPT. A pretty equity curve with p=0.4 is
   a coin flip.
7. **Real-money URL left in `.env`** — verify `ALPACA_BASE_URL` before every run.
8. **Adding look-ahead bias** — when editing a backtester, only ever read bars up
   to the current index.

---

## Part 8 — A concrete first project (do this)

1. Pick one liquid name (SPY).
2. Hypothesis: *"Buying long calls when bias is Bullish and IV is cheap
   (IV < HRV×0.9) has positive expectancy in RISK_ON."*
3. `./nox_backtest watchlist=SPY range=5y` — read trades, win rate, drawdown.
4. Split: eyeball 2018–2022 vs hold out 2023–2025. Does it survive OOS?
5. Run MCPT on the returns. p < 0.05?
6. If it survives: set `risk_tier=1`, point at paper, watch advisory signals 2–4
   weeks.
7. Only then consider auto-execute / live, at minimum size.

If it dies at step 4 or 5 — **good**. You just saved real money. Move to the next
idea.

---

*Private document — Nocturnal repo only. Keep strategy logic and parameters out
of the public Nox repo.*
