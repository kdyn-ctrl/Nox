"""
Unit tests for celh_signal_tracker's checkpoint-dedup and report logic.
Mirrors test_fundamentals_bullish_monitor.py's shape. No network access —
_run_analysis, _send_telegram, and _today are monkeypatched.

Run: python3 test_celh_signal_tracker.py
"""
import os
import sqlite3
import tempfile
import unittest
from datetime import date

import celh_signal_tracker as cst


def _fake_result(oos_r=0.6, is_r=0.24, oos_n=20, is_n=40, price_change_pct=12.3):
    return {"is_r": is_r, "is_n": is_n, "oos_r": oos_r, "oos_n": oos_n,
            "price_change_pct": price_change_pct}


class TestRunDailyCheck(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self._orig_db_path = cst.DB_PATH
        cst.DB_PATH = self.db_path

        self._orig_run_analysis = cst._run_analysis
        self._orig_telegram = cst._send_telegram
        self._orig_today = cst._today
        self.sent_messages = []
        cst._send_telegram = lambda msg: self.sent_messages.append(msg)

    def tearDown(self):
        cst.DB_PATH = self._orig_db_path
        cst._run_analysis = self._orig_run_analysis
        cst._send_telegram = self._orig_telegram
        cst._today = self._orig_today
        os.remove(self.db_path)

    def test_noop_before_either_checkpoint(self):
        cst._today = lambda: date(2026, 8, 1)
        cst._run_analysis = lambda today_str: (_ for _ in ()).throw(
            AssertionError("should not run analysis before a checkpoint date"))
        cst.run_daily_check()
        self.assertEqual(self.sent_messages, [])

    def test_fires_at_90d_checkpoint_and_writes_row(self):
        cst._today = lambda: date(2026, 10, 20)  # past the 90d target (2026-10-16)
        cst._run_analysis = lambda today_str: (_fake_result(), None)
        cst.run_daily_check()

        self.assertEqual(len(self.sent_messages), 1)
        self.assertIn("90d", self.sent_messages[0])

        conn = sqlite3.connect(self.db_path)
        rows = conn.execute(
            "SELECT checkpoint_label, oos_r FROM celh_search_signal_checkpoints"
        ).fetchall()
        conn.close()
        self.assertEqual(rows, [("90d", 0.6)])

    def test_does_not_refire_same_checkpoint(self):
        cst._today = lambda: date(2026, 10, 20)
        cst._run_analysis = lambda today_str: (_fake_result(), None)
        cst.run_daily_check()
        cst.run_daily_check()
        self.assertEqual(len(self.sent_messages), 1)

    def test_fires_both_checkpoints_once_180d_reached(self):
        cst._today = lambda: date(2027, 1, 20)  # past both targets
        cst._run_analysis = lambda today_str: (_fake_result(), None)
        cst.run_daily_check()
        self.assertEqual(len(self.sent_messages), 2)
        labels = {msg.split("— ")[1].split(" ")[0] for msg in self.sent_messages}
        self.assertEqual(labels, {"90d", "180d"})

    def test_analysis_failure_does_not_mark_checkpoint_fired(self):
        cst._today = lambda: date(2026, 10, 20)
        cst._run_analysis = lambda today_str: (None, "network down")
        cst.run_daily_check()
        self.assertEqual(self.sent_messages, [])

        conn = sqlite3.connect(self.db_path)
        count = conn.execute("SELECT COUNT(*) FROM celh_search_signal_checkpoints").fetchone()[0]
        conn.close()
        self.assertEqual(count, 0)

        # A later run with working analysis should still fire — no permanent lockout.
        cst._run_analysis = lambda today_str: (_fake_result(), None)
        cst.run_daily_check()
        self.assertEqual(len(self.sent_messages), 1)


class TestFormatReport(unittest.TestCase):
    def test_flags_sign_flip_vs_baseline(self):
        report = cst._format_report("90d", "2026-10-16", _fake_result(oos_r=-0.4))
        self.assertIn("SIGN FLIPPED", report)

    def test_flags_holding_up(self):
        report = cst._format_report("90d", "2026-10-16", _fake_result(oos_r=0.55))
        self.assertIn("HOLDING UP", report)

    def test_flags_weakening(self):
        report = cst._format_report("90d", "2026-10-16", _fake_result(oos_r=0.1))
        self.assertIn("WEAKENING", report)

    def test_handles_insufficient_oos_data(self):
        report = cst._format_report("90d", "2026-10-16", _fake_result(oos_r=None, oos_n=0))
        self.assertIn("insufficient OOS data", report)


if __name__ == "__main__":
    unittest.main()
