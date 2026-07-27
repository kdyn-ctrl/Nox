"""Unit tests for barbell.py (core/satellite personal-trade allocation).

Pure-logic tests — no telebot/anthropic/DB. Run: python3 -m pytest test_barbell.py
"""
import barbell
from barbell import CORE, SATELLITE


# A stand-in for monitor._personal_contract_multiplier: 100 for options, else 1.
def _mult(strategy, asset_class):
    if asset_class == "OPTION" or strategy in ("LONG_CALL", "LONG_PUT", "CSP", "CC"):
        return 100
    return 1


def _row(**kw):
    base = dict(id=None, bucket=CORE, action="OPEN", quantity=None, price=None,
                pnl=None, asset_class="EQUITY", strategy=None, closes_trade_id=None)
    base.update(kw)
    return base


# ── normalize_bucket ──────────────────────────────────────────────────────
class TestNormalizeBucket:
    def test_satellite_aliases(self):
        for a in ("satellite", "SAT", "moon", "Moonshot", "lotto", "spec"):
            assert barbell.normalize_bucket(a) == SATELLITE

    def test_core_aliases(self):
        for a in ("core", "CORE", "base"):
            assert barbell.normalize_bucket(a) == CORE

    def test_unknown_returns_none(self):
        assert barbell.normalize_bucket("banana") is None
        assert barbell.normalize_bucket("") is None
        assert barbell.normalize_bucket(None) is None


# ── extract_bucket_override ───────────────────────────────────────────────
class TestExtractBucketOverride:
    def test_absent_defaults_core(self):
        cleaned, bucket = barbell.extract_bucket_override("p:12 5 4.85 tighter stop")
        assert bucket == CORE
        assert cleaned == "p:12 5 4.85 tighter stop"

    def test_prefixed_satellite(self):
        cleaned, bucket = barbell.extract_bucket_override("p:12 3 0.45 bucket:satellite")
        assert bucket == SATELLITE
        assert "bucket:" not in cleaned
        assert cleaned == "p:12 3 0.45"

    def test_prefixed_alias(self):
        _, bucket = barbell.extract_bucket_override("s:47 bucket:moon")
        assert bucket == SATELLITE

    def test_bare_word_in_notes_is_not_a_bucket(self):
        # The whole reason for prefixed-only: a bare "moon" in notes must NOT
        # reclassify the trade.
        cleaned, bucket = barbell.extract_bucket_override("p:12 5 4.85 moon shot thesis")
        assert bucket == CORE
        assert cleaned == "p:12 5 4.85 moon shot thesis"

    def test_unrecognized_value_falls_back_to_default(self):
        cleaned, bucket = barbell.extract_bucket_override("p:12 bucket:banana")
        assert bucket == CORE          # falls back, doesn't invent a bucket
        assert cleaned == "p:12"       # still stripped

    def test_only_first_bucket_token_honored(self):
        cleaned, bucket = barbell.extract_bucket_override("p:12 bucket:satellite bucket:core")
        assert bucket == SATELLITE
        assert cleaned == "p:12 bucket:core"


# ── snapshot ──────────────────────────────────────────────────────────────
class TestSnapshot:
    def test_empty(self):
        snap = barbell.snapshot([], contract_multiplier=_mult, capital=5000, cap_pct=15)
        assert snap["cap_dollars"] == 750.0
        assert snap[CORE]["open_exposure"] == 0.0
        assert snap[SATELLITE]["open_exposure"] == 0.0

    def test_open_option_exposure_uses_multiplier(self):
        # 3 contracts @ $0.45 * 100 = $135 satellite exposure
        rows = [_row(id=1, bucket=SATELLITE, action="OPEN", quantity=3, price=0.45,
                     asset_class="OPTION", strategy="LONG_CALL")]
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        assert snap[SATELLITE]["open_exposure"] == 135.0
        assert snap[SATELLITE]["open_count"] == 1
        assert snap[CORE]["open_exposure"] == 0.0

    def test_closed_position_not_counted_as_open(self):
        rows = [
            _row(id=1, bucket=SATELLITE, action="OPEN", quantity=3, price=0.45,
                 asset_class="OPTION", strategy="LONG_CALL"),
            _row(id=2, bucket=SATELLITE, action="CLOSE", quantity=3, price=0.90,
                 asset_class="OPTION", strategy="LONG_CALL", pnl=135.0, closes_trade_id=1),
        ]
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        assert snap[SATELLITE]["open_exposure"] == 0.0   # id=1 is closed
        assert snap[SATELLITE]["open_count"] == 0
        assert snap[SATELLITE]["realized_pnl"] == 135.0
        assert snap[SATELLITE]["wins"] == 1
        assert snap[SATELLITE]["closed"] == 1

    def test_realized_wins_losses_split_by_bucket(self):
        rows = [
            _row(id=1, bucket=CORE, action="CLOSE", pnl=200.0, closes_trade_id=10),
            _row(id=2, bucket=CORE, action="CLOSE", pnl=-50.0, closes_trade_id=11),
            _row(id=3, bucket=SATELLITE, action="CLOSE", pnl=-135.0, closes_trade_id=12),
            _row(id=4, bucket=SATELLITE, action="CLOSE", pnl=900.0, closes_trade_id=13),
        ]
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        assert snap[CORE]["wins"] == 1 and snap[CORE]["losses"] == 1
        assert snap[CORE]["realized_pnl"] == 150.0
        assert snap[SATELLITE]["wins"] == 1 and snap[SATELLITE]["losses"] == 1
        assert snap[SATELLITE]["realized_pnl"] == 765.0

    def test_null_bucket_is_core(self):
        rows = [_row(id=1, bucket=None, action="OPEN", quantity=10, price=5.0,
                     asset_class="EQUITY")]
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        assert snap[CORE]["open_exposure"] == 50.0

    def test_open_without_price_is_uncomputable(self):
        rows = [_row(id=1, bucket=SATELLITE, action="OPEN", quantity=None, price=None,
                     asset_class="OPTION", strategy="LONG_CALL")]
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        assert snap[SATELLITE]["open_count"] == 1
        assert snap[SATELLITE]["uncomputable_open"] == 1
        assert snap[SATELLITE]["open_exposure"] == 0.0


# ── satellite_soft_warnings ───────────────────────────────────────────────
class TestSoftWarnings:
    def test_within_cap_no_warnings(self):
        rows = [_row(id=1, bucket=SATELLITE, action="OPEN", quantity=1, price=1.0,
                     asset_class="OPTION", strategy="LONG_CALL")]  # $100 < $750 cap
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        assert barbell.satellite_soft_warnings(snap) == []

    def test_over_cap_warns(self):
        rows = [_row(id=1, bucket=SATELLITE, action="OPEN", quantity=20, price=1.0,
                     asset_class="OPTION", strategy="LONG_CALL")]  # $2000 > $750 cap
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        warns = barbell.satellite_soft_warnings(snap)
        assert any("exceeds your" in w and "cap" in w for w in warns)

    def test_negative_sleeve_warns_refund(self):
        rows = [
            _row(id=1, bucket=SATELLITE, action="CLOSE", pnl=-300.0, closes_trade_id=9),
        ]
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        warns = barbell.satellite_soft_warnings(snap)
        assert any("re-fund" in w for w in warns)

    def test_uncomputable_open_note(self):
        rows = [_row(id=1, bucket=SATELLITE, action="OPEN", asset_class="OPTION",
                     strategy="LONG_CALL")]  # no qty/price
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        warns = barbell.satellite_soft_warnings(snap)
        assert any("no" in w and "qty/price" in w for w in warns)


# ── render_status ─────────────────────────────────────────────────────────
class TestRenderStatus:
    def test_renders_both_sleeves_and_cap(self):
        rows = [
            _row(id=1, bucket=CORE, action="OPEN", quantity=10, price=50.0, asset_class="EQUITY"),
            _row(id=2, bucket=SATELLITE, action="OPEN", quantity=2, price=1.0,
                 asset_class="OPTION", strategy="LONG_CALL"),
        ]
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        out = barbell.render_status(snap)
        assert "Barbell" in out
        assert "Core" in out and "Satellite" in out
        assert "cap" in out.lower()

    def test_over_cap_shows_red_marker(self):
        rows = [_row(id=1, bucket=SATELLITE, action="OPEN", quantity=20, price=1.0,
                     asset_class="OPTION", strategy="LONG_CALL")]
        snap = barbell.snapshot(rows, contract_multiplier=_mult, capital=5000, cap_pct=15)
        out = barbell.render_status(snap)
        assert "OVER" in out


# ── core_scope_warning ──────────────────────────────────────────────────────
class TestCoreScopeWarning:
    # As of 2026-07-18, CORE_EDGE_TICKERS defaults to empty — every real-chains
    # structure tested so far had its pre-cost edge erased by realistic costs
    # (see research/naked_strangle_live_params_findings_2026_07_18.md), so the
    # honest default is "nothing has earned core status", not a placeholder
    # basket. These tests confirm that current (empty) default warns on
    # everything, then monkeypatch the constants to exercise the matching
    # logic itself independent of the current evidence state.

    def test_empty_default_warns_on_everything(self):
        assert not barbell.CORE_EDGE_TICKERS  # documents the current, correct default
        w = barbell.core_scope_warning("SPY", "STRANGLE")
        assert w is not None
        assert "nothing has cleared" in w

    def test_missing_fields_do_not_crash(self):
        w = barbell.core_scope_warning(None, None)
        assert w is not None
        assert "no strategy" in w

    def test_matching_combination_is_silent(self, monkeypatch):
        monkeypatch.setattr(barbell, "CORE_EDGE_TICKERS", frozenset({"SPY", "XLF"}))
        monkeypatch.setattr(barbell, "CORE_EDGE_STRATEGY", "STRANGLE")
        assert barbell.core_scope_warning("SPY", "STRANGLE") is None
        assert barbell.core_scope_warning("spy", "strangle") is None  # case-insensitive
        assert barbell.core_scope_warning("xlf", "Strangle") is None

    def test_right_ticker_wrong_strategy_warns(self, monkeypatch):
        monkeypatch.setattr(barbell, "CORE_EDGE_TICKERS", frozenset({"SPY"}))
        monkeypatch.setattr(barbell, "CORE_EDGE_STRATEGY", "STRANGLE")
        w = barbell.core_scope_warning("SPY", "LONG_CALL")
        assert w is not None
        assert "SPY" in w and "LONG_CALL" in w

    def test_right_strategy_wrong_ticker_warns(self, monkeypatch):
        monkeypatch.setattr(barbell, "CORE_EDGE_TICKERS", frozenset({"SPY"}))
        monkeypatch.setattr(barbell, "CORE_EDGE_STRATEGY", "STRANGLE")
        w = barbell.core_scope_warning("TSLA", "STRANGLE")
        assert w is not None
        assert "TSLA" in w
