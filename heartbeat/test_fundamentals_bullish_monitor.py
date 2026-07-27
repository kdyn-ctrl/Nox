"""
Unit tests for fundamentals_bullish_monitor's sqlite persistence and
new-flag-only alert logic. Mirrors test_fundamentals_risk_monitor.py exactly.
No network access — _fetch_fundamentals_bullish and _send_telegram are
monkeypatched.

Run: python3 test_fundamentals_bullish_monitor.py
"""
import os
import sqlite3
import tempfile
import unittest

import fundamentals_bullish_monitor as fbm


def _result(f_score=None, flag=False, data_quality="OK"):
    return {
        "piotroski": {"f_score": f_score},
        "flags": {"piotroski_high_quality": flag},
        "data_quality": data_quality,
    }


class TestRunDailyCheck(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self._orig_db_path = fbm.DB_PATH
        fbm.DB_PATH = self.db_path

        self._orig_fetch = fbm._fetch_fundamentals_bullish
        self._orig_telegram = fbm._send_telegram
        self.sent_messages = []
        fbm._send_telegram = lambda msg: self.sent_messages.append(msg)

    def tearDown(self):
        fbm.DB_PATH = self._orig_db_path
        fbm._fetch_fundamentals_bullish = self._orig_fetch
        fbm._send_telegram = self._orig_telegram
        os.remove(self.db_path)

    def test_fail_open_when_data_engine_unreachable(self):
        fbm._fetch_fundamentals_bullish = lambda: {}
        result = fbm.run_daily_check()
        self.assertEqual(result["checked"], 0)
        self.assertEqual(result["newly_flagged"], [])
        self.assertEqual(self.sent_messages, [])

    def test_creates_table_and_writes_one_row_per_ticker(self):
        fbm._fetch_fundamentals_bullish = lambda: {
            "IBM": _result(f_score=3),
            "MSFT": _result(f_score=9, flag=True),
        }
        fbm.run_daily_check()

        conn = sqlite3.connect(self.db_path)
        rows = conn.execute(
            "SELECT ticker, high_quality_flag FROM fundamentals_bullish_status ORDER BY ticker"
        ).fetchall()
        conn.close()
        self.assertEqual(rows, [("IBM", 0), ("MSFT", 1)])

    def test_alerts_only_on_newly_flagged_ticker(self):
        fbm._fetch_fundamentals_bullish = lambda: {"MSFT": _result(f_score=9, flag=True)}
        result = fbm.run_daily_check()
        self.assertEqual(len(result["newly_flagged"]), 1)
        self.assertEqual(len(self.sent_messages), 1)
        self.assertIn("MSFT", self.sent_messages[0])
        self.assertIn("Surface-only", self.sent_messages[0])

    def test_no_repeat_alert_on_persistent_flag(self):
        fbm._fetch_fundamentals_bullish = lambda: {"MSFT": _result(f_score=9, flag=True)}
        fbm.run_daily_check()
        self.sent_messages.clear()

        # Same ticker still flagged on the next run — should not re-alert.
        fbm.run_daily_check()
        self.assertEqual(self.sent_messages, [])

    def test_no_alert_when_nothing_flagged(self):
        fbm._fetch_fundamentals_bullish = lambda: {"IBM": _result(f_score=3)}
        result = fbm.run_daily_check()
        self.assertEqual(result["newly_flagged"], [])
        self.assertEqual(self.sent_messages, [])

    def test_score_drops_then_reflags_alerts_again(self):
        fbm._fetch_fundamentals_bullish = lambda: {"MSFT": _result(f_score=9, flag=True)}
        fbm.run_daily_check()
        self.sent_messages.clear()

        # Score drops below the flag threshold — no alert either way.
        fbm._fetch_fundamentals_bullish = lambda: {"MSFT": _result(f_score=6, flag=False)}
        fbm.run_daily_check()
        self.assertEqual(self.sent_messages, [])

        # Flags again later — this IS a new transition, should alert again.
        fbm._fetch_fundamentals_bullish = lambda: {"MSFT": _result(f_score=8, flag=True)}
        fbm.run_daily_check()
        self.assertEqual(len(self.sent_messages), 1)


if __name__ == "__main__":
    unittest.main()
