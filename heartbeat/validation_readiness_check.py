"""
Read-only status report for every "still open" live/paper-validation item
CLAUDE.md lists across Phase 1-4. All the underlying logic (signal_events,
alpha_decay_monitor, IvRankStore, PortfolioRiskManager) is already built and
unit-tested against mocks/seeded data — the only thing missing for most items
is elapsed paper-trading time. This script answers "how much time is left /
is it ready to actually audit yet?" without needing to hand-write SQL each
time someone checks.

Deliberately NOT importing monitor.py (see polygon_iv_backfill.py's own note):
that module hard-aborts without live Telegram/Anthropic/Alpaca secrets and has
import-time side effects. Owns its own read-only handle to the same
memory_bank.db instead.

Run manually:
    python3 validation_readiness_check.py [--regen-days 7]
"""

import argparse
import os
import sqlite3
from collections import defaultdict
from datetime import datetime, timedelta, timezone

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")

ALPHA_DECAY_MIN_DAYS = 30    # rolling window alpha_decay_monitor.py needs
ALPHA_DECAY_BASELINE_DAYS = 252
IV_RANK_MIN_DAYS = 30        # same threshold calculate_iv_rank()/IvRankStore use

MAX_PORTFOLIO_DELTA = float(os.getenv("MAX_PORTFOLIO_DELTA", "500.0"))
MAX_PORTFOLIO_VEGA = float(os.getenv("MAX_PORTFOLIO_VEGA", "2000.0"))


def _connect():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def _table_exists(conn, name):
    return conn.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
    ).fetchone() is not None


def check_signal_regeneration(conn, lookback_days):
    print(f"\n=== Phase 2: Signal regeneration audit (last {lookback_days}d) ===")
    if not _table_exists(conn, "signal_events"):
        print("  signal_events table not found — engine hasn't run yet.")
        return

    since = int((datetime.now(timezone.utc) - timedelta(days=lookback_days)).timestamp())
    rows = conn.execute(
        "SELECT signature, ticker, strategy, outcome, reason, scan_at "
        "FROM signal_events WHERE scan_at >= ? ORDER BY signature, scan_at",
        (since,),
    ).fetchall()

    if not rows:
        print("  No signal_events in this window yet — nothing to audit.")
        return

    by_sig = defaultdict(list)
    for r in rows:
        by_sig[r["signature"]].append(r)

    regenerated, silenced, filled = 0, 0, 0
    for sig, events in by_sig.items():
        outcomes = [e["outcome"] for e in events]
        if outcomes.count("submitted") >= 2:
            regenerated += 1
        elif "submitted" in outcomes and outcomes[-1] != "submitted":
            filled += 1
        elif all(o.startswith("suppressed_") or o.startswith("gate_blocked_") for o in outcomes):
            silenced += 1

    print(f"  {len(by_sig)} distinct signatures, {len(rows)} events total")
    print(f"  Regenerated through a gate/failure (submitted 2+ times): {regenerated}")
    print(f"  Filled then went silent (correct — position exists): {filled}")
    print(f"  Silenced with a documented reason only: {silenced}")
    print("  Read the underlying rows before trusting a bucket count — this is a coarse "
          "classifier, not the audit itself. Query signal_events WHERE signature=? for detail.")


def check_iv_rank_backfill(conn):
    print("\n=== Phase 2: IV rank history (Polygon backfill target) ===")
    has_key = bool(os.getenv("POLYGON_API_KEY"))
    print(f"  POLYGON_API_KEY set: {has_key}" + ("" if has_key else " — backfill script can't run yet"))
    if not _table_exists(conn, "historical_volatility"):
        print("  historical_volatility table not found.")
        return
    rows = conn.execute(
        "SELECT ticker, COUNT(DISTINCT date) AS days FROM historical_volatility GROUP BY ticker ORDER BY ticker"
    ).fetchall()
    if not rows:
        print("  No IV history yet for any ticker.")
        return
    for r in rows:
        status = "true 52wk rank" if r["days"] >= 252 else (
            f"proxy fallback — {IV_RANK_MIN_DAYS - r['days']} days short of 30" if r["days"] < IV_RANK_MIN_DAYS
            else f"real rank, {252 - r['days']} days short of full 52wk"
        )
        print(f"  {r['ticker']:6s} {r['days']:4d} days — {status}")


def check_alpha_decay(conn):
    print("\n=== Phase 3: Alpha decay monitor readiness ===")
    if not _table_exists(conn, "daily_ledger"):
        print("  daily_ledger table not found — nothing recorded yet.")
        return
    days = conn.execute("SELECT COUNT(DISTINCT date) AS d FROM daily_ledger").fetchone()["d"]
    print(f"  daily_ledger has {days} distinct trading days")
    if days < ALPHA_DECAY_MIN_DAYS:
        print(f"  Need {ALPHA_DECAY_MIN_DAYS - days} more days before the 30-day rolling Sharpe is even computable.")
    elif days < ALPHA_DECAY_BASELINE_DAYS:
        print(f"  30-day window is live; {ALPHA_DECAY_BASELINE_DAYS - days} more days until the 252-day baseline is real (not partial).")
    else:
        print("  Both windows have enough history — full comparison is meaningful now.")

    if _table_exists(conn, "alpha_decay_status"):
        row = conn.execute(
            "SELECT computed_at, rolling_sharpe_30d, baseline_sharpe_12mo, degraded_pct, tier_multiplier, triggered, days_available "
            "FROM alpha_decay_status ORDER BY id DESC LIMIT 1"
        ).fetchone()
        if row:
            print(f"  Last computed: {row['computed_at']} | triggered={bool(row['triggered'])} | "
                  f"tier_multiplier={row['tier_multiplier']} | days_available={row['days_available']}")
        else:
            print("  alpha_decay_status table exists but has no rows yet — run_daily_check() hasn't fired.")


def check_portfolio_risk(conn):
    print("\n=== Phase 4: Portfolio circuit breaker — current headroom ===")
    if not _table_exists(conn, "live_greeks"):
        print("  live_greeks table not found — monitor_positions() hasn't run yet.")
        return
    row = conn.execute(
        "SELECT COALESCE(SUM(ABS(delta)),0) AS d, COALESCE(SUM(ABS(vega)),0) AS v, COUNT(*) AS n FROM live_greeks"
    ).fetchone()
    if row["n"] == 0:
        print("  No open positions with live Greeks — nothing to check against caps yet.")
        return
    d_pct = row["d"] / MAX_PORTFOLIO_DELTA * 100 if MAX_PORTFOLIO_DELTA else 0
    v_pct = row["v"] / MAX_PORTFOLIO_VEGA * 100 if MAX_PORTFOLIO_VEGA else 0
    print(f"  {row['n']} positions | portfolio |delta|={row['d']:.1f} ({d_pct:.0f}% of {MAX_PORTFOLIO_DELTA}) | "
          f"|vega|={row['v']:.1f} ({v_pct:.0f}% of {MAX_PORTFOLIO_VEGA})")
    if d_pct >= 80 or v_pct >= 80:
        print("  ⚠️ Within 20% of a cap — a real breach could happen soon; worth watching closely.")
    else:
        print("  Comfortably under both caps — no breach observed yet to validate the force-close path against.")


def check_order_ledger(conn):
    print("\n=== Phase 1: Order ledger / reconciliation health ===")
    if not _table_exists(conn, "order_ledger"):
        print("  order_ledger table not found.")
        return
    rows = conn.execute(
        "SELECT status, COUNT(*) AS n FROM order_ledger GROUP BY status ORDER BY n DESC"
    ).fetchall()
    for r in rows:
        print(f"  {r['status']:10s} {r['n']}")
    stuck = conn.execute(
        "SELECT COUNT(*) AS n FROM order_ledger WHERE status IN ('pending','unknown')"
    ).fetchone()["n"]
    if stuck:
        print(f"  {stuck} order(s) still pending/unknown — reconcile_options_orders() should clear these within its 75s grace period.")


def check_ibkr(conn):
    print("\n=== Phase 3: IBKR live validation ===")
    vendor_script = os.path.join(os.path.dirname(__file__), "..", "execution", "setup_ibkr_vendor.sh")
    print(f"  setup_ibkr_vendor.sh exists: {os.path.exists(vendor_script)}")
    print("  Hard-blocked on a human accepting IBKR's TWS API license + a real IB Gateway — "
          "not a 'wait more days' item. See CLAUDE.md Phase 3 and execution/IBKR_MIGRATION.md.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regen-days", type=int, default=7, help="lookback window for the signal-regeneration audit")
    args = parser.parse_args()

    conn = _connect()
    try:
        check_signal_regeneration(conn, args.regen_days)
        check_iv_rank_backfill(conn)
        check_alpha_decay(conn)
        check_portfolio_risk(conn)
        check_order_ledger(conn)
        check_ibkr(conn)
    finally:
        conn.close()


if __name__ == "__main__":
    main()
