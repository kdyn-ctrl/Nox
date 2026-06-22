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
A: No. Nox only buys and sells whole shares of SPY. No options, no margin, no leverage. It's conservative by design.

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

**Last Updated: 2026-06-22**  
**Status: Ready to use**

This guide should be re-read every time the bot's behavior changes. If you notice something in the actual bot that doesn't match this guide, that's a sign the guide needs updating.
