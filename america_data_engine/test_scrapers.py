"""Tests for scrapers.py's Alpaca broad-universe helpers (fetch_tradable_universe,
fetch_price_snapshots) — used by the market-wide fundamentals-risk scan.
All network calls are mocked, per project convention."""
import os
import unittest
from unittest.mock import patch, MagicMock

os.environ.setdefault("ALPACA_API_KEY", "test")
os.environ.setdefault("ALPACA_SECRET_KEY", "test")

import scrapers  # noqa: E402


def _resp(status_code=200, json_body=None):
    m = MagicMock()
    m.status_code = status_code
    m.json.return_value = json_body if json_body is not None else {}
    return m


class TestFetchTradableUniverse(unittest.TestCase):
    def test_filters_to_alpha_only_symbols_at_most_5_chars(self):
        assets = [
            {"symbol": "AAPL"},
            {"symbol": "BRK.B"},   # non-alpha (dot) — warrant/class-share style, dropped
            {"symbol": "TOOLONG1"},  # >5 chars, dropped
            {"symbol": "F"},
        ]
        with patch("scrapers.fetch_with_retry", return_value=_resp(json_body=assets)):
            tickers = scrapers.fetch_tradable_universe()
        self.assertEqual(tickers, ["AAPL", "F"])

    def test_fetch_failure_returns_empty_list(self):
        with patch("scrapers.fetch_with_retry", return_value=None):
            self.assertEqual(scrapers.fetch_tradable_universe(), [])

    def test_non_200_returns_empty_list(self):
        with patch("scrapers.fetch_with_retry", return_value=_resp(status_code=500)):
            self.assertEqual(scrapers.fetch_tradable_universe(), [])

    def test_malformed_json_fails_open_to_empty_list(self):
        bad_resp = MagicMock()
        bad_resp.status_code = 200
        bad_resp.json.side_effect = ValueError("bad json")
        with patch("scrapers.fetch_with_retry", return_value=bad_resp):
            self.assertEqual(scrapers.fetch_tradable_universe(), [])


class TestFetchPriceSnapshots(unittest.TestCase):
    def test_prefers_latest_trade_price_over_daily_bar_close(self):
        snap = {"AAPL": {"latestTrade": {"p": 210.5}, "dailyBar": {"c": 200.0}}}
        with patch("scrapers.fetch_with_retry", return_value=_resp(json_body=snap)), \
             patch("scrapers.time.sleep"):
            prices = scrapers.fetch_price_snapshots(["AAPL"])
        self.assertEqual(prices["AAPL"], 210.5)

    def test_falls_back_to_daily_bar_close_when_no_latest_trade(self):
        snap = {"F": {"latestTrade": {}, "dailyBar": {"c": 12.3}}}
        with patch("scrapers.fetch_with_retry", return_value=_resp(json_body=snap)), \
             patch("scrapers.time.sleep"):
            prices = scrapers.fetch_price_snapshots(["F"])
        self.assertEqual(prices["F"], 12.3)

    def test_chunk_failure_is_skipped_not_raised(self):
        with patch("scrapers.fetch_with_retry", return_value=None), \
             patch("scrapers.time.sleep"):
            prices = scrapers.fetch_price_snapshots(["AAPL"])
        self.assertEqual(prices, {})

    def test_chunks_requests_at_300_symbols(self):
        tickers = [f"T{i}" for i in range(650)]
        with patch("scrapers.fetch_with_retry", return_value=_resp(json_body={})) as mock_fetch, \
             patch("scrapers.time.sleep"):
            scrapers.fetch_price_snapshots(tickers)
        self.assertEqual(mock_fetch.call_count, 3)  # 300 + 300 + 50


class TestFetchEarningsCalendar(unittest.TestCase):
    """
    2026-07-19: fetch_earnings_calendar switched from Alpaca's
    /v2/corporate-actions (confirmed live to 404/400 — Alpaca has no
    "earnings" corporate-action type at all) to Finnhub's /calendar/earnings,
    a whole-market call filtered client-side to the requested tickers.

    2026-07-22: confirmed live that a single 30-day-wide call silently
    truncates at Finnhub's ~1500-event free-tier cap, and drops near-term
    dates specifically (not just an arbitrary tail) — so the fetch is now
    chunked into 10-day slices (3 calls covering the 30-day horizon) instead
    of one wide call.
    """

    def setUp(self):
        self._patched_key = patch.object(scrapers, "FINNHUB_API_KEY", "test-key")
        self._patched_key.start()

    def tearDown(self):
        self._patched_key.stop()

    def test_no_api_key_fails_open_to_none_per_ticker(self):
        with patch.object(scrapers, "FINNHUB_API_KEY", ""):
            result = scrapers.fetch_earnings_calendar(["AAPL", "TSLA"])
        self.assertEqual(result, {"AAPL": None, "TSLA": None})

    def test_whole_call_failure_returns_none_per_ticker(self):
        with patch("scrapers.fetch_with_retry", return_value=None):
            result = scrapers.fetch_earnings_calendar(["AAPL", "TSLA"])
        self.assertEqual(result, {"AAPL": None, "TSLA": None})

    def test_chunked_calls_cover_whole_market_filtered_to_tickers(self):
        body = {"earningsCalendar": [
            {"symbol": "AAPL", "date": "2026-07-30", "hour": "amc", "quarter": 3, "year": 2026},
            {"symbol": "SMCI", "date": "2026-08-05", "hour": "bmo", "quarter": 4, "year": 2026},
            {"symbol": "UNRELATED_TICKER", "date": "2026-08-01", "quarter": 1, "year": 2026},
        ]}
        with patch("scrapers.fetch_with_retry", return_value=_resp(json_body=body)) as mock_fetch:
            result = scrapers.fetch_earnings_calendar(["AAPL", "SMCI", "TSLA"])
        # 30-day horizon / 3-day chunks = 10 requests, not one wide call (which
        # was confirmed live to truncate near-term dates past ~1500 events).
        self.assertEqual(mock_fetch.call_count, 10)
        # Same mocked body returned for every chunk, so each ticker's events
        # accumulate 10x — the important assertion is presence/shape, not count.
        self.assertEqual(
            result["AAPL"],
            [{"date": "2026-07-30", "description": "Q3 2026 earnings (amc)"}] * 10,
        )
        self.assertEqual(
            result["SMCI"],
            [{"date": "2026-08-05", "description": "Q4 2026 earnings (bmo)"}] * 10,
        )
        self.assertEqual(result["TSLA"], [])  # confirmed no earnings, not None — distinct from a fetch failure
        self.assertNotIn("UNRELATED_TICKER", result)  # filtered to only the requested tickers

    def test_near_term_chunk_failure_returns_none_even_if_later_chunks_succeed(self):
        """The near-term (offset=0) chunk is what Scout's 'reports TODAY/in
        Nd' section and the options engine's 5-day earnings gates depend on —
        its failure must not be masked by later, lower-stakes chunks succeeding."""
        good_body = {"earningsCalendar": [
            {"symbol": "AAPL", "date": "2026-08-15", "hour": "amc", "quarter": 3, "year": 2026},
        ]}
        responses = [None] + [_resp(json_body=good_body)] * 9
        with patch("scrapers.fetch_with_retry", side_effect=responses):
            result = scrapers.fetch_earnings_calendar(["AAPL"])
        self.assertEqual(result, {"AAPL": None})

    def test_later_chunk_failure_does_not_null_out_near_term_results(self):
        """A distant-horizon chunk failing (informational-only window) should
        not wipe out real near-term data the first chunk already found."""
        near_term_body = {"earningsCalendar": [
            {"symbol": "AAPL", "date": "2026-07-23", "hour": "bmo", "quarter": 3, "year": 2026},
        ]}
        responses = [_resp(json_body=near_term_body)] + [None] * 9
        with patch("scrapers.fetch_with_retry", side_effect=responses):
            result = scrapers.fetch_earnings_calendar(["AAPL"])
        self.assertEqual(result["AAPL"], [{"date": "2026-07-23", "description": "Q3 2026 earnings (bmo)"}])

    def test_malformed_json_fails_open_to_none_per_ticker(self):
        bad_resp = MagicMock()
        bad_resp.status_code = 200
        bad_resp.json.side_effect = ValueError("bad json")
        with patch("scrapers.fetch_with_retry", return_value=bad_resp):
            result = scrapers.fetch_earnings_calendar(["AAPL"])
        self.assertEqual(result, {"AAPL": None})


if __name__ == "__main__":
    unittest.main()
