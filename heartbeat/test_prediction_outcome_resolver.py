"""
Unit tests for prediction_outcome_resolver.py. Price lookups
(_closest_close_on_or_after, imported from signal_outcome_resolver) are
monkeypatched — no live Alpaca calls, same convention as
test_alpha_decay_monitor.py's DB_PATH override.

Run: python3 test_prediction_outcome_resolver.py
"""
import os
import sqlite3
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

import prediction_outcome_resolver as por


def _insert_prediction(conn, source_type, ticker, direction, confidence, days_ago):
    logged_at = int((datetime.now(timezone.utc) - timedelta(days=days_ago)).timestamp())
    conn.execute(
        "INSERT INTO predictions_log (source_type, source_ref_id, ticker, direction, confidence, logged_at, detail) "
        "VALUES (?, NULL, ?, ?, ?, ?, '')",
        (source_type, ticker, direction, confidence, logged_at),
    )
    conn.commit()


class TestResolvePredictionOutcomes(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self._orig_db_path = por.DB_PATH
        por.DB_PATH = self.db_path

        self._orig_price_lookup = por._closest_close_on_or_after
        self.prices = {}  # (ticker, date) -> price
        por._closest_close_on_or_after = lambda ticker, date: self.prices.get((ticker, date))

        self._orig_ohlc = por._fetch_daily_ohlc
        por._fetch_daily_ohlc = lambda ticker, start, end: []  # plan tests override

    def tearDown(self):
        por.DB_PATH = self._orig_db_path
        por._closest_close_on_or_after = self._orig_price_lookup
        por._fetch_daily_ohlc = self._orig_ohlc
        os.remove(self.db_path)

    def test_resolves_correct_direction(self):
        conn = por._connect()
        _insert_prediction(conn, "ws1_contradiction", "AAPL", "BULLISH", 0.8, days_ago=10)
        logged_date = (datetime.now(timezone.utc) - timedelta(days=10)).date()
        checkpoint_date = logged_date + timedelta(days=1)
        self.prices[("AAPL", logged_date.isoformat())] = 100.0
        self.prices[("AAPL", checkpoint_date.isoformat())] = 105.0
        conn.close()

        result = por.resolve_prediction_outcomes()
        self.assertGreater(result["resolved"], 0)

        conn = sqlite3.connect(self.db_path)
        row = conn.execute(
            "SELECT direction_correct, move_pct FROM prediction_outcomes "
            "WHERE source_type = 'ws1_contradiction' AND checkpoint_label = 'T+1'"
        ).fetchone()
        conn.close()
        self.assertEqual(row[0], 1)
        self.assertAlmostEqual(row[1], 5.0)

    def test_resolves_incorrect_direction(self):
        conn = por._connect()
        _insert_prediction(conn, "skeptic_altmacro", "XOM", "BULLISH", 0.6, days_ago=10)
        logged_date = (datetime.now(timezone.utc) - timedelta(days=10)).date()
        checkpoint_date = logged_date + timedelta(days=1)
        self.prices[("XOM", logged_date.isoformat())] = 100.0
        self.prices[("XOM", checkpoint_date.isoformat())] = 95.0
        conn.close()

        por.resolve_prediction_outcomes()
        conn = sqlite3.connect(self.db_path)
        row = conn.execute(
            "SELECT direction_correct FROM prediction_outcomes "
            "WHERE source_type = 'skeptic_altmacro' AND checkpoint_label = 'T+1'"
        ).fetchone()
        conn.close()
        self.assertEqual(row[0], 0)

    def test_not_yet_due_checkpoint_is_skipped(self):
        conn = por._connect()
        _insert_prediction(conn, "ws1_contradiction", "AAPL", "BULLISH", 0.8, days_ago=1)
        conn.close()

        result = por.resolve_prediction_outcomes()
        # days_ago=1 means only T+1 could possibly be due; T+5/7/10/30 are not.
        conn = sqlite3.connect(self.db_path)
        count = conn.execute("SELECT COUNT(*) FROM prediction_outcomes").fetchone()[0]
        conn.close()
        self.assertEqual(count, 0)  # no price data seeded, so even T+1 fails open

    def test_missing_price_data_does_not_resolve(self):
        conn = por._connect()
        _insert_prediction(conn, "ws1_contradiction", "UNKNOWN", "BULLISH", 0.8, days_ago=10)
        conn.close()
        # No prices seeded for UNKNOWN — _closest_close_on_or_after returns None.

        result = por.resolve_prediction_outcomes()
        self.assertEqual(result["resolved"], 0)

    def test_non_equity_asset_class_is_skipped(self):
        # audit §1 C3: a FUTURES prediction must NOT be priced against equity bars.
        conn = por._connect()
        logged_at = int((datetime.now(timezone.utc) - timedelta(days=10)).timestamp())
        conn.execute(
            "INSERT INTO predictions_log (source_type, source_ref_id, ticker, direction, "
            "confidence, logged_at, detail, asset_class) VALUES "
            "('personal_signal', NULL, 'CL', 'BEARISH', NULL, ?, 'SHORT_OIL', 'FUTURES')",
            (logged_at,),
        )
        conn.commit()
        # Seed an equity price for CL (Colgate-Palmolive) — if the resolver
        # wrongly used it, we'd get a resolved row. It must not.
        logged_date = (datetime.now(timezone.utc) - timedelta(days=10)).date()
        self.prices[("CL", logged_date.isoformat())] = 80.0
        self.prices[("CL", (logged_date + timedelta(days=1)).isoformat())] = 85.0
        conn.close()

        result = por.resolve_prediction_outcomes()
        self.assertEqual(result["resolved"], 0)
        self.assertGreater(result["skipped_non_equity"], 0)
        conn = sqlite3.connect(self.db_path)
        count = conn.execute("SELECT COUNT(*) FROM prediction_outcomes").fetchone()[0]
        conn.close()
        self.assertEqual(count, 0)

    def _seed_personal_plan(self, ticker, direction, entry, target, stop, days_ago):
        conn = por._connect()
        conn.execute(
            "CREATE TABLE IF NOT EXISTS personal_signals ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, created_at DATETIME, ticker TEXT, "
            "strategy TEXT, direction TEXT, entry_level REAL, target REAL, stop_loss REAL, "
            "thesis TEXT, status TEXT DEFAULT 'open', market_price_at_log REAL, asset_class TEXT)"
        )
        cur = conn.execute(
            "INSERT INTO personal_signals (ticker, direction, entry_level, target, stop_loss, asset_class) "
            "VALUES (?, ?, ?, ?, ?, 'EQUITY')",
            (ticker, direction, entry, target, stop),
        )
        sig_id = cur.lastrowid
        logged_at = int((datetime.now(timezone.utc) - timedelta(days=days_ago)).timestamp())
        conn.execute(
            "INSERT INTO predictions_log (source_type, source_ref_id, ticker, direction, "
            "confidence, logged_at, detail, asset_class) VALUES "
            "('personal_signal', ?, ?, ?, NULL, ?, '', 'EQUITY')",
            (sig_id, ticker, direction, logged_at),
        )
        conn.commit()
        conn.close()
        return sig_id

    def test_plan_target_hit_is_a_win(self):
        # audit §1 C4: LONG plan, entry triggered then target reached first.
        self._seed_personal_plan("AAPL", "BULLISH", entry=100.0, target=110.0, stop=95.0, days_ago=10)
        logged_date = (datetime.now(timezone.utc) - timedelta(days=10)).date()
        # bar day 0 brackets entry (100), day 1 tags target high (110)
        ohlc = [
            (logged_date.isoformat(), 101.0, 99.0, 100.5),
            ((logged_date + timedelta(days=1)).isoformat(), 111.0, 100.0, 108.0),
        ]
        por._fetch_daily_ohlc = lambda t, s, e: ohlc

        por.resolve_prediction_outcomes()
        conn = sqlite3.connect(self.db_path)
        row = conn.execute(
            "SELECT direction_correct, entry_price, checkpoint_price FROM prediction_outcomes "
            "WHERE checkpoint_label = 'T+1'"
        ).fetchone()
        conn.close()
        self.assertEqual(row[0], 1)
        self.assertEqual(row[1], 100.0)
        self.assertEqual(row[2], 110.0)

    def test_plan_stop_hit_before_recovery_is_a_loss(self):
        # audit §1 C4: stopped out at -8% before recovering must count as a LOSS,
        # not a win (the exact defect called out).
        self._seed_personal_plan("MSFT", "BULLISH", entry=100.0, target=110.0, stop=95.0, days_ago=10)
        logged_date = (datetime.now(timezone.utc) - timedelta(days=10)).date()
        ohlc = [
            (logged_date.isoformat(), 101.0, 99.0, 100.5),
            ((logged_date + timedelta(days=1)).isoformat(), 100.0, 94.0, 98.0),  # stop touched
        ]
        por._fetch_daily_ohlc = lambda t, s, e: ohlc

        por.resolve_prediction_outcomes()
        conn = sqlite3.connect(self.db_path)
        row = conn.execute(
            "SELECT direction_correct FROM prediction_outcomes WHERE checkpoint_label = 'T+1'"
        ).fetchone()
        conn.close()
        self.assertEqual(row[0], 0)

    def test_plan_untriggered_entry_not_scored(self):
        # audit §1 C4: a limit entry that never fills is not a trade — recorded
        # with direction_correct=NULL so it's out of the hit rate.
        self._seed_personal_plan("NVDA", "BULLISH", entry=100.0, target=110.0, stop=95.0, days_ago=10)
        logged_date = (datetime.now(timezone.utc) - timedelta(days=10)).date()
        # price stays well above the limit — never brackets 100.0
        ohlc = [
            (logged_date.isoformat(), 130.0, 125.0, 128.0),
            ((logged_date + timedelta(days=1)).isoformat(), 135.0, 129.0, 132.0),
        ]
        por._fetch_daily_ohlc = lambda t, s, e: ohlc

        por.resolve_prediction_outcomes()
        conn = sqlite3.connect(self.db_path)
        row = conn.execute(
            "SELECT direction_correct FROM prediction_outcomes WHERE checkpoint_label = 'T+1'"
        ).fetchone()
        conn.close()
        self.assertIsNotNone(row)          # recorded (won't re-scan forever)
        self.assertIsNone(row[0])          # but not counted as a hit or miss

    def test_dedup_via_unique_constraint(self):
        conn = por._connect()
        _insert_prediction(conn, "ws1_contradiction", "AAPL", "BULLISH", 0.8, days_ago=10)
        logged_date = (datetime.now(timezone.utc) - timedelta(days=10)).date()
        checkpoint_date = logged_date + timedelta(days=1)
        self.prices[("AAPL", logged_date.isoformat())] = 100.0
        self.prices[("AAPL", checkpoint_date.isoformat())] = 105.0
        conn.close()

        first = por.resolve_prediction_outcomes()
        second = por.resolve_prediction_outcomes()
        self.assertGreater(first["resolved"], 0)
        self.assertEqual(second["resolved"], 0)  # already resolved — no duplicate rows


if __name__ == "__main__":
    unittest.main()
