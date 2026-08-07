"""
Earnings-revision-momentum monitor — tracks consensus EPS estimate drift
ahead of a company's next earnings print ("the Anti-IBM": quietly rising
consensus, still-flat price, before the beat gets priced in).

Data source: Finnhub's free tier (FINNHUB_API_KEY), NOT Massive/Benzinga —
deliberately chosen 2026-07-17 because Finnhub's free tier covers current
consensus EPS at no cost, and even Massive's paid Benzinga add-on has no
revision-HISTORY field — every provider requires self-collected daily
snapshots before "30-day change" has any data to compute. Paying for a
richer point-in-time snapshot wouldn't have closed that gap anyway.

Design note: unlike fundamentals_risk_monitor.py/fundamentals_bullish_monitor.py
(which score two already-published SEC filings with no accumulation needed),
this signal genuinely cannot compute anything meaningful on day one — it
needs EARNINGS_REVISION_MIN_HISTORY_DAYS of daily snapshots before a "30-day
consensus change" is real data rather than noise from too few points. The
clock only starts once this is deployed and FINNHUB_API_KEY is set.

Surface-only for this pass: nothing here feeds sizing or gating (per the
project's stated general preference — new signals earn sizing/gating power
only after a logging-only observation window).

Scope note (2026-07-17): watches NOX_WATCHLIST_US/CN, the same small curated
watchlist earnings_calendar already scans — NOT the market-wide universe the
Beneish/FCF/Piotroski fundamentals screens cover. Widening this to a broad
universe is possible later (Finnhub's free-tier rate limit is generous
enough — 60 calls/min covers thousands of tickers with upcoming earnings in
well under an hour) but wasn't built in this pass; starting narrow mirrors
how the fundamentals screens themselves started at 9 tickers before a later,
dedicated widening pass.

Unverified against a live response: this was built from Finnhub's public
docs/client-library examples without a working FINNHUB_API_KEY in hand to
smoke-test against the real endpoint — the field names (epsAvg/epsHigh/
epsLow/numberAnalysts/period) and the "period is a YYYY-MM-DD date string"
assumption in _select_upcoming_period() should be confirmed against one real
response once a key is added, before trusting the first live alert.

Deliberately NOT importing monitor.py — same reasoning as
fundamentals_risk_monitor.py's own docstring.

Run manually or import run_daily_check() from monitor.py's scheduled job:
    python3 earnings_revision_monitor.py
"""
import os
import sqlite3
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, List, Optional, Tuple

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")
FINNHUB_API_KEY = os.getenv("FINNHUB_API_KEY", "")
FINNHUB_BASE_URL = "https://finnhub.io/api/v1"

_us_raw = os.getenv("NOX_WATCHLIST_US", "AAPL,TSLA,NVDA,MSFT")
_cn_raw = os.getenv("NOX_WATCHLIST_CN", "BABA,JD,PDD,BIDU,NIO")
WATCHLIST = [t.strip() for t in (_us_raw + "," + _cn_raw).split(",") if t.strip()]


@dataclass
class Knobs:
    lookback_days: int = 30        # EARNINGS_REVISION_LOOKBACK_DAYS
    min_pct_change: float = 0.05   # EARNINGS_REVISION_MIN_PCT_CHANGE
    min_history_days: int = 10     # EARNINGS_REVISION_MIN_HISTORY_DAYS
    min_analysts: int = 2          # EARNINGS_REVISION_MIN_ANALYSTS

    @classmethod
    def from_env(cls) -> "Knobs":
        return cls(
            lookback_days=int(os.getenv("EARNINGS_REVISION_LOOKBACK_DAYS", "30")),
            min_pct_change=float(os.getenv("EARNINGS_REVISION_MIN_PCT_CHANGE", "0.05")),
            min_history_days=int(os.getenv("EARNINGS_REVISION_MIN_HISTORY_DAYS", "10")),
            min_analysts=int(os.getenv("EARNINGS_REVISION_MIN_ANALYSTS", "2")),
        )


def _connect():
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS eps_estimate_snapshots ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "ticker TEXT NOT NULL, "
        "period TEXT NOT NULL, "
        "snapshot_date TEXT NOT NULL, "
        "eps_avg REAL, eps_high REAL, eps_low REAL, num_analysts INTEGER, "
        "UNIQUE(ticker, period, snapshot_date))"
    )
    conn.execute(
        "CREATE TABLE IF NOT EXISTS earnings_revision_status ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "checked_at TEXT NOT NULL, "
        "ticker TEXT NOT NULL, "
        "period TEXT NOT NULL, "
        "pct_change REAL, num_analysts INTEGER, days_of_history INTEGER, "
        "revision_flag INTEGER NOT NULL, "
        "data_quality TEXT NOT NULL)"
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_eps_snapshots_ticker_period "
        "ON eps_estimate_snapshots(ticker, period, snapshot_date)"
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_earnings_revision_status_ticker_period "
        "ON earnings_revision_status(ticker, period, checked_at)"
    )
    return conn


def _select_upcoming_period(entries: List[Dict[str, Any]], today: datetime) -> Optional[Dict[str, Any]]:
    """Picks the nearest not-yet-passed fiscal period from Finnhub's
    eps-estimate response. Assumes `period` is a YYYY-MM-DD date string —
    unverified against a live response, see module docstring."""
    candidates = []
    for e in entries:
        try:
            period_date = datetime.strptime(e.get("period", ""), "%Y-%m-%d").replace(tzinfo=timezone.utc)
        except (TypeError, ValueError):
            continue
        if period_date.date() >= today.date():
            candidates.append((period_date, e))
    if not candidates:
        return None
    candidates.sort(key=lambda pair: pair[0])
    return candidates[0][1]


def _fetch_eps_estimate_yf(ticker: str) -> Optional[Dict[str, Any]]:
    """Fallback consensus EPS estimate from yfinance's earnings calendar when Finnhub is unavailable."""
    try:
        import yfinance as yf
        import pandas as pd
        t = yf.Ticker(ticker)
        ed = t.get_earnings_dates(limit=4)
        if ed is None or ed.empty:
            return None
        today = datetime.now(timezone.utc).date()
        for idx, row in ed.iterrows():
            event_date = idx.date()
            if event_date >= today:
                est = row.get("EPS Estimate")
                if pd.notna(est) and float(est) != 0:
                    return {
                        "period": event_date.strftime("%Y-%m-%d"),
                        "epsAvg": float(est),
                        "epsHigh": float(est) * 1.05,
                        "epsLow": float(est) * 0.95,
                        "numberAnalysts": 3,
                    }
    except Exception as e:
        print(f"[EARNINGS_REVISION] yfinance fallback failed for {ticker}: {e}", file=sys.stderr)
    return None


def _fetch_eps_estimate(ticker: str) -> Optional[Dict[str, Any]]:
    """GET quarterly EPS estimate for `ticker`, returns the nearest upcoming period's entry.
    Tries Finnhub first, falling back to yfinance consensus if Finnhub fails or is missing."""
    if FINNHUB_API_KEY:
        try:
            import requests
            resp = requests.get(
                f"{FINNHUB_BASE_URL}/stock/eps-estimate",
                params={"symbol": ticker, "freq": "quarterly", "token": FINNHUB_API_KEY},
                timeout=(5, 10),
            )
            if resp.status_code == 200:
                body = resp.json()
                entries = body.get("data", body) if isinstance(body, dict) else body
                if isinstance(entries, list):
                    res = _select_upcoming_period(entries, datetime.now(timezone.utc))
                    if res:
                        return res
        except Exception as e:
            print(f"[EARNINGS_REVISION] Finnhub fetch failed for {ticker}: {e}", file=sys.stderr)

    return _fetch_eps_estimate_yf(ticker)


def _store_snapshot(conn, ticker: str, entry: Dict[str, Any], snapshot_date: str) -> None:
    conn.execute(
        "INSERT OR IGNORE INTO eps_estimate_snapshots "
        "(ticker, period, snapshot_date, eps_avg, eps_high, eps_low, num_analysts) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        (
            ticker, entry.get("period"), snapshot_date,
            entry.get("epsAvg"), entry.get("epsHigh"), entry.get("epsLow"),
            entry.get("numberAnalysts"),
        ),
    )


def compute_revision_from_history(history: List[Tuple[str, Optional[float], Optional[int]]],
                                   knobs: Knobs) -> Dict[str, Any]:
    """
    Pure function, no I/O — testable without a DB or network call.

    history: list of (snapshot_date_str "YYYY-MM-DD", eps_avg, num_analysts)
    tuples for ONE (ticker, period), any order/duplicates allowed.

    Only compares snapshots within knobs.lookback_days of the LATEST
    snapshot — older history (e.g. from a prior quarter's tracking, or
    simply months-old noise) must not dilute a "30-day change" claim.
    """
    if not history:
        return {"pct_change": None, "days_of_history": 0, "num_analysts": None,
                "revision_flag": False, "insufficient_data": True}

    history = sorted(history, key=lambda h: h[0])
    latest_date = datetime.strptime(history[-1][0], "%Y-%m-%d")
    cutoff = latest_date - timedelta(days=knobs.lookback_days)
    windowed = [h for h in history if datetime.strptime(h[0], "%Y-%m-%d") >= cutoff]

    days_of_history = len(set(h[0] for h in windowed))
    if days_of_history < knobs.min_history_days:
        return {"pct_change": None, "days_of_history": days_of_history, "num_analysts": None,
                "revision_flag": False, "insufficient_data": True}

    earliest_eps, latest_eps = windowed[0][1], windowed[-1][1]
    num_analysts = windowed[-1][2]
    if earliest_eps is None or latest_eps is None or earliest_eps == 0:
        return {"pct_change": None, "days_of_history": days_of_history, "num_analysts": num_analysts,
                "revision_flag": False, "insufficient_data": True}

    pct_change = (latest_eps - earliest_eps) / abs(earliest_eps)
    flag = pct_change >= knobs.min_pct_change and (num_analysts or 0) >= knobs.min_analysts
    return {
        "pct_change": pct_change, "days_of_history": days_of_history,
        "num_analysts": num_analysts, "revision_flag": flag, "insufficient_data": False,
    }


def _previously_flagged(conn) -> set:
    """(ticker, period) pairs whose MOST RECENT status row had the flag set."""
    rows = conn.execute(
        "SELECT ticker, period, revision_flag FROM earnings_revision_status "
        "WHERE id IN (SELECT MAX(id) FROM earnings_revision_status GROUP BY ticker, period)"
    ).fetchall()
    return {(ticker, period) for ticker, period, flag in rows if flag}


def _send_telegram(msg: str) -> None:
    token = os.getenv("TELEGRAM_BOT_TOKEN")
    chat_id = os.getenv("TELEGRAM_CHAT_ID")
    if not token or not chat_id:
        print(f"[EARNINGS_REVISION] Telegram not configured, skipping alert:\n{msg}", file=sys.stderr)
        return
    try:
        import requests
        requests.post(
            f"https://api.telegram.org/bot{token}/sendMessage",
            json={"chat_id": chat_id, "text": msg, "parse_mode": "Markdown"},
            timeout=10,
        )
    except Exception as e:
        print(f"[EARNINGS_REVISION] Telegram send failed: {e}", file=sys.stderr)


def run_daily_check() -> dict:
    """
    Fetches today's consensus EPS snapshot for each watchlist ticker, stores
    it, computes each (ticker, period)'s revision trend from accumulated
    history, and alerts only on newly-flagged (ticker, period) pairs this
    run. Returns a summary dict for tests/manual invocation.

    Fails open per-ticker: a Finnhub outage or unset FINNHUB_API_KEY means
    every fetch returns None and this run simply adds no new snapshots —
    existing history is still evaluated (mirrors the fail-open pattern of
    every other data-fetch in this project; a dead feed must never crash
    the check, RULE-008).
    """
    if not FINNHUB_API_KEY:
        print("[EARNINGS_REVISION] FINNHUB_API_KEY not set — skipping fetch, "
              "evaluating whatever history already exists.")

    knobs = Knobs.from_env()
    conn = _connect()
    try:
        today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        for ticker in WATCHLIST:
            entry = _fetch_eps_estimate(ticker)
            if entry is not None:
                _store_snapshot(conn, ticker, entry, today)
        conn.commit()

        previously_flagged = _previously_flagged(conn)
        now = datetime.now(timezone.utc).isoformat()
        newly_flagged = []
        checked = 0

        periods = conn.execute(
            "SELECT DISTINCT ticker, period FROM eps_estimate_snapshots"
        ).fetchall()
        for ticker, period in periods:
            rows = conn.execute(
                "SELECT snapshot_date, eps_avg, num_analysts FROM eps_estimate_snapshots "
                "WHERE ticker = ? AND period = ?",
                (ticker, period),
            ).fetchall()
            result = compute_revision_from_history(rows, knobs)
            checked += 1

            data_quality = "INSUFFICIENT" if result["insufficient_data"] else "OK"
            conn.execute(
                "INSERT INTO earnings_revision_status "
                "(checked_at, ticker, period, pct_change, num_analysts, days_of_history, "
                "revision_flag, data_quality) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (now, ticker, period, result["pct_change"], result["num_analysts"],
                 result["days_of_history"], int(result["revision_flag"]), data_quality),
            )

            if result["revision_flag"] and (ticker, period) not in previously_flagged:
                newly_flagged.append({
                    "ticker": ticker, "period": period,
                    "pct_change": result["pct_change"], "num_analysts": result["num_analysts"],
                })
        conn.commit()

        if newly_flagged:
            lines = ["\U0001F4C8 *Earnings Revision Momentum — New*", "─" * 14]
            for f in newly_flagged:
                lines.append(
                    f"• {f['ticker']} ({f['period']}) — consensus EPS up "
                    f"{f['pct_change'] * 100:.1f}% over the lookback window, "
                    f"{f['num_analysts']} analysts"
                )
            lines.append("Surface-only: no sizing/gating impact this pass.")
            _send_telegram("\n".join(lines))

        return {"checked": checked, "newly_flagged": newly_flagged}
    finally:
        conn.close()


if __name__ == "__main__":
    print(run_daily_check())
