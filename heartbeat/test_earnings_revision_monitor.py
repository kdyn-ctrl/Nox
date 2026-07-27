"""
Unit tests for earnings_revision_monitor.py — the pure compute function
(no I/O), the period-selection logic, and the sqlite persistence/alert
transition logic (mirrors test_fundamentals_bullish_monitor.py's pattern).

Run: python3 test_earnings_revision_monitor.py
"""
import os
import sqlite3
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

import earnings_revision_monitor as erm
from earnings_revision_monitor import Knobs, compute_revision_from_history, _select_upcoming_period


class TestComputeRevisionFromHistory(unittest.TestCase):
    def test_empty_history_is_insufficient(self):
        result = compute_revision_from_history([], Knobs())
        self.assertTrue(result["insufficient_data"])
        self.assertIsNone(result["pct_change"])

    def test_below_min_history_days_is_insufficient(self):
        knobs = Knobs(min_history_days=10)
        history = [("2026-07-01", 1.0, 5), ("2026-07-02", 1.02, 5)]  # only 2 days
        result = compute_revision_from_history(history, knobs)
        self.assertTrue(result["insufficient_data"])
        self.assertEqual(result["days_of_history"], 2)

    def test_rising_consensus_flags_above_threshold(self):
        knobs = Knobs(min_history_days=3, min_pct_change=0.05, min_analysts=2)
        history = [
            ("2026-07-01", 1.00, 5),
            ("2026-07-08", 1.03, 5),
            ("2026-07-15", 1.06, 6),  # +6% over the window
        ]
        result = compute_revision_from_history(history, knobs)
        self.assertFalse(result["insufficient_data"])
        self.assertAlmostEqual(result["pct_change"], 0.06, places=6)
        self.assertTrue(result["revision_flag"])

    def test_below_pct_threshold_does_not_flag(self):
        knobs = Knobs(min_history_days=3, min_pct_change=0.05, min_analysts=2)
        history = [("2026-07-01", 1.00, 5), ("2026-07-08", 1.01, 5), ("2026-07-15", 1.02, 5)]  # +2%
        result = compute_revision_from_history(history, knobs)
        self.assertFalse(result["insufficient_data"])
        self.assertFalse(result["revision_flag"])

    def test_too_few_analysts_does_not_flag_despite_large_change(self):
        knobs = Knobs(min_history_days=3, min_pct_change=0.05, min_analysts=3)
        history = [("2026-07-01", 1.00, 1), ("2026-07-08", 1.10, 1), ("2026-07-15", 1.20, 1)]
        result = compute_revision_from_history(history, knobs)
        self.assertFalse(result["insufficient_data"])
        self.assertGreater(result["pct_change"], 0.05)
        self.assertFalse(result["revision_flag"])  # only 1 analyst, floor is 3

    def test_falling_consensus_does_not_flag(self):
        knobs = Knobs(min_history_days=3, min_pct_change=0.05, min_analysts=2)
        history = [("2026-07-01", 1.00, 5), ("2026-07-08", 0.95, 5), ("2026-07-15", 0.90, 5)]
        result = compute_revision_from_history(history, knobs)
        self.assertFalse(result["revision_flag"])
        self.assertLess(result["pct_change"], 0)

    def test_old_snapshots_outside_lookback_window_excluded(self):
        # A stale snapshot from 90 days ago must not count as "earliest" —
        # only the lookback_days window around the latest snapshot matters.
        knobs = Knobs(min_history_days=3, min_pct_change=0.05, min_analysts=2, lookback_days=30)
        history = [
            ("2026-04-01", 0.50, 5),   # ~100 days before latest — must be excluded
            ("2026-07-01", 1.00, 5),
            ("2026-07-08", 1.03, 5),
            ("2026-07-15", 1.06, 5),
        ]
        result = compute_revision_from_history(history, knobs)
        # If the stale 0.50 snapshot were included, pct_change would be huge
        # (112%). With it correctly excluded, it should match the 3-point
        # in-window test above (+6%).
        self.assertAlmostEqual(result["pct_change"], 0.06, places=6)

    def test_zero_earliest_eps_guards_against_zero_division(self):
        knobs = Knobs(min_history_days=2, min_pct_change=0.05, min_analysts=2)
        history = [("2026-07-01", 0.0, 5), ("2026-07-08", 0.10, 5)]
        result = compute_revision_from_history(history, knobs)  # must not raise
        self.assertTrue(result["insufficient_data"])

    def test_unordered_input_still_computes_correctly(self):
        knobs = Knobs(min_history_days=3, min_pct_change=0.05, min_analysts=2)
        history = [
            ("2026-07-15", 1.06, 5),
            ("2026-07-01", 1.00, 5),
            ("2026-07-08", 1.03, 5),
        ]
        result = compute_revision_from_history(history, knobs)
        self.assertAlmostEqual(result["pct_change"], 0.06, places=6)


class TestSelectUpcomingPeriod(unittest.TestCase):
    def test_picks_nearest_future_period(self):
        today = datetime(2026, 7, 17, tzinfo=timezone.utc)
        entries = [
            {"period": "2026-12-31", "epsAvg": 2.0},
            {"period": "2026-09-30", "epsAvg": 1.5},
            {"period": "2026-06-30", "epsAvg": 1.0},  # already passed
        ]
        picked = _select_upcoming_period(entries, today)
        self.assertEqual(picked["period"], "2026-09-30")

    def test_no_future_period_returns_none(self):
        today = datetime(2026, 7, 17, tzinfo=timezone.utc)
        entries = [{"period": "2026-01-01", "epsAvg": 1.0}]
        self.assertIsNone(_select_upcoming_period(entries, today))

    def test_malformed_period_skipped_not_crashed(self):
        today = datetime(2026, 7, 17, tzinfo=timezone.utc)
        entries = [{"period": "not-a-date", "epsAvg": 1.0}, {"period": "2026-09-30", "epsAvg": 1.5}]
        picked = _select_upcoming_period(entries, today)  # must not raise
        self.assertEqual(picked["period"], "2026-09-30")


class TestRunDailyCheck(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self._orig_db_path = erm.DB_PATH
        erm.DB_PATH = self.db_path

        self._orig_fetch = erm._fetch_eps_estimate
        self._orig_telegram = erm._send_telegram
        self.sent_messages = []
        erm._send_telegram = lambda msg: self.sent_messages.append(msg)

    def tearDown(self):
        erm.DB_PATH = self._orig_db_path
        erm._fetch_eps_estimate = self._orig_fetch
        erm._send_telegram = self._orig_telegram
        os.remove(self.db_path)

    def test_no_key_no_history_checks_nothing(self):
        erm._fetch_eps_estimate = lambda ticker: None
        result = erm.run_daily_check()
        self.assertEqual(result["checked"], 0)
        self.assertEqual(result["newly_flagged"], [])
        self.assertEqual(self.sent_messages, [])

    def test_accumulates_snapshots_across_multiple_runs_and_eventually_flags(self):
        knobs_days = 3
        os.environ["EARNINGS_REVISION_MIN_HISTORY_DAYS"] = str(knobs_days)
        os.environ["EARNINGS_REVISION_MIN_PCT_CHANGE"] = "0.05"
        os.environ["EARNINGS_REVISION_MIN_ANALYSTS"] = "2"
        try:
            # run_daily_check() calls _fetch_eps_estimate exactly once per
            # ticker per run and stamps the result as "today" — so the fake
            # only needs to supply today's value; d1/d2 (2 and 1 days ago)
            # are pre-seeded directly below to simulate two prior days' runs.
            erm._fetch_eps_estimate = lambda ticker: (
                {"period": "2026-09-30", "epsAvg": 1.06, "epsHigh": 1.16,
                 "epsLow": 0.96, "numberAnalysts": 5}
                if ticker == "AAPL" else None
            )
            erm.WATCHLIST = ["AAPL"]

            conn = sqlite3.connect(self.db_path)
            conn.execute(
                "CREATE TABLE IF NOT EXISTS eps_estimate_snapshots ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT NOT NULL, "
                "period TEXT NOT NULL, snapshot_date TEXT NOT NULL, "
                "eps_avg REAL, eps_high REAL, eps_low REAL, num_analysts INTEGER, "
                "UNIQUE(ticker, period, snapshot_date))"
            )
            today = datetime.now(timezone.utc)
            d1 = (today - timedelta(days=2)).strftime("%Y-%m-%d")
            d2 = (today - timedelta(days=1)).strftime("%Y-%m-%d")
            conn.execute(
                "INSERT INTO eps_estimate_snapshots (ticker, period, snapshot_date, eps_avg, "
                "eps_high, eps_low, num_analysts) VALUES (?,?,?,?,?,?,?)",
                ("AAPL", "2026-09-30", d1, 1.00, 1.1, 0.9, 5),
            )
            conn.execute(
                "INSERT INTO eps_estimate_snapshots (ticker, period, snapshot_date, eps_avg, "
                "eps_high, eps_low, num_analysts) VALUES (?,?,?,?,?,?,?)",
                ("AAPL", "2026-09-30", d2, 1.03, 1.1, 0.9, 5),
            )
            conn.commit()
            conn.close()

            result = erm.run_daily_check()
            self.assertEqual(len(result["newly_flagged"]), 1)
            self.assertEqual(result["newly_flagged"][0]["ticker"], "AAPL")
            self.assertEqual(len(self.sent_messages), 1)
            self.assertIn("AAPL", self.sent_messages[0])
            self.assertIn("Surface-only", self.sent_messages[0])
        finally:
            del os.environ["EARNINGS_REVISION_MIN_HISTORY_DAYS"]
            del os.environ["EARNINGS_REVISION_MIN_PCT_CHANGE"]
            del os.environ["EARNINGS_REVISION_MIN_ANALYSTS"]

    def test_no_repeat_alert_on_persistent_flag(self):
        os.environ["EARNINGS_REVISION_MIN_HISTORY_DAYS"] = "1"
        os.environ["EARNINGS_REVISION_MIN_ANALYSTS"] = "1"
        try:
            erm.WATCHLIST = ["AAPL"]
            erm._fetch_eps_estimate = lambda ticker: (
                {"period": "2026-09-30", "epsAvg": 2.0, "epsHigh": 2.1, "epsLow": 1.9, "numberAnalysts": 5}
                if ticker == "AAPL" else None
            )
            conn = sqlite3.connect(self.db_path)
            conn.execute(
                "CREATE TABLE IF NOT EXISTS eps_estimate_snapshots ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT NOT NULL, "
                "period TEXT NOT NULL, snapshot_date TEXT NOT NULL, "
                "eps_avg REAL, eps_high REAL, eps_low REAL, num_analysts INTEGER, "
                "UNIQUE(ticker, period, snapshot_date))"
            )
            yesterday = (datetime.now(timezone.utc) - timedelta(days=1)).strftime("%Y-%m-%d")
            conn.execute(
                "INSERT INTO eps_estimate_snapshots (ticker, period, snapshot_date, eps_avg, "
                "eps_high, eps_low, num_analysts) VALUES (?,?,?,?,?,?,?)",
                ("AAPL", "2026-09-30", yesterday, 1.0, 1.1, 0.9, 5),
            )
            conn.commit()
            conn.close()

            erm.run_daily_check()
            self.sent_messages.clear()
            erm.run_daily_check()  # same flag persists — should not re-alert
            self.assertEqual(self.sent_messages, [])
        finally:
            del os.environ["EARNINGS_REVISION_MIN_HISTORY_DAYS"]
            del os.environ["EARNINGS_REVISION_MIN_ANALYSTS"]


if __name__ == "__main__":
    unittest.main()
