"""
Pure math for the Beneish M-Score (earnings-manipulation risk), FCF
burn-runway (bearish/fragility), and Piotroski F-Score (bullish/quality)
screens. No I/O — mirrors execution/SkepticIntelligence.hpp's "pure decision
layer" shape so this is unit-testable without a network call or a live SEC
fetch (fundamentals_scraper.py owns all I/O).

All scores are surface-only for this pass: nothing here feeds sizing or
gating. Every tunable threshold is env-sourced with a fake-safe default
(CLAUDE.md's "hardcode nothing tunable" rule) via Knobs.from_env().
"""
import os
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

# The 8 Beneish M-Score input fields (see fundamentals_scraper.BENEISH_TAGS).
_BENEISH_FIELDS = [
    "receivables", "revenue", "cogs", "current_assets", "ppe_net",
    "securities", "total_assets", "depreciation", "sga",
    "total_liabilities", "current_liabilities", "ltd", "net_income", "cfo",
]

# Piotroski's 9 criteria need these two annual periods' worth of fields — all
# already extracted for Beneish except shares_outstanding (see
# fundamentals_scraper.PIOTROSKI_EXTRA_TAGS).
_PIOTROSKI_FIELDS = [
    "net_income", "total_assets", "cfo", "ltd", "current_assets",
    "current_liabilities", "revenue", "cogs", "shares_outstanding",
]


@dataclass
class Knobs:
    beneish_threshold: float = -1.78          # FUNDAMENTALS_BENEISH_THRESHOLD
    fcf_runway_min_quarters: float = 3.5      # FUNDAMENTALS_FCF_RUNWAY_MIN_QUARTERS
    piotroski_flag_min_score: int = 8         # FUNDAMENTALS_PIOTROSKI_MIN_SCORE

    @classmethod
    def from_env(cls) -> "Knobs":
        return cls(
            beneish_threshold=float(os.getenv("FUNDAMENTALS_BENEISH_THRESHOLD", "-1.78")),
            fcf_runway_min_quarters=float(os.getenv("FUNDAMENTALS_FCF_RUNWAY_MIN_QUARTERS", "3.5")),
            piotroski_flag_min_score=int(os.getenv("FUNDAMENTALS_PIOTROSKI_MIN_SCORE", "8")),
        )


def _safe_div(numerator: Optional[float], denominator: Optional[float]) -> Optional[float]:
    if numerator is None or denominator is None or denominator == 0:
        return None
    return numerator / denominator


def compute_beneish_mscore(current: Dict[str, Optional[float]],
                            prior: Dict[str, Optional[float]]) -> Dict[str, Any]:
    """
    current/prior: dicts keyed by _BENEISH_FIELDS, for two consecutive fiscal
    years (current = most recent). Any missing field or zero-denominator
    ratio makes the whole score insufficient_data — Beneish's formula has no
    legitimate partial-data mode, so this is deliberately all-or-nothing —
    EXCEPT "securities" (ShortTermInvestments), which fundamentals_scraper.
    BENEISH_TAGS's own comment documents as "optional; treated as 0 if
    absent." That fallback was never actually implemented until now: a
    missing ShortTermInvestments tag (common — many filers hold none and
    never report the tag at all) was forcing insufficient_data on every
    such company, confirmed live against real AAPL/MSFT companyfacts data
    2026-07-16.
    """
    current = dict(current)
    prior = dict(prior)
    current["securities"] = current.get("securities") if current.get("securities") is not None else 0.0
    prior["securities"] = prior.get("securities") if prior.get("securities") is not None else 0.0

    missing = [f for f in _BENEISH_FIELDS if current.get(f) is None or prior.get(f) is None]
    if missing:
        return {"m_score": None, "components": {}, "missing_fields": missing, "insufficient_data": True}

    c, p = current, prior

    dsri = _safe_div(_safe_div(c["receivables"], c["revenue"]), _safe_div(p["receivables"], p["revenue"]))
    c_gross_margin = _safe_div(c["revenue"] - c["cogs"], c["revenue"])
    p_gross_margin = _safe_div(p["revenue"] - p["cogs"], p["revenue"])
    gmi = _safe_div(p_gross_margin, c_gross_margin)
    c_aqi = _safe_div(c["total_assets"] - c["current_assets"] - c["ppe_net"] - c["securities"], c["total_assets"])
    p_aqi = _safe_div(p["total_assets"] - p["current_assets"] - p["ppe_net"] - p["securities"], p["total_assets"])
    aqi = _safe_div(c_aqi, p_aqi)
    sgi = _safe_div(c["revenue"], p["revenue"])
    c_depi_rate = _safe_div(c["depreciation"], c["depreciation"] + c["ppe_net"])
    p_depi_rate = _safe_div(p["depreciation"], p["depreciation"] + p["ppe_net"])
    depi = _safe_div(p_depi_rate, c_depi_rate)
    sgai = _safe_div(_safe_div(c["sga"], c["revenue"]), _safe_div(p["sga"], p["revenue"]))
    c_lvg = _safe_div(c["total_liabilities"], c["total_assets"])
    p_lvg = _safe_div(p["total_liabilities"], p["total_assets"])
    lvgi = _safe_div(c_lvg, p_lvg)
    tata = _safe_div(c["net_income"] - c["cfo"], c["total_assets"])

    components = {
        "DSRI": dsri, "GMI": gmi, "AQI": aqi, "SGI": sgi,
        "DEPI": depi, "SGAI": sgai, "LVGI": lvgi, "TATA": tata,
    }
    if any(v is None for v in components.values()):
        missing = [k for k, v in components.items() if v is None]
        return {"m_score": None, "components": components, "missing_fields": missing, "insufficient_data": True}

    m_score = (
        -4.84
        + 0.920 * dsri
        + 0.528 * gmi
        + 0.404 * aqi
        + 0.892 * sgi
        + 0.115 * depi
        - 0.172 * sgai
        + 4.679 * tata
        - 0.327 * lvgi
    )
    return {"m_score": m_score, "components": components, "missing_fields": [], "insufficient_data": False}


def compute_fcf_runway(cash: Optional[float], cfo_q: Optional[float],
                        capex_q: Optional[float]) -> Dict[str, Any]:
    """
    quarterly_fcf = cfo_q - capex_q. runway_quarters is only meaningful (and
    computed) when quarterly_fcf is negative — a company with positive FCF
    isn't burning, so runway is reported as None/not-applicable rather than
    an infinite or misleading number.
    """
    if cash is None or cfo_q is None or capex_q is None:
        return {"runway_quarters": None, "quarterly_fcf": None, "is_burning": False, "insufficient_data": True}

    quarterly_fcf = cfo_q - capex_q
    is_burning = quarterly_fcf < 0
    runway_quarters = (cash / abs(quarterly_fcf)) if is_burning else None
    return {
        "runway_quarters": runway_quarters,
        "quarterly_fcf": quarterly_fcf,
        "is_burning": is_burning,
        "insufficient_data": False,
    }


def evaluate_fundamental_risk(raw: Dict[str, Any], knobs: Optional[Knobs] = None) -> Dict[str, Any]:
    """
    Combines both screens for one ticker's raw fetch (fundamentals_scraper.
    get_fundamentals_raw output) into the final flagged/scored shape the
    data-engine caches and heartbeat consumes.
    """
    knobs = knobs or Knobs.from_env()
    ticker = raw.get("ticker", "")

    if not raw.get("resolved") or raw.get("fetch_error"):
        return {
            "ticker": ticker,
            "beneish": {"m_score": None, "insufficient_data": True},
            "fcf_runway": {"runway_quarters": None, "insufficient_data": True},
            "flags": {"beneish_manipulation_risk": False, "fcf_burn_risk": False},
            "data_quality": "INSUFFICIENT",
            "fetch_error": raw.get("fetch_error"),
            "evaluated_at": datetime.now(tz=timezone.utc).isoformat(),
        }

    beneish = compute_beneish_mscore(raw.get("current_period", {}), raw.get("prior_period", {}))
    quarterly = raw.get("quarterly", {})
    fcf = compute_fcf_runway(quarterly.get("cash"), quarterly.get("cfo_q"), quarterly.get("capex"))

    beneish_flag = (not beneish["insufficient_data"]) and beneish["m_score"] > knobs.beneish_threshold
    fcf_flag = (
        not fcf["insufficient_data"]
        and fcf["runway_quarters"] is not None
        and fcf["runway_quarters"] < knobs.fcf_runway_min_quarters
    )

    both_insufficient = beneish["insufficient_data"] and fcf["insufficient_data"]
    either_insufficient = beneish["insufficient_data"] or fcf["insufficient_data"]
    data_quality = "INSUFFICIENT" if both_insufficient else ("PARTIAL" if either_insufficient else "OK")

    return {
        "ticker": ticker,
        "beneish": beneish,
        "fcf_runway": fcf,
        "flags": {"beneish_manipulation_risk": beneish_flag, "fcf_burn_risk": fcf_flag},
        "data_quality": data_quality,
        "fetch_error": None,
        "evaluated_at": datetime.now(tz=timezone.utc).isoformat(),
    }


def compute_piotroski_fscore(current: Dict[str, Optional[float]],
                              prior: Dict[str, Optional[float]]) -> Dict[str, Any]:
    """
    Standard 9-point Piotroski F-Score, all-or-nothing on missing fields —
    same rationale as compute_beneish_mscore: a partial F-Score (e.g. "7 of
    the 6 computable criteria") isn't the real Piotroski score and shouldn't
    be presented as one.

    current/prior: dicts keyed by _PIOTROSKI_FIELDS, for two consecutive
    fiscal years (current = most recent).
    """
    missing = [f for f in _PIOTROSKI_FIELDS if current.get(f) is None or prior.get(f) is None]
    if missing:
        return {"f_score": None, "criteria": {}, "missing_fields": missing, "insufficient_data": True}

    c, p = current, prior

    roa_c = _safe_div(c["net_income"], c["total_assets"])
    roa_p = _safe_div(p["net_income"], p["total_assets"])
    lev_c = _safe_div(c["ltd"], c["total_assets"])
    lev_p = _safe_div(p["ltd"], p["total_assets"])
    cr_c = _safe_div(c["current_assets"], c["current_liabilities"])
    cr_p = _safe_div(p["current_assets"], p["current_liabilities"])
    gm_c = _safe_div(c["revenue"] - c["cogs"], c["revenue"])
    gm_p = _safe_div(p["revenue"] - p["cogs"], p["revenue"])
    at_c = _safe_div(c["revenue"], c["total_assets"])
    at_p = _safe_div(p["revenue"], p["total_assets"])

    ratios = {
        "roa_c": roa_c, "roa_p": roa_p, "lev_c": lev_c, "lev_p": lev_p,
        "cr_c": cr_c, "cr_p": cr_p, "gm_c": gm_c, "gm_p": gm_p,
        "at_c": at_c, "at_p": at_p,
    }
    if any(v is None for v in ratios.values()):
        missing = [k for k, v in ratios.items() if v is None]
        return {"f_score": None, "criteria": {}, "missing_fields": missing, "insufficient_data": True}

    criteria = {
        "positive_roa": roa_c > 0,
        "positive_cfo": c["cfo"] > 0,
        "roa_improving": roa_c > roa_p,
        "cfo_exceeds_net_income": c["cfo"] > c["net_income"],
        "leverage_decreasing": lev_c < lev_p,
        "current_ratio_improving": cr_c > cr_p,
        "no_dilution": c["shares_outstanding"] <= p["shares_outstanding"],
        "gross_margin_improving": gm_c > gm_p,
        "asset_turnover_improving": at_c > at_p,
    }
    f_score = sum(1 for v in criteria.values() if v)
    return {"f_score": f_score, "criteria": criteria, "missing_fields": [], "insufficient_data": False}


def evaluate_bullish_quality(raw: Dict[str, Any], knobs: Optional[Knobs] = None) -> Dict[str, Any]:
    """
    Bullish mirror of evaluate_fundamental_risk() — same raw fetch
    (fundamentals_scraper.get_fundamentals_raw), different question: is this
    a structurally high-quality compounder rather than a fragile one. Flags
    piotroski_high_quality when the F-Score clears
    knobs.piotroski_flag_min_score (default 8, i.e. 8 or 9 of 9).
    """
    knobs = knobs or Knobs.from_env()
    ticker = raw.get("ticker", "")

    if not raw.get("resolved") or raw.get("fetch_error"):
        return {
            "ticker": ticker,
            "piotroski": {"f_score": None, "insufficient_data": True},
            "flags": {"piotroski_high_quality": False},
            "data_quality": "INSUFFICIENT",
            "fetch_error": raw.get("fetch_error"),
            "evaluated_at": datetime.now(tz=timezone.utc).isoformat(),
        }

    piotroski = compute_piotroski_fscore(raw.get("current_period", {}), raw.get("prior_period", {}))
    flag = (not piotroski["insufficient_data"]) and piotroski["f_score"] >= knobs.piotroski_flag_min_score
    data_quality = "INSUFFICIENT" if piotroski["insufficient_data"] else "OK"

    return {
        "ticker": ticker,
        "piotroski": piotroski,
        "flags": {"piotroski_high_quality": flag},
        "data_quality": data_quality,
        "fetch_error": None,
        "evaluated_at": datetime.now(tz=timezone.utc).isoformat(),
    }
