"""
Unit tests for china_macro_lag — the WS8 China macro information-lag signal.

Pure/stateless like edgar_cn_lag, so everything here is deterministic: a fixed
injected clock and synthetic PMI dicts. A wrong bias or a mis-computed freshness
window would silently mis-size (or wrongly suppress) every Chinese-ADR options
order via the C++ Skeptic intelligence layer, so the direction/strength/fresh
logic is pinned here.

Run: python3 test_china_macro_lag.py
"""

import os
import unittest
from datetime import datetime, timezone

import china_macro_lag as cml


NOW = datetime(2026, 7, 15, tzinfo=timezone.utc)


def _pmi(mfg, non_mfg=0.0, month="2026-07-10", source="Caixin"):
    return {
        "month": month,
        "manufacturing": mfg,
        "non_manufacturing": non_mfg,
        "source": source,
    }


class TestBias(unittest.TestCase):
    def setUp(self):
        # Clear env so defaults apply regardless of shell.
        for k in ("CHINA_LAG_ADR_TICKERS", "CHINA_LAG_FRESH_DAYS",
                  "CHINA_PMI_STRENGTH_DENOM", "CHINA_LAG_NEUTRAL_BAND"):
            os.environ.pop(k, None)

    def test_expansion_is_bullish(self):
        out = cml.compute_macro_lag(_pmi(53.0), {}, now=NOW)
        self.assertEqual(out["macro_bias"], "bullish")
        # |53-50|/5 = 0.6
        self.assertAlmostEqual(out["macro_strength"], 0.6, places=4)

    def test_contraction_is_bearish(self):
        out = cml.compute_macro_lag(_pmi(47.0), {}, now=NOW)
        self.assertEqual(out["macro_bias"], "bearish")
        self.assertAlmostEqual(out["macro_strength"], 0.6, places=4)

    def test_neutral_band(self):
        out = cml.compute_macro_lag(_pmi(50.1), {}, now=NOW)
        self.assertEqual(out["macro_bias"], "neutral")
        self.assertEqual(out["macro_strength"], 0.0)

    def test_strength_clamps_at_one(self):
        out = cml.compute_macro_lag(_pmi(58.0), {}, now=NOW)
        self.assertEqual(out["macro_strength"], 1.0)

    def test_non_mfg_disagreement_damps_strength(self):
        # mfg bullish (53), non-mfg bearish (47) → strength halved from 0.6 to 0.3
        out = cml.compute_macro_lag(_pmi(53.0, non_mfg=47.0), {}, now=NOW)
        self.assertEqual(out["macro_bias"], "bullish")
        self.assertAlmostEqual(out["macro_strength"], 0.3, places=4)


class TestFreshness(unittest.TestCase):
    def setUp(self):
        for k in ("CHINA_LAG_FRESH_DAYS",):
            os.environ.pop(k, None)

    def test_recent_print_is_fresh_when_lag_open(self):
        # month 2026-07-10, now 2026-07-15 → 5 days, within default 10-day window
        out = cml.compute_macro_lag(_pmi(53.0, month="2026-07-10"),
                                    {"BABA": True}, now=NOW)
        baba = next(r for r in out["results"] if r["ticker"] == "BABA")
        self.assertTrue(out["macro_fresh"])
        self.assertTrue(baba["fresh"])

    def test_media_already_covered_closes_ticker_freshness(self):
        # Macro is fresh, but BABA is already on the hot board / in Cailian
        # (lag_open False) → that ticker's edge is closed even though the
        # basket-level macro print is fresh.
        out = cml.compute_macro_lag(_pmi(53.0, month="2026-07-10"),
                                    {"BABA": False}, now=NOW)
        baba = next(r for r in out["results"] if r["ticker"] == "BABA")
        self.assertTrue(out["macro_fresh"])
        self.assertFalse(baba["fresh"])

    def test_stale_print_not_fresh(self):
        # month 2026-06-01, now 2026-07-15 → 44 days, well past the window
        out = cml.compute_macro_lag(_pmi(53.0, month="2026-06-01"),
                                    {"BABA": True}, now=NOW)
        self.assertFalse(out["macro_fresh"])
        baba = next(r for r in out["results"] if r["ticker"] == "BABA")
        self.assertFalse(baba["fresh"])
        # Bias still surfaced as confirmation
        self.assertEqual(baba["bias"], "bullish")

    def test_missing_media_flag_defaults_lag_open(self):
        # No media flag for BABA → default lag_open True (no coverage evidence)
        out = cml.compute_macro_lag(_pmi(53.0, month="2026-07-10"), {}, now=NOW)
        baba = next(r for r in out["results"] if r["ticker"] == "BABA")
        self.assertTrue(baba["lag_open"])
        self.assertTrue(baba["fresh"])

    def test_no_reference_date_is_not_fresh(self):
        out = cml.compute_macro_lag(_pmi(53.0, month="N/A"), {"BABA": True}, now=NOW)
        self.assertFalse(out["macro_fresh"])


class TestDataGaps(unittest.TestCase):
    def test_empty_pmi_reports_gap(self):
        out = cml.compute_macro_lag({}, {}, now=NOW)
        self.assertFalse(out["complete"])
        self.assertIn("china_pmi", out["data_gaps"])
        self.assertEqual(out["results"], [])

    def test_zero_pmi_reports_gap(self):
        # fetch_china_pmi uses 0.0 as its no-data sentinel
        out = cml.compute_macro_lag(_pmi(0.0, non_mfg=0.0), {}, now=NOW)
        self.assertFalse(out["complete"])
        self.assertIn("china_pmi", out["data_gaps"])


class TestReleaseLabel(unittest.TestCase):
    def test_caixin_vs_nbs_label(self):
        out_cx = cml.compute_macro_lag(_pmi(53.0, source="Caixin"), {}, now=NOW)
        out_nbs = cml.compute_macro_lag(_pmi(53.0, source="NBS"), {}, now=NOW)
        self.assertEqual(out_cx["results"][0]["release"], "caixin_pmi")
        self.assertEqual(out_nbs["results"][0]["release"], "nbs_pmi")


class TestEnvOverrides(unittest.TestCase):
    def tearDown(self):
        for k in ("CHINA_LAG_ADR_TICKERS", "CHINA_LAG_FRESH_DAYS",
                  "CHINA_PMI_STRENGTH_DENOM"):
            os.environ.pop(k, None)

    def test_custom_ticker_list(self):
        os.environ["CHINA_LAG_ADR_TICKERS"] = "BABA,KWEB"
        out = cml.compute_macro_lag(_pmi(53.0), {}, now=NOW)
        self.assertEqual({r["ticker"] for r in out["results"]}, {"BABA", "KWEB"})

    def test_custom_fresh_window(self):
        os.environ["CHINA_LAG_FRESH_DAYS"] = "3"
        # 5 days old, window now 3 → not fresh
        out = cml.compute_macro_lag(_pmi(53.0, month="2026-07-10"),
                                    {"BABA": True}, now=NOW)
        self.assertFalse(out["macro_fresh"])


if __name__ == "__main__":
    unittest.main()
