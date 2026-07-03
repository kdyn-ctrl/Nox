# Documentation Updates — July 2026

## New Documents Created

This update adds 4 new comprehensive guides and updates the primary README. All documents are markdown-formatted for easy navigation.

### 1. **README_CURRENT.md** (Complete System Guide)
- Replaces the outdated README.md (which was options-focused, inaccurate about current architecture)
- **Content:** Live system status, architecture overview, daily operations, configuration, validation checklist, signal messages, troubleshooting
- **Who should read:** Anyone starting with Nox or preparing for live trading
- **Key sections:**
  - What Nox is now (equity + options, rule-based exits)
  - What changed recently (4 critical fixes)
  - How to use it right now (Telegram commands, logs to watch)
  - Configuration (watchlists, exit rules, Kelly sizing)
  - Validation before live deployment
  - Signal message examples (new format: `[rule:Take-profit]` instead of TradingView webhooks)

### 2. **TICKER_STRATEGY.md** (Watchlist & Account Sizing)
- **Content:** Position sizing formula, recommended watchlists by account size ($5k, $20k+), ticker counts per component, cheap stocks for small accounts, migration path (paper → $5k → $20k)
- **Who should read:** Before choosing your watchlist or setting up paper trading
- **Key sections:**
  - Position sizing formula (Kelly Criterion, hard cap 10%)
  - Recommended lists (paper, $5k, $20k+)
  - Ticker counts by component (scanner, options, analyst, research)
  - Signal message examples (rule-based vs webhook)
  - Migration path with phase descriptions and exit criteria
  - Validation checklist before each phase

### 3. **CHANGES_JULY_2026.md** (Critical Fixes & Release Notes)
- **Content:** Detailed technical explanation of 4 critical bugs fixed, paper/live hardcoding fix, new diagnostic endpoint, message format changes, backward compatibility, verification steps
- **Who should read:** Anyone deploying after July 1 or curious what changed
- **Key sections:**
  - Sell-path unification (rule-based exits now logged + gated)
  - Paper/live seamlessness (ALPACA_BASE_URL fix)
  - Chinese A-share correctness (3 real bugs + 1 non-bug)
  - Persistent logging fix (June 28 incident)
  - New `/cn-status` diagnostic
  - Signal message examples (with [rule:XXX] tags)
  - Backward compatibility (all changes safe)
  - Verification steps (quick check, paper test, live checklist)

### 4. **WATCHLIST_RECOMMENDATIONS.md** (Ticker Selection Reference)
- **Content:** Quick ticker count reference table, specific recommended lists with reasoning, stock selection criteria, cheap stocks for $5k sizing, backtesting commands, ticker migration path, example cheap stocks
- **Who should read:** When choosing which stocks to watch
- **Key sections:**
  - Ticker counts by component (min/recommended/max)
  - Watchlist recommendations (8-ticker paper, 6-ticker $5k live, 10-ticker $20k)
  - Design principles (price range, diversification, backtested vs experimental)
  - Phase migration (paper → $5k → $20k)
  - Stock selection criteria (must-have, nice-to-have, red flags)
  - Table of cheap stocks with position sizing examples
  - Backtesting command examples with expected output interpretation

---

## Updated Documents

### .env.example
**Changes:**
- Updated NOX_WATCHLIST_US from `AAPL,TSLA,NVDA,MSFT` to `SPY,QQQ,PLTR,AMD,COIN,SOFI,F` (cheaper for $5k accounts)
- Added NOX_DAILY_REPORT_TICKERS explicitly (was commented out)
- Added EQUITY_RULE_EXITS_* configuration block with detailed comments explaining each parameter
- Added OPTIONS scanner section with guidance (disable for $5k, enable for $20k+)
- Added docker-compose.yml hint about ALPACA_BASE_URL in heartbeat-monitor
- Added TICKER_STRATEGY.md reference for detailed watchlist design

---

## Old Documents (Now Outdated)

The following documents predate the July 2026 fixes and are inaccurate:

- **README.md** — Heavily options-focused, doesn't mention rule-based equity exits, outdated watchlist (AAPL/TSLA/NVDA/MSFT expensive for small accounts)
  - **Recommendation:** Read README_CURRENT.md instead
  
- **docs/NOX_USER_GUIDE.md** — References SPY-only trading, doesn't mention rule-based exits or persistent position tracking
  - **Recommendation:** Use as historical reference only; read README_CURRENT.md for current behavior

- **TRAILING_STOP_MONITOR_README.md** — Still accurate for the trailing-stop subsystem, but context needs README_CURRENT.md
  - **Recommendation:** Good as supplementary detail on broker-side stops

---

## Signal Message Format Changes

### Old Format (TradingView Webhook-Dependent)
```
🔔 SIGNAL FROM TRADINGVIEW
BUY SPY @ $450

(webhook-only, no system-generated signals documented)
```

### New Format (Rule-Based Exits)

All signals now tagged with their origin:

**[rule:Take-profit]**
```
✅ POSITION CLOSED
• Ticker: SPY
• Trigger: [rule:Take-profit (+15.2%)]
• Entry: $450, Exit: $518
• P&L: +$680
```

**[rule:Stop-loss]**
```
✅ POSITION CLOSED
• Ticker: SPY
• Trigger: [rule:Stop-loss (-10.0%)]
• Entry: $450, Exit: $405
• P&L: -$450
```

**[rule:RSI exhaustion]**
```
✅ POSITION CLOSED
• Ticker: SPY
• Trigger: [rule:RSI exhaustion (82.5 ≥ 78)]
• Entry: $450, Exit: $480
• P&L: +$300
```

**[rule:Trend break]**
```
✅ POSITION CLOSED
• Ticker: SPY
• Trigger: [rule:Trend break (close below SMA20)]
• Entry: $450, Exit: $445
• P&L: -$50
```

**[trailing_stop_close]**
```
✅ POSITION CLOSED
• Ticker: SPY
• Trigger: [trailing_stop_close]
• Entry: $450, Exit: $440
• P&L: -$100
```

**[webhook]**
```
✅ POSITION CLOSED
• Ticker: SPY
• Trigger: [webhook]
• Entry: $450, Exit: $480
• P&L: +$300
```

---

## Documentation Navigation Map

```
START HERE:
  ↓
README_CURRENT.md          ← "What is Nox, how do I use it?"
  ├→ System overview
  ├→ What changed recently
  ├→ Daily operations (Telegram commands, logs)
  ├→ Configuration basics
  ├→ Validation checklist
  └→ Troubleshooting

THEN CHOOSE YOUR PATH:

PATH 1: Setting Up Watchlist
  TICKER_STRATEGY.md        ← "Which stocks should I watch?"
  ├→ Position sizing formula
  ├→ Recommended lists by account size
  ├→ Ticker counts per component
  ├→ Migration path (paper → $5k → $20k)
  └→ Validation before each phase
  
    + WATCHLIST_RECOMMENDATIONS.md  ← "Specific ticker selection guide"
      ├→ Quick ticker count reference
      ├→ Specific lists with reasoning
      ├→ Stock selection criteria
      ├→ Cheap stocks table
      └→ Backtesting examples

PATH 2: Understanding Recent Changes
  CHANGES_JULY_2026.md      ← "What's new, what's fixed?"
  ├→ Sell-path unification (rule-based)
  ├→ Paper/live seamlessness (hardcoding fix)
  ├→ CN A-share bugs (3 real, 1 documented)
  ├→ Signal message format (new [rule:XXX] tags)
  └→ Verification steps

PATH 3: Deep Technical Details
  execution/main.cpp        ← Source code (well-commented)
  heartbeat/monitor.py      ← Telegram interface + reports
  docs/DESIGN_THINKING.md   ← Architecture rationale
  execution/IMPLEMENTATION_CHECKLIST.md ← C++ subsystems

KEEP AS REFERENCE:
  .env.example              ← All tunable parameters
  TRAILING_STOP_MONITOR_README.md ← Broker-side stops
  docs/                     ← Older design docs, still useful
```

---

## Key Improvements Made

### Clarity
- **Old:** System description scattered across README.md + multiple outdated guides
- **New:** README_CURRENT.md gives complete picture in one place

### Specificity
- **Old:** No ticker guidance; README.md used expensive tickers (AAPL/TSLA/NVDA/MSFT)
- **New:** TICKER_STRATEGY.md + WATCHLIST_RECOMMENDATIONS.md with specific lists, position sizing examples, and migration path for $5k → $20k

### Signal Messages
- **Old:** Only TradingView webhook messages documented
- **New:** All 6 signal types documented with examples ([webhook], [rule:XXX], [trailing_stop_close])

### Reproducibility
- **Old:** Backtesting commands scattered, results interpretation unclear
- **New:** Example backtest output with interpretation guide

### Completeness
- **Old:** Configuration parameters in code comments only
- **New:** Fully documented in .env.example with explanations

---

## How to Use These Documents

### For Live Trading Preparation
1. Read **README_CURRENT.md** (system overview)
2. Read **TICKER_STRATEGY.md** (position sizing, migration path)
3. Choose watchlist from **WATCHLIST_RECOMMENDATIONS.md**
4. Run backtest on chosen list
5. Follow phase migration (paper → $5k live → $20k+)
6. Reference **CHANGES_JULY_2026.md** if you see unexpected signal tags

### For Troubleshooting
- **"Why is my position not closing?"** → README_CURRENT.md: "Rule Exits Not Firing" section
- **"What does [rule:XXX] mean?"** → CHANGES_JULY_2026.md: "Signal Message Format" section
- **"Should I trade AAPL on my $5k account?"** → WATCHLIST_RECOMMENDATIONS.md: "Position sizing" section (answer: no, too expensive)
- **"What's the migration path?"** → TICKER_STRATEGY.md: "Migration Path" section

### For Context on Recent Fixes
- **"What broke on June 28?"** → CHANGES_JULY_2026.md: "Persistent Logging & Silent Container Failures"
- **"Why do rule-based exits now show [rule:Take-profit]?"** → CHANGES_JULY_2026.md: "Equity Order Execution" section
- **"Is the paper/live flip safe now?"** → CHANGES_JULY_2026.md: "Paper/Live Seamlessness" section

---

## Document Cross-References

All documents link to each other for easy navigation:

- README_CURRENT.md references TICKER_STRATEGY.md for watchlist design
- TICKER_STRATEGY.md references WATCHLIST_RECOMMENDATIONS.md for specific tickers
- WATCHLIST_RECOMMENDATIONS.md references TICKER_STRATEGY.md for position sizing
- CHANGES_JULY_2026.md references README_CURRENT.md for signal message examples
- .env.example references TICKER_STRATEGY.md and WATCHLIST_RECOMMENDATIONS.md for configuration guidance

---

## Recommended First Read for New Users

**If you have $5k and want to start live trading:**

1. README_CURRENT.md (15 min) — Understand the system
2. TICKER_STRATEGY.md, sections "Position Sizing" + "Recommended Watchlists" → "Small Live Account ($5k)" (10 min)
3. WATCHLIST_RECOMMENDATIONS.md, sections "Recommended Watchlists by Account Size" → "$5k Live" (5 min)
4. Follow the paper → $5k migration path in TICKER_STRATEGY.md (30–60 days)

**Total prep time:** 30 minutes + 30–60 days of paper trading before live.

---

## Summary Table: What Changed & Where to Find It

| What Changed | Type | Document | Section |
|---|---|---|---|
| Sell-path unification (rule exits now logged) | Fix | CHANGES_JULY_2026.md | "Equity Order Execution" |
| Paper/live URL hardcoding (all broker calls now env-driven) | Fix | CHANGES_JULY_2026.md | "Paper/Live Seamlessness" |
| Thread-unsafe date computation (gmtime_r) | Fix | CHANGES_JULY_2026.md | "Chinese A-Share Correctness" → "Bug 1" |
| Timezone mismatch (UTC vs ET, T+1 gate fail) | Fix | CHANGES_JULY_2026.md | "Chinese A-Share Correctness" → "Bug 2" |
| New /cn-status endpoint | Feature | CHANGES_JULY_2026.md | "New Diagnostic Surface" |
| Signal tags [rule:XXX] | Feature | CHANGES_JULY_2026.md | "Signal Message Updates" |
| Recommended watchlists for $5k | Guidance | TICKER_STRATEGY.md, WATCHLIST_RECOMMENDATIONS.md | All sections |
| Position sizing examples | Guidance | TICKER_STRATEGY.md | "Position Sizing Formula" |
| Ticker migration path | Guidance | TICKER_STRATEGY.md | "Migration Path" |
| Cheap stocks list | Reference | WATCHLIST_RECOMMENDATIONS.md | "Example Cheap Stocks" |

---

**Last Updated:** July 1, 2026  
**Status:** Ready for live trading after 30-day paper validation  
**Questions?** Start with README_CURRENT.md, then reference specific guides as needed.

