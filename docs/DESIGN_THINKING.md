# Nox — Design Thinking & Development Notes

This document captures the reasoning behind Nox's architecture, what the backtests revealed, and the technical direction forward. It's meant to show **how I think about trading systems**, not to be a recipe for profitability.

---

## Why C++ + Black-Scholes for an options engine?

**The core insight:** Options pricing requires tight latency on ticks + precise numerical computation. C++ delivers both.

- **Black-Scholes** is the industry standard. It's not perfect (constant vol assumption, European exercise, no dividends), but it's *known* — implementable from first principles, validatable against paper, and extensible (jump-diffusion, stochastic vol) when needed.
- **Five Greeks** (delta, gamma, theta, vega, rho) give you the sensitivities to hedge or express directional + volatility views. Gamma is the most important for understanding risk: it tells you how fast your delta changes.
- **Newton-Raphson IV solver** instead of a lookup table forces you to understand the input-output relationship, not memorize a grid.
- **C++** + httplib gives you the speed for real-time tick ingestion and sub-100ms order placement. The regexes on signal parsing, the mutex guards on open positions, the atomic flags on shutdown — these aren't premature optimization, they're about correctness under concurrency.

**Trade-off:** C++ means more code to write correctly. Python would have been faster to prototype; C++ is right for a system you might run for years.

---

## Architecture: Dual Profiles + Regime Gating

**Why two profiles?**
- **BOT** — conservative: enforces capital tiers (higher bars to unlock aggressive strategies), gates on macro regime, auto-executes only single-leg orders.
- **PERSONAL** — high-risk-tolerance advisory: all strategies available, no tier gates, signals only (manual execution).

This separation lets you test without blowing up the account. BOT runs unattended; PERSONAL is a sandbox for ideas.

**Why regime gating (VIX + SPY 200-SMA)?**
The single biggest predictor of options strategy performance is *market regime*. When VIX ≥ 30 or SPY is below its 200-day SMA, volatility is elevated and mean-reversion trades die. A system that ignores regime and trades into crashes learns an expensive lesson.

**Why Kelly fraction 0.15, not full Kelly?**
Raw Kelly (K = W − (1−W)/R) is mathematically optimal for long-run growth. With our parameters (W≈0.68, R≈2.32), raw Kelly ≈ 55% — insanely aggressive for live trading. Fractional Kelly (multiply by 0.15) drops it to ~8.2%, capped at 10%. You lose ~5% long-run growth but cut the worst drawdowns in half. For a real account, that trade is worth it.

---

## What the Backtests Revealed

### The IV proxy signal doesn't work as designed
- **Hypothesis:** when IV > HRV × 1.20, sell premium (CSP/CC/strangle) because options are overpriced.
- **Reality:** with a flat IV proxy (HRV × 1.15), the signal never fires. Real IV surface data is required — you need:
  - True snapshot IV (Alpaca gives this, but only point-in-time)
  - Historical 52-week IV percentile (true IV Rank, not snapshot-relative; costs money or requires 1 year of data collection)
  - Volatility surface (strike × expiry IV grid; premium feature)
- **Lesson:** don't commit to a signal without real data backing it. The variance risk premium exists, but you need the right instruments to capture it.

### SPY straddles underperformed
- Long straddles (buy a call and put at ATM) profit when realized vol > implied vol.
- Historical data shows realized vol **consistently below** implied vol on SPY — the variance premium is real, and it's being collected by the people you're paying to sell you the options.
- **Lesson:** mean-reverting to expected moves is harder than it looks. The distribution of moves is left-skewed (rare large moves down), so the average P&L on straddles is negative.

### Small-sample illusions
- BEAR_PUT_SPREAD showed 100% win rate on 3 trades.
- Another strategy showed 50% win rate on 2 trades.
- **Lesson:** under 30 trades, your backtest is noise. You need at least 50 trades, ideally 100+, to trust the win rate. A single black swan can wipe out years of 55% win-rate edge.

### Holding to expiry is the main source of losses
- The backtester showed that letting positions decay to expiration cost more than taking the 50% profit or 21-DTE exit.
- Theta works against you when you're long premium (long calls/puts) and for you when you're short (CSP/CC).
- **Lesson:** set hard exit rules (profit target, time exit, stop loss) and honor them. The temptation to "let this one run" is how edge erodes.

---

## Known Limitations (Not Excuses — Just Reality)

1. **No real options prices** — the backtester re-prices via Black-Scholes with an IV proxy (HRV × 1.15). Real backtests use historical option bid/ask spreads.
2. **Market-order only** — Alpaca's API forces market fills. Limit orders are cheaper but have execution risk.
3. **American exercise not modeled** — equity options are American (exercise anytime). The engine prices them as European. Early-exercise values matter most for short-dated ITM puts.
4. **No transaction costs** — commissions and bid/ask spreads aren't subtracted. They're small on SPY but material on single stocks.
5. **Backtester is backtest-only** — no walk-forward validation (train on rolling windows, test on fresh data). That's the next level of rigor.
6. **IV surface collapsed to a point** — real pricing uses a 2D grid (strike × expiry IVs). Using a single IV understates the gamma risk on spread wings.

---

## Design Decisions I'd Defend

- **Regime gating over trend-following** — macro regime (VIX, SPY 200-SMA) is more predictive than 20/50-SMA crosses for option strategy performance.
- **Kelly sizing with hard stops** — fractional Kelly + no exceptions for "just one more" has the best risk-adjusted return profile empirically.
- **Atomic flags + mutex guards on state** — correctness > performance in options trading; concurrency bugs are worse than 1ms latency.
- **Alpaca integration** — REST API is simple and good enough for paper/small live. The next upgrade (Interactive Brokers) is only needed at scale.
- **Backtester as a standalone tool** — not integrated into the live engine means you can iterate on historical params without restarting production.

---

## The Roadmap (Ordered by Impact)

### Near-term (weeks, no external data needed)
1. **Earnings avoidance** ✅ DONE — skip tickers with earnings within 5 days. Single biggest source of gap-through-strikes losses.
2. **Position monitoring & exits** ✅ DONE — persist open positions, auto-close on 50% profit / 21 DTE / stop loss. Backtests showed holding to expiry bleeds money.
3. **Parameter sweep** — test stop-loss tightness (1.0× vs 1.5× vs 2.0×) and scan frequency (daily vs weekly) to find what maximizes win rate × expected value.

### Medium-term (weeks to months, requires data)
4. **Historical IV accumulation** ✅ DONE — save daily IV snapshots to build a 52-week percentile. This is free if you start now; costs money if you buy it.
5. **True IV Rank consumption** — wire the 52-week IV percentile into the buy/sell-premium gates so they actually fire on real volatility regimes.
6. **Portfolio Greeks tracking** — delta/gamma/theta/vega per position, net portfolio Greeks. Needed for hedging and understanding tail risk.

### Advanced (months, architecture work)
7. **Delta hedging** — once you know net portfolio delta, offset directional risk with small SPY positions or put spreads. Useful for pure vol plays (straddles/strangles).
8. **Volatility surface** — real multi-leg pricing needs per-strike IVs. This requires Polygon.io or similar historical chain data.
9. **Limit order routing** — trade latency for cost by using limit orders with execution probability modeling.
10. **Interactive Brokers migration** — native socket API for richer order control, futures/index options, non-US venues.

---

## What's Next for a Real Implementation

If I were building this for real capital:

1. **Spend a month on the backtest** — parameter sweep (what stop tightness works?), walk-forward validation (does it work OOS?), Monte Carlo on returns (is it luck?).
2. **Paper trade for 60+ days** — live market data is the only data that matters. Slippage, fills, and real queue dynamics show in paper before real capital.
3. **Track portfolio Greeks** — know your net delta/gamma/vega. Hedging when gamma gets extreme is how you survive vol spikes.
4. **Get real IV data** — 1 year of accumulation is free (just save daily snapshots); $29/month gets you Polygon.io historical chains. The difference in backtest fidelity is huge.
5. **Use limit orders** — market orders leave money on the table. The cost is execution risk, which you manage with wide limits and time-priority logic.
6. **Audit transaction costs** — slippage + commission + bid/ask are often >50% of edge in equity options. Measure them in paper before going live.

---

## On Validation & Epistemic Humility

The hardest part of systematic trading isn't the code — it's staying honest about what you don't know.

- A backtest that **looks good** is usually **overfit to the data you tested on**, not ready for live trading.
- A signal that **worked on SPY 2015-2022** might **fail on SPY 2023-2025** because the regime changed (rates, vol surface, player composition).
- A 55% win rate on 50 trades **could still be luck** — run a Monte Carlo permutation test (MCPT) to see if the p-value says it's real.
- **Acknowledge your assumptions.** This engine assumes: constant vol (wrong), European exercise (wrong for equity options), no dividends (wrong for some names), log-normal returns (empirically fat-tailed).

These aren't excuses — they're constraints you price in. Every real trading system makes trade-offs between fidelity and practicality. The key is knowing which ones you made and why.

---

*This document is a snapshot of how I think about building trading systems: combining first-principles theory (Black-Scholes, Kelly Criterion, variance risk premium) with empirical reality (backtests fail OOS, small samples mislead, real execution is hard). It's not a recipe for profit — it's a record of the reasoning.*
