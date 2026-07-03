# Nox Trading Engine — Changes Summary (July 2026)

## Overview

This document catalogs critical fixes shipped in July 2026 that make the system production-ready for live $5k accounts. All changes are backward-compatible; existing strategies continue unchanged.

---

## Critical Fixes

### 1. Equity Order Execution (Sell Path Unification)

**Problem:** Rule-based exits were closing positions directly, bypassing:
- CN-RULE-002 T+1 settlement gate (could illegally round-trip CN A-shares same-day)
- Trade ledger recording (exits invisible in `/trades` command)
- Signal logging (exits not tagged in `/signals` or `/details`)

**Impact:** Live trading with rule exits was partially blind; orders succeeded but were invisible to monitoring.

**Solution:** All three sell paths now route through the unified pipeline:

| Path | Trigger | Before | After |
|------|---------|--------|-------|
| A. Webhook | Analyst/TradingView webhook | ✅ Logged | ✅ Same |
| B. Trailing-stop | Broker fills ATR stop | ✅ Logged | ✅ Same |
| C. Rule-based | RSI>78, 20-SMA break, TP/SL | ❌ Not logged, bypassed gate | ✅ Now logged + gated |

**Code changes:**
- Added `source` field to `TradeSignal` struct (empty = webhook, non-empty = rule:XXX)
- Rule-exit evaluator now builds `TradeSignal{SELL, source="rule:Take-profit"}` and calls `record_signal()` → `process()`
- Trailing-stop detector tags signal with `source="trailing_stop_close"` for consistency
- `/recent-signals` JSON endpoint now includes `"source"` field

**Telegram messages (new format):**

Before (rule exit, invisible):
```
(no message from the engine; only ledger shows it)
```

After:
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [rule:Take-profit (+15.2%)]
• Entry: $450 @ 10 shares
• Exit: $518 @ 10 shares
• P&L: +$680
```

**Verification:**
- `/signals` and `/details` now show all three types tagged with `[webhook]`, `[rule:XXX]`, `[trailing_stop_close]`
- Rule exits appear in `/trades` persistent ledger (survive restarts)
- T+1 gate applies to rule exits (dormant now, ready if IBKR added)

---

### 2. Paper/Live Seamlessness (Hardcoded URL Fix)

**Problem:** `heartbeat/monitor.py` had 6 hardcoded `https://paper-api.alpaca.markets` URLs:
- `get_alpaca_portfolio()` (2x)
- `_fetch_account_and_positions()` (2x, backs EOD/EOW reports)
- `get_iv_snapshot_alpaca()` (2x, options chains)

**Impact:** Flipping `.env` ALPACA_BASE_URL to live would switch the trading engine to live, but reports + portfolio commands would **silently continue showing paper account data**. Worse than a crash — you'd think you're seeing live balances when you're not.

**Solution:** All hardcoded literals replaced with `ALPACA_BROKER_URL` environment variable (same pattern as C++ engine).

**Code changes:**
```python
# Before:
acc_resp = requests.get('https://paper-api.alpaca.markets/v2/account', ...)

# After:
acc_resp = requests.get(f'{ALPACA_BROKER_URL}/v2/account', ...)
# where ALPACA_BROKER_URL = os.getenv("ALPACA_BASE_URL", "https://paper-api.alpaca.markets")
```

**docker-compose.yml change:**
```yaml
# Before:
heartbeat-monitor:
  environment:
    - ALPACA_API_KEY=...
    - ALPACA_SECRET_KEY=...
    # (no ALPACA_BASE_URL)

# After:
heartbeat-monitor:
  environment:
    - ALPACA_API_KEY=...
    - ALPACA_SECRET_KEY=...
    - ALPACA_BASE_URL=${ALPACA_BASE_URL}  # NEW: tracks execution-engine's choice
```

**Verification:**
- `grep -r paper-api.alpaca.markets heartbeat/monitor.py` returns only the fallback default
- Flip ALPACA_BASE_URL from paper to live, restart containers, confirm `/eod` shows live account (not paper)

---

### 3. Chinese A-Share Correctness (3 Bugs + 1 Non-Bug Clarification)

#### Bug 1: Thread-Unsafe Date Computation

**Problem:** `get_today_date_string()` used `std::localtime()`, which returns a pointer to a **single process-wide static buffer** — not thread-safe. Called concurrently from:
- Webhook handler thread (every BUY/SELL)
- Monitor thread (rule-exit evaluation every 5 min)
- A race here could corrupt the date the T+1 gate compares

**Impact:** Rare but possible silent T+1 gate failures (date mismatch allowing illegal round-trips).

**Solution:** Switched to `gmtime_r` (reentrant). Every other timestamp function in the file already used `gmtime_r`; this one was missed.

```cpp
// Before (thread-unsafe):
std::tm* tm_ptr = std::localtime(&time_t);
oss << std::put_time(tm_ptr, "%Y-%m-%d");

// After (thread-safe):
std::tm utc{};
gmtime_r(&time_t, &utc);
oss << std::put_time(&utc, "%Y-%m-%d");
```

**Verification:** No new compile warnings, same test results.

---

#### Bug 2: Timezone Mismatch (The Important One)

**Problem:** `get_today_date_string()` computed "today" in the **container's local timezone**, which defaults to **UTC** on Ubuntu (checked via docker-compose.yml + Dockerfile). US Eastern market trades on ET, not UTC.

UTC's calendar flips 4–5 hours *before* US Eastern's does. Concretely:
- A BUY at 2pm ET = 6pm UTC = still same UTC calendar day ✓
- A SELL at 8:30pm EDT = 00:30 UTC the *next* day ✗ (wrong!)

So a same-day round-trip could compute:
- Entry date (BUY): 2026-07-01 (both UTC + ET agree)
- Sell date (SELL evening): 2026-07-02 in UTC, but 2026-07-01 in ET

The T+1 gate compares: `2026-07-02 != 2026-07-01` → **ALLOWED** (illegal round-trip).

**Impact:** Silent violations of T+1 rule in evening windows (the exact kind of bug that survives until live trading).

**Solution:** Compute date from UTC, then apply the ET-DST offset already used elsewhere in the codebase:

```cpp
// Before: UTC calendar only
std::tm* tm_ptr = std::localtime(&time_t);  // uses container TZ (UTC)
oss << std::put_time(tm_ptr, "%Y-%m-%d");

// After: ET-anchored
std::tm utc{};
gmtime_r(&time_t, &utc);
int offset_h = (utc.tm_mon >= 3 && utc.tm_mon <= 9) ? 4 : 5;  // EDT vs EST
std::time_t et_time = time_t - offset_h * 3600;
std::tm et{};
gmtime_r(&et_time, &et);
oss << std::put_time(&et, "%Y-%m-%d");
```

**Verification:** New `/cn-status` endpoint shows "Today (ET)" — confirm it matches your wall-clock ET date, not a UTC-shifted date.

---

#### Bug 3: IBKR BUY Path Missing Guard

**Problem:** The Alpaca BUY path correctly gates T+1 recording behind `if (cnBoardLotSize > 1)`:

```cpp
if (cnBoardLotSize > 1) {  // Only for CN A-shares
    china_positions_[sig.ticker] = ChinaPositionRecord{entry_date};
}
```

But the IBKR BUY path (within `#ifdef IBKR_ENABLED`) was **unconditional**:

```cpp
china_positions_[sig.ticker] = ChinaPositionRecord{entry_date};  // always records!
```

**Impact:** Currently zero (IBKR code is dead — `build.sh` never passes `-DIBKR_ENABLED`). But when IBKR is wired up, every US equity BUY would spuriously start a T+1 clock.

**Solution:** Wrapped IBKR T+1 block in the same `if (cnBoardLotSize > 1)` guard as Alpaca, with a comment explaining the design.

---

#### Not a Bug: Repeat-Buy Overwrite Behavior

**Observation:** Repeat-buying the same ticker overwrites `china_positions_[ticker]` with the *newest* entry date, resetting the T+1 clock for the whole position.

**First glance:** Looks wrong (lot-level tracking would let settled shares sell immediately).

**Actually correct:** This system only ever fully liquidates positions (`DELETE /v2/positions/{ticker}` — no partial sell exists). Given that design, blocking the *entire* position until the *most recent* buy clears T+1 is the only sound behavior without real per-lot tracking.

A partial-sell model would need real per-lot dates (bigger feature, out of scope).

**Verification:** Added a one-line comment in the code so future readers don't "fix" this into a bug.

---

### 4. Persistent Logging & Silent Container Failures (Fixed)

**June 28 Incident:** Both analyst-brain and execution-engine containers crashed silently. Their logs were lost because stderr/stdout were never persisted to a volume.

**Impact:** No post-mortem; can't debug what broke.

**Solution:** Added persistent logging volume (`data/docker-logs/`) in docker-compose.yml. All container crashes now leave behind readable transcripts.

**Verification:** Check existing logs at `/root/Nox/data/docker-logs/` (if the incident happened, or manually trigger a crash for testing).

---

## New Diagnostic Surface: `/cn-status` Command

**What it does:** Answers "is CN A-share protection active right now, and what is it tracking?" without grepping logs or reading code.

**Endpoint:** `GET /cn-status` (execution-engine) → JSON response

```json
{
  "board_lot_size": 1,
  "gate_active": false,
  "today": "2026-07-01",
  "positions": []
}
```

**Telegram command:** `/cn_status` in chat (heartbeat-monitor)

```
🇨🇳 CN-RULE-001/002 Status
────────────────────────
• Board Lot Size: 1
• Gate: ⚪ DORMANT — board_lot_size=1, no CN-specific restriction
• Today (ET): 2026-07-01

_No positions currently tracked._
```

**Why useful:** During live $5k testing, `/cn_status` proves the gate is dormant (no surprises when IBKR is added later).

---

## Signal Message Updates (TradingView → Rule-Based)

Old system relied on external TradingView webhooks. New system is internal rule-driven.

### Old Message Format (Webhook-Dependent)
```
🔔 SIGNAL FROM TRADINGVIEW
BUY SPY @ $450

(no info on why, no reason string, no source)
```

### New Message Format (Rule-Based, Webhook Optional)

**Rule-Based Exit (Take-Profit):**
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [rule:Take-profit (+15.2%)]
• Entry: $450 @ 10 shares
• Exit: $518 @ 10 shares
• P&L: +$680 (realized)

Reason: Take-profit threshold (15% configured)
```

**Rule-Based Exit (Stop-Loss):**
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [rule:Stop-loss (-10.0%)]
• Entry: $450 @ 10 shares
• Exit: $405 @ 10 shares
• P&L: -$450 (realized)

Reason: Stop-loss threshold (10% configured)
```

**Rule-Based Exit (RSI Exhaustion):**
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [rule:RSI exhaustion (82.5 ≥ 78)]
• Entry: $450 @ 10 shares
• Exit: $480 @ 10 shares
• P&L: +$300

Reason: RSI ceiling threshold exceeded
```

**Rule-Based Exit (Trend Break):**
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [rule:Trend break (close below SMA20)]
• Entry: $450 @ 10 shares
• Exit: $445 @ 10 shares
• P&L: -$50

Reason: Price closed below 20-day SMA (trend reversal)
```

**Trailing-Stop Close (Broker-Side):**
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [trailing_stop_close]
• Entry: $450 @ 10 shares
• Exit: $440 @ 10 shares (estimated)
• P&L: -$100

Reason: Alpaca ATR-based trailing stop filled
(Exact exit price fetched from Alpaca at next cycle)
```

**Webhook Override (Manual Signal):**
```
✅ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: [webhook]  ← analyst sent it directly
• Entry: $450 @ 10 shares
• Exit: $480 @ 10 shares
• P&L: +$300

Reason: Webhook SELL Signal (from analyst)
```

**T+1 Gate Block (Example — Dormant Today):**
```
🚫 CN T+1 GATE BLOCKED
────────────────────────
• Ticker: TEST_CN
• Trigger: [rule:Take-profit (+12%)]
• Entry Date: 2026-07-01
• Sell Date: 2026-07-01

⛔ Same-day sell prohibited (T+1 rule). Signal discarded.
(This only activates when CN_BOARD_LOT_SIZE=100 in .env — not set today.)
```

---

## Configuration Changes for Users

### `.env` Updates

**Recommended change (cheaper tickers for $5k accounts):**

```bash
# Old (expensive):
NOX_WATCHLIST_US=AAPL,TSLA,NVDA,MSFT

# New (better for small accounts):
NOX_WATCHLIST_US=SPY,QQQ,PLTR,AMD,COIN,SOFI,F
```

See **TICKER_STRATEGY.md** for full watchlist guidance.

**New option: Rule-exit configuration:**

```bash
EQUITY_RULE_EXITS_ENABLED=true        # master switch (already default)
EQUITY_EXIT_TAKE_PROFIT_PCT=0.15      # 15% — adjust based on backtest
EQUITY_EXIT_STOP_LOSS_PCT=0.10        # 10% — adjust based on backtest
EQUITY_EXIT_RSI_CEILING=78            # RSI exhaustion threshold
EQUITY_EXIT_SMA_BREAK=true            # Exit on trend-break
EQUITY_EXIT_MAX_HOLD_DAYS=0           # 0 = off; set to 5+ to auto-exit after N days
```

Test these on paper before live deployment.

---

## Backward Compatibility

✅ **All changes are backward-compatible:**
- Existing `.env` files continue to work (defaults unchanged)
- Webhook SELL signals work identically (source field empty = webhook, no visual change)
- Trailing-stop closes work identically (now just tagged in output)
- Paper/live flipping behavior unchanged (now *correct* for reports too)
- Backtest results unchanged (same signal, same entry/exit logic)

---

## How to Verify Everything Works

### Quick Check (5 min)
```bash
# 1. Rebuild images
docker compose build execution-engine heartbeat-monitor

# 2. Restart containers
docker compose up -d execution-engine heartbeat-monitor

# 3. Check /cn_status endpoint
curl http://localhost:8080/cn-status
# Expected: {"board_lot_size":1, "gate_active":false, "today":"2026-07-01", "positions":[]}

# 4. Check logs for no errors
docker compose logs execution-engine | grep -i error | head -5
```

### Paper Trade Test (1–2 days)
```bash
# 1. Set a low take-profit threshold to trigger exits quickly
# Edit .env: EQUITY_EXIT_TAKE_PROFIT_PCT=0.01  (1% instead of 15%)

# 2. Restart, add a small position manually
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{"action":"BUY","ticker":"SPY","price":450,"rsi":35}'

# 3. Wait 5–10 min, confirm rule exit fires
docker compose logs execution-engine | grep EQUITY_EXIT

# 4. Check Telegram: /signals should show [rule:Take-profit]
# 5. Check Telegram: /trades should show the trade in the ledger
```

### Live $5k Migration Checklist
- [ ] 30+ paper trades with 55%+ win rate
- [ ] Backtest agrees (2y historical match)
- [ ] Telegram `/cn_status` shows dormant (no surprises)
- [ ] Telegram `/eod` shows correct account summary
- [ ] No `paper-api.alpaca.markets` remains except in fallback default
- [ ] Flip ALPACA_BASE_URL to live API endpoint
- [ ] Restart: confirm `/eod` now shows live account
- [ ] Set KELLY_FRACTION=0.10 (conservative)
- [ ] Set drawdown stop-loss (close all at -8%)
- [ ] Trade for 90+ days, monitor for any silent failures

---

## References

- **TICKER_STRATEGY.md** — Watchlist design, position sizing, migration path
- **README_CURRENT.md** — Full system guide (replaces old README.md)
- **TRAILING_STOP_MONITOR_README.md** — Broker-side trailing stops
- **docs/** folder — Design thinking, testing philosophy, implementation details
- **Plan file:** `/root/.claude/plans/ethereal-sleeping-balloon.md` — Full technical details of this release

---

**Shipped:** July 1, 2026  
**Status:** Ready for live $5k accounts after 30-day paper validation  
**Questions?** Check `/help` in Telegram or read source comments in execution/main.cpp

