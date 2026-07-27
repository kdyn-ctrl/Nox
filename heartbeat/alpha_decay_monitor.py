"""
Alpha decay monitor — CLAUDE.md Phase 3.

Watches whether Nox's actual trading edge is holding up: computes a rolling
30-day Sharpe ratio from the `daily_ledger` table (the same table Phase 2's
daily_ledger P&L tracking populates — see PositionManager::upsert_unrealized/
add_realized) and compares it against a trailing 12-month (252 trading day)
baseline Sharpe. If the rolling window has degraded more than
ALPHA_DECAY_THRESHOLD_PCT against a baseline that was actually positive, it
writes a reduced position-size multiplier into `alpha_decay_status` and fires
a Telegram alert. The C++ engine's AlphaDecayStore reads that table and
scales `qty_contracts` sizing by the multiplier before every new options
order — this script only ever writes an advisory number, it never touches
the broker directly.

Deliberately NOT importing monitor.py: that module's top-level code
hard-aborts (require_env) unless Telegram/Anthropic/Alpaca/webhook secrets are
all set, and eagerly constructs live clients on import — side effects this
pure analytics script has no business triggering. Owns its own handle to the
same memory_bank.db file instead, mirroring polygon_iv_backfill.py's pattern.

Run manually or import run_daily_check() from monitor.py's scheduled job:
    python3 alpha_decay_monitor.py
"""

import math
import os
import sqlite3
import sys
from datetime import datetime, timezone

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")

# Tuned-but-generic — env-sourced per project convention, nothing tunable
# stays a bare literal.
ROLLING_WINDOW_DAYS   = int(os.getenv("ALPHA_DECAY_ROLLING_DAYS", "30"))
BASELINE_WINDOW_DAYS  = int(os.getenv("ALPHA_DECAY_BASELINE_DAYS", "252"))
MIN_ROLLING_DAYS      = int(os.getenv("ALPHA_DECAY_MIN_ROLLING_DAYS", "30"))
MIN_BASELINE_DAYS     = int(os.getenv("ALPHA_DECAY_MIN_BASELINE_DAYS", "60"))
DECAY_THRESHOLD_PCT   = float(os.getenv("ALPHA_DECAY_THRESHOLD_PCT", "0.20"))
TIERED_DOWN_MULTIPLIER = float(os.getenv("ALPHA_DECAY_TIER_DOWN_MULTIPLIER", "0.50"))
TRADING_DAYS_PER_YEAR  = 252.0


def _connect():
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS alpha_decay_status ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "computed_at TEXT NOT NULL, "
        "rolling_sharpe_30d REAL, "
        "baseline_sharpe_12mo REAL, "
        "degraded_pct REAL, "
        "tier_multiplier REAL NOT NULL, "
        "triggered INTEGER NOT NULL, "
        "days_available INTEGER NOT NULL)"
    )
    return conn


def _daily_totals(conn) -> list:
    """Portfolio mark-to-market total per date, oldest first: SUM(realized +
    unrealized) across every ticker/asset_class row for that date. Returns
    [(date, total), ...]."""
    cur = conn.execute(
        "SELECT date, SUM(realized_pnl + unrealized_pnl) AS total "
        "FROM daily_ledger GROUP BY date ORDER BY date ASC"
    )
    return cur.fetchall()


def _daily_returns(totals: list) -> list:
    """Day-over-day change in the portfolio total — a proxy for that day's
    net trading P&L, robust to whether a position closed (unrealized drops
    to 0, realized absorbs it) or stayed open (unrealized just re-marks)."""
    return [totals[i][1] - totals[i - 1][1] for i in range(1, len(totals))]


def _sharpe(returns: list) -> float:
    n = len(returns)
    if n < 2:
        return 0.0
    mean = sum(returns) / n
    variance = sum((r - mean) ** 2 for r in returns) / (n - 1)
    stdev = math.sqrt(variance)
    if stdev < 1e-9:
        return 0.0
    return (mean / stdev) * math.sqrt(TRADING_DAYS_PER_YEAR)


def compute_alpha_decay(conn=None) -> dict:
    """Pure computation, no writes/alerts — kept separate so tests can assert
    on the numbers without touching sqlite or Telegram."""
    owns_conn = conn is None
    conn = conn or _connect()
    try:
        totals = _daily_totals(conn)
        returns = _daily_returns(totals)
        days_available = len(returns)

        result = {
            "days_available": days_available,
            "rolling_sharpe_30d": None,
            "baseline_sharpe_12mo": None,
            "degraded_pct": None,
            "triggered": False,
            "tier_multiplier": 1.0,
            "sufficient_data": days_available >= MIN_ROLLING_DAYS and days_available >= MIN_BASELINE_DAYS,
        }
        if not result["sufficient_data"]:
            return result

        rolling = returns[-ROLLING_WINDOW_DAYS:]
        baseline = returns[-BASELINE_WINDOW_DAYS:]

        rolling_sharpe = _sharpe(rolling)
        baseline_sharpe = _sharpe(baseline)
        result["rolling_sharpe_30d"] = rolling_sharpe
        result["baseline_sharpe_12mo"] = baseline_sharpe

        # Only a positive baseline can "decay" — a negative baseline getting
        # more negative isn't the alpha-decay scenario this monitor targets.
        if baseline_sharpe > 0:
            degraded_pct = (baseline_sharpe - rolling_sharpe) / baseline_sharpe
            result["degraded_pct"] = degraded_pct
            if degraded_pct > DECAY_THRESHOLD_PCT:
                result["triggered"] = True
                result["tier_multiplier"] = TIERED_DOWN_MULTIPLIER

        return result
    finally:
        if owns_conn:
            conn.close()


def _previously_triggered(conn) -> bool:
    row = conn.execute(
        "SELECT triggered FROM alpha_decay_status ORDER BY id DESC LIMIT 1"
    ).fetchone()
    return bool(row and row[0])


def _send_telegram(msg: str) -> None:
    token = os.getenv("TELEGRAM_BOT_TOKEN")
    chat_id = os.getenv("TELEGRAM_CHAT_ID")
    if not token or not chat_id:
        print(f"[ALPHA_DECAY] Telegram not configured, skipping alert:\n{msg}", file=sys.stderr)
        return
    try:
        import requests
        requests.post(
            f"https://api.telegram.org/bot{token}/sendMessage",
            json={"chat_id": chat_id, "text": msg, "parse_mode": "Markdown"},
            timeout=10,
        )
    except Exception as e:
        print(f"[ALPHA_DECAY] Telegram send failed: {e}", file=sys.stderr)


def run_daily_check() -> dict:
    """Computes decay status, persists it, and alerts only on a state
    transition (silent→triggered or triggered→recovered) so a degraded
    regime doesn't spam an alert every single day it persists."""
    conn = _connect()
    try:
        result = compute_alpha_decay(conn)
        if not result["sufficient_data"]:
            print(f"[ALPHA_DECAY] Insufficient history ({result['days_available']} days) — "
                  f"need {max(MIN_ROLLING_DAYS, MIN_BASELINE_DAYS)}. Skipping.")
            return result

        was_triggered = _previously_triggered(conn)
        conn.execute(
            "INSERT INTO alpha_decay_status "
            "(computed_at, rolling_sharpe_30d, baseline_sharpe_12mo, degraded_pct, "
            " tier_multiplier, triggered, days_available) VALUES (?, ?, ?, ?, ?, ?, ?)",
            (
                datetime.now(timezone.utc).isoformat(),
                result["rolling_sharpe_30d"],
                result["baseline_sharpe_12mo"],
                result["degraded_pct"],
                result["tier_multiplier"],
                int(result["triggered"]),
                result["days_available"],
            ),
        )
        conn.commit()

        if result["triggered"] and not was_triggered:
            _send_telegram(
                "\U0001F7E1 *Alpha Decay Triggered*\n"
                "──────────────\n"
                f"• *30d Sharpe:* {result['rolling_sharpe_30d']:.2f}\n"
                f"• *12mo Sharpe:* {result['baseline_sharpe_12mo']:.2f}\n"
                f"• *Degraded:* {result['degraded_pct'] * 100:.0f}% "
                f"(threshold {DECAY_THRESHOLD_PCT * 100:.0f}%)\n"
                f"• *Position sizing:* tiered down to {TIERED_DOWN_MULTIPLIER * 100:.0f}% "
                "of normal until Sharpe recovers."
            )
        elif was_triggered and not result["triggered"]:
            _send_telegram(
                "\U0001F7E2 *Alpha Decay Recovered*\n"
                "──────────────\n"
                f"• *30d Sharpe:* {result['rolling_sharpe_30d']:.2f}\n"
                f"• *12mo Sharpe:* {result['baseline_sharpe_12mo']:.2f}\n"
                "• *Position sizing:* restored to normal."
            )
        return result
    finally:
        conn.close()


if __name__ == "__main__":
    print(run_daily_check())
