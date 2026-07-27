"""
Unit tests for fundamentals_risk_monitor's sqlite persistence and
new-flag-only alert logic. No network access — _fetch_fundamentals and
_send_telegram are monkeypatched, same convention as
test_alpha_decay_monitor.py's DB_PATH override.

Run: python3 test_fundamentals_risk_monitor.py
"""
import os
import sqlite3
import tempfile
import unittest

import fundamentals_risk_monitor as frm


def _result(m_score=None, beneish_flag=False, runway_quarters=None, fcf_flag=False, data_quality="OK"):
    return {
        "beneish": {"m_score": m_score},
        "fcf_runway": {"runway_quarters": runway_quarters},
        "flags": {"beneish_manipulation_risk": beneish_flag, "fcf_burn_risk": fcf_flag},
        "data_quality": data_quality,
    }


class TestRunDailyCheck(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self._orig_db_path = frm.DB_PATH
        frm.DB_PATH = self.db_path

        self._orig_fetch = frm._fetch_fundamentals
        self._orig_telegram = frm._send_telegram
        self.sent_messages = []
        frm._send_telegram = lambda msg: self.sent_messages.append(msg)

    def tearDown(self):
        frm.DB_PATH = self._orig_db_path
        frm._fetch_fundamentals = self._orig_fetch
        frm._send_telegram = self._orig_telegram
        os.remove(self.db_path)

    def test_fail_open_when_data_engine_unreachable(self):
        frm._fetch_fundamentals = lambda: {}
        result = frm.run_daily_check()
        self.assertEqual(result["checked"], 0)
        self.assertEqual(result["newly_flagged"], [])
        self.assertEqual(self.sent_messages, [])

    def test_creates_table_and_writes_one_row_per_ticker(self):
        frm._fetch_fundamentals = lambda: {
            "AAPL": _result(m_score=-2.5),
            "LCID": _result(runway_quarters=2.0, fcf_flag=True),
        }
        frm.run_daily_check()

        conn = sqlite3.connect(self.db_path)
        rows = conn.execute("SELECT ticker, fcf_flag FROM fundamentals_risk_status ORDER BY ticker").fetchall()
        conn.close()
        self.assertEqual(rows, [("AAPL", 0), ("LCID", 1)])

    def test_alerts_only_on_newly_flagged_ticker(self):
        frm._fetch_fundamentals = lambda: {"LCID": _result(runway_quarters=2.0, fcf_flag=True)}
        result = frm.run_daily_check()
        self.assertEqual(len(result["newly_flagged"]), 1)
        self.assertEqual(len(self.sent_messages), 1)
        self.assertIn("LCID", self.sent_messages[0])
        self.assertIn("Surface-only", self.sent_messages[0])

    def test_no_repeat_alert_on_persistent_flag(self):
        frm._fetch_fundamentals = lambda: {"LCID": _result(runway_quarters=2.0, fcf_flag=True)}
        frm.run_daily_check()
        self.sent_messages.clear()

        # Same ticker still flagged on the next run — should not re-alert.
        frm.run_daily_check()
        self.assertEqual(self.sent_messages, [])

    def test_no_alert_when_nothing_flagged(self):
        frm._fetch_fundamentals = lambda: {"AAPL": _result(m_score=-2.5)}
        result = frm.run_daily_check()
        self.assertEqual(result["newly_flagged"], [])
        self.assertEqual(self.sent_messages, [])

    def test_recovers_then_reflags_alerts_again(self):
        frm._fetch_fundamentals = lambda: {"LCID": _result(runway_quarters=2.0, fcf_flag=True)}
        frm.run_daily_check()
        self.sent_messages.clear()

        # Ticker recovers (no longer flagged) — no alert either way.
        frm._fetch_fundamentals = lambda: {"LCID": _result(runway_quarters=10.0, fcf_flag=False)}
        frm.run_daily_check()
        self.assertEqual(self.sent_messages, [])

        # Flags again later — this IS a new transition, should alert again.
        frm._fetch_fundamentals = lambda: {"LCID": _result(runway_quarters=1.5, fcf_flag=True)}
        frm.run_daily_check()
        self.assertEqual(len(self.sent_messages), 1)


if __name__ == "__main__":
    unittest.main()
