# Nox Trading System — Complete Guide

**For anyone who wants to understand exactly what this system does, how it works, and how to use every feature.**

No programming knowledge required. If you built this yourself and want to understand it end-to-end, or you're handing this off to someone else, this is the document.

---

## Table of Contents

1. [What Is Nox?](#1-what-is-nox)
2. [The Big Picture — How Everything Connects](#2-the-big-picture--how-everything-connects)
3. [The Five Services](#3-the-five-services)
4. [How Equity Trades Work — Step by Step](#4-how-equity-trades-work--step-by-step)
5. [The Regime System — When Nox Trades and When It Stops](#5-the-regime-system--when-nox-trades-and-when-it-stops)
6. [Position Sizing — How Nox Decides How Much to Buy](#6-position-sizing--how-nox-decides-how-much-to-buy)
7. [Risk Rules — The Safety Gates](#7-risk-rules--the-safety-gates)
8. [Options Signal Generator](#8-options-signal-generator)
9. [Personal vs Bot Options Profiles](#9-personal-vs-bot-options-profiles)
10. [Reading Your Telegram Alerts](#10-reading-your-telegram-alerts)
11. [Environment Variables — The Control Panel](#11-environment-variables--the-control-panel)
12. [Chinese A-Share Rules (CN-RULE)](#12-chinese-a-share-rules-cn-rule)
13. [Troubleshooting](#13-troubleshooting)
14. [Quick Reference](#14-quick-reference)

---

## 1. What Is Nox?

Nox is a self-contained algorithmic trading system that runs on your VPS (a cloud server). It connects to your Alpaca brokerage account and trades automatically on your behalf.

It does three distinct things:

**1. Monitors the market** — constantly checking whether conditions are healthy enough to trade.

**2. Executes equity trades** — based on rule-based exit conditions (take-profit, stop-loss, RSI exhaustion, trend-break), it automatically manages positions and places orders with Alpaca.

**3. Generates options signals** — independently on a background timer, it scans a list of stocks every 30 minutes and sends you options trade ideas via Telegram. It can also place these options trades automatically if you enable that.

Nox does **not** pick stocks from scratch or do its own fundamental analysis. It relies on:
- Rule-based exit logic for equity position management
- Quantitative models (Black-Scholes, Kelly Criterion) for options and sizing
- Macro data (VIX, SPY trend) for regime classification

Think of it as a disciplined execution layer that enforces rules you set, not a stock picker.

---

## 2. The Big Picture — How Everything Connects

Here is the full flow of information through the system:

```
┌─────────────────────────────────────────────────────────────┐
│                        EXTERNAL                              │
│                                                              │
│   Yahoo Finance (VIX, SPY data) ──────┐                      │
│   Alpaca Market Data (options chain) ─┤                      │
└───────────────────────────────────────┼──────────────────────┘
                                        │
                    ┌───────────────────▼──────────────────────────────────────────┐
                    │                   YOUR VPS (Docker)                          │
                    │                                                              │
                    │  ┌─────────────┐    ┌──────────────────────────────────┐   │
                    │  │   Analyst   │───▶│        Execution Engine           │   │
                    │  │   Brain     │    │                                   │   │
                    │  │  (C++)      │    │  • Rule-based position manager    │   │
                    │  │             │    │  • Auto-apply exit rules          │   │
                    │  │  • VIX      │    │  • Sizes positions (Kelly)        │   │
                    │  │  • SPY SMA  │    │  • Routes to Alpaca               │   │
                    │  │  • Regime   │    │  • Options signal generator       │   │
                    │  └─────────────┘    │    (BOT profile + PERSONAL)       │   │
                    │                     └──────────────┬───────────────────┘   │
                    │  ┌─────────────┐                   │
                    │  │  Heartbeat  │                   │
                    │  │  Monitor    │                   │
                    │  │  (Python)   │                   │
                    │  │             │                   │
                    │  │  • Health   │                   │
                    │  │  • Reports  │                   │
                    │  └─────────────┘                   │
                    │                                     │
                    │  ┌──────────────┐  ┌─────────────┐ │
                    │  │ America Data │  │ China Data  │ │
                    │  │   Engine     │  │   Engine    │ │
                    │  │  (Python)    │  │  (Python)   │ │
                    │  └──────────────┘  └─────────────┘ │
                    └─────────────────────────────────────┘
                                        │
                    ┌───────────────────▼─────────────────────┐
                    │               OUTPUT                     │
                    │                                          │
                    │   Alpaca Account (orders placed here)    │
                    │   Your Telegram (all alerts sent here)   │
                    └──────────────────────────────────────────┘
```

Every service runs in its own Docker container. They talk to each other over an internal network (`nox_net`) with no external exposure except error logging.

---

## 3. The Five Services

### 3.1 Execution Engine

**The brain of the operation.** This is a C++ program that runs 24/7 and:

- Listens at port 8080 for incoming signals and commands
- Continuously monitors open positions and applies exit rules
- Validates every signal through a series of safety gates
- Sizes positions using the Kelly Criterion formula
- Places buy and sell orders with Alpaca
- Runs the options signal generator on a background timer
- Sends all trade confirmations and alerts to your Telegram

When you see any trade happen, it was this service that did it.

**Key endpoint:** `POST /webhook` — accepts custom trade signals or regime updates.
**Options endpoint:** `POST /options/price` — you can query Black-Scholes prices directly.
**Health check:** `GET /health` — returns `{"status": "healthy"}` if running.
**Trade ledger:** `GET /trades` — returns all executed trades with exit rules and P&L.

---

### 3.2 Analyst Brain

**The market watcher.** Runs every 6 hours (configurable). It:

1. Fetches the current VIX (fear index) from Yahoo Finance
2. Fetches SPY's price and calculates its 200-day moving average from Yahoo Finance
3. Feeds those numbers into the Regime State Machine
4. Sends the regime classification to the Execution Engine via `/webhook`
5. Alerts you on Telegram with the current market status

This service classifies market conditions (RISK_ON / TRANSITION / RISK_OFF) but does not generate specific entry or exit signals. The exit rules are applied automatically by the Execution Engine based on position metrics and configured thresholds.

---

### 3.3 Heartbeat Monitor

**The watchdog.** A Python service that:

- Monitors that all other services are alive
- Sends health reports to Telegram
- Runs intelligence reports using the Claude AI API
- Checks SEC filings radar for news on your holdings
- Alerts you if anything goes wrong

You don't interact with this service directly. It runs quietly in the background and pages you if something breaks.

---

### 3.4 America Data Engine

**US market data cache.** Connects to Alpaca's market data API to cache equity prices and related data. Used by other services internally so they don't all make separate API calls.

---

### 3.5 China Data Engine

**Chinese market data cache.** Scrapes East Money, Cailian Press, NBS PMI (China's economic activity index), and PBOC LPR (China's benchmark interest rate) on a 15-minute cycle. Used if you trade Chinese A-shares. Runs internally — not exposed to the internet.

---

## 4. How Equity Trades Work — Rule-Based Exits

This is the most important section if you want to understand when and why Nox manages positions.

### Overview

Nox manages open positions continuously using quantitative rules. Rather than relying on external signals, it automatically closes positions when predefined conditions are met. All exit rules are applied in parallel on every evaluation cycle (every 5 minutes or configurable).

### Exit Rules

**1. Take-Profit Exit**
- **Trigger:** Position reaches +15% (or configured `TAKE_PROFIT_PCT`)
- **Action:** Market sell order placed immediately
- **Reason:** Lock in gains before momentum reversal
- **Alert:** `✅ [EXIT] AAPL sold at take-profit (+15.2%)`

**2. Stop-Loss Exit**
- **Trigger:** Position falls to -8% (or configured `STOP_LOSS_PCT`)
- **Action:** Market sell order placed immediately
- **Reason:** Protect capital from further decline
- **Alert:** `🛑 [EXIT] AAPL sold at stop-loss (-8.1%)`

**3. RSI Exhaustion Exit**
- **Trigger:** RSI > 78 (overbought) AND price falls below 20-SMA
- **Action:** Market sell order placed
- **Reason:** Detect momentum exhaustion before reversal
- **Alert:** `⚠️ [EXIT] AAPL sold on RSI exhaustion (78.5)`

**4. Trend-Break Exit**
- **Trigger:** Price falls below 200-day SMA × 0.98
- **Action:** Market sell order placed
- **Reason:** Long-term trend has broken; position is no longer valid
- **Alert:** `📉 [EXIT] AAPL sold on trend-break (below 200-SMA)`

**5. Time-Stop Exit**
- **Trigger:** Position held > 10 days (or configured `MAX_HOLD_DAYS`)
- **Action:** Market sell order placed
- **Reason:** Prevent indefinite hold; reduce capital lock-up
- **Alert:** `⏱️ [EXIT] AAPL sold on time-stop (10 days)`

**6. Trailing Stop Exit**
- **Trigger:** Price falls by ATR × 2.0 distance from entry
- **Action:** Stop-loss order triggers at broker
- **Reason:** Dynamic protection that adapts to volatility
- **Alert:** `🛑 [EXIT] AAPL sold by trailing stop`

### Position Sizing (Entry Rules)

When new positions are entered:

**Kelly Criterion (Tier 0)**
- Mathematical formula based on historical win rate and win/loss ratio
- Adapts position size to confidence level
- Never exceeds 10% of portfolio per trade

**Fixed Tiers (Tier 1, 3)**
- **Tier 1**: 1% of portfolio regardless of Kelly
- **Tier 3**: 5% of portfolio, wider stop loss (3.5× ATR)

### Regime Gates (Portfolio Protection)

Before ANY action, the regime is checked:

- **RISK_ON**: All rules apply normally; full capital available
- **TRANSITION**: All rules apply but position sizes are cut by 50%
- **RISK_OFF**: Entry rules are blocked; only exit rules apply

More on regimes in Section 5.

---

## 5. The Regime System — When Nox Trades and When It Stops

The regime system is the most important safety mechanism. It answers the question: **"Is the market environment safe enough to take on new risk right now?"**

### The Three Regimes

```
VIX < 35 AND SPY > 200-day SMA
           ↓
        RISK_ON ✅
        Full capital deployed
        Stop loss: 2× ATR

VIX < 35 AND SPY between SMA×0.98 and SMA
           ↓
       TRANSITION 🟡
        Half capital deployed
        Stop loss: 1.5× ATR

VIX ≥ 35 OR SPY < SMA × 0.98
           ↓
       RISK_OFF 🔴
        Zero new entries
        Stop loss: 1.0× ATR
```

### VIX — The Fear Index

VIX measures how much the options market expects the S&P 500 to move over the next 30 days. It's essentially a gauge of fear and uncertainty:

- **VIX < 15**: Very calm markets. Everyone is confident.
- **VIX 15–25**: Normal market conditions.
- **VIX 25–35**: Elevated concern. Something is worrying investors.
- **VIX ≥ 35**: Crisis territory. This level triggers RISK_OFF in Nox.

Historical examples where VIX spiked above 35: COVID crash (2020), 2022 rate hike panic, 2025 tariff shock.

### SPY and the 200-Day SMA

SPY is an ETF (Exchange Traded Fund) that tracks the S&P 500 — the 500 biggest US companies. The 200-day Simple Moving Average (SMA) is the average closing price over the last 200 trading days (~10 months).

When SPY is above its 200-day SMA, the broad market is in an uptrend. When it falls below, something has changed. The 0.98 buffer (2% below the SMA) prevents the system from flipping into RISK_OFF on a tiny blip.

### Why does this matter to you?

If you're not seeing trades and the market looks fine to you, check the regime. A VIX spike that you might have forgotten about could have locked the system into RISK_OFF. The analyst sends a regime update every 6 hours so you always know the current state.

---

## 6. Position Sizing — How Nox Decides How Much to Buy

### Kelly Criterion (Tier 0 — default)

Kelly is a mathematical formula that answers: "Given my historical win rate and average win/loss size, what fraction of my bankroll should I bet?"

The formula is: `K% = Win Rate − ((1 − Win Rate) / Win:Loss Ratio)`

With Nox's configured parameters (Win Rate = 68.42%, Win:Loss = 2.316):
- Raw Kelly = 68.42% − ((31.58%) / 2.316) = **54.8%**

That's enormous — raw Kelly is aggressive. So Nox applies a **Kelly Fraction of 0.15** (15%):
- Adjusted Kelly = 54.8% × 0.15 = **8.2%** of your portfolio per trade

This is below the 10% hard cap, which means Kelly is actually doing real work — it's not being overridden every single trade.

**Hard cap:** No matter what Kelly says, the system never risks more than **10%** of your portfolio on a single trade. This is a physical gate in code, not just a guideline.

**Zero-share protection:** If Kelly allocates less than the price of one share (e.g., you have $1,000 and the stock costs $350 — Kelly gives you $82, which doesn't buy one share), the trade is cancelled. Forcing a purchase would mean spending 35% of your account on a single trade, which completely defeats the 10% cap.

### Tier 1 — Standard (1%)

Simple: risk exactly 1% of your portfolio. Used for lower-conviction signals or when you want predictable position sizes.

Example: $10,000 portfolio → $100 per trade → if SPY is $580, that's 0 shares (rounded down). This tier really only makes sense on larger accounts.

### Tier 3 — Aggressive (5%)

Risk 5% with a wider stop loss (3.5× ATR instead of 2×). Used for high-conviction setups where you expect a larger move. Named "let the knife cut" — you're giving the trade more room to breathe.

### Regime adjustment

The regime multiplier (1.0 / 0.5 / 0.0) is applied before sizing. During TRANSITION, your effective equity is halved before Kelly runs, so position sizes naturally come out smaller.

---

## 7. Risk Rules — The Safety Gates

These rules are enforced in code. They cannot be bypassed by a signal.

| Rule | What it does |
|------|-------------|
| **RULE-004** (Auth Gate) | Silent drop of signals with wrong secret key |
| **RULE-005** (Kelly Guard) | Negative Kelly = no edge = trade cancelled. Zero shares = trade cancelled |
| **RULE-007** (Telegram Required) | Bot refuses to start if Telegram credentials are missing |
| **RULE-008** (Timeouts) | All Alpaca API calls have strict 5s connect / 10s read timeouts |
| **RULE-009** (Startup Validation) | All required env vars checked at startup. Missing = bot refuses to start |
| **RULE-013** (Dual Observability) | The system cannot act without being able to alert you |
| **RULE-014** (No Hardcoded URLs) | Live vs paper API is always set via env var, never in code |
| **RULE-018** (Notional Cap) | Order value cannot exceed 10% of portfolio at submission time |
| **CN-RULE-001** (Board Lots) | Chinese A-shares must be bought in multiples of 100 (one 手, shǒu) |
| **CN-RULE-002** (T+1) | Chinese A-shares cannot be sold the same day they were bought |

These aren't arbitrary — each one exists because of a real scenario where the system could lose money or behave incorrectly without it. The comments in the code explain each one.

### The trailing stop

Every BUY order gets a paired trailing stop order on Alpaca. The stop trails the price by `ATR × multiplier` dollars:

- ATR (Average True Range) = the average daily price range over the last 14 days. It measures how volatile the stock currently is.
- If ATR = $5 and multiplier = 2.0, the stop trails 10 points below the highest price reached.

This means if you buy at $100 and the stock rises to $120, your stop is now at $110. It locks in profit automatically without you doing anything.

---

## 8. Options Signal Generator

This is a self-contained system that runs inside the Execution Engine on a background timer. It generates its own signals independently by scanning the market every 30 minutes.

### What it does

For each ticker on your watchlist, every scan cycle it:

1. **Fetches price history** from Yahoo Finance and calculates:
   - RSI (14-day) — momentum indicator
   - ATR (14-day) — volatility measure
   - 20-day moving average — short-term trend
   - 50-day moving average — medium-term trend

2. **Determines the directional bias:**
   - *Bullish*: RSI 40–65, price above both moving averages
   - *Bearish*: RSI 35–60, price below both moving averages
   - *Neutral*: Everything in between

3. **Fetches IV Rank** from Alpaca's options chain. IV Rank tells you whether options are cheap or expensive right now:
   - **IV Rank below threshold** = Options are cheap → good time to *buy* premium (calls, puts)
   - **IV Rank above threshold** = Options are expensive → good time to *sell* premium (covered calls, cash-secured puts)

4. **Selects the best strategy** for the combination of bias and IV environment.

5. **Prices the contract** using Black-Scholes math and finds the real-world Alpaca contract symbol closest to the target.

6. **Sends you a Telegram alert** with the full trade idea.

7. **Places the order** if `OPTIONS_BOT_AUTO_EXECUTE=true` is set.

### The options strategies

| Strategy | Plain English | When it's used |
|----------|--------------|----------------|
| **Long Call** | Bet the stock goes up. Pay a fixed price, profit if it rises above your strike. | Bullish + cheap vol |
| **Long Put** | Bet the stock goes down. Pay a fixed price, profit if it falls below your strike. | Bearish + cheap vol |
| **Bull Call Spread** | Bet the stock goes up but limit your cost. Buy one call, sell a higher-strike call. Max risk and max reward are both capped. | Bullish + any vol |
| **Bear Put Spread** | Bet the stock goes down but limit your cost. Buy one put, sell a lower-strike put. | Bearish + any vol |
| **Cash-Secured Put (CSP)** | Get paid to agree to buy a stock at a lower price. You keep the payment if the stock stays above your strike. | Bullish/neutral + expensive vol |
| **Covered Call (CC)** | Get paid to agree to sell a stock you own at a higher price. You keep the payment either way. | Neutral/bearish + expensive vol |
| **Long Straddle** | Bet the stock makes a big move but you don't know which direction. Buy both a call and a put at the same strike. | Neutral + very cheap vol |
| **Long Strangle** | Same as straddle but cheaper — buy an OTM call and OTM put. Needs a bigger move to profit. | Neutral + expensive vol wanting a breakout |

### Black-Scholes and the Greeks

The options pricing engine uses the Black-Scholes model — the industry-standard mathematical formula for pricing options. It calculates the "Greeks," which measure how the option's price is expected to change:

| Greek | What it measures | Example |
|-------|-----------------|---------|
| **Delta (Δ)** | How much the option moves per $1 move in the stock | Delta 0.45 → option gains ~$0.45 if stock rises $1 |
| **Gamma (Γ)** | How fast delta itself changes | High gamma = delta changes quickly |
| **Theta (Θ)** | Daily time decay — how much the option loses per day just from time passing | Theta -$0.05 → option loses $0.05 per calendar day |
| **Vega (V)** | Sensitivity to implied volatility changes | Positive vega = benefits if vol rises |
| **Rho (ρ)** | Sensitivity to interest rate changes | Less important for short-dated options |

**The most important one for beginners is theta.** Every day that passes, your long option loses a small amount of value just from the clock ticking. This is why options have expiration dates and why timing matters.

### IV Rank — the key filter

IV Rank (Implied Volatility Rank) tells you how cheap or expensive options are *right now* compared to the past year:

```
IV Rank = (Current IV − 52-week Low IV) / (52-week High IV − 52-week Low IV) × 100
```

- **IV Rank 0**: Options are at their cheapest point in the past year. Buy premium.
- **IV Rank 100**: Options are at their most expensive point in the past year. Sell premium.
- **IV Rank 50**: Right in the middle — neutral.

The rule of thumb: **"Buy low IV, sell high IV."** You want to buy options when they're cheap and sell them when they're expensive. IV Rank is the compass.

---

## 9. Personal vs Bot Options Profiles

This is one of the most important design decisions in the system. **You** have a higher risk tolerance than you want your automated bot to have. So the system runs two completely separate options signal generators simultaneously.

### Why separate?

The bot trades real money automatically. If it gets an aggressive signal wrong, it loses money without you doing anything. So it's conservative by design.

You, on the other hand, review the signal and decide whether to act. You can tolerate more risk because you have a human filter in the loop.

### The two profiles side by side

| Parameter | BOT (conservative) | PERSONAL (aggressive) |
|-----------|-------------------|----------------------|
| **Delta target** | 0.45 (near ATM) | **0.60 (ITM)** — more intrinsic value, less theta drag |
| **Days to expiry — long** | 45 days | **14 days** — short gamma plays, faster resolution |
| **Days to expiry — spreads** | 45 days | **21 days** |
| **IV Rank buy threshold** | < 30% | **< 50%** — willing to buy even moderately expensive vol |
| **IV Rank sell threshold** | > 50% | **> 40%** |
| **Risk per trade** | 1.0–2.0% | **2.0–3.0%** |
| **Strategy restrictions** | Tiered by capital | **All strategies always available** |
| **RISK_OFF behaviour** | Hard blocks long premium | **Shows 50% confidence warning — never blocks you** |
| **Auto-execute** | Configurable | **Always advisory — never auto-executes** |
| **Default watchlist** | SPY, QQQ, AAPL, TSLA, NVDA | **SPY, QQQ, AAPL, TSLA, NVDA, AMZN, META** |

### How to tell which is which in Telegram

Every alert header shows the profile:

```
📊 OPTIONS SIGNAL — AAPL [PERSONAL · FREE_CAPITAL]   ← This is your personal signal
📊 OPTIONS SIGNAL — AAPL [BOT · STANDARD]            ← This is the bot's conservative signal
```

### Capital tiers (bot only — personal ignores these)

| Tier | Capital | Strategies available |
|------|---------|---------------------|
| **STARTER** | Under $5k | Long calls and puts only |
| **STANDARD** | $5k – $30k | + Cash-secured puts, covered calls |
| **ADVANCED** | $30k – $75k | + Spreads, straddles, strangles |
| **FREE_CAPITAL** | $75k+ or custom | Everything, math uses your specified amount |

**Free Capital mode:** If you have a separate pool of money you want to trade options with (say $10k in a different account), set `OPTIONS_PERSONAL_FREE_CAPITAL_AMOUNT=10000` in your `.env`. The signal math will use that number instead of the bot's Alpaca balance. This lets you get correctly-sized personal signals regardless of what's in the bot's account.

---

## 10. Reading Your Telegram Alerts

Every significant event sends you a message. Here's what they all mean.

### Equity trade alerts

**🟢 BUY ORDER EXECUTED**
```
🟢 BUY ORDER EXECUTED
────────────────────────
• Ticker: SPY
• Quantity: 12 Shares (Dynamic Kelly)
• Order ID: abc123-def456
```
A buy order was successfully placed. The order ID lets you look it up in Alpaca.

---

**⚪ POSITION CLOSED**
```
⚪ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: Webhook SELL Signal
• Alpaca Order ID: xyz789
```
A position was closed by exit rule or trailing stop. This happens automatically when configured exit conditions are met.

---

**📊 Regime Check**
```
📊 Regime Check: STATUS: RISK-ON. Volatility low. Deploying full capital.
```
The analyst just evaluated the market. Sent every 6 hours.

---

**🚧 RSI GATE BLOCK**
```
🚧 RSI GATE BLOCK
────────────────────────
• Ticker: SPY
• Action: BUY
• RSI: 28.3 (Below Floor < 30)
⚠️ Order canceled to protect buying power.
```
A buy signal came in but RSI was too low. The trade was cancelled. This is the system working correctly — it's protecting you from buying into a momentum crash.

---

**🛑 REGIME BLOCK: RISK-OFF**
```
🛑 REGIME BLOCK: RISK-OFF
────────────────────────
• Ticker: SPY
⛔ VIX ≥ 30 or SPY below 200 SMA. No new entries.
```
The macro environment is too dangerous for new trades. No money is being deployed until this lifts.

---

**🚨 CRITICAL: Equity Fetch Failed**
```
🚨 CRITICAL: Equity Fetch Failed
────────────────────────
All 3 Alpaca equity fetch attempts failed.
⛔ New order entries halted for this cycle.
Manual review required.
```
The system couldn't connect to Alpaca to check your balance. This could be a network issue, Alpaca API outage, or your API key expiring. Check Alpaca's status page and your logs.

---

### Options signal alerts

**📊 Options Signal (Advisory)**
```
📊 OPTIONS SIGNAL — AAPL [PERSONAL · FREE_CAPITAL]
────────────────────────────────────
🎯 Strategy: Long Call
📅 Expiry: 2026-08-01 (39 DTE)
💵 Strike: $200
💰 Entry: $4.20 | Max Risk: $420 | Max Gain: Unlimited

📐 Greeks
• Delta: 0.60 | Gamma: 0.021
• Theta: -$0.09/day | Vega: +0.38
• IV Rank: 22% ← LOW (buy premium zone ✅)

📈 Technicals — AAPL
• RSI(14): 57 | ATR(14): $2.80
• Price vs 20-SMA: ✅ above | vs 50-SMA: ✅ above

🌐 Macro Regime: RISK_ON ✅ (VIX 16.4, SPY > 200-SMA)
🎯 Signal Confidence: 87%

⚠️ Advisory only — manual execution required.
```

Reading this alert:
- **Strategy**: Long Call — bullish directional bet
- **Expiry**: August 1, 2026, which is 39 days away
- **Strike**: $200 — you profit if AAPL closes above $204.20 at expiry (strike + premium paid)
- **Max Risk**: $420 — the most you can lose is what you paid (for a long option, that's it)
- **Delta 0.60**: If AAPL rises $1, this option gains ~$0.60
- **Theta -$0.09/day**: The option loses about $0.09 per day just from time passing
- **IV Rank 22%**: Options are in the cheap zone. Good time to buy.
- **Confidence 87%**: Regime is favourable, technicals are aligned

---

**✅ OPTIONS ORDER PLACED** (only if auto-execute is on)
```
✅ OPTIONS ORDER PLACED
────────────────────────
• Ticker: AAPL
• Strategy: LONG_CALL
• Contracts: 1
• Expiry: 2026-08-01
• Order ID: ord-abc123
```
The bot actually placed this order in Alpaca. Go check your positions.

---

**🚨 OPTIONS ORDER FAILED**
```
🚨 OPTIONS ORDER FAILED
────────────────────────
• Ticker: AAPL
• Strategy: LONG_CALL
• Reason: Contract lookup failed for AAPL — HTTP 403
```
The order was attempted but Alpaca rejected it. Common reasons: options trading not enabled on your account, paper account doesn't have options access enabled, or the specific contract is unavailable (too far OTM, expired, halted).

---

**🚫 CN T+1 GATE BLOCKED**
```
🚫 CN T+1 GATE BLOCKED
────────────────────────
• Ticker: 600519.SH
• Entry Date: 2026-06-23
• Sell Date: 2026-06-23
⛔ Same-day sell prohibited (T+1 rule). Signal discarded.
```
You bought a Chinese A-share today and a sell signal came in the same day. Chinese stock exchange rules prohibit same-day round trips. The sell will be allowed tomorrow.

---

## 11. Environment Variables — The Control Panel

All configuration lives in your `.env` file. This is the master control panel for Nox's behaviour. **Never commit this file to git.**

### Required — bot won't start without these

| Variable | What it does |
|----------|-------------|
| `ALPACA_API_KEY` | Your Alpaca API key ID |
| `ALPACA_SECRET_KEY` | Your Alpaca API secret key |
| `ALPACA_BASE_URL` | `https://paper-api.alpaca.markets` for paper, `https://api.alpaca.markets` for live |
| `WEBHOOK_SECRET_TOKEN` | Secret password required in webhook payloads for authentication |
| `TELEGRAM_BOT_TOKEN` | Your Telegram bot's token |
| `TELEGRAM_CHAT_ID` | Your Telegram chat ID (where alerts are sent) |
| `KELLY_WIN_RATE` | Your strategy's historical win rate (e.g. `0.6842` = 68.42%) |
| `KELLY_WIN_LOSS_RATIO` | Average win ÷ average loss (e.g. `2.316`) |
| `KELLY_FRACTION` | Scaling factor on raw Kelly (e.g. `0.15` = 15% of raw Kelly) |
| `ANTHROPIC_API_KEY` | Claude AI API key (used by the heartbeat for intelligence reports) |

### Optional — equity trading

| Variable | Default | What it does |
|----------|---------|-------------|
| `ANALYST_CYCLE_HOURS` | `6` | How often the analyst checks VIX and regime |
| `CN_BOARD_LOT_SIZE` | `100` | Share lot size for Chinese A-share orders |
| `CN_POSITIONS_PATH` | `/tmp/china_positions.json` | Where T+1 position records are saved |

### Optional — options signals

| Variable | Default | What it does |
|----------|---------|-------------|
| `OPTIONS_BOT_WATCHLIST` | `SPY,QQQ,AAPL,TSLA,NVDA` | Tickers the BOT profile scans |
| `OPTIONS_BOT_SCAN_INTERVAL_MINUTES` | `30` | How often the bot profile scans |
| `OPTIONS_BOT_AUTO_EXECUTE` | `false` | Set to `true` to auto-place bot options orders |
| `OPTIONS_BOT_QTY_CONTRACTS` | `1` | Contracts per bot order |
| `OPTIONS_BOT_FREE_CAPITAL_AMOUNT` | _(off)_ | Override bot capital amount |
| `OPTIONS_PERSONAL_WATCHLIST` | `SPY,QQQ,AAPL,TSLA,NVDA,AMZN,META` | Tickers the PERSONAL profile scans |
| `OPTIONS_PERSONAL_SCAN_INTERVAL_MINUTES` | `30` | How often the personal profile scans |
| `OPTIONS_PERSONAL_FREE_CAPITAL_AMOUNT` | _(off)_ | Your personal capital for sizing (e.g. `5000`) |
| `OPTIONS_PERSONAL_QTY_CONTRACTS` | `1` | Contracts per personal signal sizing |

### Switching between paper and live

**Paper trading** (safe, fake money):
```
ALPACA_BASE_URL=https://paper-api.alpaca.markets
```

**Live trading** (real money):
```
ALPACA_BASE_URL=https://api.alpaca.markets
```

Always test new strategies in paper mode first. The paper API behaves identically to live except your money isn't real.

---

## 12. Chinese A-Share Rules (CN-RULE)

If you trade Chinese stocks (Shanghai or Shenzhen exchange), two rules apply automatically.

### CN-RULE-001 — Board Lots

Chinese exchanges require orders in multiples of 100 shares (one 手 — pronounced "shǒu"). You cannot buy 150 shares; you must buy 100 or 200. Nox automatically truncates: if Kelly says buy 145 shares, the order goes in for 100.

If Kelly says buy 60 shares (less than one full lot), the trade is cancelled entirely — it would be impossible to execute on the exchange anyway.

### CN-RULE-002 — T+1 Settlement

Chinese regulations prohibit selling a stock on the same calendar day you bought it. The "T" is the trade date; you can't sell until T+1 (the next day) or later.

Nox tracks every A-share purchase with a timestamp. If a SELL signal arrives on the same day as the recorded buy, the sell is rejected. The position record persists to disk so it survives server restarts. Each day at startup, records from previous days are pruned (the restriction has lifted).

If you restart the engine and a buy was made before the restart (the record was lost), the system logs a warning and allows the sell rather than holding your position indefinitely — the safer of the two failure modes.

---

## 13. Troubleshooting

### "I haven't received any exit signals"

Work through this list in order:

**1. Check if you have open positions**
From your Alpaca dashboard or run `docker compose exec execution-engine curl localhost:8080/status`. Look at the `open_positions` field. If it's empty, no exit rules can fire.

**2. Check the current regime**
Look at your last Telegram analyst report. If it says RISK_OFF, the system is in protective mode but exit rules should still apply to existing positions.

**3. Check that the engine is running**
From your VPS: `docker-compose ps`. You should see `execution-engine` with status `Up`. If it's `Exit` or missing, run `docker-compose up -d execution-engine`.

**4. Check your exit rule configuration**
Verify your `.env` has the exit thresholds set:
- `TAKE_PROFIT_PCT` (default: 15)
- `STOP_LOSS_PCT` (default: 8)
- `MAX_HOLD_DAYS` (default: 10)

**5. Check the trade ledger**
From your VPS: `curl localhost:8080/trades | python3 -m json.tool`. Look for recent exits with reasons: `take_profit`, `stop_loss`, `rsi_exhaustion`, `trend_break`, `time_stop`.

**6. Check your logs**
From your VPS: `docker-compose logs -f execution-engine | grep -i exit`. Look for exit rule evaluations and Telegram alerts sent.

---

### "The options signals stopped"

The options scanner runs on a background thread inside the execution engine. If the execution engine is running, it should be scanning.

Check: `docker-compose logs execution-engine | grep OPTIONS_SCAN`

You should see lines like `[INFO] [OPTIONS_SCAN][PERSONAL] Tier=STARTER | Capital=...` every 30 minutes.

If you see `Skipping scan — equity unavailable`, Alpaca isn't responding (same issue as equity fetch failure above).

---

### "Options order failed with HTTP 403"

Your Alpaca account needs options trading explicitly enabled. Log in to Alpaca → Account Settings → Trading → Options Trading. Enable it. Paper accounts may also need options enabled separately.

---

### "T+1 gate keeps blocking my sells"

This only affects Chinese A-shares. It's working correctly — you're trying to sell the same day you bought. Wait until tomorrow. If you're sure you didn't buy today and it's still blocking, the persistence file may have a stale record. Check the file at `CN_POSITIONS_PATH` (default `/tmp/china_positions.json`) and remove the stale entry if needed.

---

### "Kelly keeps calculating 0 shares"

Two scenarios:
1. **Win rate and win/loss ratio produce negative Kelly**: Your strategy has no mathematical edge. The trade is correctly cancelled. This is RULE-005 working as intended. Review your Kelly parameters.
2. **Kelly share count is valid but rounds down to 0**: Your account is too small relative to the stock price for even 1% allocation to buy a whole share. You need either more capital or to trade a cheaper stock.

---

### "Telegram alerts stopped"

1. Check your bot token hasn't been revoked: message `@BotFather` on Telegram → `/mybots` → check status
2. Check `TELEGRAM_CHAT_ID` is your personal chat ID, not a channel ID (they have different formats)
3. Check network connectivity: `docker-compose logs execution-engine | grep TELEGRAM`
4. Try sending a test message manually: `docker-compose exec execution-engine curl -s "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/getMe"`

---

### "How do I switch from paper to live trading?"

1. Open your `.env` file
2. Change `ALPACA_BASE_URL=https://paper-api.alpaca.markets` to `ALPACA_BASE_URL=https://api.alpaca.markets`
3. Restart: `docker-compose down && docker-compose up -d`
4. Verify the next Telegram regime report confirms the engine is live

**Before doing this:** confirm you've seen consistent paper trading results for at least 30 days and you understand every alert type in Section 10.

---

## 14. Quick Reference

### Service commands

```bash
# Start everything
docker-compose up -d

# Stop everything
docker-compose down

# Check all service statuses
docker-compose ps

# Watch live execution engine logs
docker-compose logs -f execution-engine

# Watch live analyst logs
docker-compose logs -f analyst-brain

# Restart just the execution engine
docker-compose restart execution-engine

# Rebuild and restart after code changes
docker-compose up -d --build execution-engine
```

### Key URLs (replace with your server address)

| URL | What it does |
|-----|-------------|
| `GET https://yourserver.com/health` | Check if execution engine is running |
| `GET https://yourserver.com/last-report` | When did the analyst last report? |
| `POST https://yourserver.com/webhook` | Accepts custom trade signals (analyst or manual overrides) |
| `POST https://yourserver.com/options/price` | Query Black-Scholes prices directly |

### Test the webhook manually

```bash
curl -X POST https://yourserver.com/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "secret_key": "your_WEBHOOK_SECRET_TOKEN",
    "ticker": "SPY",
    "action": "BUY",
    "price": 580.0,
    "rsi": 54.0,
    "atr": 5.0,
    "vol": 50000000,
    "stop_loss_atr_multiplier": 2.0,
    "risk_tier": 0,
    "vix": 16.0,
    "spy_price": 580.0,
    "spy_200_sma": 565.0
  }'
```

Expected response: `Processed 1 signal(s)`

### Query an options price directly

```bash
curl -X POST https://yourserver.com/options/price \
  -H "Content-Type: application/json" \
  -d '{
    "symbol": "AAPL",
    "option_type": "call",
    "strike": 200,
    "underlying": 195.5,
    "expiry": 0.11,
    "risk_free_rate": 0.05,
    "volatility": 0.25
  }'
```

Returns full Greeks: price, delta, gamma, theta, vega, rho, implied_volatility.

### Regime thresholds at a glance

| Condition | Regime | Capital multiplier |
|-----------|--------|-------------------|
| VIX < 35 AND SPY > 200-SMA | RISK_ON | 1.0 (full) |
| VIX < 35 AND SPY between SMA×0.98 and SMA | TRANSITION | 0.5 (half) |
| VIX ≥ 35 OR SPY < SMA×0.98 | RISK_OFF | 0.0 (stopped) |

### Options strategies at a glance

| Bias | IV cheap (rank < threshold) | IV expensive (rank > threshold) |
|------|-----------------------------|--------------------------------|
| Bullish | Bull Call Spread / Long Call | Cash-Secured Put |
| Bearish | Bear Put Spread / Long Put | Covered Call |
| Neutral | Long Straddle | Long Strangle / CSP |

---

*This document covers the system as of June 2026. If you add features or change parameters, update this guide at the same time — the best time to document something is right when you build it.*
