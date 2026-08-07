import os
import sys
import sqlite3
import pytest
from datetime import datetime, timezone, timedelta

HEARTBEAT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HEARTBEAT_DIR)

from monthly_signal_performance_analyzer import (
    analyze_monthly_signals,
    generate_monthly_performance_markdown
)


@pytest.fixture
def temp_db(tmp_path):
    db_file = tmp_path / "test_memory_bank.db"
    conn = sqlite3.connect(db_file)
    conn.execute(
        "CREATE TABLE options_signals ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT, strategy TEXT, direction TEXT, "
        "quality_score REAL, scan_at INTEGER, outcome TEXT, reason TEXT)"
    )
    conn.execute(
        "CREATE TABLE equity_signals ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT, strategy TEXT, direction TEXT, "
        "entry_price REAL, scan_at INTEGER, status TEXT, reason TEXT)"
    )
    conn.execute(
        "CREATE TABLE signal_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT, strategy TEXT, quality_score REAL, "
        "scan_at INTEGER, outcome TEXT, reason TEXT)"
    )

    now_ts = int(datetime.now(timezone.utc).timestamp())
    conn.execute(
        "INSERT INTO options_signals (ticker, strategy, direction, quality_score, scan_at, outcome, reason) "
        "VALUES ('AMZN', 'PEAD_DRIFT', 'BULLISH', 0.85, ?, 'APPROVED', 'PASSED')", (now_ts,)
    )
    conn.execute(
        "INSERT INTO options_signals (ticker, strategy, direction, quality_score, scan_at, outcome, reason) "
        "VALUES ('AAPL', 'PRE_EARNINGS_IV', 'BULLISH', 0.60, ?, 'REJECTED', 'DTE too short')", (now_ts,)
    )
    conn.commit()
    conn.close()
    return str(db_file)


def test_empty_database_returns_zero(tmp_path, monkeypatch):
    empty_db = tmp_path / "empty.db"
    conn = sqlite3.connect(empty_db)
    conn.execute("CREATE TABLE options_signals (id INTEGER PRIMARY KEY, scan_at INTEGER)")
    conn.close()

    monkeypatch.setattr("monthly_signal_performance_analyzer.DB_PATH", str(empty_db))
    stats = analyze_monthly_signals(window_days=30)
    assert stats["total_signals"] == 0
    assert stats["profit_factor"] == 0.0


def test_analyze_monthly_signals_with_data(temp_db, monkeypatch):
    monkeypatch.setattr("monthly_signal_performance_analyzer.DB_PATH", temp_db)
    monkeypatch.setattr("monthly_signal_performance_analyzer.fetch_historical_closes", lambda t, s, e: {
        datetime.now(timezone.utc).strftime("%Y-%m-%d"): 100.0
    })

    stats = analyze_monthly_signals(window_days=30)
    assert stats["total_signals"] == 2
    assert "PEAD_DRIFT" in stats["strategy_counts"]
    assert "PRE_EARNINGS_IV" in stats["strategy_counts"]


def test_generate_monthly_performance_markdown():
    dummy_stats = {
        "window_days": 30,
        "start_date": "2026-07-07",
        "total_signals": 10,
        "strategy_counts": {"PEAD_DRIFT": 6, "TIMEZONE_LEAD_LAG": 4},
        "hit_rate_t1": 60.0,
        "hit_rate_t5": 70.0,
        "hit_rate_t10": 65.0,
        "hit_rate_t30": 80.0,
        "avg_win_pct": 5.2,
        "avg_loss_pct": -2.1,
        "profit_factor": 2.47,
        "high_quality_hit_rate": 75.0,
        "low_quality_hit_rate": 50.0,
        "rejection_reasons": {"DTE too short": 2}
    }
    md = generate_monthly_performance_markdown(dummy_stats)
    assert "Monthly Signal Performance & Accuracy Report" in md
    assert "Total Signals Generated" in md
    assert "PEAD_DRIFT" in md
    assert "2.47" in md
