"""
Fundamental-risk monitor — Beneish M-Score + FCF burn-runway, surface-only.

Pulls the cached /risk/fundamentals scan from america-data-engine and
persists one row per ticker per day into `fundamentals_risk_status`, then
alerts on Telegram only for tickers that are NEWLY flagged this run (no flag
yesterday, flag today) — same silent->triggered transition logic as
alpha_decay_monitor.py, so a persistently-flagged name doesn't spam daily.

This is deliberately surface-only: nothing here touches sizing or gating.
The user wants to validate the scores against real fragile names first.

Deliberately NOT importing monitor.py — same reasoning as
alpha_decay_monitor.py's own docstring: that module's top-level code
hard-aborts (require_env) unless every secret is set, and eagerly
constructs live clients on import. Owns its own handle to the same
memory_bank.db file instead.

Run manually or import run_daily_check() from monitor.py's scheduled job:
    python3 fundamentals_risk_monitor.py
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
        "CREATE TABLE IF NOT EXISTS fundamentals_risk_status ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "checked_at TEXT NOT NULL, "
        "ticker TEXT NOT NULL, "
        "m_score REAL, "
        "beneish_flag INTEGER NOT NULL, "
        "fcf_runway_quarters REAL, "
        "fcf_flag INTEGER NOT NULL, "
        "data_quality TEXT NOT NULL)"
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_fundamentals_risk_ticker_time "
        "ON fundamentals_risk_status(ticker, checked_at)"
    )
    return conn


def _fetch_fundamentals() -> dict:
    """GET /risk/fundamentals from america-data-engine. {} on any failure —
    fail-open, same contract as every other data-engine consumer in heartbeat."""
    if not WEBHOOK_SECRET_TOKEN:
        return {}
    try:
        import requests
        resp = requests.get(
            f"{AMERICA_DATA_ENGINE_URL}/risk/fundamentals",
            headers={"X-Nox-Token": WEBHOOK_SECRET_TOKEN},
            timeout=(5, 10),
        )
        if resp.status_code != 200:
            return {}
        return resp.json().get("results", {})
    except Exception as e:
        print(f"[FUNDAMENTALS_RISK] Fetch failed: {e}", file=sys.stderr)
        return {}


def _previously_flagged_tickers(conn) -> set:
    """Tickers whose MOST RECENT row (before this run) had either flag set."""
    rows = conn.execute(
        "SELECT ticker, beneish_flag, fcf_flag FROM fundamentals_risk_status "
        "WHERE id IN (SELECT MAX(id) FROM fundamentals_risk_status GROUP BY ticker)"
    ).fetchall()
    return {ticker for ticker, beneish_flag, fcf_flag in rows if beneish_flag or fcf_flag}


def _send_telegram(msg: str) -> None:
    token = os.getenv("TELEGRAM_BOT_TOKEN")
    chat_id = os.getenv("TELEGRAM_CHAT_ID")
    if not token or not chat_id:
        print(f"[FUNDAMENTALS_RISK] Telegram not configured, skipping alert:\n{msg}", file=sys.stderr)
        return
    try:
        import requests
        requests.post(
            f"https://api.telegram.org/bot{token}/sendMessage",
            json={"chat_id": chat_id, "text": msg, "parse_mode": "Markdown"},
            timeout=10,
        )
    except Exception as e:
        print(f"[FUNDAMENTALS_RISK] Telegram send failed: {e}", file=sys.stderr)


def run_daily_check() -> dict:
    """Persists today's per-ticker fundamentals-risk row and alerts only on
    newly-flagged tickers this run. Returns a summary dict for tests/manual
    invocation."""
    results = _fetch_fundamentals()
    if not results:
        print("[FUNDAMENTALS_RISK] No data from america-data-engine this run (fail-open, skipping).")
        return {"checked": 0, "newly_flagged": []}

    conn = _connect()
    try:
        previously_flagged = _previously_flagged_tickers(conn)
        now = datetime.now(timezone.utc).isoformat()
        newly_flagged = []

        for ticker, r in results.items():
            beneish = r.get("beneish", {}) or {}
            fcf = r.get("fcf_runway", {}) or {}
            flags = r.get("flags", {}) or {}
            beneish_flag = bool(flags.get("beneish_manipulation_risk"))
            fcf_flag = bool(flags.get("fcf_burn_risk"))

            conn.execute(
                "INSERT INTO fundamentals_risk_status "
                "(checked_at, ticker, m_score, beneish_flag, fcf_runway_quarters, fcf_flag, data_quality) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (
                    now, ticker,
                    beneish.get("m_score"), int(beneish_flag),
                    fcf.get("runway_quarters"), int(fcf_flag),
                    r.get("data_quality", "INSUFFICIENT"),
                ),
            )

            if (beneish_flag or fcf_flag) and ticker not in previously_flagged:
                newly_flagged.append({
                    "ticker": ticker,
                    "m_score": beneish.get("m_score"),
                    "beneish_flag": beneish_flag,
                    "runway_quarters": fcf.get("runway_quarters"),
                    "fcf_flag": fcf_flag,
                })
        conn.commit()

        if newly_flagged:
            lines = ["⚠️ *Fundamental Risk Flags — New*", "─" * 14]
            for f in newly_flagged:
                if f["beneish_flag"]:
                    lines.append(
                        f"• {f['ticker']} — Beneish M-Score: {f['m_score']:.2f} "
                        "(earnings-manipulation risk)"
                    )
                if f["fcf_flag"]:
                    lines.append(
                        f"• {f['ticker']} — FCF runway: {f['runway_quarters']:.1f} quarters "
                        "(cash burn risk)"
                    )
            lines.append("Surface-only: no sizing/gating impact this pass.")
            _send_telegram("\n".join(lines))

        return {"checked": len(results), "newly_flagged": newly_flagged}
    finally:
        conn.close()


if __name__ == "__main__":
    print(run_daily_check())
