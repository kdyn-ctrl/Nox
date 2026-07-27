"""Pure-math tests for fundamentals_calc.py — no network, no I/O."""
import unittest

from fundamentals_calc import (
    compute_beneish_mscore, compute_fcf_runway, evaluate_fundamental_risk,
    compute_piotroski_fscore, evaluate_bullish_quality, Knobs,
)

_BENEISH_FIELDS = [
    "receivables", "revenue", "cogs", "current_assets", "ppe_net",
    "securities", "total_assets", "depreciation", "sga",
    "total_liabilities", "current_liabilities", "ltd", "net_income", "cfo",
]
_PIOTROSKI_FIELDS = [
    "net_income", "total_assets", "cfo", "ltd", "current_assets",
    "current_liabilities", "revenue", "cogs", "shares_outstanding",
]


def _flat_period(**overrides):
    """A synthetic fiscal-year dict where every ratio-component evaluates to
    a known, hand-computable value unless overridden."""
    base = {
        "receivables": 100.0, "revenue": 1000.0, "cogs": 600.0,
        "current_assets": 300.0, "ppe_net": 400.0, "securities": 0.0,
        "total_assets": 1000.0, "depreciation": 50.0, "sga": 100.0,
        "total_liabilities": 500.0, "current_liabilities": 200.0,
        "ltd": 200.0, "net_income": 80.0, "cfo": 80.0,
        "shares_outstanding": 1000.0,
    }
    base.update(overrides)
    return base


def _improving_period(**overrides):
    """A synthetic 'high quality, improving' fiscal-year dict — every
    Piotroski criterion should read True against _flat_period() as the prior
    year (used as the 'current' side of an all-9 f_score test)."""
    base = {
        "receivables": 100.0, "revenue": 1300.0, "cogs": 650.0,
        "current_assets": 400.0, "ppe_net": 400.0, "securities": 0.0,
        "total_assets": 1100.0, "depreciation": 50.0, "sga": 100.0,
        "total_liabilities": 500.0, "current_liabilities": 200.0,
        "ltd": 150.0, "net_income": 150.0, "cfo": 200.0,
        "shares_outstanding": 1000.0,
    }
    base.update(overrides)
    return base


class TestBeneishMScore(unittest.TestCase):
    def test_identical_years_gives_known_baseline_score(self):
        # If every YoY ratio component is unchanged, DSRI=GMI=AQI=SGI=DEPI=
        # SGAI=LVGI=1.0 and TATA = (net_income - cfo) / total_assets = 0.
        period = _flat_period()
        result = compute_beneish_mscore(period, period)
        self.assertFalse(result["insufficient_data"])
        expected = (
            -4.84 + 0.920 * 1.0 + 0.528 * 1.0 + 0.404 * 1.0 + 0.892 * 1.0
            + 0.115 * 1.0 - 0.172 * 1.0 + 4.679 * 0.0 - 0.327 * 1.0
        )
        self.assertAlmostEqual(result["m_score"], expected, places=6)
        for ratio in ("DSRI", "GMI", "AQI", "SGI", "DEPI", "SGAI", "LVGI"):
            self.assertAlmostEqual(result["components"][ratio], 1.0, places=6)
        self.assertAlmostEqual(result["components"]["TATA"], 0.0, places=6)

    def test_missing_field_is_insufficient_data(self):
        # "securities" is the one exception (see test_missing_securities_
        # defaults_to_zero_not_insufficient below) — every other field is
        # still strictly required.
        current = _flat_period()
        prior = _flat_period()
        for field in _BENEISH_FIELDS:
            if field == "securities":
                continue
            with self.subTest(field=field):
                c = dict(current)
                c[field] = None
                result = compute_beneish_mscore(c, prior)
                self.assertTrue(result["insufficient_data"])
                self.assertIsNone(result["m_score"])
                self.assertIn(field, result["missing_fields"])

    def test_missing_securities_defaults_to_zero_not_insufficient(self):
        # Regression: fundamentals_scraper.BENEISH_TAGS's own comment says
        # "securities" (ShortTermInvestments) is "optional; treated as 0 if
        # absent" — that fallback was never actually implemented until this
        # fix. A missing tag (common — many filers hold none and never
        # report it) must compute the same score as an explicit 0.0, not
        # insufficient_data. Confirmed live against real AAPL/MSFT data.
        current_missing = _flat_period(securities=None)
        current_explicit_zero = _flat_period(securities=0.0)
        prior = _flat_period()
        result_missing = compute_beneish_mscore(current_missing, prior)
        result_explicit = compute_beneish_mscore(current_explicit_zero, prior)
        self.assertFalse(result_missing["insufficient_data"])
        self.assertAlmostEqual(result_missing["m_score"], result_explicit["m_score"], places=9)

    def test_zero_revenue_denominator_is_insufficient_data(self):
        current = _flat_period(revenue=0.0)
        prior = _flat_period()
        result = compute_beneish_mscore(current, prior)
        self.assertTrue(result["insufficient_data"])
        self.assertIsNone(result["m_score"])

    def test_zero_prior_receivables_guards_against_zero_division(self):
        current = _flat_period()
        prior = _flat_period(receivables=0.0)
        result = compute_beneish_mscore(current, prior)  # must not raise
        self.assertTrue(result["insufficient_data"])

    def test_zero_prior_total_assets_guards_against_zero_division(self):
        current = _flat_period()
        prior = _flat_period(total_assets=0.0)
        result = compute_beneish_mscore(current, prior)  # must not raise
        self.assertTrue(result["insufficient_data"])

    def test_deteriorating_receivables_raises_score_above_baseline(self):
        # Receivables growing much faster than revenue (DSRI >> 1) should
        # push M-Score up relative to the all-1.0 baseline.
        current = _flat_period(receivables=400.0)
        prior = _flat_period()
        baseline = compute_beneish_mscore(_flat_period(), prior)
        result = compute_beneish_mscore(current, prior)
        self.assertGreater(result["m_score"], baseline["m_score"])


class TestFcfRunway(unittest.TestCase):
    def test_burning_computes_runway(self):
        result = compute_fcf_runway(cash=1000.0, cfo_q=50.0, capex_q=150.0)
        self.assertFalse(result["insufficient_data"])
        self.assertTrue(result["is_burning"])
        self.assertAlmostEqual(result["quarterly_fcf"], -100.0)
        self.assertAlmostEqual(result["runway_quarters"], 10.0)

    def test_not_burning_returns_none_runway(self):
        result = compute_fcf_runway(cash=1000.0, cfo_q=200.0, capex_q=50.0)
        self.assertFalse(result["insufficient_data"])
        self.assertFalse(result["is_burning"])
        self.assertIsNone(result["runway_quarters"])

    def test_zero_cash_while_burning_flags_immediately(self):
        result = compute_fcf_runway(cash=0.0, cfo_q=10.0, capex_q=50.0)
        self.assertTrue(result["is_burning"])
        self.assertAlmostEqual(result["runway_quarters"], 0.0)

    def test_missing_field_is_insufficient_data(self):
        for kwargs in (
            {"cash": None, "cfo_q": 1.0, "capex_q": 1.0},
            {"cash": 1.0, "cfo_q": None, "capex_q": 1.0},
            {"cash": 1.0, "cfo_q": 1.0, "capex_q": None},
        ):
            with self.subTest(**kwargs):
                result = compute_fcf_runway(**kwargs)
                self.assertTrue(result["insufficient_data"])


class TestEvaluateFundamentalRisk(unittest.TestCase):
    def _raw(self, current=None, prior=None, quarterly=None, resolved=True, fetch_error=None):
        return {
            "ticker": "TEST",
            "resolved": resolved,
            "fetch_error": fetch_error,
            "current_period": current or _flat_period(),
            "prior_period": prior or _flat_period(),
            "quarterly": quarterly or {"cash": 1000.0, "cfo_q": 200.0, "capex": 50.0},
        }

    def test_unresolved_ticker_is_insufficient(self):
        result = evaluate_fundamental_risk(self._raw(resolved=False, fetch_error="cik_not_found"))
        self.assertEqual(result["data_quality"], "INSUFFICIENT")
        self.assertFalse(result["flags"]["beneish_manipulation_risk"])
        self.assertFalse(result["flags"]["fcf_burn_risk"])

    def test_ok_when_both_screens_compute(self):
        result = evaluate_fundamental_risk(self._raw())
        self.assertEqual(result["data_quality"], "OK")

    def test_partial_when_only_one_screen_computes(self):
        raw = self._raw(quarterly={"cash": None, "cfo_q": 200.0, "capex": 50.0})
        result = evaluate_fundamental_risk(raw)
        self.assertEqual(result["data_quality"], "PARTIAL")

    def test_beneish_threshold_boundary_uses_strict_greater_than(self):
        knobs = Knobs(beneish_threshold=0.0, fcf_runway_min_quarters=3.5)
        current = _flat_period()
        prior = _flat_period()
        baseline_score = compute_beneish_mscore(current, prior)["m_score"]
        raw_at_threshold = self._raw()
        # Force m_score to exactly match a chosen threshold via a Knobs value
        # equal to the computed baseline — must NOT flag at the boundary.
        knobs_at_baseline = Knobs(beneish_threshold=baseline_score, fcf_runway_min_quarters=3.5)
        result_at = evaluate_fundamental_risk(raw_at_threshold, knobs_at_baseline)
        self.assertFalse(result_at["flags"]["beneish_manipulation_risk"])

        knobs_below_baseline = Knobs(beneish_threshold=baseline_score - 0.01, fcf_runway_min_quarters=3.5)
        result_above = evaluate_fundamental_risk(raw_at_threshold, knobs_below_baseline)
        self.assertTrue(result_above["flags"]["beneish_manipulation_risk"])

    def test_fcf_runway_threshold_boundary(self):
        raw = self._raw(quarterly={"cash": 350.0, "cfo_q": 50.0, "capex": 150.0})  # runway = 3.5
        knobs = Knobs(beneish_threshold=999.0, fcf_runway_min_quarters=3.5)
        result = evaluate_fundamental_risk(raw, knobs)
        self.assertFalse(result["flags"]["fcf_burn_risk"])  # 3.5 < 3.5 is False

        knobs_tighter = Knobs(beneish_threshold=999.0, fcf_runway_min_quarters=3.51)
        result2 = evaluate_fundamental_risk(raw, knobs_tighter)
        self.assertTrue(result2["flags"]["fcf_burn_risk"])


class TestPiotroskiFScore(unittest.TestCase):
    def test_flat_unchanged_company_scores_three(self):
        # Identical current/prior years: only the two static-positivity
        # criteria (ROA>0, CFO>0) and no_dilution (shares unchanged, <=
        # is not strict) are True; every YoY-improvement criterion is False
        # since nothing improved.
        period = _flat_period()
        result = compute_piotroski_fscore(period, period)
        self.assertFalse(result["insufficient_data"])
        self.assertEqual(result["f_score"], 3)
        self.assertTrue(result["criteria"]["positive_roa"])
        self.assertTrue(result["criteria"]["positive_cfo"])
        self.assertTrue(result["criteria"]["no_dilution"])
        for k in ("roa_improving", "cfo_exceeds_net_income", "leverage_decreasing",
                  "current_ratio_improving", "gross_margin_improving", "asset_turnover_improving"):
            self.assertFalse(result["criteria"][k], k)

    def test_improving_company_scores_nine(self):
        result = compute_piotroski_fscore(_improving_period(), _flat_period())
        self.assertFalse(result["insufficient_data"])
        self.assertEqual(result["f_score"], 9)
        self.assertTrue(all(result["criteria"].values()))

    def test_missing_field_is_insufficient_data(self):
        current = _improving_period()
        prior = _flat_period()
        for field in _PIOTROSKI_FIELDS:
            with self.subTest(field=field):
                c = dict(current)
                c[field] = None
                result = compute_piotroski_fscore(c, prior)
                self.assertTrue(result["insufficient_data"])
                self.assertIsNone(result["f_score"])
                self.assertIn(field, result["missing_fields"])

    def test_zero_denominator_guards_against_zero_division(self):
        for field in ("total_assets", "current_liabilities", "revenue"):
            with self.subTest(field=field):
                current = _improving_period(**{field: 0.0})
                result = compute_piotroski_fscore(current, _flat_period())  # must not raise
                self.assertTrue(result["insufficient_data"])

    def test_dilution_flips_no_dilution_false(self):
        current = _improving_period(shares_outstanding=1001.0)  # 1 share more than prior 1000
        result = compute_piotroski_fscore(current, _flat_period())
        self.assertFalse(result["criteria"]["no_dilution"])
        self.assertEqual(result["f_score"], 8)  # every other criterion still true

    def test_buyback_keeps_no_dilution_true(self):
        current = _improving_period(shares_outstanding=999.0)  # fewer shares than prior
        result = compute_piotroski_fscore(current, _flat_period())
        self.assertTrue(result["criteria"]["no_dilution"])
        self.assertEqual(result["f_score"], 9)


class TestEvaluateBullishQuality(unittest.TestCase):
    def _raw(self, current=None, prior=None, resolved=True, fetch_error=None):
        return {
            "ticker": "TEST",
            "resolved": resolved,
            "fetch_error": fetch_error,
            "current_period": current or _improving_period(),
            "prior_period": prior or _flat_period(),
        }

    def test_unresolved_ticker_is_insufficient(self):
        result = evaluate_bullish_quality(self._raw(resolved=False, fetch_error="cik_not_found"))
        self.assertEqual(result["data_quality"], "INSUFFICIENT")
        self.assertFalse(result["flags"]["piotroski_high_quality"])

    def test_ok_when_score_computes(self):
        result = evaluate_bullish_quality(self._raw())
        self.assertEqual(result["data_quality"], "OK")
        self.assertEqual(result["piotroski"]["f_score"], 9)

    def test_default_knob_flags_at_8_and_9(self):
        knobs = Knobs(piotroski_flag_min_score=8)
        nine = evaluate_bullish_quality(self._raw(), knobs)
        self.assertTrue(nine["flags"]["piotroski_high_quality"])

        eight_raw = self._raw(current=_improving_period(shares_outstanding=1001.0))  # f_score=8
        eight = evaluate_bullish_quality(eight_raw, knobs)
        self.assertTrue(eight["flags"]["piotroski_high_quality"])

    def test_below_threshold_does_not_flag(self):
        knobs = Knobs(piotroski_flag_min_score=8)
        result = evaluate_bullish_quality(self._raw(current=_flat_period()), knobs)  # f_score=3
        self.assertFalse(result["flags"]["piotroski_high_quality"])


if __name__ == "__main__":
    unittest.main()
