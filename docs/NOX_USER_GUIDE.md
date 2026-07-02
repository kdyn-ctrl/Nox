# Nox Trading Bot — User Guide

**This guide is for non-technical users who want to understand what the bot does and how to monitor it.**

---

## What is Nox?

Nox is a **fully automated trading robot** that watches the stock market, makes trading decisions, and places buy/sell orders automatically. It focuses on trading **SPY** (an index that tracks the S&P 500) and manages risk carefully.

**Key idea**: When market conditions are favorable, it buys. When conditions turn bad, it either sells or waits. It sizes positions (decides how many shares to buy) based on how confident it is about the trade.

---

## How Does Nox Work? (Simple Version)

### The Daily Cycle

Every day during market hours, Nox does this:

```
1. WATCH: Check the current market conditions
   ↓
2. ANALYZE: Decide if now is a good time to trade
   ↓
3. DECIDE: Generate a buy or sell signal
   ↓
4. VALIDATE: Double-check that the signal is safe
   ↓
5. SIZE: Decide how many shares to buy
   ↓
6. EXECUTE: Place the order with Alpaca (your broker)
   ↓
7. REPORT: Send you updates via Telegram
```

### What Does Nox Look At?

**The VIX** — "Fear Index"
- Low VIX (< 35) = Market is calm and confident → Good time to buy
- High VIX (> 35) = Market is scared → Better to wait or sell

**SPY Price vs. 200-Day Average**
- SPY above average = Uptrend → Bullish signal
- SPY below average = Downtrend → Bearish signal

**Market Regime** (the current situation)
- **RISK_ON** (green): Market is healthy, VIX low, SPY strong → Active trading mode
- **TRANSITION** (yellow): Market is uncertain → Smaller position sizes
- **RISK_OFF** (red): Market is in crisis, VIX high, SPY weak → Stop trading, reduce risk

---

## How Much Money Does Nox Risk?

Nox uses **Kelly Criterion**, a mathematical formula that decides how much of your portfolio to risk on each trade.

**Simple explanation**: The more confident Nox is (based on historical win rate), the larger the position. But it never risks:
- More than **10%** of your total portfolio on one trade
- Less than **1%** of your portfolio on one trade

Example:
- Portfolio: $100,000
- Nox is confident → 8% = $8,000 per trade
- Nox is less confident → 2% = $2,000 per trade

---

## What Alerts Mean

You'll receive Telegram notifications. Here's what they mean:

### 🟢 Green Alerts (Normal)

**"📊 [ANALYST] Regime Classification"**
- Nox just analyzed the market
- Shows current regime (RISK_ON, TRANSITION, RISK_OFF)
- This happens automatically during market hours

**"✅ [EXECUTION] Order Submitted"**
- Trade was successful
- Shows symbol (SPY), number of shares, entry price
- Order is now active in Alpaca

### 🟡 Yellow Alerts (Caution)

**"⚠️ [MOMENTUM] Signal Blocked"**
- Nox generated a signal but blocked it
- Reason: RSI is too high (overbought) or too low (oversold)
- Nox is protecting you from buying at the peak or selling at the bottom

**"⚠️ [REGIME] Reduced Position Size"**
- Market entered TRANSITION regime
- Nox is trading smaller than usual (safer)
- Normal during market uncertainty

### 🔴 Red Alerts (Problem)

**"🔴 [CRITICAL] Regime Changed to RISK_OFF"**
- Market just turned bad (VIX jumped or SPY crashed)
- Nox **stopped trading** to protect your money
- Check the news — something important happened

**"🔴 [CRITICAL] Portfolio Check Failed"**
- Nox couldn't verify your account balance with Alpaca
- Possible issue: Network problem, API outage, or broker issue
- Nox will retry automatically

**"🔴 [CRITICAL] Order Failed"**
- Trade didn't go through
- Reason varies: Insufficient buying power, market halted, broker issue
- Nox will alert you and wait for the next opportunity

### 📋 Information Alerts

**"📈 Market Intelligence Report"**
- Summary of the day's trading activity
- Shows: Current regime, trades executed, portfolio value change
- Sent automatically (usually once per day)

---

## How to Monitor Nox

### Check Telegram Chat
- **Recommended**: Set up Telegram notifications (ask your setup person how)
- You'll see real-time alerts about every trade and market condition change
- Set Telegram to notify you only for red/critical alerts if you don't want constant messages

### Check Your Alpaca Dashboard
- Go to **https://app.alpaca.markets**
- Log in with your account
- See all open positions, closed trades, and account balance
- Verify that orders placed by Nox look correct

### Check the Logs (If Comfortable)
- Ask your technical person to show you the logs
- Logs show every decision Nox made and why
- Useful if something seems wrong

---

## What Could Go Wrong?

### Nox Isn't Trading
**Possible reasons**:
- Market hours: Nox only trades Mon-Fri, 9:30am-4pm ET
- Regime is RISK_OFF: Market conditions are bad, Nox is waiting
- Signal blocked: VIX is too high or too low (Nox is being cautious)

**What to do**: Check the latest Telegram alert. It usually says why.

### Telegram Alerts Stopped
**Possible reasons**:
- Bot token is invalid or revoked
- Chat ID is wrong
- Internet connection is down

**What to do**: Tell your technical person to verify the Telegram token and restart the bot.

### Orders Keep Failing
**Possible reasons**:
- Not enough buying power in Alpaca account
- Market is halted (usually after big news)
- Alpaca API is down
- Network connection is bad

**What to do**: Check your Alpaca balance. If it's low, deposit more money. If Alpaca API is down, wait.

### Nox Keeps Trading in RISK_OFF
**This should NOT happen.** If it does:
- Nox detected a market crisis but still placed orders
- This is a bug

**What to do**: Stop the bot immediately and tell your technical person.

---

## Understanding Performance

### Win Rate
- **What it means**: Percentage of trades that made money
- **Good**: 50%+ (better than a coin flip)
- **Great**: 60%+ (Nox is doing its job)
- **Example**: 12 trades, 7 wins, 5 losses = 58% win rate

### Average Win vs. Average Loss
- **Good**: Average win is at least 1.5x the average loss
- **Example**: Average win +$200, Average loss -$100 (2:1 ratio is great)

### Total Profit/Loss (P&L)
- **Most important number**: Did your account grow or shrink?
- Nox aims to grow your account slowly and steadily
- Don't expect +20% per month; expect +1-3% per month if conditions are good

### Sharpe Ratio (Advanced)
- **What it means**: Risk-adjusted returns (profit relative to volatility)
- **Good**: 1.0+ (making steady profit without wild swings)
- **Great**: 2.0+ (very stable, predictable growth)

---

## When Does Nox Trade?

### Nox is Active
- 📅 **Days**: Monday through Friday (not weekends/holidays)
- ⏰ **Hours**: 9:30 AM to 4:00 PM Eastern Time
- 📊 **Conditions**: Market regime is not RISK_OFF

### Nox is Inactive
- 🔴 Weekends and US market holidays
- 🔴 Before market open (before 9:30 AM ET)
- 🔴 After market close (after 4:00 PM ET)
- 🔴 When regime is RISK_OFF (market crisis mode)

---

## Key Rules Nox Follows

1. **Always know your portfolio value** — Before placing any order, Nox checks your account balance with Alpaca. If it can't confirm, it doesn't trade.

2. **Never risk more than 10%** — Even if the math says to go bigger, Nox caps it at 10% of your portfolio per trade.

3. **Never trade with 0 shares** — If the calculated position size is less than 1 share, Nox cancels the trade.

4. **Block overbought/oversold signals** — If RSI (momentum indicator) shows the market is at an extreme, Nox blocks the signal to avoid buying at the peak.

5. **Stop trading in crisis** — When VIX spikes or SPY crashes hard, Nox switches to RISK_OFF and stops trading immediately.

6. **Authenticate all trades** — Every order must have the correct secret key. Without it, the order is rejected.

---

## Telegram Notification Checklist

### Daily Routine
- [ ] Morning: Check overnight alerts (if any happened)
- [ ] During day: Monitor for RISK_OFF alerts (requires action)
- [ ] Evening: Review market intelligence report
- [ ] Once a week: Check Alpaca dashboard to verify orders

### If You See Red Alert
- [ ] Read the alert message carefully
- [ ] Note the timestamp and symbol (SPY)
- [ ] Check what happened in the news at that time
- [ ] If it's a critical API error, tell your technical person
- [ ] If it's a market event (VIX spike), it's normal — Nox is protecting you

### If Nox Isn't Trading
- [ ] Check what regime we're in (from the latest report)
- [ ] If RISK_OFF: Normal, market is bad, Nox is waiting
- [ ] If RISK_ON: Check if signals are being blocked (Telegram will say why)
- [ ] If totally silent: Check that Nox service is running (ask technical person)

---

## Updating This Guide

**This guide should be updated whenever**:
- The bot's behavior changes
- New alert types are added
- Regime rules change
- Risk limits change
- New trading hours are introduced

**Who updates**: Your technical person should update this guide when they change the code. This keeps it accurate for you.

**How to know it's outdated**: If an alert appears that's not in this guide, or Nox behaves differently than described here, the guide needs updating.

---

## FAQ

**Q: Can I manually trade while Nox is running?**
A: Yes, but be careful. Your manual orders and Nox's orders will both execute. This could lead to over-concentration or unexpected risk.

**Q: What if I don't like a trade Nox made?**
A: You can close it manually in Alpaca. Nox will see the position is gone and generate a new one if conditions are right. But remember: Nox made that trade for a mathematical reason. Closing it might lock in a loss.

**Q: Does Nox trade options or leverage?**
A: The core equity bot trades whole shares only. Options signals are now available as a separate advisory channel — see the Options Signal Generator section below.

**Q: What happens if the market crashes while Nox is on?**
A: Nox switches to RISK_OFF regime and stops trading. It doesn't sell existing positions automatically (that would lock in losses). It just waits for calm to return.

**Q: How often should I check on Nox?**
A: If Telegram alerts are working: Just check them when they ping. If Telegram isn't working: Check Alpaca dashboard once per day. Nox doesn't need babysitting.

**Q: Can Nox lose all my money?**
A: It's designed not to. The 10% position cap and Kelly sizing prevent catastrophic losses. But any trading has risk. Nox is a bot, not a guarantee.

**Q: What should I do if something looks wrong?**
A: First, check Telegram for error alerts. Second, check Alpaca to verify the orders look reasonable. Third, tell your technical person. Nox is designed to fail loud, not silent.

---

## Glossary (Plain English)

| Term | What It Means |
|------|---------------|
| **VIX** | "Fear Index" — measures how scared investors are. Higher = scarier |
| **SPY** | Stock that tracks the S&P 500 (500 big US companies). What Nox trades |
| **200-Day SMA** | Average price of SPY over the last 200 trading days. Shows the trend |
| **RISK_ON** | Market is calm and good — Nox trades normally |
| **RISK_OFF** | Market is scared or crashing — Nox stops trading |
| **TRANSITION** | Market is uncertain — Nox trades but smaller |
| **Kelly Criterion** | Math formula to decide position size based on win rate |
| **RSI** | Momentum indicator — shows if market is overbought or oversold |
| **P&L** | Profit or Loss — how much money you made or lost |
| **Sharpe Ratio** | Measures how steady your profits are (higher is better) |
| **Alpaca** | Online broker where Nox places trades |
| **Telegram** | Messaging app where Nox sends you alerts |

---

## Need Help?

- **"What does this alert mean?"** → Check "Understanding Alerts" section above
- **"Is this normal?"** → Check "What Could Go Wrong?" section
- **"How is Nox doing?"** → Check performance metrics (Win Rate, P&L, Sharpe Ratio)
- **"Something is broken"** → Tell your technical person what you saw and when

---

**Last Updated: 2026-06-23**  
**Status: Ready to use**

This guide should be re-read every time the bot's behavior changes. If you notice something in the actual bot that doesn't match this guide, that's a sign the guide needs updating.

---

## Why Am I Not Getting Trade Signals?

This is the most common operational question. Here is the honest answer:

### How equity signals actually work

Nox uses a rule-based system for equity trading. The full pipeline is:

```
Analyst brain detects market conditions (every 6 hours)
        ↓
Regime classification (RISK_ON / TRANSITION / RISK_OFF)
        ↓
Execution engine applies exit rules to open positions
        ↓
Take-profit, stop-loss, RSI exhaustion, trend-break rules trigger exits
        ↓
Alpaca orders are placed automatically
```

The system continuously monitors open positions and executes exits based on quantitative rules rather than external signals.

If you have not received a trade signal in months, one of the following is true:

---

### Diagnosis checklist — run through these in order

**Step 1: Is the execution engine running?**
- From your VPS: `docker-compose ps`
- You should see `execution-engine` with status `Up`
- **Fix:** `docker-compose up -d execution-engine`

**Step 2: Do you have open positions?**
- Check your Alpaca dashboard: https://app.alpaca.markets
- If no positions exist, the exit rules have nothing to act on
- Check your Telegram for recent trade alerts

**Step 3: Is the regime blocking new trades?**
- The regime gate stops new entries when `RISK_OFF` is active
- RISK_OFF fires when VIX ≥ 35 OR SPY is more than 2% below its 200-day average
- **Check:** Look at your last Telegram analyst report — what regime was reported?
- **In TRANSITION:** Position sizes are cut by 50%
- **In RISK_OFF:** All new BUY signals are hard-blocked

**Step 4: Are exit rules triggering?**
- Check the trade ledger: From the execution engine, `curl localhost:8080/trades`
- Look for recent exits with reasons: `take_profit`, `stop_loss`, `rsi_exhaustion`, `trend_break`
- Exits should fire automatically when conditions are met
- **Check:** Your Telegram should show exit confirmations (e.g., `✅ [EXIT] AAPL sold at take-profit`)

**Step 5: Check the execution engine logs**
- `docker-compose logs -f execution-engine | grep -i exit`
- Look for rule evaluations and position updates
- If you see errors, the engine may have crashed or lost connection to Alpaca

---

### Course of action — if positions are stalled

The regime and RSI gates exist for a reason: forcing trades in bad market conditions destroys capital. If conditions aren't right, the system is working as intended. Instead:

**Option A: Monitor regime changes**
Check the analyst Telegram report daily. When regime returns to RISK_ON and VIX drops below 35, the execution engine will be ready for new positions. During TRANSITION, exits work but position sizes are reduced.

**Option B: Use the Options Signal Generator**
The options signaler runs independently on a background thread every 30 minutes (configurable). It generates advisory Telegram alerts automatically. During RISK_OFF regimes, long premium signals are suppressed but income strategies (cash-secured puts, covered calls) are still generated — these actually benefit from high volatility. See the Options Signal Generator section below.

**Option C: Use paper trading to validate rules**
If you want to test new exit rules without risking real capital:
1. Set `ALPACA_BASE_URL=https://paper-api.alpaca.markets` in your `.env`
2. Run for 30–60 days to validate exit behavior before switching to live
3. Check the trade ledger (`/trades` endpoint) to see rule evaluations

**Option D: Verify exit rule configuration**
Review the `.env` variables for exit rules (take-profit %, stop-loss %, RSI thresholds). These control when existing positions close. You can adjust them to fit your risk tolerance.

---

## Options Signal Generator

This is a self-contained advisory system built into the execution engine. It runs independently on a background timer and sends Telegram alerts to you directly.

### What it does

Every 30 minutes (configurable), it scans a watchlist of tickers and generates options trade ideas based on:
- **Technicals** on the underlying: RSI, ATR, 20-SMA, 50-SMA from Yahoo Finance
- **IV Rank**: how expensive/cheap options are right now vs. the past year
- **Macro regime**: same VIX + SPY regime gate the equity bot uses

### Understanding the capital tiers

The generator automatically adjusts which strategies it recommends based on your account size:

| Tier | Capital | What you get |
|------|---------|-------------|
| **STARTER** | Under $5k | Long calls and puts only. Defined risk — you can only lose what you pay |
| **STANDARD** | $5k – $30k | + Cash-secured puts and covered calls. Income strategies that work with what you own |
| **ADVANCED** | $30k – $75k | + Vertical spreads, straddles, strangles. Defined risk with better capital efficiency |
| **FREE_CAPITAL** | $75k+ OR custom | All strategies unlocked. Math uses your specified capital amount |

The tier is read from your live Alpaca equity automatically. If you want to ring-fence a separate pool of capital (say $10k just for options, separate from the bot's money), set `OPTIONS_FREE_CAPITAL_AMOUNT=10000` in your `.env` — this overrides the Alpaca equity check entirely for the options signaler.

### Free Capital mode

Free Capital mode is designed for when you want to run the signal generator against a specific dollar amount you control, not the bot's broker balance. Use cases:
- You have money in a separate account (TD, IBKR, Robinhood) that you trade manually
- You want to paper-trade options ideas with a hypothetical $20k while your real bot has $5k
- You have $100k+ and want full strategy access without tying it to the bot's Alpaca balance

To enable it: add `OPTIONS_FREE_CAPITAL_AMOUNT=<your amount>` to `.env` and restart.

### How to read an options signal alert

```
📊 OPTIONS SIGNAL — AAPL [STANDARD]
────────────────────────────────────
🎯 Strategy: Bull Call Spread
📅 Expiry: 2026-08-15 (53 DTE)
💵 Strikes: $195 / $200 Call
💰 Entry: $1.85 | Max Risk: $185 | Max Gain: $315
📊 R:R Ratio: 1.7:1
⚖️ Breakeven: $196.85

📐 Greeks
• Delta: +0.42 | Gamma: 0.019
• Theta: -$0.04/day | Vega: +0.31
• IV Rank: 24% ← LOW (buy premium zone ✅)

📈 Technicals — AAPL
• RSI(14): 52 | ATR(14): $2.31
• Price vs 20-SMA: ✅ above | vs 50-SMA: ✅ above

🌐 Macro Regime: RISK_ON ✅ (VIX 16.4, SPY > 200-SMA)
🎯 Signal Confidence: 87%

⚠️ Advisory only — manual execution required.
```

**What each field means:**

| Field | What it tells you |
|-------|------------------|
| **Strategy** | What kind of options trade this is |
| **Expiry / DTE** | When the options expire. DTE = Days To Expiration |
| **Strikes** | The price(s) you would trade at |
| **Entry / Max Risk / Max Gain** | The cost and limits of the trade. Max Risk is the most you can lose |
| **R:R Ratio** | Reward-to-risk. 1.7:1 means you can make $1.70 for every $1 risked |
| **Breakeven** | The price the stock needs to reach for you to not lose money |
| **Delta** | How much the option moves per $1 move in the stock. +0.42 = gains $0.42 per $1 stock rise |
| **Theta** | Daily time decay — how much the option loses per day just from time passing |
| **Vega** | Sensitivity to volatility. Positive = benefits from vol rising |
| **IV Rank** | How cheap or expensive options are right now (0–100). Low = cheap; high = expensive |
| **Confidence** | Regime-adjusted score. Below 50% means conditions are uncertain |

### IV Rank — the most important number

- **IV Rank < 30%** → Options are **cheap** → Buy premium (calls, puts, spreads, straddles)
- **IV Rank > 50%** → Options are **expensive** → Sell premium (CSPs, covered calls, strangles)
- **IV Rank 30–50%** → Neutral zone → Spreads or wait

During RISK_OFF (high VIX), IV Rank is typically high. This is a good time for income strategies — you collect large premiums.

### Strategy glossary

| Strategy | What it is | When it's used |
|----------|-----------|---------------|
| **Long Call** | Buy the right to buy 100 shares at the strike | Bullish, cheap options |
| **Long Put** | Buy the right to sell 100 shares at the strike | Bearish, cheap options |
| **Cash-Secured Put (CSP)** | Sell a put and hold cash to buy the stock | Bullish/neutral, expensive options. Good income strategy |
| **Covered Call (CC)** | Sell a call against shares you own | Neutral/slightly bearish, expensive options. Reduces cost basis |
| **Bull Call Spread** | Buy a call, sell a higher-strike call | Bullish, defined risk and cost |
| **Bear Put Spread** | Buy a put, sell a lower-strike put | Bearish, defined risk and cost |
| **Long Straddle** | Buy a call AND a put at the same strike | Neutral — expecting a big move either way. Cheap vol |
| **Long Strangle** | Buy OTM call AND OTM put | Neutral — same as straddle but cheaper and needs a bigger move |

### Configuration

These go in your `.env` file:

| Variable | Default | What it does |
|----------|---------|-------------|
| `OPTIONS_WATCHLIST` | `SPY,QQQ,AAPL,TSLA,NVDA` | Tickers to scan. Comma-separated |
| `OPTIONS_SCAN_INTERVAL_MINUTES` | `30` | How often to run a scan |
| `OPTIONS_FREE_CAPITAL_AMOUNT` | _(off)_ | Dollar amount for Free Capital mode. Set to a number to enable |

### Important: advisory only

These signals are **for your manual review and execution**. The bot will not automatically place these options trades. You decide whether to act on each signal, when to enter, and how to manage the position.

The signal gives you the trade idea backed by quantitative analysis. You decide if it fits your view, your broker, and your schedule.

---

## Customizing Exit Rules

The system uses quantitative rules to exit positions automatically. You can customize these in your `.env`:

### Exit Rule Configuration

| Variable | Default | Purpose |
|----------|---------|---------|
| `TAKE_PROFIT_PCT` | 15 | Close position at +15% profit |
| `STOP_LOSS_PCT` | 8 | Close position at -8% loss |
| `RSI_THRESHOLD_EXIT` | 78 | Close if RSI > 78 (overbought exhaustion) |
| `TREND_BREAK_THRESHOLD` | 0.98 | Close if price falls below 200-SMA × this factor |
| `MAX_HOLD_DAYS` | 10 | Close if held longer than N days |

### Testing Custom Rules

1. Update `.env` with your preferred exit thresholds
2. Set `ALPACA_BASE_URL=https://paper-api.alpaca.markets` to test on paper
3. Monitor Telegram for exit confirmations and check the trade ledger
4. After 30–60 trading days, review the P&L and exit patterns
5. Switch to live if the results match your expectations

### Custom Signal Entry (Advanced)

The `/webhook` endpoint accepts custom signals from Python scripts, trading bots, or other services. Format:

```json
{
  "secret_key": "your_WEBHOOK_SECRET_TOKEN",
  "ticker": "AAPL",
  "action": "BUY",
  "price": 195.50,
  "risk_tier": 0
}
```

Examples:
- **Python script:** Use the `requests` library to POST JSON to your server
- **Jupyter notebook:** Same as Python
- **External bot:** Anything that can make HTTP POST requests with the right JSON format

**Important:** Custom signals are evaluated against all exit rules and regime gates just like internal signals.
