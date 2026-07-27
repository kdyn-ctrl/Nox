"""
Unit tests for alpha_decay_monitor's pure math and sqlite read/write paths.
No network access needed — everything here is deterministic sqlite fixtures,
since a wrong Sharpe calc or a mis-wired degraded_pct threshold would
silently mis-size every subsequent options order via the C++ AlphaDecayStore.

Run: python3 test_alpha_decay_monitor.py
"""

import os
import sqlite3
import tempfile
import unittest

import alpha_decay_monitor as adm


def _seed_daily_ledger(db_path, daily_totals):
    """daily_totals: [(date_str, total_pnl_that_day), ...]. Writes one
    synthetic daily_ledger row per day whose realized+unrealized sums to the
    requested total (matches the real schema's columns closely enough for
    the SUM(realized_pnl + unrealized_pnl) GROUP BY date query under test)."""
    conn = sqlite3.connect(db_path)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS daily_ledger ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, date TEXT NOT NULL, ticker TEXT NOT NULL, "
        "asset_class TEXT NOT NULL, detail TEXT NOT NULL DEFAULT '', quantity REAL, "
        "entry_price REAL, mark_price REAL, unrealized_pnl REAL DEFAULT 0, "
        "realized_pnl REAL DEFAULT 0, updated_at INTEGER NOT NULL)"
    )
    for date_str, total in daily_totals:
        conn.execute(
            "INSERT INTO daily_ledger (date, ticker, asset_class, quantity, entry_price, "
            "mark_price, unrealized_pnl, realized_pnl, updated_at) "
            "VALUES (?, 'TEST', 'option', 1, 1, 1, 0, ?, 0)",
            (date_str, total),
        )
    conn.commit()
    conn.close()


def _dates(n):
    # Deterministic ascending fake dates — the monitor only cares about sort
    # order, not real calendar semantics.
    return [f"2025-{(i // 28) + 1:02d}-{(i % 28) + 1:02d}" for i in range(n)]


class TestSharpeCalc(unittest.TestCase):
    def test_zero_variance_returns_zero_not_nan(self):
        self.assertEqual(adm._sharpe([5.0, 5.0, 5.0]), 0.0)

    def test_single_point_returns_zero(self):
        self.assertEqual(adm._sharpe([5.0]), 0.0)
        self.assertEqual(adm._sharpe([]), 0.0)

    def test_positive_mean_positive_sharpe(self):
        # Steady small positive daily P&L, some noise — Sharpe should be > 0.
        returns = [10.0, -2.0, 8.0, 5.0, -1.0, 12.0, 3.0]
        self.assertGreater(adm._sharpe(returns), 0.0)

    def test_negative_mean_negative_sharpe(self):
        returns = [-10.0, 2.0, -8.0, -5.0, 1.0, -12.0, -3.0]
        self.assertLess(adm._sharpe(returns), 0.0)


class TestDailyReturns(unittest.TestCase):
    def test_diffs_consecutive_totals(self):
        totals = [("d1", 100.0), ("d2", 150.0), ("d3", 120.0)]
        self.assertEqual(adm._daily_returns(totals), [50.0, -30.0])

    def test_empty_and_single_row_produce_no_returns(self):
        self.assertEqual(adm._daily_returns([]), [])
        self.assertEqual(adm._daily_returns([("d1", 100.0)]), [])


class TestComputeAlphaDecay(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)

    def tearDown(self):
        os.remove(self.db_path)

    def test_insufficient_history_is_flagged_not_computed(self):
        dates = _dates(10)
        _seed_daily_ledger(self.db_path, [(d, float(i)) for i, d in enumerate(dates)])
        conn = sqlite3.connect(self.db_path)
        result = adm.compute_alpha_decay(conn)
        conn.close()
        self.assertFalse(result["sufficient_data"])
        self.assertIsNone(result["rolling_sharpe_30d"])
        self.assertEqual(result["tier_multiplier"], 1.0)

    def test_stable_performance_does_not_trigger(self):
        # 300 days of a steady, mildly noisy positive drift — rolling and
        # baseline Sharpe should be close, well under the 20% threshold.
        n = 300
        totals = [(d, i * 10.0 + (5.0 if i % 2 == 0 else -3.0)) for i, d in enumerate(_dates(n))]
        _seed_daily_ledger(self.db_path, totals)
        conn = sqlite3.connect(self.db_path)
        result = adm.compute_alpha_decay(conn)
        conn.close()
        self.assertTrue(result["sufficient_data"])
        self.assertFalse(result["triggered"])
        self.assertEqual(result["tier_multiplier"], 1.0)

    def test_recent_degradation_triggers_tier_down(self):
        # Baseline (first ~270 days): strong steady uptrend → high positive Sharpe.
        # Most recent 30 days: flat/choppy with no net drift → near-zero Sharpe.
        # That's a severe (>20%) degradation of a genuinely positive baseline.
        n = 300
        totals = []
        cum = 0.0
        for i in range(n):
            if i < n - 30:
                cum += 20.0  # steady strong uptrend
            else:
                cum += 20.0 if i % 2 == 0 else -20.0  # flat/choppy, no net drift
            totals.append((_dates(n)[i], cum))
        _seed_daily_ledger(self.db_path, totals)

        conn = sqlite3.connect(self.db_path)
        result = adm.compute_alpha_decay(conn)
        conn.close()
        self.assertTrue(result["sufficient_data"])
        self.assertGreater(result["baseline_sharpe_12mo"], 0.0)
        self.assertTrue(result["triggered"], f"expected trigger, got {result}")
        self.assertEqual(result["tier_multiplier"], adm.TIERED_DOWN_MULTIPLIER)

    def test_negative_baseline_never_triggers(self):
        # A baseline that was already losing money isn't the "decay" scenario
        # this monitor targets — degraded_pct is only meaningful vs a positive
        # baseline, so a negative baseline must never trigger a tier-down.
        n = 300
        cum = 0.0
        totals = []
        for i, d in enumerate(_dates(n)):
            cum += -5.0 if i % 2 == 0 else -3.0  # noisy negative drift
            totals.append((d, cum))
        _seed_daily_ledger(self.db_path, totals)
        conn = sqlite3.connect(self.db_path)
        result = adm.compute_alpha_decay(conn)
        conn.close()
        self.assertLess(result["baseline_sharpe_12mo"], 0.0)
        self.assertFalse(result["triggered"])
        self.assertIsNone(result["degraded_pct"])


class TestRunDailyCheckPersistence(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self._orig_db_path = adm.DB_PATH
        adm.DB_PATH = self.db_path

    def tearDown(self):
        adm.DB_PATH = self._orig_db_path
        os.remove(self.db_path)

    def test_writes_a_row_when_data_is_sufficient(self):
        n = 300
        totals = [(d, float(i)) for i, d in enumerate(_dates(n))]
        _seed_daily_ledger(self.db_path, totals)

        adm.run_daily_check()

        conn = sqlite3.connect(self.db_path)
        rows = conn.execute("SELECT COUNT(*) FROM alpha_decay_status").fetchone()[0]
        conn.close()
        self.assertEqual(rows, 1)

    def test_skips_write_when_data_insufficient(self):
        totals = [(d, float(i)) for i, d in enumerate(_dates(5))]
        _seed_daily_ledger(self.db_path, totals)

        adm.run_daily_check()

        conn = sqlite3.connect(self.db_path)
        rows = conn.execute(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='alpha_decay_status'"
        ).fetchone()[0]
        conn.close()
        # Table gets created by _connect() regardless, but no row should land.
        if rows:
            count = sqlite3.connect(self.db_path).execute(
                "SELECT COUNT(*) FROM alpha_decay_status"
            ).fetchone()[0]
            self.assertEqual(count, 0)


if __name__ == "__main__":
    unittest.main()
