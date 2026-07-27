"""
Tests for telemetry_watchdog.py — verifies the mock-data fabrication found in
the 2026-07-26 commit is actually gone: options are skipped (not priced with
random.choice()), earnings-check is an honest no-op, and a missing live
quote skips a position instead of alerting on garbage.
"""

import os
import sqlite3
import tempfile
import unittest

import telemetry_watchdog as watchdog
import ibkr_quotes


class TestOptionPositionDetection(unittest.TestCase):

    def test_asset_class_option_detected(self):
        self.assertTrue(watchdog._is_option_position("OPTION", "LONG_CALL"))

    def test_strategy_name_detected_without_asset_class(self):
        self.assertTrue(watchdog._is_option_position(None, "bull_call_spread"))

    def test_equity_not_flagged(self):
        self.assertFalse(watchdog._is_option_position("EQUITY", None))
        self.assertFalse(watchdog._is_option_position(None, None))


class TestEarningsCheckIsNoOp(unittest.TestCase):

    def test_always_false(self):
        # No hardcoded True for any specific ticker — this used to return
        # True for "SHOP" unconditionally, which is exactly the kind of
        # invented signal RULE-D5 forbids from having alerting power.
        for ticker in ("SHOP", "AAPL", "NVDA", "MADE_UP_TICKER"):
            self.assertFalse(watchdog.check_earnings_date(ticker))


class TestIbkrQuotesGracefulDegradation(unittest.TestCase):

    def test_empty_ticker_list_returns_empty(self):
        self.assertEqual(ibkr_quotes.fetch_equity_quotes([]), {})

    def test_missing_ib_async_or_unreachable_gateway_returns_empty_not_raises(self):
        # In this test environment ib_async isn't installed, and even if it
        # were, no real IB Gateway is reachable — either way this must
        # degrade to {} rather than raise or return a placeholder price.
        result = ibkr_quotes.fetch_equity_quotes(["AAPL"])
        self.assertEqual(result, {})


class TestRunWatchdogSkipsUnpriceable(unittest.TestCase):

    def setUp(self):
        self.tmp_db = tempfile.NamedTemporaryFile(suffix=".db", delete=False)
        self.tmp_db_path = self.tmp_db.name
        self.tmp_db.close()
        watchdog.DB_PATH = self.tmp_db_path

        with sqlite3.connect(self.tmp_db_path) as conn:
            conn.execute("""
                CREATE TABLE personal_trades (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    ticker TEXT, strategy TEXT, action TEXT, quantity REAL,
                    price REAL, executed_at TEXT, direction TEXT,
                    asset_class TEXT, closes_trade_id INTEGER
                )
            """)
            conn.execute(
                "INSERT INTO personal_trades (ticker, strategy, action, quantity, price, direction, asset_class) "
                "VALUES ('AAPL', 'SWING_LONG', 'BUY', 10, 100.0, 'LONG', 'EQUITY')"
            )
            conn.execute(
                "INSERT INTO personal_trades (ticker, strategy, action, quantity, price, direction, asset_class) "
                "VALUES ('NVDA', 'LONG_CALL', 'BUY', 2, 5.0, 'LONG', 'OPTION')"
            )
            conn.commit()

        self.alerts = []
        watchdog.dispatch_alert = lambda msg: self.alerts.append(msg)

    def tearDown(self):
        if os.path.exists(self.tmp_db_path):
            os.remove(self.tmp_db_path)

    def test_option_position_never_reaches_price_or_alert_logic(self):
        watchdog.get_current_prices = lambda tickers: {"AAPL": 100.5}
        watchdog.run_watchdog()
        # Only the equity trade could ever be evaluated; the option trade must
        # not have produced any alert (it has no real price source).
        for msg in self.alerts:
            self.assertNotIn("NVDA", msg)

    def test_missing_live_price_skips_without_alert_or_crash(self):
        watchdog.get_current_prices = lambda tickers: {}  # simulates gateway down
        watchdog.run_watchdog()  # must not raise
        self.assertEqual(self.alerts, [])

    def test_stop_loss_alert_uses_real_fetched_price_not_random(self):
        watchdog.get_current_prices = lambda tickers: {"AAPL": 80.0}  # -20% from entry 100
        watchdog.run_watchdog()
        self.assertEqual(len(self.alerts), 1)
        self.assertIn("Stop Loss Hit", self.alerts[0])
        self.assertIn("$80.00", self.alerts[0])


if __name__ == "__main__":
    unittest.main()
