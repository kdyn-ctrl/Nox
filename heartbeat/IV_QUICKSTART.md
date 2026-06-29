# IV System — Quick Start Guide

## What Was Built

A **free, highly-accurate historical Implied Volatility dataset** that automatically collects end-of-day IV snapshots from Alpaca and calculates true 52-week IV Rank percentiles without premium data feeds.

## 🚀 System is Now Live

The heartbeat service automatically:
1. **Collects IV** daily at **4:30 PM ET** (post-market buffer)
2. **Stores snapshots** in the `historical_volatility` SQLite table
3. **Calculates IV Rank** percentiles once ≥30 days of data exist
4. **Falls back gracefully** to snapshot-relative calculation for young data

## ✅ Implementation Checklist

- [x] **Database schema** — `historical_volatility` table with (ticker, date, implied_volatility)
- [x] **Scheduled task** — Runs daily at 16:30 ET on trading days
- [x] **Alpaca integration** — Fetches options chain IV from `/v1beta3/options/chains/{ticker}`
- [x] **Thread safety** — All DB operations wrapped in `db_lock`
- [x] **IV Rank calculator** — Percentile calculation with graceful fallback
- [x] **Logging** — Detailed INFO/WARNING/ERROR logs for monitoring
- [x] **DST handling** — Automatic daily reschedule (no container restart needed)

## 📊 Using the System

### Get IV Rank for any ticker
```python
from heartbeat.monitor import calculate_iv_rank

result = calculate_iv_rank("NVDA")
print(f"IV Rank: {result['iv_rank']:.1%}")
print(f"Method: {result['method']}")  # 'full_history' or 'snapshot_relative'
```

### Check all watchlist tickers ranked by IV
```python
from heartbeat.monitor import calculate_iv_rank, WATCHLIST

for ticker in sorted(WATCHLIST):
    r = calculate_iv_rank(ticker)
    status = "✓" if r['method'] == 'full_history' else "⚠"
    print(f"{status} {ticker:6} {r['iv_rank']:.1%}")
```

### Use cached IV (don't refetch from Alpaca)
```python
cached_iv = 0.38  # from a live snapshot
result = calculate_iv_rank("AAPL", current_iv=cached_iv)
```

## 📈 Data Availability Timeline

| Phase | Duration | IV Rank Method | Status |
|-------|----------|-----------------|--------|
| **Ramp-up** | Days 1–29 | Snapshot-relative | ⚠ Fallback active |
| **Mature** | Day 30+ | Full 52-week | ✓ Full range available |
| **Complete** | Day 252+ | Full year | ✓ Complete annual data |

**Action needed**: None. Automatic ramp-up begins on first `collect_eod_iv_snapshots()` call.

## 🔍 Monitor Collection

### Watch logs for IV collection (every day at 16:30 ET)
```bash
docker logs heartbeat 2>&1 | grep "IV collection"
```

### Expected log output:
```
[INFO] [HEARTBEAT] Starting end-of-day IV snapshot collection...
[INFO] [HEARTBEAT] Fetched IV for AAPL: 0.3200 (from 156 contracts, OI: 78934)
[INFO] [HEARTBEAT] Fetched IV for TSLA: 0.4150 (from 203 contracts, OI: 145677)
[INFO] [HEARTBEAT] Stored IV snapshot: AAPL on 2024-06-25 = 0.3200
[INFO] [HEARTBEAT] Stored IV snapshot: TSLA on 2024-06-25 = 0.4150
...
[INFO] [HEARTBEAT] EOD IV collection complete: 8 succeeded, 1 failed
```

## 🛠️ Integration Points

### Add IV to a trading algorithm
```python
from heartbeat.monitor import calculate_iv_rank

def should_sell_volatility(ticker):
    result = calculate_iv_rank(ticker)
    if result['iv_rank'] is None:
        return None  # Can't decide yet
    return result['iv_rank'] > 0.75  # Sell if elevated

if should_sell_volatility("NVDA"):
    # Sell call spreads, iron condors, etc.
    pass
```

### IV Rank alert in Telegram bot
```python
from heartbeat.monitor import calculate_iv_rank, WATCHLIST

for ticker in WATCHLIST:
    result = calculate_iv_rank(ticker)
    if result['iv_rank'] and result['iv_rank'] > 0.80:
        bot.send_message(CHAT_ID, f"🔴 {ticker}: IV Rank at {result['iv_rank']:.0%}")
```

### IV-based position sizing
```python
from heartbeat.monitor import calculate_iv_rank

def position_size_for_premium_sell(ticker, base_size=100):
    result = calculate_iv_rank(ticker)
    if result['iv_rank'] is None:
        return 0  # Can't trade yet
    
    # Size up when IV is elevated (more premium to collect)
    size_multiplier = 1.0 + (result['iv_rank'] - 0.5) * 2
    return int(base_size * size_multiplier)
```

## 📚 Full Documentation

- **[IV_API_REFERENCE.md](IV_API_REFERENCE.md)** — Function signatures, patterns, return values
- **[IV_SYSTEM_GUIDE.md](IV_SYSTEM_GUIDE.md)** — Architecture, scheduler, logging, performance
- **[IV_IMPLEMENTATION_SUMMARY.md](IV_IMPLEMENTATION_SUMMARY.md)** — Implementation details, design decisions
- **[iv_examples.py](iv_examples.py)** — 6 runnable example patterns (run: `python3 iv_examples.py 1`)

## 💾 Database Access

### View collected IV data
```python
import sqlite3
from heartbeat.monitor import DB_PATH, db_lock

with db_lock:
    with sqlite3.connect(DB_PATH) as conn:
        c = conn.cursor()
        c.execute("""
            SELECT ticker, date, implied_volatility
            FROM historical_volatility
            ORDER BY date DESC, ticker
            LIMIT 20
        """)
        for ticker, date, iv in c.fetchall():
            print(f"{date} {ticker:6} {iv:.4f}")
```

## ⚠️ Known Constraints

1. **30-day warm-up**: Full IV Rank calculation requires ≥30 trading days (6 weeks)
   - Before that, snapshot-relative fallback is used automatically
   - Log: "⚠ Insufficient data for TICKER: only X days (need 30+)"

2. **Alpaca paper trading API**: Only works with paper trading accounts
   - Requires valid ALPACA_API_KEY and ALPACA_SECRET_KEY environment variables
   - Subject to Alpaca's rate limits (typically 200 req/min)

3. **Timezone**: Collection happens at 16:30 ET (Eastern Time)
   - Automatically adjusts for EST/EDT transitions
   - No container restart required

## 🔧 Troubleshooting

### IV collection not running
1. Check if heartbeat service is running: `docker ps | grep heartbeat`
2. Verify Alpaca credentials: `echo $ALPACA_API_KEY`
3. Check logs: `docker logs heartbeat 2>&1 | grep "IV collection"`

### No historical data yet
- **Expected on first day**: Wait for first 16:30 ET collection
- **Manual trigger**: `python3 -c "from heartbeat.monitor import collect_eod_iv_snapshots; collect_eod_iv_snapshots()"`

### IV Rank returns None
- Check `result['method']` and `result['error']` for details
- Likely causes: Alpaca API error, network timeout, or <30 days of data

### Scheduler issues
1. View all scheduled jobs: `python3 -c "import schedule; print(schedule.get_jobs())"`
2. Check if 'iv_collection' tag exists: `python3 -c "import schedule; print([j.tag for j in schedule.get_jobs()])"`

## 🎯 Next Steps

1. **Deploy** — Heartbeat is already running; collections begin at 16:30 ET
2. **Monitor** — Watch logs for "IV collection complete" messages
3. **Wait** — After 30 trading days (6 weeks), full IV Rank calculations unlock
4. **Integrate** — Use `calculate_iv_rank()` in your trading logic
5. **Extend** — Build on IV Rank (term structure, heatmaps, alerts, breadth signals)

## 📞 Support

For detailed API docs, see [IV_API_REFERENCE.md](IV_API_REFERENCE.md).  
For architecture questions, see [IV_SYSTEM_GUIDE.md](IV_SYSTEM_GUIDE.md).  
For implementation details, see [IV_IMPLEMENTATION_SUMMARY.md](IV_IMPLEMENTATION_SUMMARY.md).

---

**Status**: ✅ Live and collecting. First full IV Rank calculations available in 30 trading days.
