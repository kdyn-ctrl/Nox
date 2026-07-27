"""Tests for main.py's _build_fundamentals_universe() — the broad-universe
ticker source for the market-wide fundamentals-risk (Beneish/FCF) scan.
Decoupled from WATCHLIST; verifies the price filter, the SEC-fair-use ticker
cap (and that a truncation logs its drop count), and the fail-open fallback
to WATCHLIST when Alpaca's asset list comes back empty."""
import os
import unittest
from unittest.mock import patch

os.environ.setdefault("ALPACA_API_KEY", "test")
os.environ.setdefault("ALPACA_SECRET_KEY", "test")
os.environ.setdefault("WEBHOOK_SECRET_TOKEN", "test")

import main  # noqa: E402


class TestBuildFundamentalsUniverse(unittest.TestCase):
    def test_falls_back_to_watchlist_when_alpaca_universe_empty(self):
        with patch("main.fetch_tradable_universe", return_value=[]):
            self.assertEqual(main._build_fundamentals_universe(), main.WATCHLIST)

    def test_filters_out_tickers_below_min_price(self):
        universe = ["AAPL", "PENNY"]
        prices = {"AAPL": 200.0, "PENNY": 0.50}
        with patch("main.fetch_tradable_universe", return_value=universe), \
             patch("main.fetch_price_snapshots", return_value=prices), \
             patch("main.FUNDAMENTALS_UNIVERSE_MIN_PRICE", 5.0), \
             patch("main.FUNDAMENTALS_UNIVERSE_MAX_TICKERS", 4000):
            result = main._build_fundamentals_universe()
        self.assertEqual(result, ["AAPL"])

    def test_ticker_missing_from_snapshots_is_excluded(self):
        universe = ["AAPL", "NOSNAP"]
        prices = {"AAPL": 200.0}  # NOSNAP absent — treated as unpriced, excluded
        with patch("main.fetch_tradable_universe", return_value=universe), \
             patch("main.fetch_price_snapshots", return_value=prices), \
             patch("main.FUNDAMENTALS_UNIVERSE_MIN_PRICE", 5.0), \
             patch("main.FUNDAMENTALS_UNIVERSE_MAX_TICKERS", 4000):
            result = main._build_fundamentals_universe()
        self.assertEqual(result, ["AAPL"])

    def test_caps_at_max_tickers(self):
        universe = [f"T{i}" for i in range(10)]
        prices = {t: 100.0 for t in universe}
        with patch("main.fetch_tradable_universe", return_value=universe), \
             patch("main.fetch_price_snapshots", return_value=prices), \
             patch("main.FUNDAMENTALS_UNIVERSE_MIN_PRICE", 5.0), \
             patch("main.FUNDAMENTALS_UNIVERSE_MAX_TICKERS", 3):
            result = main._build_fundamentals_universe()
        self.assertEqual(len(result), 3)
        self.assertEqual(result, universe[:3])

    def test_cap_truncation_is_logged_not_silent(self):
        universe = [f"T{i}" for i in range(10)]
        prices = {t: 100.0 for t in universe}
        with patch("main.fetch_tradable_universe", return_value=universe), \
             patch("main.fetch_price_snapshots", return_value=prices), \
             patch("main.FUNDAMENTALS_UNIVERSE_MIN_PRICE", 5.0), \
             patch("main.FUNDAMENTALS_UNIVERSE_MAX_TICKERS", 3), \
             patch("builtins.print") as mock_print:
            main._build_fundamentals_universe()
        logged = " ".join(str(c.args[0]) for c in mock_print.call_args_list)
        self.assertIn("capped", logged)
        self.assertIn("7 dropped", logged)


class TestRefreshFundamentalsCacheSuccessFlag(unittest.TestCase):
    """evaluate_fundamental_risk() always includes a "fetch_error" key (None
    on success, a string on failure) — checking key ABSENCE for "did anything
    succeed" is always False regardless of real outcome. Caught 2026-07-16 in
    production: a scan where every ticker actually resolved still logged
    "failed for every ticker" and never stamped last_fundamentals_update."""

    def setUp(self):
        main._CACHE["last_fundamentals_update"] = None
        main._CACHE["fundamentals"] = {}

    def test_stamps_update_when_at_least_one_ticker_succeeds(self):
        with patch("main._build_fundamentals_universe", return_value=["AAPL", "BAD"]), \
             patch("main.get_fundamentals_raw", return_value={}), \
             patch("main.evaluate_fundamental_risk", side_effect=[
                 {"ticker": "AAPL", "fetch_error": None},
                 {"ticker": "BAD", "fetch_error": "cik_not_found"},
             ]), \
             patch("main.time.sleep"):
            main._refresh_fundamentals_cache()
        self.assertIsNotNone(main._CACHE["last_fundamentals_update"])

    def test_does_not_stamp_update_when_every_ticker_fails(self):
        with patch("main._build_fundamentals_universe", return_value=["AAPL", "BAD"]), \
             patch("main.get_fundamentals_raw", return_value={}), \
             patch("main.evaluate_fundamental_risk", side_effect=[
                 {"ticker": "AAPL", "fetch_error": "cik_not_found"},
                 {"ticker": "BAD", "fetch_error": "cik_not_found"},
             ]), \
             patch("main.time.sleep"):
            main._refresh_fundamentals_cache()
        self.assertIsNone(main._CACHE["last_fundamentals_update"])


if __name__ == "__main__":
    unittest.main()
