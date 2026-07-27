"""
Engine-wide weekly/monthly prediction-quality scoring (see CLAUDE.md's
"Engine-Wide Prediction Quality Scoring").

Answers, per signal source, "how good does this system expect to be, and
how good is it actually?" — not just hit-rate, but whether stated confidence
means anything (confidence_calibration) and whether the calls that hit also
caught real size (avg_move_pct_when_correct), at a 7-day ("weekly") and
30-day ("monthly") horizon.

Reads from TWO tables and unifies them into one per-source rollup:
  - `signal_outcomes` (written by signal_outcome_resolver.py) for the
    `options_signal` source — already resolves options_signals against real
    price action; joined back to options_signals.quality_score for
    confidence, since signal_outcomes itself has no confidence column.
  - `prediction_outcomes` (written by prediction_outcome_resolver.py) for
    every other source (ws1_contradiction, skeptic_insider, skeptic_altmacro,
    china_lag_ws8, personal_signal, and later fundamentals_beneish/fcf).

"Weekly" and "monthly" map directly to the T+7/T+30 checkpoints rather than
a separate rolling calendar-day sample window on top of them — the
checkpoint horizon IS the window being scored. Cumulative over all
available history (same "all-time, not just today" precedent as the
existing EOD Signal Outcomes report) rather than calendar-aligned, so this
never depends on whether a weekly cron actually fired.

Purely a self-assessment/reporting layer: nothing here feeds sizing or
gating (same surface-only boundary as fundamentals_risk_monitor.py).

Deliberately NOT importing monitor.py — same reasoning as
signal_outcome_resolver.py's own docstring.

Run via monitor.py's daily EOD-report hook, or manually:
    python3 prediction_quality_scorer.py
"""
import os
import sqlite3
import sys
from datetime import datetime, timezone

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")

WEEKLY_CHECKPOINT_DAYS = int(os.getenv("PREDICTION_QUALITY_WEEKLY_DAYS", "7"))
MONTHLY_CHECKPOINT_DAYS = int(os.getenv("PREDICTION_QUALITY_MONTHLY_DAYS", "30"))
MIN_SAMPLE_SIZE = int(os.getenv("PREDICTION_QUALITY_MIN_SAMPLE", "5"))
# A prediction counts as "high confidence" above this, for the calibration
# bucket comparison (does stated confidence actually correlate with hits?).
HIGH_CONFIDENCE_THRESHOLD = float(os.getenv("PREDICTION_QUALITY_HIGH_CONFIDENCE", "0.5"))

_WINDOWS = {"weekly": WEEKLY_CHECKPOINT_DAYS, "monthly": MONTHLY_CHECKPOINT_DAYS}


def _connect():
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS prediction_quality_rollup ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, source_type TEXT NOT NULL, "
        "window_days INTEGER NOT NULL, computed_at TEXT NOT NULL, n INTEGER NOT NULL, "
        "hit_rate REAL, avg_confidence REAL, confidence_calibration REAL, "
        "avg_move_pct_when_correct REAL, avg_move_pct_when_wrong REAL, quality_score REAL)"
    )
    return conn


def _rows_for_options_signal(conn, checkpoint_days: int) -> list:
    """(direction_correct, confidence, move_pct) tuples for the options_signal
    source, joining signal_outcomes back to options_signals for quality_score
    as the confidence proxy (signal_outcomes has no confidence column)."""
    label = f"T+{checkpoint_days}"
    try:
        return conn.execute(
            "SELECT so.direction_correct, os.quality_score, so.move_pct "
            "FROM signal_outcomes so "
            "LEFT JOIN options_signals os ON os.id = so.options_signal_id "
            "WHERE so.checkpoint_label = ? AND so.direction_correct IS NOT NULL",
            (label,),
        ).fetchall()
    except sqlite3.OperationalError:
        # signal_outcomes/options_signals are owned by the C++ engine /
        # signal_outcome_resolver.py — fail open if neither has run yet on
        # this DB (e.g. a fresh test fixture or a heartbeat-only deployment).
        return []


def _rows_for_source(conn, source_type: str, checkpoint_days: int) -> list:
    """(direction_correct, confidence, move_pct) tuples for a predictions_log-
    backed source."""
    label = f"T+{checkpoint_days}"
    rows = conn.execute(
        "SELECT direction_correct, confidence, move_pct FROM prediction_outcomes "
        "WHERE source_type = ? AND checkpoint_label = ? AND direction_correct IS NOT NULL",
        (source_type, label),
    ).fetchall()
    return rows


def _known_source_types(conn) -> list:
    types = set(r[0] for r in conn.execute(
        "SELECT DISTINCT source_type FROM prediction_outcomes"
    ).fetchall())
    types.add("options_signal")
    return sorted(types)


def compute_rollup_for_source(rows: list) -> dict:
    """Pure computation over a list of (direction_correct, confidence,
    move_pct) tuples — kept separate from I/O so tests can assert on the
    numbers directly."""
    n = len(rows)
    result = {
        "n": n, "hit_rate": None, "avg_confidence": None, "confidence_calibration": None,
        "avg_move_pct_when_correct": None, "avg_move_pct_when_wrong": None, "quality_score": None,
        "sufficient_data": n >= MIN_SAMPLE_SIZE,
    }
    if n < MIN_SAMPLE_SIZE:
        return result

    correct = [r for r in rows if r[0] == 1]
    wrong = [r for r in rows if r[0] == 0]
    result["hit_rate"] = len(correct) / n

    confidences = [r[1] for r in rows if r[1] is not None]
    if confidences:
        result["avg_confidence"] = sum(confidences) / len(confidences)

        high = [r for r in rows if r[1] is not None and r[1] >= HIGH_CONFIDENCE_THRESHOLD]
        low = [r for r in rows if r[1] is not None and r[1] < HIGH_CONFIDENCE_THRESHOLD]
        if high and low:
            high_hit = sum(1 for r in high if r[0] == 1) / len(high)
            low_hit = sum(1 for r in low if r[0] == 1) / len(low)
            result["confidence_calibration"] = high_hit - low_hit

    if correct:
        result["avg_move_pct_when_correct"] = sum(abs(r[2]) for r in correct) / len(correct)
    if wrong:
        result["avg_move_pct_when_wrong"] = sum(abs(r[2]) for r in wrong) / len(wrong)

    if result["avg_move_pct_when_correct"] is not None:
        result["quality_score"] = result["hit_rate"] * result["avg_move_pct_when_correct"]

    return result


def run_rollup() -> dict:
    """Computes and persists the weekly + monthly rollup for every known
    source, returning a summary dict for tests/manual invocation."""
    conn = _connect()
    summary = {}
    try:
        source_types = _known_source_types(conn)
        now = datetime.now(timezone.utc).isoformat()

        for source_type in source_types:
            summary[source_type] = {}
            for window_label, checkpoint_days in _WINDOWS.items():
                rows = (
                    _rows_for_options_signal(conn, checkpoint_days)
                    if source_type == "options_signal"
                    else _rows_for_source(conn, source_type, checkpoint_days)
                )
                result = compute_rollup_for_source(rows)
                summary[source_type][window_label] = result
                if not result["sufficient_data"]:
                    continue
                conn.execute(
                    "INSERT INTO prediction_quality_rollup "
                    "(source_type, window_days, computed_at, n, hit_rate, avg_confidence, "
                    " confidence_calibration, avg_move_pct_when_correct, avg_move_pct_when_wrong, quality_score) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (
                        source_type, checkpoint_days, now, result["n"], result["hit_rate"],
                        result["avg_confidence"], result["confidence_calibration"],
                        result["avg_move_pct_when_correct"], result["avg_move_pct_when_wrong"],
                        result["quality_score"],
                    ),
                )
        conn.commit()
    finally:
        conn.close()

    return summary


def format_quality_report(summary: dict) -> str:
    """Per-source-type weekly-vs-monthly table for /quality and the EOD
    summary line. Deliberately plain text, no charts — same "transparent
    over overfit" convention as the rest of the EOD report."""
    lines = ["📊 *Prediction Quality (Weekly / Monthly)*", "─" * 14]
    any_data = False
    for source_type in sorted(summary.keys()):
        weekly = summary[source_type].get("weekly", {})
        monthly = summary[source_type].get("monthly", {})
        if not weekly.get("sufficient_data") and not monthly.get("sufficient_data"):
            continue
        any_data = True
        lines.append(f"*{source_type}*")
        for label, r in (("  7d", weekly), (" 30d", monthly)):
            if not r.get("sufficient_data"):
                lines.append(f"{label}: insufficient data")
                continue
            hit_pct = r["hit_rate"] * 100 if r["hit_rate"] is not None else 0.0
            cal = r.get("confidence_calibration")
            cal_str = f", calib={cal:+.2f}" if cal is not None else ""
            lines.append(f"{label}: {hit_pct:.0f}% hit (n={r['n']}){cal_str}")
    if not any_data:
        lines.append("No source has enough resolved predictions yet.")
    return "\n".join(lines)


if __name__ == "__main__":
    result = run_rollup()
    print(format_quality_report(result))
