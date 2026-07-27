"""
C2 — does the system's signal direction actually end up right?

`options_signals.outcome` (written by the C++ engine) is a generation-time
gate/lifecycle string (submitted/suppressed_*/gate_blocked_*) — it is never
revisited against real price action. This module closes that gap in
Python, reading the SAME shared memory_bank.db `options_signals` table the
C++ engine writes, without touching the engine at all: no new C++ hooks,
no schema changes on the system-signal side, nothing that could affect
live trading decisions (per the explicit design boundary: this informs the
human, it does not retrain or gate the bot — see CLAUDE.md Phase 5's
"not overfit" framing this project already established).

Two kinds of checkpoint, because holding periods vary a lot in practice
(some trades are intraday, others held for weeks):

  1. Fixed calendar-day checkpoints (T+1, T+5, T+10 by default, env-tunable
     via SIGNAL_OUTCOME_CHECKPOINT_DAYS) applied to EVERY resolvable system
     signal, whether or not you ever traded it. This answers "is the
     system's directional call generally right" independent of your own
     trading — the deeper part of the original ask.
  2. A hold-duration checkpoint, applied only when a signal was correlated
     to one of your actual entries (personal_trades or imported_fills) AND
     that position has since been closed — checks the realized price move
     over the ACTUAL time you held it, not an arbitrary fixed window. This
     is what makes the resolver correct for a position held 3 weeks instead
     of silently checking it at T+1 like everything else would.

Deliberately simple: a plain % price move compared against the signal's
BULLISH/BEARISH direction, nothing weighted or scored — same "transparent
over overfit" principle as the EOD report's Signal Correlation section.

Deliberately NOT importing monitor.py — see plaid_fills_importer.py's own
comment on this pattern. Owns its own handle to the SAME memory_bank.db.

Run via monitor.py's daily EOD-report hook, or manually:
    python3 signal_outcome_resolver.py
"""
import os
import sqlite3
import sys
from datetime import datetime, timedelta, timezone

from retry_utils import fetch_with_retry

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")

ALPACA_DATA_URL = "https://data.alpaca.markets"
ALPACA_API = os.getenv("ALPACA_API_KEY", "")
ALPACA_SEC = os.getenv("ALPACA_SECRET_KEY", "")

_checkpoint_raw = os.getenv("SIGNAL_OUTCOME_CHECKPOINT_DAYS", "1,5,10")
SIGNAL_OUTCOME_CHECKPOINT_DAYS = sorted({int(d.strip()) for d in _checkpoint_raw.split(",") if d.strip()})

# How far around a target date to widen the bars request to skip
# weekends/holidays and still land on a real trading day.
_PRICE_LOOKUP_PADDING_DAYS = 5

_SYSTEM_DIRECTION_TO_SIGN = {"BULLISH": 1, "BEARISH": -1}

_price_cache = {}


def _fetch_daily_ohlc(ticker: str, start_date: str, end_date: str) -> list:
    """Returns a list of (date_str, high, low, close) tuples, oldest first,
    from Alpaca. The one real historical-price path in this codebase — both
    _fetch_daily_bars (close-only) and the plan-aware resolver (needs high/low
    for target/stop first-touch, audit §1 C4) read through here so there is a
    single request shape and cache to reason about (RULE-D6)."""
    cache_key = (ticker, start_date, end_date)
    if cache_key in _price_cache:
        return _price_cache[cache_key]

    if not ALPACA_API or not ALPACA_SEC:
        _price_cache[cache_key] = []
        return []

    resp = fetch_with_retry(
        f"{ALPACA_DATA_URL}/v2/stocks/{ticker}/bars",
        source=f"Alpaca bars (signal outcome):{ticker}",
        headers={"APCA-API-KEY-ID": ALPACA_API, "APCA-API-SECRET-KEY": ALPACA_SEC},
        params={"timeframe": "1Day", "start": start_date, "end": end_date, "adjustment": "raw", "feed": "iex", "limit": 50},
        timeout=(10, 20),
    )
    if resp is None or resp.status_code != 200:
        _price_cache[cache_key] = []
        return []
    try:
        bars = resp.json().get("bars", [])
        result = [(b["t"][:10], b["h"], b["l"], b["c"]) for b in bars]
    except Exception as e:
        print(f"[SIGNAL_OUTCOME] failed to parse bars for {ticker}: {e}", file=sys.stderr)
        result = []
    _price_cache[cache_key] = result
    return result


def _fetch_daily_bars(ticker: str, start_date: str, end_date: str) -> list:
    """Returns a list of (date_str, close) tuples, oldest first — the close-only
    projection of _fetch_daily_ohlc, kept for the existing close-based callers."""
    return [(d, c) for d, _h, _l, c in _fetch_daily_ohlc(ticker, start_date, end_date)]


def _closest_close_on_or_after(ticker: str, target_date: str) -> float | None:
    """Nearest trading-day close ON OR AFTER target_date, within a small padded window."""
    start = target_date
    end = (datetime.fromisoformat(target_date) + timedelta(days=_PRICE_LOOKUP_PADDING_DAYS)).strftime("%Y-%m-%d")
    bars = _fetch_daily_bars(ticker, start, end)
    for date_str, close in bars:  # oldest first — first bar >= target_date
        if date_str >= target_date:
            return close
    return None


def _closest_close_on_or_before(ticker: str, target_date: str) -> float | None:
    """Nearest trading-day close ON OR BEFORE target_date, within a small padded window."""
    start = (datetime.fromisoformat(target_date) - timedelta(days=_PRICE_LOOKUP_PADDING_DAYS)).strftime("%Y-%m-%d")
    end = target_date
    bars = _fetch_daily_bars(ticker, start, end)
    for date_str, close in reversed(bars):  # newest first — first bar <= target_date
        if date_str <= target_date:
            return close
    return None


def _record_outcome(conn, options_signal_id, ticker, direction, checkpoint_label,
                     entry_price, checkpoint_price) -> bool:
    if entry_price is None or checkpoint_price is None or entry_price == 0:
        return False
    move_pct = (checkpoint_price - entry_price) / entry_price * 100
    sign = _SYSTEM_DIRECTION_TO_SIGN.get(direction)
    direction_correct = None if sign is None else int((move_pct * sign) > 0)
    try:
        conn.execute(
            "INSERT INTO signal_outcomes "
            "(options_signal_id, ticker, direction, checkpoint_label, entry_price, checkpoint_price, move_pct, direction_correct) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (options_signal_id, ticker, direction, checkpoint_label, entry_price, checkpoint_price, move_pct, direction_correct),
        )
        conn.commit()
        return True
    except sqlite3.IntegrityError:
        return False  # already resolved for this signal+checkpoint — dedup via UNIQUE constraint


def _resolve_fixed_checkpoints(conn: sqlite3.Connection) -> int:
    today = datetime.now(timezone.utc).date()
    resolved = 0

    signals = conn.execute(
        "SELECT id, ticker, direction, scan_at FROM options_signals "
        "WHERE direction IN ('BULLISH', 'BEARISH') "
        "ORDER BY scan_at DESC LIMIT 500"
    ).fetchall()

    for signal_id, ticker, direction, scan_at in signals:
        scan_date = datetime.fromtimestamp(scan_at, tz=timezone.utc).date()
        for checkpoint_days in SIGNAL_OUTCOME_CHECKPOINT_DAYS:
            checkpoint_date = scan_date + timedelta(days=checkpoint_days)
            if checkpoint_date > today:
                continue  # not enough calendar time has passed yet — resolve on a later run
            label = f"T+{checkpoint_days}"
            already = conn.execute(
                "SELECT 1 FROM signal_outcomes WHERE options_signal_id = ? AND checkpoint_label = ?",
                (signal_id, label),
            ).fetchone()
            if already:
                continue
            entry_price = _closest_close_on_or_after(ticker, scan_date.isoformat())
            checkpoint_price = _closest_close_on_or_after(ticker, checkpoint_date.isoformat())
            if _record_outcome(conn, signal_id, ticker, direction, label, entry_price, checkpoint_price):
                resolved += 1

    return resolved


def _find_nearby_signal(conn: sqlite3.Connection, ticker: str, trade_ts_unix: int, window_hours: float = 4.0):
    """Same plain ticker+time-window rule the EOD report's correlation section
    uses — duplicated here (not imported from monitor.py, which has
    import-time side effects) rather than sharing code across the two
    always-separate-process modules."""
    window_seconds = int(window_hours * 3600)
    return conn.execute(
        "SELECT id, direction FROM options_signals WHERE ticker = ? AND scan_at BETWEEN ? AND ? "
        "ORDER BY ABS(scan_at - ?) ASC LIMIT 1",
        (ticker, trade_ts_unix - window_seconds, trade_ts_unix + window_seconds, trade_ts_unix),
    ).fetchone()


def _resolve_entry_exit_pairs(conn: sqlite3.Connection, entries: list, exits: list) -> int:
    """entries/exits: list of (ticker, explicit_signal_id_or_None, timestamp_iso).
    Shared by both personal_trades and imported_fills — an entry followed by
    a later exit on the same ticker gets checked against whatever system
    signal was nearby at entry time, over the ACTUAL realized holding
    period (not a fixed T+N window)."""
    resolved = 0
    for ticker, explicit_signal_id, entry_at in entries:
        if not entry_at:
            continue
        exit_at = next((e_at for e_ticker, e_at in exits if e_ticker == ticker and e_at > entry_at), None)
        if not exit_at:
            continue  # still open — nothing to resolve yet

        signal_row = None
        if explicit_signal_id:
            signal_row = conn.execute(
                "SELECT id, direction FROM options_signals WHERE id = ?", (explicit_signal_id,)
            ).fetchone()
        if not signal_row:
            entry_ts_unix = int(datetime.fromisoformat(entry_at).astimezone(timezone.utc).timestamp())
            signal_row = _find_nearby_signal(conn, ticker, entry_ts_unix)
        if not signal_row:
            continue  # no signal was ever near this entry — nothing to resolve

        signal_id, direction = signal_row
        if direction not in ("BULLISH", "BEARISH"):
            continue
        already = conn.execute(
            "SELECT 1 FROM signal_outcomes WHERE options_signal_id = ? AND checkpoint_label = 'hold_duration'",
            (signal_id,),
        ).fetchone()
        if already:
            continue

        entry_price = _closest_close_on_or_after(ticker, entry_at[:10])
        checkpoint_price = _closest_close_on_or_after(ticker, exit_at[:10])
        if _record_outcome(conn, signal_id, ticker, direction, "hold_duration", entry_price, checkpoint_price):
            resolved += 1

    return resolved


def _resolve_hold_duration_checkpoints(conn: sqlite3.Connection) -> int:
    resolved = 0

    # Personal trades: an OPEN/BUY entry followed by a later CLOSE/SELL exit
    # on the same ticker. signal_source/signal_id already links most of
    # these back to a specific signal — reuse that when present.
    pt_entries = conn.execute(
        "SELECT ticker, (CASE WHEN signal_source = 'system' THEN signal_id ELSE NULL END), executed_at "
        "FROM personal_trades WHERE action IN ('OPEN', 'BUY') ORDER BY executed_at ASC"
    ).fetchall()
    pt_exits = conn.execute(
        "SELECT ticker, executed_at FROM personal_trades "
        "WHERE action IN ('CLOSE', 'SELL') ORDER BY executed_at ASC"
    ).fetchall()
    resolved += _resolve_entry_exit_pairs(conn, pt_entries, pt_exits)

    # imported_fills: no explicit signal_id link exists at all here (Plaid/
    # Robinhood have no concept of "the signal that caused this") — always
    # falls back to the ticker+time-window lookup. trade_time is used when
    # a source provides one (Robinhood); otherwise trade_date at midday.
    fill_rows = conn.execute(
        "SELECT ticker, is_entry, COALESCE(trade_time, trade_date || 'T12:00:00') AS ts "
        "FROM imported_fills WHERE ts IS NOT NULL ORDER BY ts ASC"
    ).fetchall()
    fill_entries = [(ticker, None, ts) for ticker, is_entry, ts in fill_rows if is_entry]
    fill_exits = [(ticker, ts) for ticker, is_entry, ts in fill_rows if not is_entry]
    resolved += _resolve_entry_exit_pairs(conn, fill_entries, fill_exits)

    return resolved


def resolve_signal_outcomes() -> dict:
    with sqlite3.connect(DB_PATH) as conn:
        fixed = _resolve_fixed_checkpoints(conn)
        hold_duration = _resolve_hold_duration_checkpoints(conn)
    return {"fixed_checkpoints_resolved": fixed, "hold_duration_resolved": hold_duration}


if __name__ == "__main__":
    result = resolve_signal_outcomes()
    print(f"[SIGNAL_OUTCOME] {result}")
