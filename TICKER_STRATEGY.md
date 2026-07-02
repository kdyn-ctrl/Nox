# Nox Ticker Strategy & Watchlist Design

## Account Size & Ticker Selection

The number and type of tickers you watch directly impact position sizing, margin usage, and diversification. This guide shows recommended watchlists for different account sizes and trading modes.

---

## Position Sizing Formula

For a $5,000 account with Kelly Criterion-based sizing:

```
Max Position Size = Kelly % × Account × (1 - Margin Buffer)
Kelly % = min(raw_kelly, 10%)  # hard cap
```

**Example: $5k account, 60% Kelly win-rate scenario:**
- Raw Kelly: 0.12 = 12%
- Hard cap: min(12%, 10%) = 10%
- Per-trade allocation: $5,000 × 10% = $500 max

**For a $100 stock:** $500 / $100 = 5 shares per trade
**For a $30 stock:** $500 / $30 ≈ 17 shares per trade
**For a $15 stock:** $500 / $15 ≈ 33 shares per trade

**Key insight:** Cheaper stocks ($15–50 range) allow fuller position sizing on small accounts. Expensive stocks ($150+) force micro-positions that barely move the portfolio.

---

## Recommended Watchlists

### 🧪 Paper Testing (Learning Phase) — 6–8 Tickers
**Goal:** Understand the engine's behavior without live capital risk. Include index proxies + growth + rotation plays.

**Recommended list:**
```
SPY         $450    Index baseline, very liquid, macro regime indicator
QQQ         $360    Tech concentration, separate from SPY
PLTR        $30     Growth + value blend, good sized positions
AMD         $130    Tech, lower than NVDA, sector play
COIN        $120    Growth + real business, regulatory exposure
SOFI        $25     Fintech, good position size room, hype-sensitive
F           $12     Automotive cyclical, cheap, tests mean-reversion
GLD         $180    Commodity hedge, negative equity beta
```

**Why this mix:**
- **SPY + QQQ**: Regime & macro context (free market signals)
- **PLTR, AMD**: Growth with cheaper entry than AAPL/NVDA (test momentum)
- **COIN**: Regulatory + crypto sentiment proxy (tests sentiment edge)
- **SOFI**: Fintech rotation play (tests sector rotation)
- **F**: Cheap + cyclical (tests contrarian / mean-reversion signals)
- **GLD**: Non-correlated, tests risk-off behavior

**Run backtests:**
```bash
./nox_backtest watchlist=SPY,QQQ,PLTR,AMD,COIN,SOFI,F range=2y
```

---

### 💰 Small Live Account ($5k initial) — 5–6 Tickers
**Goal:** Live capital with full position sizing (no micro-lots). Conservative, liquid, positive expected value.

**Recommended list:**
```
SPY         $450    Baseline + regime, 10 shares = $4.5k max position
QQQ         $360    Tech hedge, 15 shares = $5.4k max position
AMD         $130    Sector play, 30 shares = $3.9k position room
PLTR        $30     Growth, 150 shares = $4.5k position room
SOFI        $25     Growth + rate-sensitive, 200 shares = $5k max
F           $12     Rotation, 400 shares = $4.8k max (use conservatively)
```

**Rationale:**
- **Only tickers ≤$30 & SPY/QQQ**: Ensures useful position sizes on $5k
- **No micro-cap junk**: All are SEC-regulated, institutional holdings, >1M daily volume
- **Sector mix**: Tech (PLTR, AMD), financials (SOFI), autos (F), index (SPY, QQQ)
- **Backtested**: All appear in the paper-testing backtest above

**Kelly position sizes (assuming 60% win-rate):**
- SPY: ~$450 (10 shares)
- QQQ: ~$400 (11 shares)
- AMD: ~$350 (2-3 shares)
- PLTR: ~$320 (10 shares)
- SOFI: ~$300 (12 shares)
- F: ~$250 (20+ shares)
- **Total: ~$2,070 average position** across the account

**When to scale up to $20k:**
- 30+ trades with 55%+ win rate on live signals
- Trailing 90-day Sharpe > 0.5
- Maximum live drawdown < 8%
- No unexpected loss events

---

### 🚀 Growth Account ($20k+) — 8–10 Tickers
**Goal:** Full diversification, sector coverage, ready for options.

**Recommended list (add to small-account base):**
```
SPY, QQQ, PLTR, AMD, SOFI, F  (keep the above)
NVDA        $870    Semiconductor heavyweight (cap allocation, not sizing)
META        $480    Mega-cap tech + AI exposure
COIN        $120    Crypto / regulation proxy
ARKK        $70     Thematic (clean energy, disruptive tech)
```

**Allocation logic:**
- **$5k base portfolio:** Covers core signals
- **$15k additional:** Add NVDA + META (large-cap tech), COIN (crypto exposure), ARKK (themes)
- **Larger positions:** Can now take 5-10 share positions in expensive names

**Sector breakdown:**
- Tech: PLTR, AMD, NVDA, META (40%)
- Financials: SOFI (10%)
- Index: SPY, QQQ (30%)
- Thematic: COIN, ARKK (15%)
- Rotation: F (5%)

---

## Ticker Counts by Component

### Equity Signal Generator
- **Recommended:** 5–8 tickers for live, 8–12 for backtesting
- **Constraint:** Each ticker scans for RSI + SMA + ATR every 5 min → CPU load
- **Current system:** 4 US + 5 CN (ADRs) = 9 total, runs comfortably on 1 core

**Too few (<3):** No diversification, single-ticker drawdowns brutal
**Too many (>15):** Market data fetches slow down, scan window overlaps next cycle

---

### Options Signal Generator (via WS1 Contradiction Vector)
- **Recommended:** 3–6 tickers per profile (BOT conservative, PERSONAL aggressive)
- **Reason:** Each ticker needs 30-minute IV snapshot + Greeks computation
- **Current env example:** Commented out, but typically SPY, QQQ, AAPL, TSLA, NVDA

```env
# Conservative profile
OPTIONS_BOT_WATCHLIST=SPY,QQQ,AAPL,TSLA
OPTIONS_BOT_MAX_SIGNALS=3

# Aggressive profile
OPTIONS_PERSONAL_WATCHLIST=SPY,QQQ,AAPL,TSLA,NVDA,AMD,COIN
OPTIONS_PERSONAL_MAX_SIGNALS=2
```

**For $5k account:** Disable options entirely (too much margin complexity)
**For $20k account:** 4–5 tickers, BOT profile only (conservative)

---

### Analyst Cycle (WS4 Decay / Regime Logic)
- **Recommended:** 6–8 tickers for daily SEC reports
- **Constraint:** Claude Haiku prompt reliability degrades past 8 tickers
- **Max configured:** `MAX_DAILY_REPORT_SEC_TICKERS=8`

**Example (good):**
```env
NOX_DAILY_REPORT_TICKERS=SPY,QQQ,PLTR,AMD,SOFI,F,COIN,ARKK
```

This covers all sectors, stays under the prompt limit, and generates actionable reports.

---

### Skeptic Architecture (WS6 Weekend Research)
- **Recommended:** Full 8–12 ticker watch for deep research
- **Frequency:** Runs once weekly (Friday → Monday report)
- **CPU:** Negligible; JSON research, no live data fetches

**Use for:**
- Full correlation matrix recompute
- Insider cluster detection (Form 4 scrapes)
- Earnings calendar cross-check
- Liquidity regime classification

---

## Cheap Stocks for $5k Sizing

| Ticker | Price | Sector | Why | Sizing ($500 max) |
|--------|-------|--------|-----|-------------------|
| SPY    | $450  | Index  | Baseline, macro regime | 1 share |
| QQQ    | $360  | Tech Index | Tech alpha | 1–2 shares |
| PLTR   | $30   | Data/AI | Growth + undervalued | 16 shares |
| SOFI   | $25   | Fintech | Rate-sensitive, hype | 20 shares |
| AMD    | $130  | Chips  | Semi cycle play | 3–4 shares |
| COIN   | $120  | Crypto | Regulation proxy | 4 shares |
| F      | $12   | Auto   | Cyclical mean-reversion | 40 shares |
| GEVO   | $8    | Clean Energy | Micro-cap growth | 60 shares |
| SIRI   | $7    | Media | Buyout + dividend | 70 shares |
| RIOT   | $18   | Crypto Mining | Leveraged BTC | 27 shares |

**Avoid for live:** Micro-cap (<$5 = custody nightmare), no-earnings junk, or single-digit survivors (GEVO, SIRI only for paper experimentation).

---

## Migration Path: Paper → $5k Live → $20k+

### Phase 1: Paper (1–3 months)
```env
ALPACA_BASE_URL=https://paper-api.alpaca.markets
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,COIN,SOFI,F
NOX_WATCHLIST_CN=  # (disable for now; no real A-shares on Alpaca)
EQUITY_RULE_EXITS_ENABLED=true
KELLY_FRACTION=0.25  # aggressive, learn the win rate
```

**Goals:**
- 30+ signals on equity BUY/SELL paths
- 55%+ directional accuracy
- 50%+ trade win rate (% profitable, not win-loss ratio)
- No unexplained drawdowns >10%

---

### Phase 2: Live $5k ($5k initial, $0 reinvestment)
```env
ALPACA_BASE_URL=https://api.alpaca.markets  # flip to live
ALPACA_API_KEY=your_live_key
ALPACA_SECRET_KEY=your_live_secret
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,SOFI,F  # same as phase 1
KELLY_FRACTION=0.10  # conservative, trust the backtest
```

**Constraints:**
- Max 2 simultaneous positions to avoid margin
- No options (margin + complexity)
- Drawdown limit: Stop at -8% (close all positions, re-evaluate)
- Review weekly, adjust watchlist if a ticker underperforms

---

### Phase 3: Live $20k+ (reinvestment or deposit)
```env
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,NVDA,SOFI,COIN,F,ARKK  # +3 tickers
OPTIONS_BOT_WATCHLIST=SPY,QQQ,PLTR,AMD,NVDA
OPTIONS_BOT_AUTO_EXECUTE=false  # still manual for safety
OPTIONS_BOT_MAX_SIGNALS=2
KELLY_FRACTION=0.15  # scale up as confidence builds
```

**New allowances:**
- Up to 4 simultaneous equity positions
- 2–3 simultaneous options positions (conservative BOT profile)
- Options assignment risk acceptable (have 200 shares SPY per short call)

---

## Validation Checklist

### Before Paper (ensure backtest is clean)
- [ ] Run 2-year backtest on proposed watchlist
- [ ] Win rate ≥ 55% on ≥50 signals
- [ ] Directional accuracy > 52% (statistical edge, not noise)
- [ ] Sharpe > 0.3 (average return per unit risk)
- [ ] Max drawdown < 15% (survived volatility)

### Before Live $5k
- [ ] 30+ paper trades with 55%+ win rate
- [ ] Trailing 30-day paper Sharpe > 0.5
- [ ] No losses on opening day (proves execution works)
- [ ] Confirm Alpaca account has $5k minimum (PDT rules)
- [ ] Telegram alerts firing correctly (rule exits visible)

### Before Scaling to $20k
- [ ] 90+ live trades with 50%+ win rate
- [ ] P&L positive or near-breakeven (show alpha > 0)
- [ ] Maximum live drawdown < 8%
- [ ] No mystery fills or slippage >1% on any trade
- [ ] Regime gating working (RISK_OFF suppresses trades)

---

## Signal Message Examples (Rule-Based, Not TradingView)

**Old (TradingView webhook):**
```
🔔 SIGNAL FROM TRADINGVIEW
BUY SPY @ $450
```

**New (Rule-Based Equity Exit):**
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [rule:Take-profit (+12.5%)]
• Entry: $450 @ 100 shares
• Exit: $505 @ 100 shares
• P&L: $5,500
• Reason: Take-profit threshold reached
```

**Webhook override (manual signal from analyst):**
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [webhook]  ← analyst sent it manually
• Entry: $450
• Exit: $480
• Reason: Webhook SELL Signal
```

---

## Recommended Reading

See also:
- [[feature_trade_ledger_and_exits]] — how rule-based exits work
- [[project_skeptic_architecture]] — WS4/WS6 research layers
- `.env.example` — all tunable parameters per component

