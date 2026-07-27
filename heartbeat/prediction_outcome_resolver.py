"""
Engine-wide prediction-quality resolver (see CLAUDE.md's "Engine-Wide
Prediction Quality Scoring").

signal_outcome_resolver.py already does "log a call, wait, check the real
price, record whether direction was correct" — but only for
`options_signals`. This module generalizes the same idea to every other
directional source that logs into the new `predictions_log` table (WS1
contradiction, Skeptic WS2/WS3, China macro lag WS8, personal signals, and
eventually the Beneish/FCF fundamentals flags): resolves each pending row at
T+1/T+5/T+7/T+10/T+30 (the last two specifically for the weekly/monthly
rollup) and writes the result into `prediction_outcomes`.

Deliberately reuses signal_outcome_resolver's Alpaca price-lookup helpers
(_fetch_daily_bars/_closest_close_on_or_after) rather than duplicating a
second date-pinned historical-price path — that module owns the one real
price-lookup utility in the codebase.

`options_signals` itself is NOT re-resolved here — signal_outcome_resolver.py
+ `signal_outcomes` already covers it end-to-end. prediction_quality_scorer.py
reads BOTH tables to produce one unified per-source rollup.

Deliberately NOT importing monitor.py — same reasoning as
signal_outcome_resolver.py's own docstring.

Run via monitor.py's daily EOD-report hook, or manually:
    python3 prediction_outcome_resolver.py
"""
import os
import sqlite3
import sys
from datetime import datetime, timedelta, timezone

from signal_outcome_resolver import _closest_close_on_or_after, _fetch_daily_ohlc

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")

_checkpoint_raw = os.getenv("PREDICTION_OUTCOME_CHECKPOINT_DAYS", "1,5,7,10,30")
PREDICTION_OUTCOME_CHECKPOINT_DAYS = sorted({int(d.strip()) for d in _checkpoint_raw.split(",") if d.strip()})

_DIRECTION_TO_SIGN = {"BULLISH": 1, "BEARISH": -1}

# Asset classes whose ticker is (or resolves to) an equity symbol Alpaca's
# stock-bars feed can price: plain equities and options (options carry the
# underlying equity symbol, and BULLISH/BEARISH refers to the underlying move).
# FUTURES/CRYPTO/FOREX have no equity bar — pricing them against
# /v2/stocks/<ticker>/bars silently scored e.g. `CL` (crude) against
# Colgate-Palmolive (audit §1 C3). Those are skipped, not mis-resolved.
_EQUITY_PRICEABLE_ASSET_CLASSES = {"EQUITY", "OPTION", None, ""}


def _connect():
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS predictions_log ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, source_type TEXT NOT NULL, "
        "source_ref_id INTEGER, ticker TEXT NOT NULL, direction TEXT NOT NULL, "
        "confidence REAL, logged_at INTEGER NOT NULL, detail TEXT)"
    )
    # Additive: asset_class (audit §1 C3). NULL means EQUITY for older rows and
    # for C++-engine-written rows that never set it (all equity/index tickers).
    try:
        conn.execute("ALTER TABLE predictions_log ADD COLUMN asset_class TEXT")
    except sqlite3.OperationalError:
        pass  # column already exists
    conn.execute(
        "CREATE TABLE IF NOT EXISTS prediction_outcomes ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, resolved_at TEXT NOT NULL, "
        "source_type TEXT NOT NULL, source_ref_id INTEGER NOT NULL, ticker TEXT NOT NULL, "
        "direction TEXT NOT NULL, checkpoint_label TEXT NOT NULL, confidence REAL, "
        "entry_price REAL, checkpoint_price REAL, move_pct REAL, direction_correct INTEGER, "
        "UNIQUE(source_type, source_ref_id, checkpoint_label))"
    )
    return conn


def _insert_outcome(conn, source_type, source_ref_id, ticker, direction, confidence,
                    checkpoint_label, entry_price, checkpoint_price, move_pct,
                    direction_correct) -> bool:
    try:
        conn.execute(
            "INSERT INTO prediction_outcomes "
            "(resolved_at, source_type, source_ref_id, ticker, direction, checkpoint_label, "
            " confidence, entry_price, checkpoint_price, move_pct, direction_correct) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                datetime.now(timezone.utc).isoformat(), source_type, source_ref_id, ticker,
                direction, checkpoint_label, confidence, entry_price, checkpoint_price,
                move_pct, direction_correct,
            ),
        )
        conn.commit()
        return True
    except sqlite3.IntegrityError:
        return False  # already resolved for this source+checkpoint — dedup via UNIQUE constraint


def _record_outcome(conn, source_type, source_ref_id, ticker, direction, confidence,
                     checkpoint_label, entry_price, checkpoint_price) -> bool:
    """Sign-of-move resolution — the right metric for a pure directional call
    with no trade plan (WS1/skeptic/china-lag). Personal signals resolve via
    _record_plan_outcome instead (audit §1 C4)."""
    if entry_price is None or checkpoint_price is None or entry_price == 0:
        return False
    move_pct = (checkpoint_price - entry_price) / entry_price * 100
    sign = _DIRECTION_TO_SIGN.get(direction)
    direction_correct = None if sign is None else int((move_pct * sign) > 0)
    return _insert_outcome(conn, source_type, source_ref_id, ticker, direction, confidence,
                           checkpoint_label, entry_price, checkpoint_price, move_pct,
                           direction_correct)


def _resolve_plan(direction, entry_level, target, stop, ohlc):
    """Path-aware resolution for a signal that carries a trade plan (audit §1
    C4). `ohlc` is a list of (date, high, low, close) oldest-first covering
    logged_date..checkpoint_date. Returns one of:

      None                         — no bars / can't resolve yet (retry later)
      ('not_entered', ...)         — price never traded through entry_level in
                                     the window; scored as no trade (correct=None)
      ('target'|'stop'|'timeout', entry_price, exit_price, direction_correct)

    Semantics: entry_level is treated as a fillable limit — the entry is only
    "taken" once a bar's range brackets it (low <= entry <= high). From that
    bar forward, whichever of target/stop is touched first decides the outcome;
    a bar touching BOTH is scored as a STOP (pessimistic — never credit a win
    that might have stopped out first). If neither is touched by the end of the
    window, fall back to entry->last-close sign vs direction (a still-running
    plan, scored on realized move so far)."""
    if not ohlc or entry_level is None or target is None or stop is None:
        return None
    long_side = _DIRECTION_TO_SIGN.get(direction) == 1

    entered_idx = None
    for i, (_d, hi, lo, _c) in enumerate(ohlc):
        if lo <= entry_level <= hi:
            entered_idx = i
            break
    if entered_idx is None:
        return ("not_entered", None, None, None)

    for _d, hi, lo, _c in ohlc[entered_idx:]:
        hit_target = hi >= target if long_side else lo <= target
        hit_stop = lo <= stop if long_side else hi >= stop
        if hit_stop and hit_target:
            return ("stop", entry_level, stop, 0)  # pessimistic on same-bar ambiguity
        if hit_target:
            return ("target", entry_level, target, 1)
        if hit_stop:
            return ("stop", entry_level, stop, 0)

    last_close = ohlc[-1][3]
    move = last_close - entry_level
    sign = _DIRECTION_TO_SIGN.get(direction)
    direction_correct = None if sign is None else int((move * sign) > 0)
    return ("timeout", entry_level, last_close, direction_correct)


def _record_plan_outcome(conn, source_type, source_ref_id, ticker, direction, confidence,
                         checkpoint_label, plan) -> bool:
    entry_level, target, stop, logged_date, checkpoint_date = plan
    # Widen the window slightly past the checkpoint so weekends/holidays around
    # the checkpoint date still land on a real trading day (same padding spirit
    # as _closest_close_on_or_after).
    window_end = (checkpoint_date + timedelta(days=5)).isoformat()
    ohlc = _fetch_daily_ohlc(ticker, logged_date.isoformat(), window_end)
    # Trim to bars on/before the checkpoint so a later checkpoint doesn't peek
    # past its own horizon.
    ohlc = [b for b in ohlc if b[0] <= checkpoint_date.isoformat()]
    result = _resolve_plan(direction, entry_level, target, stop, ohlc)
    if result is None:
        return False
    kind, entry_price, exit_price, direction_correct = result
    if kind == "not_entered":
        # Record so it isn't re-scanned forever, but keep it out of hit-rate
        # (direction_correct = NULL): an untriggered limit is not a trade.
        return _insert_outcome(conn, source_type, source_ref_id, ticker, direction,
                               confidence, checkpoint_label, None, None, None, None)
    move_pct = (exit_price - entry_price) / entry_price * 100 if entry_price else None
    return _insert_outcome(conn, source_type, source_ref_id, ticker, direction, confidence,
                           checkpoint_label, entry_price, exit_price, move_pct,
                           direction_correct)


def _personal_plan(conn, source_ref_id):
    """(entry_level, target, stop_loss, asset_class) for a personal signal, or
    None if the row is missing/lacks a full plan."""
    if source_ref_id is None:
        return None
    row = conn.execute(
        "SELECT entry_level, target, stop_loss, asset_class FROM personal_signals WHERE id = ?",
        (source_ref_id,),
    ).fetchone()
    return row


def resolve_prediction_outcomes() -> dict:
    today = datetime.now(timezone.utc).date()
    resolved = 0
    skipped_non_equity = 0

    conn = _connect()
    try:
        rows = conn.execute(
            "SELECT id, source_type, ticker, direction, confidence, logged_at, "
            "       source_ref_id, asset_class "
            "FROM predictions_log WHERE direction IN ('BULLISH', 'BEARISH') "
            "ORDER BY logged_at DESC LIMIT 2000"
        ).fetchall()

        for (row_id, source_type, ticker, direction, confidence, logged_at,
             source_ref_id, asset_class) in rows:
            # prediction_outcomes has always keyed on predictions_log.id (the
            # SELECT aliased `id` as the ref) — keep that so dedup/UNIQUE stays
            # stable for existing WS1/skeptic rows. The real source_ref_id
            # column is used only to find the personal signal's trade plan.
            ref_id = row_id
            if asset_class not in _EQUITY_PRICEABLE_ASSET_CLASSES:
                skipped_non_equity += 1
                continue  # no equity bar for FUTURES/CRYPTO/FOREX (audit §1 C3)

            logged_date = datetime.fromtimestamp(logged_at, tz=timezone.utc).date()

            # Personal signals carry a trade plan → path-aware resolution.
            plan_row = _personal_plan(conn, source_ref_id) if source_type == "personal_signal" else None

            for checkpoint_days in PREDICTION_OUTCOME_CHECKPOINT_DAYS:
                checkpoint_date = logged_date + timedelta(days=checkpoint_days)
                if checkpoint_date > today:
                    continue  # not enough calendar time has passed yet — resolve on a later run
                label = f"T+{checkpoint_days}"
                already = conn.execute(
                    "SELECT 1 FROM prediction_outcomes WHERE source_type = ? AND source_ref_id = ? "
                    "AND checkpoint_label = ?",
                    (source_type, ref_id, label),
                ).fetchone()
                if already:
                    continue

                if plan_row and plan_row[0] is not None:
                    entry_level, target, stop, _ac = plan_row
                    plan = (entry_level, target, stop, logged_date, checkpoint_date)
                    if _record_plan_outcome(conn, source_type, ref_id, ticker, direction,
                                            confidence, label, plan):
                        resolved += 1
                else:
                    entry_price = _closest_close_on_or_after(ticker, logged_date.isoformat())
                    checkpoint_price = _closest_close_on_or_after(ticker, checkpoint_date.isoformat())
                    if _record_outcome(conn, source_type, ref_id, ticker, direction,
                                       confidence, label, entry_price, checkpoint_price):
                        resolved += 1
    finally:
        conn.close()

    return {"resolved": resolved, "skipped_non_equity": skipped_non_equity}


if __name__ == "__main__":
    result = resolve_prediction_outcomes()
    print(f"[PREDICTION_OUTCOME] {result}")
