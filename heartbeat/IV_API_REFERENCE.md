# IV System — API Reference Card

## Core Functions

### `fetch_options_chain_iv(ticker: str) -> float | None`
Fetch current IV from Alpaca options chain (weighted by open interest).

```python
iv = fetch_options_chain_iv("NVDA")
# Returns: 0.3845 (35.45% IV) or None on error
```

### `store_iv_snapshot(ticker: str, iv: float, date_str: str) -> bool`
Write an IV snapshot to the database (thread-safe, uses db_lock).

```python
success = store_iv_snapshot("NVDA", 0.3845, "2024-06-25")
# Returns: True if successful, False if error
```

### `collect_eod_iv_snapshots() -> None`
Main task: fetch IV for all watchlist tickers, store to DB. Runs at 16:30 ET daily.

```python
collect_eod_iv_snapshots()
# Logs: "EOD IV collection complete: 8 succeeded, 1 failed"
```

### `calculate_iv_rank(ticker: str, current_iv: float | None = None) -> dict`
Calculate IV percentile in historical range. Returns full dict with metadata.

```python
result = calculate_iv_rank("NVDA")
# result['iv_rank'] = 0.685 (68.5th percentile)
# result['method'] = 'full_history' (or 'snapshot_relative', 'error')
# result['current_iv'] = 0.3845
# result['days_available'] = 127
# result['iv_min'] = 0.2850
# result['iv_max'] = 0.4100
```

## Common Patterns

### Single Ticker IV Rank
```python
result = calculate_iv_rank("TSLA")
if result['iv_rank'] is not None:
    print(f"TSLA IV Rank: {result['iv_rank']:.1%}")
```

### All Watchlist, Ranked
```python
from heartbeat.monitor import WATCHLIST, calculate_iv_rank

results = {}
for ticker in WATCHLIST:
    results[ticker] = calculate_iv_rank(ticker)

# Sort high→low
ranked = sorted(
    [(t, r) for t, r in results.items() if r['iv_rank'] is not None],
    key=lambda x: x[1]['iv_rank'],
    reverse=True
)
for ticker, result in ranked:
    print(f"{ticker:6} {result['iv_rank']:.1%}")
```

### Check Data Maturity
```python
result = calculate_iv_rank("AAPL")

if result['method'] == 'full_history':
    print(f"✓ 52-week: {result['iv_min']:.2%}–{result['iv_max']:.2%}")
elif result['method'] == 'snapshot_relative':
    print(f"⚠ {result['days_available']} days; fallback active")
else:
    print(f"✗ {result['error']}")
```

### Use Cached IV (don't refetch from Alpaca)
```python
cached_iv = 0.38  # from live snapshot
result = calculate_iv_rank("AAPL", current_iv=cached_iv)
# Uses cached IV, only queries historical data
```

### Batch Query with Error Filtering
```python
results = {t: calculate_iv_rank(t) for t in WATCHLIST}
valid = {t: r for t, r in results.items() if r['method'] == 'full_history'}
print(f"Full data available: {len(valid)}/{len(WATCHLIST)}")
```

## Return Dict Structure

### Full History Case
```python
{
    'iv_rank': 0.685,                  # 68.5th percentile
    'current_iv': 0.3845,               # Today's IV
    'method': 'full_history',           # Calculation method used
    'data_points': 127,                 # Records in database
    'days_available': 127,              # Trading days with data
    'iv_min': 0.2850,                   # 52-week low
    'iv_max': 0.4100,                   # 52-week high
}
```

### Snapshot-Relative Fallback
```python
{
    'iv_rank': 0.15,                    # 15% above average
    'current_iv': 0.3200,
    'method': 'snapshot_relative',      # <30 days available
    'data_points': 12,                  # Only 12 snapshots
    'days_available': 12,
    'average_iv': 0.2783,               # Average over available data
}
```

### Error Case
```python
{
    'iv_rank': None,                    # Failed to calculate
    'current_iv': None,
    'method': 'error',
    'data_points': 0,
    'days_available': 0,
    'error': 'No historical data'       # Error message
}
```

## Interpretation Guide

### IV Rank Percentile
| Rank Range | Interpretation | Vol Environment |
|-----------|-----------------|-----------------|
| 75-100% | Elevated IV | Extended vol, premium selling attractive |
| 50-75%  | Normal IV | Balanced; typical conditions |
| 25-50%  | Compressed IV | Low vol; look for volatility sellers |
| 0-25%   | Minimal IV | Historically tight; breakout risk |

### Method Reliability
| Method | Data Requirement | Use Case |
|--------|-----------------|----------|
| `full_history` | ≥30 days | Primary calculation; most reliable |
| `snapshot_relative` | <30 days | Early-stage; graceful degradation |
| `error` | Any | Failed fetch; data unavailable |

## Database Query Patterns

### Recent IV for ticker
```python
import sqlite3
from heartbeat.monitor import DB_PATH, db_lock

with db_lock:
    with sqlite3.connect(DB_PATH) as conn:
        c = conn.cursor()
        c.execute("""
            SELECT date, implied_volatility FROM historical_volatility
            WHERE ticker = 'NVDA'
            ORDER BY date DESC LIMIT 30
        """)
        for date, iv in c.fetchall():
            print(f"{date}: {iv:.4f}")
```

### IV range (min/max) for ticker
```python
with db_lock:
    with sqlite3.connect(DB_PATH) as conn:
        c = conn.cursor()
        c.execute("""
            SELECT MIN(implied_volatility), MAX(implied_volatility)
            FROM historical_volatility
            WHERE ticker = 'AAPL'
        """)
        min_iv, max_iv = c.fetchone()
        print(f"52-week range: {min_iv:.4f}–{max_iv:.4f}")
```

### All tickers on a specific date
```python
with db_lock:
    with sqlite3.connect(DB_PATH) as conn:
        c = conn.cursor()
        c.execute("""
            SELECT ticker, implied_volatility FROM historical_volatility
            WHERE date = '2024-06-25'
            ORDER BY ticker
        """)
        for ticker, iv in c.fetchall():
            print(f"{ticker:6} {iv:.4f}")
```

## Scheduler Integration

### Manually trigger EOD collection
```python
from heartbeat.monitor import collect_eod_iv_snapshots
collect_eod_iv_snapshots()
```

### Check if IV collection is scheduled
```python
import schedule
iv_jobs = [j for j in schedule.get_jobs() if 'iv_collection' in str(j.tag)]
if iv_jobs:
    print(f"✓ IV collection scheduled: {iv_jobs[0]}")
else:
    print("✗ IV collection not scheduled")
```

### View all scheduled jobs
```python
import schedule
for job in schedule.get_jobs():
    print(f"Tag: {job.tag}, Function: {job.job_func.func.__name__}")
```

## Common Errors & Fixes

### "No historical data"
- **Cause**: Database is empty; IV collection hasn't run yet
- **Fix**: Wait for first 16:30 ET collection, or manually call `collect_eod_iv_snapshots()`

### "Insufficient data for TICKER: only X days"
- **Cause**: <30 days of snapshots collected
- **Fix**: Wait until ≥30 trading days pass (6 weeks); snapshot-relative fallback active until then

### "Could not fetch current IV for TICKER"
- **Cause**: Alpaca request failed (429 rate limit, network issue, etc.)
- **Fix**: Retry later; check Alpaca API status; consider providing cached `current_iv`

### "database is locked"
- **Cause**: Concurrent writes without db_lock (should not occur)
- **Fix**: Always use `with db_lock:` for any write operation

## Performance Notes

- **Fetch time**: ~200ms per ticker, ~10s total for 9-ticker watchlist
- **DB insert**: ~1ms per record
- **Rank calculation**: ~2ms (full history) or ~1ms (fallback)
- **No blocking**: All operations timeout at 10s (HTTP_TIMEOUT)

## Related Documentation

- [IV_SYSTEM_GUIDE.md](IV_SYSTEM_GUIDE.md) — Full system architecture
- [IV_IMPLEMENTATION_SUMMARY.md](IV_IMPLEMENTATION_SUMMARY.md) — Implementation details
- [iv_examples.py](iv_examples.py) — 6 runnable example patterns
