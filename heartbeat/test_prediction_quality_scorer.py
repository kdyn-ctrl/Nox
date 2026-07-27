"""
Unit tests for prediction_quality_scorer.py — pure calibration math
(compute_rollup_for_source) plus the sqlite read/write/join paths.

Run: python3 test_prediction_quality_scorer.py
"""
import os
import sqlite3
import tempfile
import unittest

import prediction_quality_scorer as pqs


class TestComputeRollupForSource(unittest.TestCase):
    def test_insufficient_sample_reports_none(self):
        rows = [(1, 0.8, 5.0)] * (pqs.MIN_SAMPLE_SIZE - 1)
        result = pqs.compute_rollup_for_source(rows)
        self.assertFalse(result["sufficient_data"])
        self.assertIsNone(result["hit_rate"])

    def test_hit_rate_and_quality_score(self):
        # 4 correct (avg |move| = 5), 1 wrong (|move| = 3) — n=5 meets MIN_SAMPLE_SIZE.
        rows = [(1, 0.8, 5.0), (1, 0.8, 5.0), (1, 0.8, 5.0), (1, 0.8, 5.0), (0, 0.8, -3.0)]
        result = pqs.compute_rollup_for_source(rows)
        self.assertTrue(result["sufficient_data"])
        self.assertAlmostEqual(result["hit_rate"], 0.8)
        self.assertAlmostEqual(result["avg_move_pct_when_correct"], 5.0)
        self.assertAlmostEqual(result["avg_move_pct_when_wrong"], 3.0)
        self.assertAlmostEqual(result["quality_score"], 0.8 * 5.0)

    def test_confidence_calibration_positive_when_high_confidence_hits_more(self):
        high = [(1, 0.9, 5.0)] * 5  # all correct at high confidence
        low = [(0, 0.1, 5.0)] * 5   # all wrong at low confidence
        result = pqs.compute_rollup_for_source(high + low)
        self.assertIsNotNone(result["confidence_calibration"])
        self.assertGreater(result["confidence_calibration"], 0)

    def test_confidence_calibration_none_when_no_confidence_data(self):
        rows = [(1, None, 5.0)] * 5 + [(0, None, -3.0)] * 5
        result = pqs.compute_rollup_for_source(rows)
        self.assertIsNone(result["avg_confidence"])
        self.assertIsNone(result["confidence_calibration"])

    def test_all_correct_has_no_wrong_average(self):
        rows = [(1, 0.7, 4.0)] * 6
        result = pqs.compute_rollup_for_source(rows)
        self.assertIsNone(result["avg_move_pct_when_wrong"])
        self.assertIsNotNone(result["avg_move_pct_when_correct"])


class TestRunRollupPersistence(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self._orig_db_path = pqs.DB_PATH
        pqs.DB_PATH = self.db_path

    def tearDown(self):
        pqs.DB_PATH = self._orig_db_path
        os.remove(self.db_path)

    def _seed_prediction_outcomes(self, source_type, checkpoint_label, n_correct, n_wrong):
        conn = sqlite3.connect(self.db_path)
        conn.execute(
            "CREATE TABLE IF NOT EXISTS prediction_outcomes ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, resolved_at TEXT, source_type TEXT, "
            "source_ref_id INTEGER, ticker TEXT, direction TEXT, checkpoint_label TEXT, "
            "confidence REAL, entry_price REAL, checkpoint_price REAL, move_pct REAL, direction_correct INTEGER)"
        )
        for i in range(n_correct):
            conn.execute(
                "INSERT INTO prediction_outcomes (resolved_at, source_type, source_ref_id, ticker, "
                "direction, checkpoint_label, confidence, move_pct, direction_correct) "
                "VALUES ('now', ?, ?, 'T', 'BULLISH', ?, 0.7, 4.0, 1)",
                (source_type, i, checkpoint_label),
            )
        for i in range(n_wrong):
            conn.execute(
                "INSERT INTO prediction_outcomes (resolved_at, source_type, source_ref_id, ticker, "
                "direction, checkpoint_label, confidence, move_pct, direction_correct) "
                "VALUES ('now', ?, ?, 'T', 'BULLISH', ?, 0.3, -2.0, 0)",
                (source_type, 1000 + i, checkpoint_label),
            )
        conn.commit()
        conn.close()

    def test_writes_rollup_row_when_sufficient(self):
        self._seed_prediction_outcomes("ws1_contradiction", "T+7", n_correct=4, n_wrong=2)
        pqs.run_rollup()

        conn = sqlite3.connect(self.db_path)
        rows = conn.execute(
            "SELECT n, hit_rate FROM prediction_quality_rollup "
            "WHERE source_type = 'ws1_contradiction' AND window_days = 7"
        ).fetchall()
        conn.close()
        self.assertEqual(len(rows), 1)
        n, hit_rate = rows[0]
        self.assertEqual(n, 6)
        self.assertAlmostEqual(hit_rate, 4 / 6)

    def test_skips_write_when_insufficient(self):
        self._seed_prediction_outcomes("skeptic_insider", "T+30", n_correct=1, n_wrong=1)
        pqs.run_rollup()

        conn = sqlite3.connect(self.db_path)
        rows = conn.execute(
            "SELECT COUNT(*) FROM prediction_quality_rollup WHERE source_type = 'skeptic_insider'"
        ).fetchone()[0]
        conn.close()
        self.assertEqual(rows, 0)

    def test_format_quality_report_handles_no_data(self):
        text = pqs.format_quality_report({"ws1_contradiction": {
            "weekly": {"sufficient_data": False}, "monthly": {"sufficient_data": False},
        }})
        self.assertIn("No source has enough resolved predictions yet.", text)

    def test_format_quality_report_renders_hit_rate(self):
        text = pqs.format_quality_report({"ws1_contradiction": {
            "weekly": {"sufficient_data": True, "hit_rate": 0.75, "n": 8, "confidence_calibration": 0.2},
            "monthly": {"sufficient_data": False},
        }})
        self.assertIn("75%", text)
        self.assertIn("n=8", text)


if __name__ == "__main__":
    unittest.main()
