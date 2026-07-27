"""Tests for fundamentals_scraper.py's CIK resolution and tag extraction.
All SEC network calls are mocked — no live requests, per project convention
(test_fundamentals_calc.py covers the pure math; this covers the plumbing)."""
import os
import unittest
from datetime import timedelta
from unittest.mock import patch, MagicMock

# fundamentals_scraper imports SEC_USER_AGENT/HTTP_TIMEOUT from scrapers.py,
# which hard-aborts at import time without Alpaca creds (RULE-009) even
# though nothing in this module touches Alpaca — set harmless dummy values
# so the import succeeds under test, same workaround plaid_fills_importer.py
# documents for the equivalent monitor.py constraint.
os.environ.setdefault("ALPACA_API_KEY", "test")
os.environ.setdefault("ALPACA_SECRET_KEY", "test")

import fundamentals_scraper as fs


def _resp(status_code=200, json_body=None):
    m = MagicMock()
    m.status_code = status_code
    m.json.return_value = json_body if json_body is not None else {}
    return m


class TestResolveCik(unittest.TestCase):
    def setUp(self):
        fs._ticker_to_cik = {}
        fs._ticker_to_cik_fetched_at = None
        fs._ticker_to_cik_last_attempt_at = None

    def test_unknown_ticker_returns_none(self):
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = _resp(json_body={"0": {"ticker": "AAPL", "cik_str": 320193}})
            self.assertIsNone(fs.resolve_cik("NOTREAL"))

    def test_known_ticker_resolves_to_padded_cik(self):
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = _resp(json_body={"0": {"ticker": "AAPL", "cik_str": 320193}})
            self.assertEqual(fs.resolve_cik("aapl"), "0000320193")

    def test_real_sec_response_shape_is_dict_of_index_not_list(self):
        """Regression: SEC's actual company_tickers.json is
        {"0": {...}, "1": {...}}, a dict keyed by stringified index — NOT a
        JSON array. Confirmed live 2026-07-16. Iterating a dict directly
        yields its string keys (no .get()), which raised AttributeError on
        every real 200 response — masked for months because every prior
        production run either 403'd (returns before this loop runs) or
        crashed here uncaught. Must not raise, and must resolve correctly."""
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = _resp(json_body={
                "0": {"ticker": "NVDA", "cik_str": 1045810},
                "1": {"ticker": "AAPL", "cik_str": 320193},
            })
            self.assertEqual(fs.resolve_cik("AAPL"), "0000320193")
            self.assertEqual(fs.resolve_cik("NVDA"), "0001045810")

    def test_fetch_failure_fails_open_to_none(self):
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = None
            self.assertIsNone(fs.resolve_cik("AAPL"))

    def test_malformed_json_fails_open_to_none(self):
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            bad_resp = MagicMock()
            bad_resp.status_code = 200
            bad_resp.json.side_effect = ValueError("bad json")
            mock_fetch.return_value = bad_resp
            self.assertIsNone(fs.resolve_cik("AAPL"))

    def test_cache_avoids_refetch_within_ttl(self):
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = _resp(json_body={"0": {"ticker": "AAPL", "cik_str": 320193}})
            fs.resolve_cik("AAPL")
            fs.resolve_cik("AAPL")
            self.assertEqual(mock_fetch.call_count, 1)

    def test_failed_fetch_does_not_retry_on_every_subsequent_ticker(self):
        """Regression for the 2026-07-16 incident: a first-request SEC 403
        must not turn into one retry per ticker for the rest of the scan —
        that's what actually hammered SEC's endpoint ~10,168 times in one run."""
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = _resp(status_code=403)
            for ticker in ["AAPL", "TSLA", "MSFT", "MISSING1", "MISSING2"]:
                self.assertIsNone(fs.resolve_cik(ticker))
            self.assertEqual(mock_fetch.call_count, 1)

    def test_retries_again_after_cooldown_elapses(self):
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = _resp(status_code=403)
            fs.resolve_cik("AAPL")
            fs._ticker_to_cik_last_attempt_at -= timedelta(hours=fs.FUNDAMENTALS_CIK_RETRY_COOLDOWN_HOURS + 1)
            fs.resolve_cik("AAPL")
            self.assertEqual(mock_fetch.call_count, 2)


class TestFetchCompanyFacts(unittest.TestCase):
    def test_non_200_returns_none(self):
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = _resp(status_code=404)
            self.assertIsNone(fs.fetch_company_facts("0000320193"))

    def test_unreachable_returns_none(self):
        with patch("fundamentals_scraper.fetch_with_retry") as mock_fetch:
            mock_fetch.return_value = None
            self.assertIsNone(fs.fetch_company_facts("0000320193"))


class TestTagExtraction(unittest.TestCase):
    def _facts(self, tag, entries):
        return {"facts": {"us-gaap": {tag: {"units": {"USD": entries}}}}}

    def test_tag_fallback_uses_first_matching_candidate(self):
        facts = self._facts("ReceivablesNetCurrent", [
            {"form": "10-K", "fp": "FY", "end": "2025-12-31", "val": 500},
        ])
        val = fs._extract_annual(facts, fs.BENEISH_TAGS["receivables"], 0)
        self.assertEqual(val, 500.0)

    def test_missing_tag_returns_none(self):
        facts = {"facts": {"us-gaap": {}}}
        val = fs._extract_annual(facts, fs.BENEISH_TAGS["receivables"], 0)
        self.assertIsNone(val)

    def test_annual_filter_excludes_10q_and_non_fy(self):
        facts = self._facts("Assets", [
            {"form": "10-Q", "fp": "Q1", "end": "2025-03-31", "val": 111},
            {"form": "10-K", "fp": "FY", "end": "2024-12-31", "val": 222},
        ])
        val = fs._extract_annual(facts, ["Assets"], 0)
        self.assertEqual(val, 222.0)

    def test_annual_period_index_selects_prior_year(self):
        facts = self._facts("Assets", [
            {"form": "10-K", "fp": "FY", "end": "2025-12-31", "val": 300},
            {"form": "10-K", "fp": "FY", "end": "2024-12-31", "val": 200},
        ])
        self.assertEqual(fs._extract_annual(facts, ["Assets"], 0), 300.0)
        self.assertEqual(fs._extract_annual(facts, ["Assets"], 1), 200.0)

    def test_quarterly_filter_selects_most_recent_10q(self):
        facts = self._facts("CashAndCashEquivalentsAtCarryingValue", [
            {"form": "10-Q", "end": "2025-03-31", "val": 10},
            {"form": "10-Q", "end": "2025-06-30", "val": 20},
            {"form": "10-K", "end": "2024-12-31", "val": 999},
        ])
        val = fs._extract_quarterly(facts, ["CashAndCashEquivalentsAtCarryingValue"])
        self.assertEqual(val, 20.0)

    def test_shares_outstanding_extracted_from_shares_unit_not_usd(self):
        # CommonStockSharesOutstanding is tagged in "shares" units, unlike
        # every other Beneish/Piotroski field which is "USD" — the default
        # unit lookup must not silently return None for this one.
        facts = {"facts": {"us-gaap": {"CommonStockSharesOutstanding": {"units": {"shares": [
            {"form": "10-K", "fp": "FY", "end": "2025-12-31", "val": 1_000_000},
        ]}}}}}
        val = fs._extract_annual(facts, fs.PIOTROSKI_EXTRA_TAGS["shares_outstanding"], 0, unit="shares")
        self.assertEqual(val, 1_000_000.0)

    def test_shares_outstanding_wrong_unit_key_returns_none(self):
        # Sanity check the negative: looking under "USD" for a shares-unit
        # tag must not accidentally find a match.
        facts = {"facts": {"us-gaap": {"CommonStockSharesOutstanding": {"units": {"shares": [
            {"form": "10-K", "fp": "FY", "end": "2025-12-31", "val": 1_000_000},
        ]}}}}}
        val = fs._extract_annual(facts, fs.PIOTROSKI_EXTRA_TAGS["shares_outstanding"], 0, unit="USD")
        self.assertIsNone(val)

    def test_sga_composite_sums_both_tags(self):
        # Regression: Microsoft (confirmed live) never reports the combined
        # SellingGeneralAndAdministrativeExpense tag — only the two separate
        # halves. Must sum them, not silently return one half.
        facts = {"facts": {"us-gaap": {
            "GeneralAndAdministrativeExpense": {"units": {"USD": [
                {"form": "10-K", "fp": "FY", "end": "2025-06-30", "val": 7223000000},
            ]}},
            "SellingAndMarketingExpense": {"units": {"USD": [
                {"form": "10-K", "fp": "FY", "end": "2025-06-30", "val": 25654000000},
            ]}},
        }}}
        val = fs._extract_annual_sga_composite(facts, 0)
        self.assertEqual(val, 32877000000.0)

    def test_sga_composite_returns_none_if_either_half_missing(self):
        # A partial sum (just one half) isn't the real SG&A figure and would
        # silently understate it — must fail closed to None instead.
        facts = {"facts": {"us-gaap": {
            "GeneralAndAdministrativeExpense": {"units": {"USD": [
                {"form": "10-K", "fp": "FY", "end": "2025-06-30", "val": 7223000000},
            ]}},
        }}}
        self.assertIsNone(fs._extract_annual_sga_composite(facts, 0))


class TestGetFundamentalsRaw(unittest.TestCase):
    def test_unresolved_ticker_reports_error(self):
        with patch("fundamentals_scraper.resolve_cik", return_value=None):
            raw = fs.get_fundamentals_raw("NOTREAL")
            self.assertFalse(raw["resolved"])
            self.assertEqual(raw["fetch_error"], "cik_not_found")

    def test_companyfacts_fetch_failure_reports_error(self):
        with patch("fundamentals_scraper.resolve_cik", return_value="0000320193"), \
             patch("fundamentals_scraper.fetch_company_facts", return_value=None):
            raw = fs.get_fundamentals_raw("AAPL")
            self.assertTrue(raw["resolved"])
            self.assertEqual(raw["fetch_error"], "companyfacts_fetch_failed")

    def test_successful_fetch_includes_shares_outstanding_alongside_beneish_fields(self):
        # One companyfacts fetch must serve both the bearish (Beneish/FCF)
        # and bullish (Piotroski) screens — no second SEC round-trip.
        facts = {"facts": {"us-gaap": {
            "Assets": {"units": {"USD": [
                {"form": "10-K", "fp": "FY", "end": "2025-12-31", "val": 1000},
            ]}},
            "CommonStockSharesOutstanding": {"units": {"shares": [
                {"form": "10-K", "fp": "FY", "end": "2025-12-31", "val": 5000},
            ]}},
        }}}
        with patch("fundamentals_scraper.resolve_cik", return_value="0000320193"), \
             patch("fundamentals_scraper.fetch_company_facts", return_value=facts):
            raw = fs.get_fundamentals_raw("AAPL")
            self.assertEqual(raw["current_period"]["total_assets"], 1000.0)
            self.assertEqual(raw["current_period"]["shares_outstanding"], 5000.0)

    def test_sga_falls_back_to_composite_when_combined_tag_absent(self):
        # End-to-end regression for the Microsoft case: no
        # SellingGeneralAndAdministrativeExpense tag at all, only the split
        # G&A + Selling/Marketing pair, for BOTH current and prior periods.
        facts = {"facts": {"us-gaap": {
            "GeneralAndAdministrativeExpense": {"units": {"USD": [
                {"form": "10-K", "fp": "FY", "end": "2025-06-30", "val": 7223000000},
                {"form": "10-K", "fp": "FY", "end": "2024-06-30", "val": 7609000000},
            ]}},
            "SellingAndMarketingExpense": {"units": {"USD": [
                {"form": "10-K", "fp": "FY", "end": "2025-06-30", "val": 25654000000},
                {"form": "10-K", "fp": "FY", "end": "2024-06-30", "val": 24456000000},
            ]}},
        }}}
        with patch("fundamentals_scraper.resolve_cik", return_value="0000789019"), \
             patch("fundamentals_scraper.fetch_company_facts", return_value=facts):
            raw = fs.get_fundamentals_raw("MSFT")
            self.assertEqual(raw["current_period"]["sga"], 32877000000.0)
            self.assertEqual(raw["prior_period"]["sga"], 32065000000.0)


class TestQuarterlyFlowNormalization(unittest.TestCase):
    """Phase 4.2: 10-Q cash-flow statements report cumulative YTD; a single
    quarter must be isolated (de-cumulated) so burn/runway isn't 2-3x wrong."""
    CFO = "NetCashProvidedByUsedInOperatingActivities"

    def _facts(self, tag, entries):
        return {"facts": {"us-gaap": {tag: {"units": {"USD": entries}}}}}

    def test_q1_three_month_used_as_is(self):
        facts = self._facts(self.CFO, [
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-03-31", "val": 100},
        ])
        self.assertEqual(fs._extract_quarterly(facts, [self.CFO]), 100.0)

    def test_q2_cumulative_is_decumulated_against_q1(self):
        facts = self._facts(self.CFO, [
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-03-31", "val": 100},  # Q1 3mo
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-06-30", "val": 250},  # H1 6mo
        ])
        # Q2 quarter = 250 - 100 = 150, NOT the raw 250.
        self.assertEqual(fs._extract_quarterly(facts, [self.CFO]), 150.0)

    def test_q3_cumulative_is_decumulated_against_q2(self):
        facts = self._facts(self.CFO, [
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-03-31", "val": 100},
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-06-30", "val": 250},
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-09-30", "val": 400},  # 9mo
        ])
        # Q3 quarter = 400 - 250 = 150.
        self.assertEqual(fs._extract_quarterly(facts, [self.CFO]), 150.0)

    def test_discrete_three_month_frame_preferred_over_cumulative_same_end(self):
        facts = self._facts(self.CFO, [
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-06-30", "val": 250},  # 6mo cumulative
            {"form": "10-Q", "start": "2025-04-01", "end": "2025-06-30", "val": 150},  # discrete 3mo
        ])
        # The discrete 3-month frame is used directly, no subtraction.
        self.assertEqual(fs._extract_quarterly(facts, [self.CFO]), 150.0)

    def test_cumulative_without_prior_returns_none_not_wrong_number(self):
        facts = self._facts(self.CFO, [
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-06-30", "val": 250},  # 6mo, no Q1
        ])
        # Can't isolate cleanly → None (insufficient), never the raw 250.
        self.assertIsNone(fs._extract_quarterly(facts, [self.CFO]))

    def test_negative_burn_decumulated_correctly(self):
        # Cash-burning company: cumulative CFO more negative each quarter.
        facts = self._facts(self.CFO, [
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-03-31", "val": -300},
            {"form": "10-Q", "start": "2025-01-01", "end": "2025-06-30", "val": -500},
        ])
        # Q2 burn = -500 - (-300) = -200, not the raw -500 (which would 2.5x the burn).
        self.assertEqual(fs._extract_quarterly(facts, [self.CFO]), -200.0)

    def test_balance_concept_without_start_uses_latest_as_is(self):
        # Cash is an instantaneous balance (no `start`) — not a cumulative flow.
        facts = self._facts("CashAndCashEquivalentsAtCarryingValue", [
            {"form": "10-Q", "end": "2025-03-31", "val": 10},
            {"form": "10-Q", "end": "2025-06-30", "val": 20},
        ])
        self.assertEqual(
            fs._extract_quarterly(facts, ["CashAndCashEquivalentsAtCarryingValue"]), 20.0)


if __name__ == "__main__":
    unittest.main()
