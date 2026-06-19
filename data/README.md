# Nox — Historical Data Directory

## Active Data File

**`spy_vix_daily_v2.csv`** — The canonical data file for all backtesting and walk-forward optimization.

| Property | Value |
|---|---|
| Source | Yahoo Finance via `yf.Ticker().history(auto_adjust=True)` |
| Columns | `Date, High, Low, Close, Volume, VIX_Close` |
| Date Range | 1993-01-29 → present (updated manually) |
| Rows | ~8,400+ trading days |
| Adjustment | Fully auto-adjusted OHLC (splits and dividends) |
| VIX Source | `^VIX` via Yahoo Finance, forward-filled to SPY trading days |

### Column Notes
- `High` / `Low` / `Close` — SPY adjusted prices (auto_adjust=True). High ≥ Close ≥ Low is enforced.
- `Volume` — SPY daily volume (unadjusted share count)
- `VIX_Close` — CBOE VIX closing value for that trading day

---

## How to Refresh the Data

### Automated Refresh (recommended)

A one-time cron setup runs the refresh every weekday evening after market close:

```bash
# Install the cron job (runs once at 01:00 UTC Tue–Sat ≈ 21:00 ET Mon–Fri)
./refresh_cron.sh --install

# Verify it's registered
./refresh_cron.sh --status

# Remove it if no longer needed
./refresh_cron.sh --remove
```

### Manual Refresh

```bash
# Smart refresh — does nothing if the file is already current for today
./refresh_data.sh

# Force a full re-download even if the file looks fresh
./refresh_data.sh --force

# Skip creating the timestamped .bak backup
./refresh_data.sh --no-backup
```

All output is logged to `logs/refresh.log`.

### Staleness Logic

`refresh_data.sh` reads the last `Date` in the CSV and compares it to the
most-recent expected trading day.  If the dates match, the run is a no-op
(exit 0).  A download is only triggered when data is genuinely stale, which
makes the script safe to call from cron or pre-backtest hooks without burning
unneeded Yahoo Finance requests.

**Exit codes:**

| Code | Meaning |
|---|---|
| `0` | Data refreshed successfully, or already current |
| `1` | Download or validation failure — existing file preserved |
| `3` | Python 3 / yfinance / pandas not found |

### Safety Features

- **Backup before overwrite** — the existing CSV is copied to
  `spy_vix_daily_v2.YYYYMMDD_HHMMSS.bak` before any new data is written.
- **Row count guard** — if Yahoo returns fewer than 7,500 rows (truncated pull),
  the script aborts and preserves the existing file.
- **High ≥ Close ≥ Low enforcement** — float rounding artifacts are corrected
  before writing.
- **Idempotent** — safe to run from multiple callers simultaneously; the Python
  downloader writes atomically to the final path only after all validation passes.

---

## Walk-Forward Date Splits

| Phase | Start | End |
|---|---|---|
| In-Sample (IS) | 1998-01-01 | 2015-12-31 |
| Out-of-Sample (OOS) | 2016-01-01 | Present |

> The IS period starts in 1998 (not 1993) because the 200-day SMA requires ~10 months of warmup data.
> Data from 1993–1997 is used implicitly for SMA/ATR warmup but not for IS optimization scoring.

---

## Walk-Forward Winner (Last Validated: 2025)

**Candidate #3 selected** — highest OOS Sharpe and most OOS trades among top-3 IS winners.

| Parameter | Value |
|---|---|
| VIX Threshold | 35.0 |
| SMA Buffer | 0.98 |
| RSI Gate (floor) | 40 |
| Cooldown Days | 10 |
| Volume Gate | 1.0× avg |
| OOS Sharpe | 0.843 |
| OOS Win Rate | 68.42% |
| OOS Win/Loss Ratio | 2.316 |
| OOS Trades | 19 |
| OOS Total Return | 165.58% |

Kelly parameters derived from these OOS results:
```
KELLY_WIN_RATE=0.6842
KELLY_WIN_LOSS_RATIO=2.316
```
