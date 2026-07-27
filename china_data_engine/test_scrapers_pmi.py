"""
Tests for scrapers.py's PMI staleness handling (audit §6 H1, Track 3 burndown).

Original PMI incident: the official NBS series can return HTTP 200 with a
non-empty dataframe that just stopped updating for months. fetch_china_pmi()
falls back to Caixin when that happens — these tests cover the two ways that
fallback used to still leak a stale print: (1) Caixin itself was never
staleness-checked, so a dead Caixin source could silently re-inject the same
bug one layer down; (2) when Caixin failed outright, fetch_china_pmi() fell
through and returned the stale NBS dict anyway instead of refusing to serve it.

akshare isn't installed in this environment — scrapers.py only imports it at
module level and never calls into it directly in the code paths under test
(call_with_retry is mocked instead), so a bare stub module is enough to
satisfy the import.
"""
import sys
import types
from datetime import datetime, timedelta

_fake_akshare = types.ModuleType("akshare")
for _name in ("macro_china_pmi_yearly", "macro_china_non_man_pmi",
              "index_pmi_man_cx", "index_pmi_com_cx"):
    setattr(_fake_akshare, _name, lambda *a, **k: None)
sys.modules.setdefault("akshare", _fake_akshare)

import unittest
from unittest.mock import patch

import scrapers


def _fresh_date(days_ago=0):
    return (datetime.utcnow() - timedelta(days=days_ago)).strftime("%Y-%m-%d")


class TestFetchCaixinPmi(unittest.TestCase):
    def test_fresh_caixin_returned(self):
        with patch("scrapers.call_with_retry") as mock_retry:
            def side_effect(func, **kwargs):
                if func is scrapers.ak.index_pmi_man_cx:
                    m = _df_row({"制造业PMI": 50.5, "日期": _fresh_date(5)})
                elif func is scrapers.ak.index_pmi_com_cx:
                    m = _df_row({"综合PMI": 51.2})
                else:
                    m = None
                return m
            mock_retry.side_effect = side_effect
            result = scrapers._fetch_caixin_pmi()
            self.assertEqual(result["source"], "Caixin")
            self.assertEqual(result["manufacturing"], 50.5)

    def test_stale_caixin_rejected(self):
        # H1: Caixin itself had no staleness check — a dead Caixin source
        # (still HTTP 200, just an old row) must not be served as fresh.
        with patch("scrapers.call_with_retry") as mock_retry:
            def side_effect(func, **kwargs):
                if func is scrapers.ak.index_pmi_man_cx:
                    return _df_row({"制造业PMI": 50.5, "日期": _fresh_date(100)})
                if func is scrapers.ak.index_pmi_com_cx:
                    return _df_row({"综合PMI": 51.2})
                return None
            mock_retry.side_effect = side_effect
            result = scrapers._fetch_caixin_pmi()
            self.assertEqual(result, {}, "a stale Caixin print must be rejected, not returned")


class TestFetchChinaPmi(unittest.TestCase):
    def test_stale_nbs_falls_back_to_fresh_caixin(self):
        with patch("scrapers.call_with_retry") as mock_retry, \
             patch("scrapers._fetch_caixin_pmi") as mock_caixin:
            def side_effect(func, **kwargs):
                if func is scrapers.ak.macro_china_pmi_yearly:
                    return _df_row({"今值": 49.0, "日期": _fresh_date(200)})  # stale NBS
                if func is scrapers.ak.macro_china_non_man_pmi:
                    return _df_row({"今值": 50.0})
                return None
            mock_retry.side_effect = side_effect
            mock_caixin.return_value = {"source": "Caixin", "manufacturing": 51.0,
                                         "non_manufacturing": 52.0, "month": _fresh_date(5),
                                         "manufacturing_yoy": 0.0, "non_manufacturing_yoy": 0.0}
            result = scrapers.fetch_china_pmi()
            self.assertEqual(result["source"], "Caixin")

    def test_stale_nbs_and_dead_caixin_returns_empty_not_stale_nbs(self):
        # H1: previously this fell through and returned the stale NBS dict.
        with patch("scrapers.call_with_retry") as mock_retry, \
             patch("scrapers._fetch_caixin_pmi") as mock_caixin:
            def side_effect(func, **kwargs):
                if func is scrapers.ak.macro_china_pmi_yearly:
                    return _df_row({"今值": 49.0, "日期": _fresh_date(200)})
                if func is scrapers.ak.macro_china_non_man_pmi:
                    return _df_row({"今值": 50.0})
                return None
            mock_retry.side_effect = side_effect
            mock_caixin.return_value = {}  # Caixin also dead/stale
            result = scrapers.fetch_china_pmi()
            self.assertEqual(result, {}, "must refuse to re-serve the stale NBS print")

    def test_fresh_nbs_returned_directly(self):
        with patch("scrapers.call_with_retry") as mock_retry:
            def side_effect(func, **kwargs):
                if func is scrapers.ak.macro_china_pmi_yearly:
                    return _df_row({"今值": 49.5, "日期": _fresh_date(3)})
                if func is scrapers.ak.macro_china_non_man_pmi:
                    return _df_row({"今值": 50.5})
                return None
            mock_retry.side_effect = side_effect
            result = scrapers.fetch_china_pmi()
            self.assertEqual(result["source"], "NBS")
            self.assertEqual(result["manufacturing"], 49.5)


def _df_row(row_dict):
    """Minimal stand-in for a pandas dataframe: `.iloc[-1]` returns a dict-like row."""
    class _Row(dict):
        def get(self, key, default=None):
            return dict.get(self, key, default)

    class _Frame:
        def __init__(self, row):
            self._row = _Row(row)

        @property
        def iloc(self):
            return [self._row]

        @property
        def empty(self):
            return False

    return _Frame(row_dict)


if __name__ == "__main__":
    unittest.main()
