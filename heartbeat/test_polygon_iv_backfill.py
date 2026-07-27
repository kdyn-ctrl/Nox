"""
Unit tests for polygon_iv_backfill's pure math — the part that must be
correct with zero network access, since a wrong IV solver would silently
poison historical_volatility (and therefore every downstream iv_rank) with
no obvious symptom other than "the number looks a little off."

Run: python3 test_polygon_iv_backfill.py
"""

import unittest
from datetime import datetime

from polygon_iv_backfill import (
    black_scholes_price,
    implied_volatility_from_price,
    _month_anchors,
)


class TestBlackScholesRoundTrip(unittest.TestCase):
    """price(known sigma) -> solve -> recovered sigma, for both call and put."""

    def test_call_round_trip_atm(self):
        S, K, T, r, sigma = 100.0, 100.0, 30 / 365.0, 0.04, 0.25
        price = black_scholes_price(S, K, T, r, sigma, "call")
        recovered = implied_volatility_from_price(price, S, K, T, r, "call")
        self.assertIsNotNone(recovered)
        self.assertAlmostEqual(recovered, sigma, delta=1e-3)

    def test_put_round_trip_atm(self):
        S, K, T, r, sigma = 100.0, 100.0, 30 / 365.0, 0.04, 0.35
        price = black_scholes_price(S, K, T, r, sigma, "put")
        recovered = implied_volatility_from_price(price, S, K, T, r, "put")
        self.assertIsNotNone(recovered)
        self.assertAlmostEqual(recovered, sigma, delta=1e-3)

    def test_round_trip_otm_and_itm(self):
        # OTM call (K > S) and ITM call (K < S) — the solver must not diverge
        # away from the money, which is where a naive Newton-Raphson can fail.
        for K, sigma in [(120.0, 0.40), (80.0, 0.30)]:
            S, T, r = 100.0, 45 / 365.0, 0.04
            price = black_scholes_price(S, K, T, r, sigma, "call")
            recovered = implied_volatility_from_price(price, S, K, T, r, "call")
            self.assertIsNotNone(recovered, f"K={K} failed to converge")
            self.assertAlmostEqual(recovered, sigma, delta=1e-3)

    def test_round_trip_across_dte_range(self):
        # From short-dated (7 DTE) to LEAP-like (180 DTE).
        for dte in (7, 30, 45, 90, 180):
            S, K, r, sigma = 100.0, 100.0, 0.04, 0.28
            T = dte / 365.0
            price = black_scholes_price(S, K, T, r, sigma, "call")
            recovered = implied_volatility_from_price(price, S, K, T, r, "call")
            self.assertIsNotNone(recovered, f"dte={dte} failed to converge")
            self.assertAlmostEqual(recovered, sigma, delta=1e-3)


class TestImpliedVolatilityEdgeCases(unittest.TestCase):
    def test_zero_or_negative_time_returns_none(self):
        self.assertIsNone(implied_volatility_from_price(5.0, 100.0, 100.0, 0.0, 0.04, "call"))
        self.assertIsNone(implied_volatility_from_price(5.0, 100.0, 100.0, -1.0, 0.04, "call"))

    def test_nonpositive_price_returns_none(self):
        self.assertIsNone(implied_volatility_from_price(0.0, 100.0, 100.0, 0.1, 0.04, "call"))
        self.assertIsNone(implied_volatility_from_price(-1.0, 100.0, 100.0, 0.1, 0.04, "call"))

    def test_price_outside_achievable_range_returns_none(self):
        # A price no sigma in [1e-4, 5.0] could ever produce (absurdly high
        # for a short-dated near-the-money option) must not silently return
        # a garbage edge value like sigma=5.0.
        S, K, T, r = 100.0, 100.0, 7 / 365.0, 0.04
        impossible_price = 99.0  # far beyond what any vol level produces here
        self.assertIsNone(implied_volatility_from_price(impossible_price, S, K, T, r, "call"))

    def test_deep_itm_call_intrinsic_floor(self):
        # Deep ITM: price must be at least intrinsic value; black_scholes_price
        # itself should never price below max(S-K, 0) for any sigma > 0.
        S, K, T, r, sigma = 150.0, 100.0, 30 / 365.0, 0.04, 0.20
        price = black_scholes_price(S, K, T, r, sigma, "call")
        self.assertGreaterEqual(price, S - K - 1e-6)


class TestMonthAnchors(unittest.TestCase):
    def test_returns_requested_count(self):
        anchors = _month_anchors(12)
        self.assertEqual(len(anchors), 12)

    def test_anchors_are_first_of_month_and_increasing(self):
        anchors = _month_anchors(6)
        for a in anchors:
            d = datetime.strptime(a, "%Y-%m-%d")
            self.assertEqual(d.day, 1)
        self.assertEqual(anchors, sorted(anchors), "anchors must be chronological, oldest first")

    def test_anchors_are_distinct(self):
        anchors = _month_anchors(12)
        self.assertEqual(len(anchors), len(set(anchors)), "no duplicate months in a 12-month backfill")


if __name__ == "__main__":
    unittest.main()
