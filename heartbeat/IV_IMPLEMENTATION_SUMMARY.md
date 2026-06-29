# Historical IV Dataset Implementation — Complete Summary

## ✅ What Was Implemented

### 1. Database Schema (historical_volatility table)
**Location**: `monitor.py:101-118` in `init_db()`

```sql
CREATE TABLE IF NOT EXISTS historical_volatility (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker TEXT NOT NULL,
    date DATE NOT NULL,
    implied_volatility REAL NOT NULL,
    snapshot_timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(ticker, date)
)
```

- **Unique constraint** on (ticker, date) prevents duplicate daily snapshots
- **Index** on (ticker, date) for fast historical lookups
- Date stored as YYYY-MM-DD for easy range queries

### 2. Scheduled Post-Market Collection (16:30 ET)
**Location**: `monitor.py:728-761` in `schedule_checker()`

- **Task**: `collect_eod_iv_snapshots()` (line 523)
- **Trigger**: Daily at 4:30 PM ET (30-min buffer after market close)
- **DST Handling**: Automatically recalculated nightly at 00:01 UTC
- **Tag**: "iv_collection" for introspection via `schedule.get_jobs()`

#### Scheduler Flow:
```
00:01 UTC ──> _combined_reschedule()
              ├─ _reschedule_scout()       [9:00 AM ET]
              └─ _reschedule_iv_collection() [4:30 PM ET]

16:30 ET ────> collect_eod_iv_snapshots()
  ├─ iterate WATCHLIST (9 tickers)
  ├─ fetch_options_chain_iv(ticker) for each
  └─ store_iv_snapshot(ticker, iv, date) with db_lock
```

### 3. IV Snapshot Fetcher
**Location**: `monitor.py:442-485` — `fetch_options_chain_iv(ticker)`

**Features**:
- Calls Alpaca `/v1beta3/options/chains/{ticker}` endpoint
- Calculates weighted-average IV across all liquid contracts
- **Weighting**: By open interest (reflects market consensus)
- **Error handling**: Returns None on fetch failure, logs exception
- **Logging**: Detailed context (contract count, total OI, calculated IV)

**Example Output**:
```
[INFO] [HEARTBEAT] Fetched IV for NVDA: 0.3845 (from 127 contracts, OI: 45782)
```

### 4. IV Snapshot Storage
**Location**: `monitor.py:488-521` — `store_iv_snapshot(ticker, iv, date_str)`

**Features**:
- **Thread-safe**: Uses existing `db_lock` (threading.Lock)
- **Upsert semantics**: INSERT OR REPLACE on (ticker, date)
- **Atomic**: Full transaction wrapped in lock
- **Error handling**: Logs failures, returns False on error

### 5. IV Rank Calculator
**Location**: `monitor.py:551-673` — `calculate_iv_rank(ticker, current_iv=None)`

#### Full History Method (≥30 days available):
```
IV Rank = (Current IV - Min IV) / (Max IV - Min IV)
Clamped to [0, 1] (represents percentile in 52-week range)
```

**Example**: IV Rank = 0.75 → current IV is at 75th percentile of recent range

#### Snapshot-Relative Fallback (<30 days available):
```
IV Rank = (Current IV - Average IV) / Average IV
Clamped to [0, 1]
WARNING logged with data point count
```

#### Return Dictionary:
```python
{
    'iv_rank': float | None,           # Percentile [0, 1]
    'current_iv': float,                # Current IV snapshot
    'method': 'full_history' | 'snapshot_relative' | 'error',
    'data_points': int,                 # Historical records queried
    'days_available': int,              # Distinct trading days
    'iv_min': float,                    # (full_history only)
    'iv_max': float,                    # (full_history only)
    'average_iv': float,                # (snapshot_relative only)
    'error': str                        # (error case only)
}
```

#### Logging:
```
# Full history success
[INFO] [HEARTBEAT] IV Rank for AAPL: 68.50% (Current=0.3200, Min=0.2850, Max=0.4100, Points=252)

# Fallback triggered
[WARNING] [HEARTBEAT] Insufficient data for TSLA: only 15 days (need 30+). Falling back to snapshot-relative calculation.

# Error case
[ERROR] [HEARTBEAT] Exception calculating IV Rank for BABA: HTTP 429 (rate limited)
```

## 📊 Codebase Integration

### New Imports
- `logging` — for detailed IV pipeline logging
- `timedelta` from `datetime` — future time-window queries

### New Global Logger
```python
logging.basicConfig(level=logging.INFO, format='[%(levelname)s] [HEARTBEAT] %(message)s')
logger = logging.getLogger(__name__)
```

### Thread Safety
All database operations use the existing `db_lock`:
```python
with db_lock:
    with sqlite3.connect(DB_PATH) as conn:
        # Read/write operations
```

### Scheduler Integration
- No conflicts with existing Scout (9:00 AM ET) or SEC Radar (continuous)
- Post-market timing (16:30 ET) creates natural separation
- Combined reschedule at 00:01 UTC handles both tasks

## 🎯 Usage Patterns

### Quick IV Rank Query
```python
from heartbeat.monitor import calculate_iv_rank

result = calculate_iv_rank("NVDA")
print(f"NVDA IV Rank: {result['iv_rank']:.2%}")
```

### Batch Query All Watchlist Tickers
```python
from heartbeat.monitor import calculate_iv_rank, WATCHLIST

results = {t: calculate_iv_rank(t) for t in WATCHLIST}
# Rank by IV Rank (highest first)
ranked = sorted(results.items(), key=lambda x: x[1]['iv_rank'], reverse=True)
```

### Check Method Reliability
```python
result = calculate_iv_rank("AAPL")
if result['method'] == 'full_history':
    print(f"✓ Full 52-week range: {result['iv_min']:.2%}–{result['iv_max']:.2%}")
elif result['method'] == 'snapshot_relative':
    print(f"⚠ Limited data ({result['days_available']} days)")
else:
    print(f"✗ Error: {result['error']}")
```

## 📁 Supporting Documentation

- **[IV_SYSTEM_GUIDE.md](IV_SYSTEM_GUIDE.md)** — Complete reference (components, logging, performance)
- **[iv_examples.py](iv_examples.py)** — Runnable example patterns (6 examples)

## 🚀 Data Accumulation Timeline

- **Day 1-29**: Snapshots collect; `calculate_iv_rank()` uses `snapshot_relative` fallback
  - Log: "⚠ Insufficient data for NVDA: only 12 days (need 30+)"
- **Day 30+**: Full 52-week calculation becomes available
  - Log: "✓ IV Rank for NVDA: 68.50% (Current=0.3200, Min=0.2850, Max=0.4100, Points=252)"
- **Day 252+**: Full trading-year of data available (~1 year)

## ✨ Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| 16:30 ET (4:30 PM) vs 16:00 ET | 30-min buffer ensures all options contracts have settled |
| Weight by open interest | Reflects market consensus; illiquid options have low weight |
| 30-day threshold for full rank | Ensures meaningful percentile range without excessive history |
| Graceful fallback to snapshot-relative | Allows IV Rank calculation even with <30 days |
| INSERT OR REPLACE semantics | Handles intraday recalculations without duplicate errors |
| db_lock on all DB operations | Prevents "database is locked" errors with Scout/SEC Radar |
| Nightly reschedule at 00:01 UTC | Automatic DST handling; no container restart needed |

## 📈 Next Steps for User

1. **Deploy & Run**: Heartbeat will automatically start collecting at 16:30 ET
2. **Monitor Logs**: Watch for "IV collection complete" messages at 16:30 ET
3. **Wait 30 Days**: Full IV Rank calculations unlock after 30 trading days
4. **Integrate**: Use `calculate_iv_rank()` in trading algorithms, alerts, or analysis pipelines
5. **Extend**: Build on IV Rank (e.g., IV percentile heatmaps, term structure, cross-market signals)

## 🔍 Debugging Tips

### Check if scheduler is running:
```python
import schedule
for job in schedule.get_jobs():
    print(job.tag, job.job_func.func.__name__)
```

### Force IV collection (for testing):
```python
from heartbeat.monitor import collect_eod_iv_snapshots
collect_eod_iv_snapshots()
```

### Query historical IV directly:
```python
import sqlite3
from heartbeat.monitor import DB_PATH, db_lock

with db_lock:
    with sqlite3.connect(DB_PATH) as conn:
        c = conn.cursor()
        c.execute("""
            SELECT date, implied_volatility FROM historical_volatility
            WHERE ticker = 'NVDA' ORDER BY date DESC LIMIT 10
        """)
        for row in c.fetchall():
            print(f"{row[0]}: {row[1]:.4f}")
```

### View IV collection logs:
```bash
docker logs heartbeat 2>&1 | grep -E "IV collection|Fetched IV|IV Rank"
```
