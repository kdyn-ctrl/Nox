"""
CELH search-sentiment signal tracker — date-triggered, monitoring-only.
NOT wired into execution/heartbeat sizing or gating (RULE-D1 research pilot,
same discipline as every other module in this file's family).

Why this exists: the 2026-07-18 consumer search-sentiment pilot
(research/consumer_search_signal.py) ran a pre-specified 6-ticker basket.
5 of 6 showed the overfit-divergence pattern (IS/OOS correlation sign flip
or sharp weakening) — no signal. CELH was the one exception: IS r=+0.24 ->
OOS r=+0.55, same sign, strengthening, on good data coverage. Per
feedback_overfitting_aware_edge_search, that does NOT clear the bar to
trade it — it clears the bar to keep tracking that SAME relationship
forward on genuinely NEW data (not re-searching terms/tickers/windows to
rescue anything). This module does exactly that and nothing else.

Fires exactly twice, ever: 90 days and 180 days after the 2026-07-18
baseline (2026-10-16, 2027-01-15). Each firing re-runs the IDENTICAL
methodology from consumer_search_signal.py (same 3 CELH terms, same fixed
IS window, same EMA smoothing/velocity-squash constants, same forward-
return correlation) with the OOS window extended through today, and
reports whether the OOS correlation held up, weakened, or flipped versus
the 2026-07-18 baseline reading. A DB row per checkpoint (UNIQUE on label)
is the dedup mechanism — once fired, never refires, mirroring
prediction_outcome_resolver.py's checkpoint-row-as-flag pattern.

NO_EXECUTION: this module places no orders, computes no position size, and
sends no signal to any execution path. It sends a Telegram report only.

Run manually or import run_daily_check() from monitor.py's scheduled job:
    python3 celh_signal_tracker.py
"""
import json
import math
import os
import sqlite3
import sys
import time
import urllib.request
from datetime import date, datetime, timedelta, timezone

try:
    import pandas as pd
except ImportError:
    pd = None

try:
    from pytrends.request import TrendReq
except ImportError:
    TrendReq = None

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")

# Anchor date + terms are fixed research facts, not tunable knobs (same
# precedent as consumer_search_signal.py's own IS_WINDOW/OOS_WINDOW) — no
# RULE-D11 env passthrough needed.
BASELINE_DATE = date(2026, 7, 18)
CHECKPOINTS = (("90d", 90), ("180d", 180))
TERMS = ["celsius flavors", "celsius energy drink", "celsius caffeine"]
IS_WINDOW = ("2022-01-01", "2024-12-31")
OOS_START = "2025-01-01"
FORWARD_WEEKS = 26
BASELINE_IS_R = 0.24
BASELINE_OOS_R = 0.55


def _connect():
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS celh_search_signal_checkpoints ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "checkpoint_label TEXT NOT NULL UNIQUE, "
        "target_date TEXT NOT NULL, "
        "fired_at TEXT NOT NULL, "
        "is_r REAL, is_n INTEGER, "
        "oos_r REAL, oos_n INTEGER, "
        "price_change_pct REAL, "
        "report_text TEXT NOT NULL)"
    )
    return conn


def _send_telegram(msg: str) -> None:
    token = os.getenv("TELEGRAM_BOT_TOKEN", "")
    chat_id = os.getenv("TELEGRAM_CHAT_ID", "")
    if not token or not chat_id:
        print("[WARN] TELEGRAM_BOT_TOKEN/TELEGRAM_CHAT_ID unset — cannot send "
              "CELH checkpoint report.", file=sys.stderr)
        return
    try:
        import requests
        requests.post(
            f"https://api.telegram.org/bot{token}/sendMessage",
            json={"chat_id": chat_id, "text": msg, "parse_mode": "Markdown"},
            timeout=10,
        )
    except Exception as e:
        print(f"[ERROR] CELH checkpoint Telegram send failed: {e}", file=sys.stderr)


def _fetch_composite(terms, start_date, end_date):
    client = TrendReq(hl="en-US", tz=360)
    timeframe = f"{start_date} {end_date}"
    df = pd.DataFrame()
    for attempt in range(3):
        try:
            client.build_payload(terms, timeframe=timeframe, geo="US")
            df = client.interest_over_time()
            break
        except Exception as e:
            if attempt == 2:
                print(f"[ERROR] Trends fetch failed for {terms}: {e}", file=sys.stderr)
                return pd.Series(dtype=float)
            time.sleep(3.0 * (attempt + 1))
    if df.empty:
        return pd.Series(dtype=float)
    cols = [c for c in terms if c in df.columns]
    return df[cols].mean(axis=1)


def _smooth(series, span=8):
    if series.empty:
        return series
    return series.ewm(span=span, adjust=False).mean()


def _velocity_score(smoothed, lookback_weeks=4):
    if len(smoothed) <= lookback_weeks:
        return pd.Series(dtype=float)
    pct_change = smoothed.pct_change(lookback_weeks)
    k = 2.0
    return pct_change.apply(lambda x: 1.0 / (1.0 + math.exp(-k * x)) if pd.notna(x) else float("nan"))


def _fetch_price_bars(ticker="CELH", rng="5y"):
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{ticker}?range={rng}&interval=1d"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    raw = json.load(urllib.request.urlopen(req, timeout=20))
    result = raw["chart"]["result"][0]
    ts = result["timestamp"]
    q = result["indicators"]["quote"][0]
    bars = [{"date": time.strftime("%Y-%m-%d", time.gmtime(ts[i])), "close": q["close"][i]}
            for i in range(len(ts)) if q["close"][i] is not None]
    s = pd.Series({b["date"]: b["close"] for b in bars})
    s.index = pd.to_datetime(s.index)
    return s.sort_index()


def _forward_return_series(prices, weeks_ahead):
    weekly = prices.resample("W").last().ffill()
    return weekly.shift(-weeks_ahead) / weekly - 1.0


def _correlate(score, fwd_ret, window_start, window_end, data_start):
    win_start = max(pd.Timestamp(window_start), data_start)
    win_end = pd.Timestamp(window_end)
    if win_start >= win_end:
        return None, 0
    idx = score.index.intersection(fwd_ret.dropna().index)
    idx = idx[(idx >= win_start) & (idx <= win_end)]
    pairs = pd.DataFrame({"score": score.reindex(idx), "fwd": fwd_ret.reindex(idx)}).dropna()
    if len(pairs) < 5:
        return None, len(pairs)
    return pairs["score"].corr(pairs["fwd"]), len(pairs)


def _run_analysis(today_str):
    if TrendReq is None or pd is None:
        return None, "pytrends/pandas not available in this container"

    raw = _fetch_composite(TERMS, IS_WINDOW[0], today_str)
    if raw.empty:
        return None, "no Google Trends data returned for CELH terms"
    score = _velocity_score(_smooth(raw))

    try:
        prices = _fetch_price_bars("CELH")
    except Exception as e:
        return None, f"price fetch failed: {e}"
    fwd_ret = _forward_return_series(prices, FORWARD_WEEKS)
    data_start = prices.index.min()

    is_r, is_n = _correlate(score, fwd_ret, IS_WINDOW[0], IS_WINDOW[1], data_start)
    oos_r, oos_n = _correlate(score, fwd_ret, OOS_START, today_str, data_start)

    baseline_cut = prices[prices.index <= pd.Timestamp(BASELINE_DATE.isoformat())]
    price_change_pct = None
    if not baseline_cut.empty:
        price_change_pct = (prices.iloc[-1] / baseline_cut.iloc[-1] - 1.0) * 100.0

    return {
        "is_r": is_r, "is_n": is_n, "oos_r": oos_r, "oos_n": oos_n,
        "price_change_pct": price_change_pct,
    }, None


def _format_report(label, target_date, result):
    def fmt(r, n):
        return f"r={r:+.3f} (n={n})" if r is not None else "insufficient data"

    if result["oos_r"] is None:
        held = "n/a (insufficient OOS data to compare)"
    elif (result["oos_r"] > 0) != (BASELINE_OOS_R > 0):
        held = "SIGN FLIPPED vs baseline — treat the baseline reading as noise, not a real signal"
    else:
        ratio = abs(result["oos_r"]) / abs(BASELINE_OOS_R)
        held = (f"HOLDING UP ({ratio:.2f}x baseline OOS r)" if ratio >= 0.5
                else f"WEAKENING ({ratio:.2f}x baseline OOS r)")

    price_line = (f"CELH price since 2026-07-18 baseline: {result['price_change_pct']:+.1f}%"
                  if result["price_change_pct"] is not None else "CELH price: unavailable")

    return (
        f"📊 *CELH Search-Signal Checkpoint — {label} ({target_date})*\n\n"
        f"Baseline (2026-07-18 pilot): IS r=+{BASELINE_IS_R:.2f}, OOS r=+{BASELINE_OOS_R:.2f}\n"
        f"Now: IS {fmt(result['is_r'], result['is_n'])}, OOS {fmt(result['oos_r'], result['oos_n'])}\n"
        f"Status: {held}\n"
        f"{price_line}\n\n"
        f"Monitoring only — no sizing, no gating, no execution. "
        f"{'Final checkpoint — decide next steps now.' if label == '180d' else 'Next checkpoint at 180 days.'}"
    )


def _today():
    return datetime.now(timezone.utc).date()


def run_daily_check():
    conn = _connect()
    today = _today()
    for label, days in CHECKPOINTS:
        target = BASELINE_DATE + timedelta(days=days)
        if today < target:
            continue
        already = conn.execute(
            "SELECT 1 FROM celh_search_signal_checkpoints WHERE checkpoint_label=?", (label,)
        ).fetchone()
        if already:
            continue
        result, err = _run_analysis(today.isoformat())
        if err:
            print(f"[WARN] CELH checkpoint {label} could not run ({err}) — will retry next scan.",
                  file=sys.stderr)
            continue
        report = _format_report(label, target.isoformat(), result)
        conn.execute(
            "INSERT INTO celh_search_signal_checkpoints "
            "(checkpoint_label, target_date, fired_at, is_r, is_n, oos_r, oos_n, "
            "price_change_pct, report_text) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (label, target.isoformat(), datetime.now(timezone.utc).isoformat(),
             result["is_r"], result["is_n"], result["oos_r"], result["oos_n"],
             result["price_change_pct"], report),
        )
        conn.commit()
        _send_telegram(report)
    conn.close()


if __name__ == "__main__":
    run_daily_check()
