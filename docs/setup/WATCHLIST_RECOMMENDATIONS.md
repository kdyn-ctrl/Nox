# Watchlist Recommendations by Account Size & Trading Mode

## Quick Reference: Ticker Counts

| Component | Min | Recommended | Max | Reason |
|-----------|-----|-------------|-----|--------|
| **Equity Scanner** (live) | 3 | 5–8 | 12 | CPU load, scan window overlap |
| **Equity Scanner** (paper) | 3 | 8–12 | 15 | More signals = faster learning |
| **Options BOT** (conservative) | 0 | 3–4 | 6 | IV snapshot + Greeks every 30 min |
| **Options PERSONAL** (aggressive) | 0 | 4–5 | 8 | More margin risk |
| **Analyst Cycle** (daily reports) | 3 | 6–8 | 8 | Claude Haiku prompt reliability |
| **Skeptic Research** (weekly) | 6 | 8–12 | 12 | Deep research, no CPU impact |

---

## Recommended Watchlists by Account Size

### 📚 Learning Phase (Paper Only) — 8 Tickers
**Goal:** Understand the engine before risking capital.

```env
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,COIN,SOFI,F,ARKK
NOX_WATCHLIST_CN=
```

| Ticker | Price | Sector | Why | Notes |
|--------|-------|--------|-----|-------|
| SPY | $450 | Index | Baseline, macro regime | 1 share/trade, liquid |
| QQQ | $360 | Tech Index | Sector rotation | Separate from SPY |
| PLTR | $30 | Data/AI | Growth + value | Good position size (16 shares @ $500) |
| AMD | $130 | Semiconductors | Cyclical, lower than NVDA | Sector play |
| COIN | $120 | Crypto Exchange | Regulation proxy | Hype-sensitive |
| SOFI | $25 | Fintech | Rate-sensitive | Cheapest in list |
| F | $12 | Automotive | Cyclical mean-reversion | Tests contrarian signals |
| ARKK | $70 | Thematic ETF | Clean energy / disruptive tech | Trend exposure |

**Backtest this watchlist:**
```bash
./nox_backtest watchlist=SPY,QQQ,PLTR,AMD,COIN,SOFI,F,ARKK range=2y
# Target: ≥55% win rate on ≥50 signals, Sharpe > 0.3
```

**Paper trade duration:** 1–3 months  
**Goal:** 30+ signals, 55%+ win rate, 0 unexpected failures

---

### 💰 Small Live ($5k Initial) — 5–6 Tickers
**Goal:** Full position sizing (no micro-lots), conservative.

```env
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,SOFI,F
NOX_WATCHLIST_CN=
```

| Ticker | Price | Max Position ($500 cap) | Reasoning |
|--------|-------|--------|-----------|
| SPY | $450 | 1–2 shares | Baseline + regime |
| QQQ | $360 | 1–2 shares | Tech hedge |
| PLTR | $30 | 15–16 shares | Growth, good sizing |
| AMD | $130 | 3–4 shares | Sector, diversification |
| SOFI | $25 | 18–20 shares | Growth + rate sensitivity |
| F | $12 | 40+ shares | Rotation, cheap |

**Deploy after:**
- 30+ paper trades, 55%+ win rate
- Backtest 2y on same list shows similar stats
- Trailing 30-day paper Sharpe > 0.5
- No execution surprises

**Rules:**
- Max 2 simultaneous open positions
- No options (margin complexity)
- Max drawdown: -8% (close all, evaluate)
- Review weekly, adjust watchlist if one ticker underperforms

---

### 🚀 Growth Account ($20k+) — 10 Tickers
**Goal:** Full diversification, sector coverage, options-ready.

```env
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,NVDA,META,SOFI,COIN,ARKK,F
NOX_WATCHLIST_CN=  (leave empty; no IBKR yet)

OPTIONS_BOT_WATCHLIST=SPY,QQQ,PLTR,AMD,NVDA
OPTIONS_BOT_MAX_SIGNALS=2
```

| Ticker | Price | Allocation % | Purpose |
|--------|-------|--------------|---------|
| SPY | $450 | 15% | Baseline |
| QQQ | $360 | 15% | Tech alpha |
| PLTR | $30 | 8% | Growth |
| AMD | $130 | 8% | Semis |
| NVDA | $870 | 8% | Mega-cap (cap sizing) |
| META | $480 | 8% | Mega-cap + AI |
| SOFI | $25 | 8% | Fintech growth |
| COIN | $120 | 8% | Crypto / regulation |
| ARKK | $70 | 8% | Thematic |
| F | $12 | 4% | Rotation |

**New allowances:**
- Up to 4 simultaneous equity positions
- 2–3 simultaneous options (BOT profile only; PERSONAL still off)
- Options assignment acceptable (have 200 shares SPY per short call)

**Scale-up conditions:**
- 90+ live trades with 50%+ win rate
- P&L positive or near-zero (show alpha)
- Max drawdown < 8%
- No mystery fills or slippage > 1%

---

## Watchlist Design Principles

### 1. **Price Range Matters for Sizing**

For a $5,000 account with Kelly 10% allocation per trade = $500 max position:

```
SPY @ $450:    $500 / $450 = 1 share (minimal impact)
AMD @ $130:    $500 / $130 = 3–4 shares (meaningful)
PLTR @ $30:    $500 / $30 = 16 shares (full Kelly sizing possible)
F @ $12:       $500 / $12 = 40+ shares (excellent sizing)
AAPL @ $230:   $500 / $230 = 2 shares (too small for $5k)
NVDA @ $870:   $500 / $870 = 0.5 shares (unusable)
```

**Rule:** For $5k accounts, prefer sub-$100 tickers. For $20k+, you can mix expensive names.

### 2. **Sector Diversification**

Avoid having 80% of your watchlist in one sector (e.g., all tech). Example balanced allocation:

```
Tech:       SPY, QQQ, AMD, NVDA, PLTR (40%)
Financials: SOFI (10%)
Growth:     COIN, ARKK (15%)
Rotation:   F (5%)
Index:      SPY, QQQ (30%)
```

### 3. **Backtested vs. Experimental**

**Keep:** Tickers with 2+ years of backtested data, >50 signals, 55%+ win rate  
**Test:** New tickers in paper for 20+ signals before live commitment  
**Drop:** Tickers with consistent <50% win rate after 30+ signals

### 4. **Avoid Known Pitfalls**

- **Micro-caps (<$5):** Custody nightmares, no options, no news
- **Single survivors:** Stocks that barely exist (F is the limit; don't go lower)
- **Earnings roulette:** Can trade earnings surprises (high IV risk; skip if you care)
- **Single-ticker concentration:** More than 40% in one name = portfolio risk, not diversification

---

## Ticker Migration Path

### Phase 1: Paper ($0 real capital)

```env
# Use recommended learning list (8 tickers)
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,COIN,SOFI,F,ARKK
ALPACA_BASE_URL=https://paper-api.alpaca.markets
KELLY_FRACTION=0.25  # aggressive, learn the win rate
```

**Duration:** 1–3 months  
**Exit criteria:** 30+ signals, 55%+ win rate, Sharpe > 0.5

---

### Phase 2: Live $5k ($5,000 initial, $0 reinvestment)

```env
# Remove COIN (optional) and ARKK (expensive); keep core 6
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,SOFI,F
ALPACA_BASE_URL=https://api.alpaca.markets  # FLIP TO LIVE
KELLY_FRACTION=0.10  # conservative
```

**Constraints:**
- Max 2 simultaneous positions
- No options
- Drawdown limit: -8%
- Review weekly

**Duration:** 90+ days  
**Exit to Phase 3:** 50%+ win rate, positive P&L, max drawdown < 8%

---

### Phase 3: Live $20k+ (reinvestment or deposit)

```env
# Add back expensive tickers + enable options
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,NVDA,META,SOFI,COIN,ARKK,F
OPTIONS_BOT_WATCHLIST=SPY,QQQ,PLTR,AMD,NVDA
ALPACA_BASE_URL=https://api.alpaca.markets
KELLY_FRACTION=0.15  # scale up
```

**New allowances:**
- Up to 4 simultaneous equity positions
- 2–3 options positions (BOT conservative profile)
- Assignment risk acceptable

---

## Stock Selection Criteria

### Must-Have (All Watchlists)
- [ ] SEC-listed company (no pink sheets, no OTC)
- [ ] $1M+ daily volume (liquidity for exits)
- [ ] $10+ stock price (avoids penny stocks, custody issues)
- [ ] Real business (not pure speculation or shell)
- [ ] Available on Alpaca (not all stocks are)

### Nice-to-Have (Preferred)
- [ ] Part of Russell 1000 (institutional holdings, stability)
- [ ] In a sector you understand (reduces surprise risk)
- [ ] Backtested 2+ years with >50 signals
- [ ] Not during earnings week (if risk-averse)
- [ ] Positive 200-day trend (less likely to go to zero)

### Avoid (Red Flags)
- [ ] Penny stocks (< $1, custody nightmare)
- [ ] Delisted soon (check SEC filings)
- [ ] Earnings in next 3 days (surprise volatility)
- [ ] Single catalyst dependent (e.g., FDA approval)
- [ ] Stock splits scheduled (changes your position size)

---

## Example Cheap Stocks for $5k Sizing

| Ticker | Price | 1% of $5k | 10% of $5k | Why |
|--------|-------|----------|-----------|-----|
| SPY | $450 | 11¢ | $1.10 | Baseline, ultra-liquid |
| QQQ | $360 | 18¢ | $1.80 | Tech, liquid |
| PLTR | $30 | $1.50 | $15 | Growth, AI exposure, cheaper |
| AMD | $130 | $6.50 | $65 | Semiconductor, sector play |
| COIN | $120 | $6 | $60 | Crypto exchange, regulation proxy |
| SOFI | $25 | $1.25 | $12.50 | Fintech, rates-sensitive |
| F | $12 | 60¢ | $6 | Automotive, cyclical, cheap |
| ARKK | $70 | $3.50 | $35 | Thematic, disruptive tech |
| GLD | $180 | $9 | $90 | Commodity hedge, negative beta |
| TLT | $95 | $4.75 | $47.50 | Bond ETF, macro hedge |

---

## Backtesting Your Watchlist

Before committing to paper, run 2-year backtests:

```bash
cd /root/Nox/execution

# Test the 8-ticker learning list
./nox_backtest watchlist=SPY,QQQ,PLTR,AMD,COIN,SOFI,F,ARKK range=2y

# Test the 6-ticker live list
./nox_backtest watchlist=SPY,QQQ,PLTR,AMD,SOFI,F range=2y

# Test a 10-ticker growth list
./nox_backtest watchlist=SPY,QQQ,PLTR,AMD,NVDA,META,SOFI,COIN,ARKK,F range=2y
```

**Expected output:**
```
────────────────────────────────────────────────────────
Backtest Results for SPY,QQQ,PLTR,AMD,SOFI,COIN,F,ARKK
────────────────────────────────────────────────────────
Total Trades:        127
Win Rate:            58.3% (74 / 127 wins)
Avg Win/Loss Ratio:  2.31
Sharpe Ratio:        0.62
Max Drawdown:        -12.4%
Total P&L:          $15,420

By Ticker:
  SPY:   28 trades, 61% win | Sharpe 0.45 | P&L +$3,200
  QQQ:   22 trades, 59% win | Sharpe 0.51 | P&L +$2,800
  PLTR:  19 trades, 53% win | Sharpe 0.38 | P&L +$1,900
  AMD:   17 trades, 65% win | Sharpe 0.72 | P&L +$2,100
  ...
```

**Interpret results:**
- Win rate < 52%? Not enough edge (coin flip + costs). Skip this watchlist.
- Sharpe < 0.3? Risk-adjusted returns too low. Add more tickers or adjust rules.
- Max drawdown > 20%? Too volatile for $5k account. Reduce position sizing or use tighter stops.

---

## Summary: What to Use When

| Scenario | Watchlist | Count | Duration |
|----------|-----------|-------|----------|
| Learning (paper) | 8-ticker mixed | 8 | 1–3 months |
| Live $5k bootstrap | 6-ticker cheap | 6 | 90+ days |
| Live $20k+ growth | 10-ticker balanced | 10 | Ongoing |
| Options testing | 4–5 tech-heavy | 4–5 | Paper only |
| Weekend research | Full watch | 8–12 | Ongoing |

---

**Last Updated:** July 1, 2026  
**See also:** TICKER_STRATEGY.md (position sizing), README_CURRENT.md (full system), CHANGES_JULY_2026.md (what changed)

