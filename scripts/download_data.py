"""
download_data.py — Nox historical data refresh utility
=======================================================
Downloads SPY + VIX daily OHLCV history from Yahoo Finance and writes the
canonical data file used by the backtest engine.

Exit codes
----------
  0  — File written successfully (or skipped because already fresh)
  1  — Data download or validation failure
  2  — File is already up-to-date and --force was not passed

Usage
-----
  python download_data.py                   # skip if already fresh today
  python download_data.py --force           # always overwrite
  python download_data.py --out /some/path  # custom output path
  python download_data.py --no-backup       # skip backup creation
"""

import argparse
import os
import shutil
import sys
from datetime import date, datetime, timedelta
from pathlib import Path

import pandas as pd
import yfinance as yf

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
DEFAULT_OUT = Path(__file__).parent / "data" / "spy_vix_daily_v2.csv"
SPY_START   = "1993-01-01"
MIN_ROWS    = 7_500   # Fewer than this almost certainly means a corrupt pull


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _last_csv_date(path: Path) -> date | None:
    """Return the most-recent date in the CSV, or None if unreadable."""
    try:
        # Read only the Date column — fast even for 8 k-row files.
        df = pd.read_csv(path, usecols=["Date"], parse_dates=["Date"])
        if df.empty:
            return None
        return df["Date"].max().date()
    except Exception:
        return None


def _last_trading_day() -> date:
    """Return today if it's a weekday after 20:00 ET, otherwise the previous trading day.
    This is a conservative heuristic — it does NOT account for market holidays,
    so on holidays the script will attempt a pull and get the prior close, which
    is still the correct most-recent data.
    """
    now_et = datetime.utcnow() - timedelta(hours=4)  # rough ET (ignores DST edge)
    today  = now_et.date()

    # Roll back from Saturday(5) and Sunday(6) to Friday
    weekday = today.weekday()            # 0=Mon … 6=Sun
    if weekday == 5:                     # Saturday → Friday
        today -= timedelta(days=1)
    elif weekday == 6:                   # Sunday → Friday
        today -= timedelta(days=2)

    # If it's a weekday but before 20:00 ET, Yahoo may not have today's close yet.
    # In that case use yesterday (which also handles the Friday-before-close edge).
    if now_et.weekday() < 5 and now_et.hour < 20:
        today -= timedelta(days=1)
        # Roll back again if we landed on a weekend
        if today.weekday() == 5:
            today -= timedelta(days=1)
        elif today.weekday() == 6:
            today -= timedelta(days=2)

    return today


def _is_fresh(path: Path) -> bool:
    """Return True if the CSV already contains the most-recent expected close."""
    if not path.exists():
        return False
    last = _last_csv_date(path)
    if last is None:
        return False
    expected = _last_trading_day()
    return last >= expected


def _backup(path: Path) -> Path | None:
    """Copy path → path.YYYYMMDD_HHMMSS.bak and return the backup path."""
    if not path.exists():
        return None
    ts     = datetime.utcnow().strftime("%Y%m%d_%H%M%S")
    backup = path.with_suffix(f".{ts}.bak")
    shutil.copy2(path, backup)
    return backup


# ---------------------------------------------------------------------------
# Core download logic
# ---------------------------------------------------------------------------
def download(out_path: Path, *, force: bool, no_backup: bool) -> int:
    """
    Download SPY + VIX, validate, and write the CSV.

    Returns
    -------
    int — exit code (0 = success, 1 = error, 2 = already fresh)
    """
    out_path = out_path.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # ------------------------------------------------------------------
    # Freshness check
    # ------------------------------------------------------------------
    if not force and _is_fresh(out_path):
        last      = _last_csv_date(out_path)
        expected  = _last_trading_day()
        print(f"[REFRESH] Data is already current. "
              f"CSV last date: {last}  |  Expected: {expected}")
        print("[REFRESH] Pass --force to overwrite anyway.")
        return 2

    # ------------------------------------------------------------------
    # Backup existing file before overwriting
    # ------------------------------------------------------------------
    if out_path.exists() and not no_backup:
        bak = _backup(out_path)
        print(f"[REFRESH] Backup created: {bak}")

    # ------------------------------------------------------------------
    # Download
    # ------------------------------------------------------------------
    print("[REFRESH] Downloading SPY...")
    try:
        spy = yf.Ticker("SPY").history(start=SPY_START, auto_adjust=True)
    except Exception as exc:
        print(f"[REFRESH] [ERROR] SPY download failed: {exc}", file=sys.stderr)
        return 1

    print("[REFRESH] Downloading VIX...")
    try:
        vix = yf.Ticker("^VIX").history(start=SPY_START, auto_adjust=True)
    except Exception as exc:
        print(f"[REFRESH] [ERROR] VIX download failed: {exc}", file=sys.stderr)
        return 1

    if spy.empty:
        print("[REFRESH] [ERROR] SPY DataFrame is empty — aborting.", file=sys.stderr)
        return 1
    if vix.empty:
        print("[REFRESH] [ERROR] VIX DataFrame is empty — aborting.", file=sys.stderr)
        return 1

    # ------------------------------------------------------------------
    # Build combined DataFrame
    # ------------------------------------------------------------------
    spy.index = spy.index.tz_localize(None)
    vix.index = vix.index.tz_localize(None)

    vix_close = vix["Close"].reindex(spy.index).ffill()

    df = pd.DataFrame({
        "Date":      spy.index.strftime("%Y-%m-%d"),
        "High":      spy["High"].round(6),
        "Low":       spy["Low"].round(6),
        "Close":     spy["Close"].round(6),
        "Volume":    spy["Volume"].astype(int),
        "VIX_Close": vix_close.round(4),
    })

    df.dropna(inplace=True)

    # Enforce High >= Close >= Low (guards against float rounding edge cases)
    df["High"] = df[["High", "Close"]].max(axis=1)
    df["Low"]  = df[["Low",  "Close"]].min(axis=1)

    # ------------------------------------------------------------------
    # Validation
    # ------------------------------------------------------------------
    if len(df) < MIN_ROWS:
        print(f"[REFRESH] [ERROR] Only {len(df)} rows returned — expected at least {MIN_ROWS}. "
              "Aborting to protect existing file.", file=sys.stderr)
        return 1

    bad_high = df[df["High"] < df["Close"]]
    if not bad_high.empty:
        print(f"[REFRESH] [WARN] {len(bad_high)} rows still have High < Close after fix — "
              "investigate yfinance output.")

    bad_vix = df[df["VIX_Close"] <= 0]
    if not bad_vix.empty:
        print(f"[REFRESH] [WARN] {len(bad_vix)} rows have VIX_Close ≤ 0.")

    # ------------------------------------------------------------------
    # Write
    # ------------------------------------------------------------------
    df.to_csv(out_path, index=False)

    last_date = df["Date"].iloc[-1]
    print(f"[REFRESH] Done. {len(df)} rows written to {out_path}")
    print(f"[REFRESH] Date range: {df['Date'].iloc[0]}  →  {last_date}")
    print(f"[REFRESH] Sanity — High < Close rows: {len(bad_high)} (should be 0)")
    return 0


# ---------------------------------------------------------------------------
# CLI entry-point
# ---------------------------------------------------------------------------
def main() -> None:
    parser = argparse.ArgumentParser(
        description="Download SPY + VIX history and write spy_vix_daily_v2.csv"
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite even if the file is already current.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"Output CSV path (default: {DEFAULT_OUT})",
    )
    parser.add_argument(
        "--no-backup",
        action="store_true",
        help="Skip creating a timestamped .bak of the existing file.",
    )
    args = parser.parse_args()

    code = download(args.out, force=args.force, no_backup=args.no_backup)
    sys.exit(code)


if __name__ == "__main__":
    main()
