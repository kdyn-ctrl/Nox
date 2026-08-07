"""
Monthly Signal Performance & Accuracy Analyzer for Nox.

Evaluates generated signals over a 30-day or Month-to-Date (MTD) window:
1. Signal Inventory & Volume Breakdown (by strategy & asset class)
2. Directional Hit Rate & Accuracy (T+1, T+5, T+10, T+30 checkpoints)
3. Return Distribution (avg win %, avg loss %, profit factor, max win/loss)
4. Confidence & Quality Score Calibration (stated quality vs actual hit rate)
5. Filter Rejection Reason Analysis
6. Formatted Markdown & Telegram Summary Generation for EOM Reports
"""

import os
import sys
import math
import sqlite3
import logging
from datetime import datetime, timedelta, timezone
from typing import Dict, List, Any, Optional, Tuple

import pandas as pd
import yfinance as yf

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("monthly_signal_performance_analyzer")

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")
if not os.path.exists(DB_PATH) and os.path.exists("/root/Nox/data/memory_bank.db"):
    DB_PATH = "/root/Nox/data/memory_bank.db"

ALPACA_DATA_URL = "https://data.alpaca.markets"
ALPACA_API_KEY = os.getenv("ALPACA_API_KEY", "")
ALPACA_SECRET_KEY = os.getenv("ALPACA_SECRET_KEY", "")


def get_db_connection():
    if not os.path.exists(DB_PATH):
        logger.warning(f"Database missing at {DB_PATH}")
        return None
    return sqlite3.connect(DB_PATH)


def fetch_historical_closes(ticker: str, start_date_str: str, end_date_str: str) -> Dict[str, float]:
    """Fetch daily close prices for `ticker` keyed by YYYY-MM-DD string."""
    closes = {}
    try:
        t = yf.Ticker(ticker)
        df = t.history(start=start_date_str, end=end_date_str)
        if df is not None and not df.empty:
            for idx, row in df.iterrows():
                date_key = idx.strftime("%Y-%m-%d")
                closes[date_key] = float(row["Close"])
            return closes
    except Exception as err:
        logger.debug(f"yfinance fetch failed for {ticker}: {err}")

    return closes


def analyze_monthly_signals(window_days: int = 30, mtd_only: bool = False) -> Dict[str, Any]:
    """
    Core analysis function — queries signal tables for the evaluation window and
    computes accuracy, return metrics, strategy breakdowns, and calibration.
    """
    conn = get_db_connection()
    if not conn:
        return {"error": "Database missing", "total_signals": 0}

    today = datetime.now(timezone.utc).date()
    if mtd_only:
        start_date = today.replace(day=1)
    else:
        start_date = today - timedelta(days=window_days)

    start_date_str = start_date.strftime("%Y-%m-%d")

    # 1. Fetch Options Signals
    options_signals = []
    try:
        query = """
        SELECT id, ticker, strategy, direction, quality_score, scan_at, outcome, reason
        FROM options_signals
        WHERE datetime(scan_at, 'unixepoch') >= ?
        ORDER BY scan_at ASC
        """
        cursor = conn.cursor()
        cursor.execute(query, (start_date_str,))
        rows = cursor.fetchall()
        for r in rows:
            scan_datetime = datetime.fromtimestamp(r[5], timezone.utc)
            options_signals.append({
                "id": r[0],
                "ticker": r[1],
                "strategy": r[2] or "MACRO_OPTIONS",
                "direction": r[3] or "BULLISH",
                "quality_score": r[4] or 0.5,
                "scan_date": scan_datetime.strftime("%Y-%m-%d"),
                "outcome": r[6],
                "reason": r[7],
                "source": "OPTIONS"
            })
    except Exception as e:
        logger.debug(f"Error reading options_signals: {e}")

    # 2. Fetch Equity Signals
    equity_signals = []
    try:
        query = """
        SELECT id, ticker, strategy, direction, entry_price, scan_at, status, reason
        FROM equity_signals
        WHERE datetime(scan_at, 'unixepoch') >= ?
        ORDER BY scan_at ASC
        """
        cursor = conn.cursor()
        cursor.execute(query, (start_date_str,))
        rows = cursor.fetchall()
        for r in rows:
            scan_datetime = datetime.fromtimestamp(r[5], timezone.utc)
            equity_signals.append({
                "id": r[0],
                "ticker": r[1],
                "strategy": r[2] or "EQUITY_STRATEGY",
                "direction": r[3] or "BULLISH",
                "entry_price": r[4],
                "scan_date": scan_datetime.strftime("%Y-%m-%d"),
                "status": r[6],
                "reason": r[7],
                "quality_score": 0.75,
                "source": "EQUITY"
            })
    except Exception as e:
        logger.debug(f"Error reading equity_signals: {e}")

    # 3. Fetch Signal Events
    signal_events = []
    try:
        query = """
        SELECT id, ticker, strategy, quality_score, scan_at, outcome, reason
        FROM signal_events
        WHERE datetime(scan_at, 'unixepoch') >= ?
        ORDER BY scan_at ASC
        """
        cursor = conn.cursor()
        cursor.execute(query, (start_date_str,))
        rows = cursor.fetchall()
        for r in rows:
            scan_datetime = datetime.fromtimestamp(r[4], timezone.utc)
            signal_events.append({
                "id": r[0],
                "ticker": r[1],
                "strategy": r[2] or "SIGNAL_EVENT",
                "direction": "BULLISH",
                "quality_score": r[3] or 0.5,
                "scan_date": scan_datetime.strftime("%Y-%m-%d"),
                "outcome": r[5],
                "reason": r[6],
                "source": "EVENT"
            })
    except Exception as e:
        logger.debug(f"Error reading signal_events: {e}")

    conn.close()

    all_signals = options_signals + equity_signals + signal_events
    total_signals = len(all_signals)

    if total_signals == 0:
        return {
            "window_days": window_days,
            "start_date": start_date_str,
            "total_signals": 0,
            "strategy_counts": {},
            "hit_rate_t1": 0.0,
            "hit_rate_t5": 0.0,
            "hit_rate_t10": 0.0,
            "hit_rate_t30": 0.0,
            "avg_win_pct": 0.0,
            "avg_loss_pct": 0.0,
            "profit_factor": 0.0,
            "high_quality_hit_rate": 0.0,
            "low_quality_hit_rate": 0.0,
            "rejection_reasons": {}
        }

    # Strategy breakdown
    strategy_counts = {}
    for s in all_signals:
        strat = s["strategy"]
        strategy_counts[strat] = strategy_counts.get(strat, 0) + 1

    # Rejection breakdown
    rejection_reasons = {}
    for s in all_signals:
        reason = s.get("reason") or s.get("outcome") or "APPROVED"
        if reason != "APPROVED" and "PASSED" not in reason:
            rejection_reasons[reason] = rejection_reasons.get(reason, 0) + 1

    # Outcome evaluation over T+1, T+5, T+10, T+30
    evaluations = []
    unique_tickers = list({s["ticker"] for s in all_signals})

    # Fetch daily price histories for evaluation
    ticker_closes = {}
    end_eval_str = (today + timedelta(days=5)).strftime("%Y-%m-%d")
    for t in unique_tickers:
        ticker_closes[t] = fetch_historical_closes(t, start_date_str, end_eval_str)

    for s in all_signals:
        ticker = s["ticker"]
        scan_date_str = s["scan_date"]
        direction = s["direction"].upper()
        closes = ticker_closes.get(ticker, {})

        if not closes or scan_date_str not in closes:
            continue

        base_price = closes[scan_date_str]
        if base_price <= 0:
            continue

        sorted_dates = sorted(closes.keys())
        try:
            base_idx = sorted_dates.index(scan_date_str)
        except ValueError:
            continue

        # Checkpoints
        for days, label in [(1, "t1"), (5, "t5"), (10, "t10"), (30, "t30")]:
            target_idx = base_idx + days
            if target_idx < len(sorted_dates):
                exit_price = closes[sorted_dates[target_idx]]
                raw_return = (exit_price - base_price) / base_price
                directional_return = raw_return if direction == "BULLISH" else -raw_return
                is_correct = directional_return > 0

                evaluations.append({
                    "signal_id": s["id"],
                    "strategy": s["strategy"],
                    "quality_score": s["quality_score"],
                    "checkpoint": label,
                    "return_pct": directional_return * 100.0,
                    "is_correct": is_correct
                })

    # Metric computations
    t1_evals = [e for e in evaluations if e["checkpoint"] == "t1"]
    t5_evals = [e for e in evaluations if e["checkpoint"] == "t5"]
    t10_evals = [e for e in evaluations if e["checkpoint"] == "t10"]
    t30_evals = [e for e in evaluations if e["checkpoint"] == "t30"]

    hit_rate_t1 = (sum(1 for e in t1_evals if e["is_correct"]) / len(t1_evals) * 100.0) if t1_evals else 0.0
    hit_rate_t5 = (sum(1 for e in t5_evals if e["is_correct"]) / len(t5_evals) * 100.0) if t5_evals else 0.0
    hit_rate_t10 = (sum(1 for e in t10_evals if e["is_correct"]) / len(t10_evals) * 100.0) if t10_evals else 0.0
    hit_rate_t30 = (sum(1 for e in t30_evals if e["is_correct"]) / len(t30_evals) * 100.0) if t30_evals else 0.0

    all_returns = [e["return_pct"] for e in t5_evals] if t5_evals else [e["return_pct"] for e in evaluations]
    wins = [r for r in all_returns if r > 0]
    losses = [r for r in all_returns if r <= 0]

    avg_win_pct = (sum(wins) / len(wins)) if wins else 0.0
    avg_loss_pct = (sum(losses) / len(losses)) if losses else 0.0
    total_gains = sum(wins) if wins else 0.0
    total_losses = abs(sum(losses)) if losses else 0.0
    profit_factor = (total_gains / total_losses) if total_losses > 0 else (99.9 if total_gains > 0 else 0.0)

    # Quality calibration
    high_q_evals = [e for e in evaluations if e["quality_score"] >= 0.70]
    low_q_evals = [e for e in evaluations if e["quality_score"] < 0.70]

    high_q_hit_rate = (sum(1 for e in high_q_evals if e["is_correct"]) / len(high_q_evals) * 100.0) if high_q_evals else 0.0
    low_q_hit_rate = (sum(1 for e in low_q_evals if e["is_correct"]) / len(low_q_evals) * 100.0) if low_q_evals else 0.0

    return {
        "window_days": window_days,
        "start_date": start_date_str,
        "total_signals": total_signals,
        "strategy_counts": strategy_counts,
        "hit_rate_t1": hit_rate_t1,
        "hit_rate_t5": hit_rate_t5,
        "hit_rate_t10": hit_rate_t10,
        "hit_rate_t30": hit_rate_t30,
        "avg_win_pct": avg_win_pct,
        "avg_loss_pct": avg_loss_pct,
        "profit_factor": profit_factor,
        "high_quality_hit_rate": high_q_hit_rate,
        "low_quality_hit_rate": low_q_hit_rate,
        "rejection_reasons": rejection_reasons,
        "evaluations_count": len(evaluations)
    }


def generate_monthly_performance_markdown(stats: Dict[str, Any]) -> str:
    """Formats monthly signal performance metrics into a clean GitHub Markdown report."""
    md = []
    md.append("# 📊 Monthly Signal Performance & Accuracy Report")
    md.append(f"**Evaluation Window:** {stats.get('window_days', 30)} Days (Since `{stats.get('start_date', 'N/A')}`)\n")

    md.append("## 1. Signal Volume & Strategy Inventory")
    md.append(f"* **Total Signals Generated:** `{stats.get('total_signals', 0)}`")
    md.append("* **Strategy Breakdown:**")
    strat_counts = stats.get("strategy_counts", {})
    if strat_counts:
        for s, c in strat_counts.items():
            md.append(f"  - `{s}`: {c} signals")
    else:
        md.append("  - *No signals recorded in window*")

    md.append("\n## 2. Directional Accuracy & Win Rate")
    md.append("| Checkpoint Horizon | Directional Hit Rate (%) | Status |")
    md.append("| :--- | :--- | :--- |")
    md.append(f"| **T+1 (Day 1)** | {stats.get('hit_rate_t1', 0.0):.1f}% | {'🟢 Strong' if stats.get('hit_rate_t1', 0) >= 55 else '🟡 Neutral'} |")
    md.append(f"| **T+5 (1 Week)** | {stats.get('hit_rate_t5', 0.0):.1f}% | {'🟢 Strong' if stats.get('hit_rate_t5', 0) >= 55 else '🟡 Neutral'} |")
    md.append(f"| **T+10 (2 Weeks)** | {stats.get('hit_rate_t10', 0.0):.1f}% | {'🟢 Strong' if stats.get('hit_rate_t10', 0) >= 55 else '🟡 Neutral'} |")
    md.append(f"| **T+30 (1 Month)** | {stats.get('hit_rate_t30', 0.0):.1f}% | {'🟢 Strong' if stats.get('hit_rate_t30', 0) >= 55 else '🟡 Neutral'} |")

    md.append("\n## 3. Return Distribution & Expectancy")
    md.append(f"* **Avg Win Move:** `+{stats.get('avg_win_pct', 0.0):.2f}%`")
    md.append(f"* **Avg Loss Move:** `{stats.get('avg_loss_pct', 0.0):.2f}%`")
    md.append(f"* **Profit Factor:** `{stats.get('profit_factor', 0.0):.2f}`")

    md.append("\n## 4. Confidence & Quality Score Calibration")
    md.append(r"* **High Confidence ($\ge 0.70$) Hit Rate:** " + f"`{stats.get('high_quality_hit_rate', 0.0):.1f}%`")
    md.append(r"* **Low Confidence ($< 0.70$) Hit Rate:** " + f"`{stats.get('low_quality_hit_rate', 0.0):.1f}%`")

    rejections = stats.get("rejection_reasons", {})
    if rejections:
        md.append("\n## 5. Candidate Rejection Breakdown")
        for reason, count in rejections.items():
            md.append(f"* `{reason}`: {count} candidate(s) filtered")

    return "\n".join(md)


if __name__ == "__main__":
    import sys
    days = 30
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        days = int(sys.argv[1])
    report_data = analyze_monthly_signals(window_days=days)
    markdown_report = generate_monthly_performance_markdown(report_data)
    print(markdown_report)
