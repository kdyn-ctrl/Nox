"""
Bullish quality monitor — Piotroski F-Score, surface-only. Mirrors
fundamentals_risk_monitor.py (the bearish Beneish/FCF screen) 1:1: same
producer (america-data-engine's fundamentals scan), same universe, same
alert-on-newly-flagged transition logic, same "surface-only" contract.

Pulls the cached /risk/fundamentals-bullish scan and persists one row per
ticker per day into `fundamentals_bullish_status`, then alerts on Telegram
only for tickers NEWLY crossing into a high-quality flag this run (no flag
yesterday, flag today) — so a persistently-flagged compounder doesn't spam
daily.

This is deliberately surface-only: nothing here touches sizing or gating.
The user wants to validate the scores against real high-quality names first.

Deliberately NOT importing monitor.py — same reasoning as
fundamentals_risk_monitor.py's own docstring: that module's top-level code
hard-aborts (require_env) unless every secret is set, and eagerly
constructs live clients on import. Owns its own handle to the same
memory_bank.db file instead.

Run manually or import run_daily_check() from monitor.py's scheduled job:
    python3 fundamentals_bullish_monitor.py
"""
import os
import sqlite3
import sys
from datetime import datetime, timezone

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")
AMERICA_DATA_ENGINE_URL = os.getenv("AMERICA_DATA_ENGINE_URL", "http://america-data-engine:8001")
WEBHOOK_SECRET_TOKEN = os.getenv("WEBHOOK_SECRET_TOKEN", "")


def _connect():
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS fundamentals_bullish_status ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "checked_at TEXT NOT NULL, "
        "ticker TEXT NOT NULL, "
        "f_score INTEGER, "
        "high_quality_flag INTEGER NOT NULL, "
        "data_quality TEXT NOT NULL)"
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_fundamentals_bullish_ticker_time "
        "ON fundamentals_bullish_status(ticker, checked_at)"
    )
    return conn


def _fetch_fundamentals_bullish() -> dict:
    """GET /risk/fundamentals-bullish from america-data-engine. {} on any
    failure — fail-open, same contract as every other data-engine consumer
    in heartbeat."""
    if not WEBHOOK_SECRET_TOKEN:
        return {}
    try:
        import requests
        resp = requests.get(
            f"{AMERICA_DATA_ENGINE_URL}/risk/fundamentals-bullish",
            headers={"X-Nox-Token": WEBHOOK_SECRET_TOKEN},
            timeout=(5, 10),
        )
        if resp.status_code != 200:
            return {}
        return resp.json().get("results", {})
    except Exception as e:
        print(f"[FUNDAMENTALS_BULLISH] Fetch failed: {e}", file=sys.stderr)
        return {}


def _previously_flagged_tickers(conn) -> set:
    """Tickers whose MOST RECENT row (before this run) had the flag set."""
    rows = conn.execute(
        "SELECT ticker, high_quality_flag FROM fundamentals_bullish_status "
        "WHERE id IN (SELECT MAX(id) FROM fundamentals_bullish_status GROUP BY ticker)"
    ).fetchall()
    return {ticker for ticker, flag in rows if flag}


def _send_telegram(msg: str) -> None:
    token = os.getenv("TELEGRAM_BOT_TOKEN")
    chat_id = os.getenv("TELEGRAM_CHAT_ID")
    if not token or not chat_id:
        print(f"[FUNDAMENTALS_BULLISH] Telegram not configured, skipping alert:\n{msg}", file=sys.stderr)
        return
    try:
        import requests
        requests.post(
            f"https://api.telegram.org/bot{token}/sendMessage",
            json={"chat_id": chat_id, "text": msg, "parse_mode": "Markdown"},
            timeout=10,
        )
    except Exception as e:
        print(f"[FUNDAMENTALS_BULLISH] Telegram send failed: {e}", file=sys.stderr)


def run_daily_check() -> dict:
    """Persists today's per-ticker bullish-quality row and alerts only on
    newly-flagged tickers this run. Returns a summary dict for tests/manual
    invocation."""
    results = _fetch_fundamentals_bullish()
    if not results:
        print("[FUNDAMENTALS_BULLISH] No data from america-data-engine this run (fail-open, skipping).")
        return {"checked": 0, "newly_flagged": []}

    conn = _connect()
    try:
        previously_flagged = _previously_flagged_tickers(conn)
        now = datetime.now(timezone.utc).isoformat()
        newly_flagged = []

        for ticker, r in results.items():
            piotroski = r.get("piotroski", {}) or {}
            flags = r.get("flags", {}) or {}
            flag = bool(flags.get("piotroski_high_quality"))

            conn.execute(
                "INSERT INTO fundamentals_bullish_status "
                "(checked_at, ticker, f_score, high_quality_flag, data_quality) "
                "VALUES (?, ?, ?, ?, ?)",
                (
                    now, ticker,
                    piotroski.get("f_score"), int(flag),
                    r.get("data_quality", "INSUFFICIENT"),
                ),
            )

            if flag and ticker not in previously_flagged:
                newly_flagged.append({
                    "ticker": ticker,
                    "f_score": piotroski.get("f_score"),
                })
        conn.commit()

        if newly_flagged:
            lines = ["🌱 *Bullish Quality Flags — New*", "─" * 14]
            for f in newly_flagged:
                lines.append(
                    f"• {f['ticker']} — Piotroski F-Score: {f['f_score']}/9 "
                    "(high quality: clean earnings, deleveraging, no dilution)"
                )
            lines.append("Surface-only: no sizing/gating impact this pass.")
            _send_telegram("\n".join(lines))

        return {"checked": len(results), "newly_flagged": newly_flagged}
    finally:
        conn.close()


if __name__ == "__main__":
    print(run_daily_check())
