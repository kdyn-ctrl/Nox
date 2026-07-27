"""
Tests for squeeze_pead_scanner.py's real-data PEAD pipeline (2026-07-27
rewrite). Verifies:
1. DTE < 14 and DTE < 30 rejection guardrails (unchanged).
2. Delta < threshold rejection for single long options (unchanged).
3. EV is DISPLAY-ONLY now — a deeply negative EV must NOT be rejected
   (this is the behavior change from the old fabricated EV gate).
4. 1R Position Sizing calculation.
5. Real earnings-surprise parsing from a Finnhub-shaped payload.
6. Price-reaction confirmation logic against real-shaped Alpaca bars —
   direction mismatch and below-floor moves are both rejected.
7. Contract discovery against a real-shaped Alpaca options-snapshot payload
   — delta/spread guardrails apply, and Black-Scholes IV inversion recovers
   a known sigma from a price generated with that same sigma.
8. End-to-end build_pead_candidate with all network calls mocked.
9. Integration with SQLite `options_signals` table and Telegram formatting.
"""

import os
import sys
import tempfile
import sqlite3
import unittest
from unittest.mock import patch
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(__file__))

import squeeze_pead_scanner as scanner
from squeeze_pead_scanner import (
    OptionSignalCandidate,
    validate_and_gate_signal,
    save_signal_to_db,
    format_telegram_message,
    black_scholes_delta,
    black_scholes_price,
    implied_volatility_from_price,
    fetch_recent_earnings_surprises,
    confirm_earnings_reaction,
    find_option_contract,
    build_pead_candidate,
)


class TestGuardrails(unittest.TestCase):

    def test_dte_guardrails(self):
        cand = OptionSignalCandidate(
            ticker="SHOP", strategy="LONG_CALL", direction="BULLISH", spot_price=130.0,
            strike=125.0, strike2=None, dte=10, delta=0.65, net_debit=5.0,
            max_gain=500.0, max_loss=500.0, prob_win=0.60, expected_value=100.0,
            engine_source="PEAD_DRIFT"
        )
        valid, reason, qty, risk = validate_and_gate_signal(cand, account_balance=500.0)
        self.assertFalse(valid)
        self.assertIn("DTE (10) < 14", reason)

        cand.dte = 25
        valid, reason, qty, risk = validate_and_gate_signal(cand, account_balance=500.0)
        self.assertFalse(valid)
        self.assertIn("DTE (25) < 30", reason)

    def test_delta_guardrail(self):
        cand = OptionSignalCandidate(
            ticker="AAPL", strategy="LONG_CALL", direction="BULLISH", spot_price=225.0,
            strike=240.0, strike2=None, dte=42, delta=0.45, net_debit=3.0,
            max_gain=400.0, max_loss=300.0, prob_win=0.60, expected_value=120.0,
            engine_source="PEAD_DRIFT"
        )
        valid, reason, qty, risk = validate_and_gate_signal(cand, account_balance=500.0)
        self.assertFalse(valid)
        self.assertIn("Delta (0.45) < 0.60", reason)

    def test_negative_ev_is_not_rejected(self):
        """
        EV is display-only (RULE-D5) — a candidate with a deeply negative
        heuristic EV must still pass, since prob_win isn't a measured win
        rate and shouldn't have filtering power. This is the deliberate
        behavior change from the old (fabricated) EV>0 gate.
        """
        cand = OptionSignalCandidate(
            ticker="META", strategy="LONG_CALL", direction="BULLISH", spot_price=500.0,
            strike=490.0, strike2=None, dte=45, delta=0.62, net_debit=15.0,
            max_gain=200.0, max_loss=1500.0, prob_win=0.40, expected_value=-800.0,
            engine_source="PEAD_DRIFT"
        )
        # Large enough account that sizing isn't the thing under test here.
        valid, reason, qty, risk = validate_and_gate_signal(cand, account_balance=100000.0)
        self.assertTrue(valid)

    def test_1r_position_sizing(self):
        cand = OptionSignalCandidate(
            ticker="NVDA", strategy="LONG_CALL", direction="BULLISH", spot_price=120.0,
            strike=118.0, strike2=None, dte=42, delta=0.64, net_debit=0.05,
            max_gain=7.5, max_loss=5.0, prob_win=0.60, expected_value=2.0,
            engine_source="PEAD_DRIFT"
        )
        # On $500 account @ 2% max risk = $10.00 max risk / $5.00 per contract = 2
        valid, reason, qty, risk = validate_and_gate_signal(cand, account_balance=500.0)
        self.assertTrue(valid)
        self.assertEqual(qty, 2)

    def test_contract_exceeding_risk_budget_is_rejected_not_floored_to_one(self):
        """
        Real 2026-07-27 case: a TSLA put's real ask priced the contract at
        $3,826 max loss against a $100 sandbox account (2% budget = $2). The
        old code's max(1, ...) floor would have forced this trade anyway —
        the same aborted_zero_contracts bug class the C++ engine already
        fixed once. Zero contracts, not one, is correct here.
        """
        cand = OptionSignalCandidate(
            ticker="TSLA", strategy="LONG_PUT", direction="BEARISH", spot_price=310.0,
            strike=340.0, strike2=None, dte=53, delta=-0.63, net_debit=38.26,
            max_gain=5739.0, max_loss=3826.0, prob_win=0.58, expected_value=1000.0,
            engine_source="PEAD_DRIFT"
        )
        valid, reason, qty, risk = validate_and_gate_signal(cand, account_balance=100.0)
        self.assertFalse(valid)
        self.assertEqual(qty, 0)
        self.assertIn("aborted_zero_contracts", reason)


class TestImpliedVolatilityInversion(unittest.TestCase):

    def test_round_trip_recovers_known_sigma(self):
        """Solving IV from a price generated with a known sigma should recover that sigma."""
        S, K, T, r, true_sigma = 100.0, 95.0, 45 / 365.0, 0.04, 0.35
        price = black_scholes_price(S, K, T, r, true_sigma, "call")
        solved = implied_volatility_from_price(price, S, K, T, r, "call")
        self.assertIsNotNone(solved)
        self.assertAlmostEqual(solved, true_sigma, places=3)

    def test_out_of_range_price_returns_none(self):
        # A price above what any reasonable sigma could produce (deep ITM
        # intrinsic value violation) must return None, not a garbage sigma.
        solved = implied_volatility_from_price(price=1000.0, S=100.0, K=95.0, T=45 / 365.0, r=0.04, option_type="call")
        self.assertIsNone(solved)


class TestEarningsSurpriseParsing(unittest.TestCase):

    def test_parses_real_shaped_finnhub_payload(self):
        """Mirrors the actual /calendar/earnings response shape verified live 2026-07-27."""
        fake_payload = {
            "earningsCalendar": [
                {"symbol": "AAPL", "date": "2026-07-24", "hour": "bmo", "quarter": 2, "year": 2026,
                 "epsEstimate": 1.9884, "epsActual": 2.01, "revenueEstimate": 1, "revenueActual": 1},
                {"symbol": "TSLA", "date": "2026-07-24", "hour": "amc", "quarter": 2, "year": 2026,
                 "epsEstimate": 0.50, "epsActual": None, "revenueEstimate": 1, "revenueActual": None},
                {"symbol": "NOTWATCHED", "date": "2026-07-24", "hour": "bmo", "quarter": 2, "year": 2026,
                 "epsEstimate": 1.0, "epsActual": 1.5, "revenueEstimate": 1, "revenueActual": 1},
            ]
        }
        with patch.object(scanner, "FINNHUB_API_KEY", "fake"), \
             patch.object(scanner, "_http_get_json", return_value=fake_payload):
            results = fetch_recent_earnings_surprises(["AAPL", "TSLA"])
        # TSLA not yet reported (epsActual None) -> excluded.
        # NOTWATCHED not in watchlist -> excluded.
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["ticker"], "AAPL")
        self.assertAlmostEqual(results[0]["surprise_pct"], (2.01 - 1.9884) / 1.9884 * 100.0, places=3)

    def test_missing_api_key_returns_empty_not_raises(self):
        with patch.object(scanner, "FINNHUB_API_KEY", ""):
            self.assertEqual(fetch_recent_earnings_surprises(["AAPL"]), [])


class TestPriceReactionConfirmation(unittest.TestCase):

    def _bars(self, date_close_pairs):
        return [{"t": f"{d}T04:00:00Z", "c": c, "o": c, "h": c, "l": c, "v": 1000} for d, c in date_close_pairs]

    def test_confirms_positive_surprise_with_positive_reaction(self):
        bars = self._bars([
            ("2026-07-23", 100.0),  # prev close before bmo report on 07-24
            ("2026-07-24", 105.0),  # reaction day, +5%
            ("2026-07-27", 106.0),
        ])
        with patch.object(scanner, "fetch_daily_bars", return_value=bars):
            result = confirm_earnings_reaction("AAPL", "2026-07-24", "bmo", surprise_pct=10.0)
        self.assertIsNotNone(result)
        self.assertAlmostEqual(result["reaction_return_pct"], 5.0, places=3)

    def test_direction_mismatch_is_rejected(self):
        # Positive surprise but price fell — real data doesn't confirm the setup.
        bars = self._bars([
            ("2026-07-23", 100.0),
            ("2026-07-24", 95.0),
        ])
        with patch.object(scanner, "fetch_daily_bars", return_value=bars):
            result = confirm_earnings_reaction("AAPL", "2026-07-24", "bmo", surprise_pct=10.0)
        self.assertIsNone(result)

    def test_reaction_below_floor_is_rejected(self):
        bars = self._bars([
            ("2026-07-23", 100.0),
            ("2026-07-24", 100.3),  # +0.3%, below PEAD_MIN_REACTION_PCT default of 2%
        ])
        with patch.object(scanner, "fetch_daily_bars", return_value=bars):
            result = confirm_earnings_reaction("AAPL", "2026-07-24", "bmo", surprise_pct=10.0)
        self.assertIsNone(result)

    def test_insufficient_bar_history_is_rejected(self):
        with patch.object(scanner, "fetch_daily_bars", return_value=[]):
            result = confirm_earnings_reaction("AAPL", "2026-07-24", "bmo", surprise_pct=10.0)
        self.assertIsNone(result)


class TestContractDiscovery(unittest.TestCase):

    def test_finds_real_shaped_contract_meeting_delta_floor(self):
        spot = 200.0
        T = 45 / 365.0
        true_sigma = 0.30
        strike = 190.0
        theoretical_price = black_scholes_price(spot, strike, T, scanner.RISK_FREE_RATE, true_sigma, "call")
        bid, ask = theoretical_price - 0.05, theoretical_price + 0.05

        exp_date = (datetime.now(timezone.utc).date() + timedelta(days=45))
        occ_symbol = f"XYZ{exp_date.strftime('%y%m%d')}C{int(strike * 1000):08d}"

        fake_snapshots = {"snapshots": {occ_symbol: {"latestQuote": {"bp": bid, "ap": ask}}}}
        with patch.object(scanner, "ALPACA_API_KEY", "fake"), \
             patch.object(scanner, "ALPACA_SECRET_KEY", "fake"), \
             patch.object(scanner, "_http_get_json", return_value=fake_snapshots):
            contract = find_option_contract("XYZ", spot, "call")

        self.assertIsNotNone(contract)
        self.assertEqual(contract["strike"], strike)
        self.assertGreaterEqual(contract["delta"], scanner.PEAD_MIN_DELTA)
        self.assertAlmostEqual(contract["iv"], true_sigma, places=2)

    def test_wide_spread_contract_is_excluded(self):
        spot = 200.0
        exp_date = (datetime.now(timezone.utc).date() + timedelta(days=45))
        occ_symbol = f"XYZ{exp_date.strftime('%y%m%d')}C{int(190 * 1000):08d}"
        # ~50% spread — far past PEAD_MAX_SPREAD_PCT.
        fake_snapshots = {"snapshots": {occ_symbol: {"latestQuote": {"bp": 5.0, "ap": 10.0}}}}
        with patch.object(scanner, "ALPACA_API_KEY", "fake"), \
             patch.object(scanner, "ALPACA_SECRET_KEY", "fake"), \
             patch.object(scanner, "_http_get_json", return_value=fake_snapshots):
            contract = find_option_contract("XYZ", spot, "call")
        self.assertIsNone(contract)

    def test_no_credentials_returns_none(self):
        with patch.object(scanner, "ALPACA_API_KEY", ""), patch.object(scanner, "ALPACA_SECRET_KEY", ""):
            self.assertIsNone(find_option_contract("XYZ", 200.0, "call"))


class TestBuildPeadCandidateEndToEnd(unittest.TestCase):

    def test_full_pipeline_with_all_data_sources_mocked(self):
        event = {"ticker": "AAPL", "date": "2026-07-24", "hour": "bmo", "surprise_pct": 12.0}
        reaction = {"reaction_date": "2026-07-24", "reaction_return_pct": 6.0, "current_price": 210.0}
        contract = {
            "symbol": "AAPL260907C00195000", "strike": 195.0, "expiration": "2026-09-07",
            "dte": 42, "bid": 14.8, "ask": 15.2, "mid": 15.0, "spread_pct": 2.7,
            "iv": 0.32, "delta": 0.68,
        }
        with patch.object(scanner, "confirm_earnings_reaction", return_value=reaction), \
             patch.object(scanner, "fetch_spot_price", return_value=205.0), \
             patch.object(scanner, "find_option_contract", return_value=contract):
            candidate = build_pead_candidate(event)

        self.assertIsNotNone(candidate)
        self.assertEqual(candidate.strategy, "LONG_CALL")
        self.assertEqual(candidate.net_debit, 15.2)  # priced at the real ask
        self.assertEqual(candidate.max_loss, 1520.0)

    def test_surprise_below_floor_short_circuits_before_any_fetch(self):
        event = {"ticker": "AAPL", "date": "2026-07-24", "hour": "bmo", "surprise_pct": 1.0}
        with patch.object(scanner, "confirm_earnings_reaction") as mock_confirm:
            candidate = build_pead_candidate(event)
        self.assertIsNone(candidate)
        mock_confirm.assert_not_called()


class TestDbPersistenceAndTelegramFormat(unittest.TestCase):

    def setUp(self):
        self.tmp_db = tempfile.NamedTemporaryFile(suffix=".db", delete=False)
        self.tmp_db_path = self.tmp_db.name
        self.tmp_db.close()
        scanner.DB_PATH = self.tmp_db_path
        scanner.init_db()

    def tearDown(self):
        if os.path.exists(self.tmp_db_path):
            os.remove(self.tmp_db_path)

    def test_db_persistence_and_telegram_format(self):
        cand = OptionSignalCandidate(
            ticker="SHOP", strategy="LONG_CALL", direction="BULLISH", spot_price=130.0,
            strike=125.0, strike2=None, dte=42, delta=0.68, net_debit=1.50,
            max_gain=225.0, max_loss=150.0, prob_win=0.60, expected_value=45.0,
            engine_source="PEAD_DRIFT", iv_rank=0.32, quality_score=0.88,
            reason="Earnings surprise +12.0%, confirmed reaction +6.0%",
            bid=1.45, ask=1.50, spread_pct=3.4,
        )
        signal_id = save_signal_to_db(cand, suggested_qty=1)
        self.assertGreater(signal_id, 0)

        with sqlite3.connect(self.tmp_db_path) as conn:
            row = conn.execute(
                "SELECT ticker, strategy, direction, strike, dte, quality_score FROM options_signals WHERE id = ?",
                (signal_id,)
            ).fetchone()
            self.assertIsNotNone(row)
            self.assertEqual(row[0], "SHOP")
            self.assertEqual(row[1], "LONG_CALL")

        tg_msg = format_telegram_message(cand, signal_id, suggested_qty=1, total_risk=150.0)
        self.assertIn("NOX PEAD ADVISORY", tg_msg)
        self.assertIn(f"s:{signal_id}", tg_msg)
        self.assertIn(f"/trade s:{signal_id}", tg_msg)
        self.assertIn("bid $1.45 / ask $1.50", tg_msg)


if __name__ == "__main__":
    unittest.main()
