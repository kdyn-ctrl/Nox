# Historical Implied Volatility Dataset System

## Overview
This system builds a free, accurate historical IV dataset by collecting end-of-day options chain snapshots from Alpaca, enabling true IV Rank percentile calculations without premium data feeds.

## Components

### 1. Database Schema
- **Table**: `historical_volatility`
- **Columns**:
  - `ticker` (TEXT): Stock symbol
  - `date` (DATE): Trading date (YYYY-MM-DD format)
  - `implied_volatility` (REAL): Weighted-average IV from options chain
  - `snapshot_timestamp` (DATETIME): When the snapshot was recorded
  - **Unique constraint** on (ticker, date) — no duplicate daily snapshots
  - **Index** on (ticker, date) for fast historical lookups

### 2. Scheduled EOD Collection (16:30 ET)
**Function**: `collect_eod_iv_snapshots()`
- Runs daily at 4:30 PM ET (30-min buffer after market close)
- Iterates through entire watchlist (DOMESTIC + CHINESE_ADRS)
- For each ticker:
  1. Fetches latest options chain from Alpaca
  2. Calculates weighted-average IV across all liquid contracts
  3. Stores snapshot to `historical_volatility` table
- Thread-safe via `db_lock`
- Non-blocking: individual ticker failures don't halt the full collection

### 3. IV Snapshot Fetcher
**Function**: `fetch_options_chain_iv(ticker: str) -> float | None`
- Calls Alpaca `/v1beta3/options/chains/{ticker}` endpoint
- Weights IV by open interest (reflects market consensus)
- Logs detailed context: contract count, total OI, calculated IV
- Returns weighted average IV as a decimal (e.g., 0.35 for 35% IV)
- Returns None on fetch failure (with logged exception)

### 4. IV Rank Calculator
**Function**: `calculate_iv_rank(ticker: str, current_iv: float | None = None) -> dict`

Returns a dictionary with:
```python
{
    'iv_rank': float in [0, 1],      # Percentile in recent IV range
    'current_iv': float,              # Current IV snapshot
    'method': str,                    # 'full_history', 'snapshot_relative', or 'error'
    'data_points': int,               # Number of historical records used
    'days_available': int,            # Distinct trading days in history
    'iv_min': float,                  # (full_history) 52-week low
    'iv_max': float,                  # (full_history) 52-week high
    'average_iv': float,              # (snapshot_relative fallback)
    'error': str                      # (error case only)
}
```

#### Logic
- **Full History (30+ days available)**:
  - IV Rank = (Current IV - Min IV) / (Max IV - Min IV)
  - Clamped to [0, 1] (percentile in historical range)
  - Example: IV Rank = 0.75 means current IV is at 75th percentile

- **Snapshot-Relative Fallback (< 30 days)**:
  - IV Rank = (Current IV - Average IV) / Average IV
  - Clamped to [0, 1]
  - WARNING logged with data point count
  - Example: 8 days of data triggers this fallback

- **Error Case**:
  - Returns dict with `iv_rank: None` and `error` message
  - Logged at ERROR level

### 5. Thread Safety
All database operations wrapped in `db_lock` (existing threading.Lock):
```python
with db_lock:
    with sqlite3.connect(DB_PATH) as conn:
        # Read/write operations here
```
- Prevents concurrent write conflicts (sqlite3.OperationalError: database is locked)
- Reentrant design shared with daily scout and SEC radar threads

## Usage Examples

### Collect IV snapshots (runs automatically at 16:30 ET)
```python
collect_eod_iv_snapshots()
# Fetches IV for all tickers in WATCHLIST, stores to DB
```

### Calculate IV Rank for a specific ticker
```python
result = calculate_iv_rank("NVDA")
print(f"NVDA IV Rank: {result['iv_rank']:.2%}")
print(f"Method: {result['method']}")  # 'full_history' or 'snapshot_relative'
print(f"Days of history: {result['days_available']}")
```

### Use cached IV (don't re-fetch from Alpaca)
```python
cached_iv = 0.38  # from a recent live snapshot
result = calculate_iv_rank("AAPL", current_iv=cached_iv)
```

### Check if we have enough data for full-rank calculation
```python
result = calculate_iv_rank("TSLA")
if result['method'] == 'full_history':
    print(f"✓ Full 52-week range available: {result['iv_min']:.2%}–{result['iv_max']:.2%}")
elif result['method'] == 'snapshot_relative':
    print(f"⚠ Limited data ({result['days_available']} days), using fallback")
```

## Scheduler Integration

The IV collection task is registered in `schedule_checker()`:
- **Trigger**: Daily at 16:30 ET (post-market)
- **Rescheduled**: Nightly at 00:01 UTC (maintains correct UTC offset through DST)
- **Tag**: "iv_collection" (can be inspected/cleared via `schedule.get_jobs()`)
- **DST Handling**: Automatic — no container restart needed

Combined with existing Scout and SEC Radar schedulers:
```
00:01 UTC ─── _combined_reschedule()
               ├─ _reschedule_scout()       [9:00 AM ET]
               └─ _reschedule_iv_collection() [4:30 PM ET]

16:30 ET  ─── collect_eod_iv_snapshots()   [Runs daily]
21:30 ET  ─── run_scout_protocol()          [Runs daily]
```

## Logging Output

All IV operations log to the heartbeat process:

```
[INFO] [HEARTBEAT] Starting end-of-day IV snapshot collection...
[INFO] [HEARTBEAT] Fetched IV for NVDA: 0.3845 (from 127 contracts, OI: 45782)
[INFO] [HEARTBEAT] Stored IV snapshot: NVDA on 2024-06-25 = 0.3845
[WARNING] [HEARTBEAT] Insufficient data for TSLA: only 15 days (need 30+). Falling back to snapshot-relative calculation.
[INFO] [HEARTBEAT] IV Rank for AAPL: 68.50% (Current=0.3200, Min=0.2850, Max=0.4100, Points=252)
[ERROR] [HEARTBEAT] Exception fetching options chain for BABA: HTTP 429 (rate limited)
[INFO] [HEARTBEAT] EOD IV collection complete: 8 succeeded, 1 failed
```

## Performance Notes

- **Alpaca API**: ~1 call per ticker, ~200ms per request = ~10s for full watchlist
- **Database**: INSERTs wrapped in single transaction, ~1ms per write
- **Lock Contention**: Minimal — EOD collection (16:30) doesn't overlap with Scout (21:30) or SEC radar (continuous polling)

## Future Enhancements

- **IV Term Structure**: Track near/far expirations separately
- **Greek IV Surfaces**: Volatility smile/skew across strike levels
- **Volume-Weighted IV**: Distinguish between liquid and illiquid contracts
- **Real-time Percentile**: Query rank within the last N days (rolling window)
