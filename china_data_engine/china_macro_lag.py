"""
WS8 — China macro information-lag signal (pure, stateless).

The edge, stated plainly: China's monthly PMI prints during Asia hours — hours
before the US options session on the Chinese-ADR basket (BABA/JD/PDD/BIDU/NIO)
can react to it. When a *fresh* print lands and Chinese retail media hasn't yet
propagated it (the existing WS7 `lag_open` check in edgar_cn_lag.py), the US
ADRs are presumed not to have fully priced the release — a directional edge on
the basket for the length of that lag window. Once the print is stale (the US
session has had a full day to absorb it) the edge is gone; we still surface the
macro *bias* but flag it `fresh=false` so the execution side sizes it as
confirmation, not as an unpriced move.

This mirrors edgar_cn_lag.py's design: PURE and stateless. compute_macro_lag()
takes the already-fetched PMI dict + the per-ticker media-lag flags and returns
per-ticker verdicts. The /lag/macro FastAPI endpoint in main.py wires the live
cache into it. Window tracking / alerting, if ever wanted, lives in heartbeat —
this service stays stateless.

Directional mapping is deliberately simple — we have free data and NO paid
consensus feed, so this is a level/direction read, not a surprise-vs-expected
model:
    manufacturing PMI > 50 (+ neutral band) → expansion → BULLISH the ADR basket
    manufacturing PMI < 50 (- neutral band) → contraction → BEARISH
    strength = clamp(|pmi - 50| / STRENGTH_DENOM, 0..1)
Non-manufacturing PMI is a confirmation tie-breaker: if the two disagree in
direction, strength is damped (a mixed macro read is lower-conviction).

Everything tunable is env-sourced with a fake-safe default (per the repo's
"hardcode nothing tunable" rule).
"""

import os
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional


def _adr_tickers() -> List[str]:
    raw = os.getenv("CHINA_LAG_ADR_TICKERS", "BABA,JD,PDD,BIDU,NIO")
    return [t.strip().upper() for t in raw.split(",") if t.strip()]


def _fresh_days() -> float:
    try:
        return float(os.getenv("CHINA_LAG_FRESH_DAYS", "10"))
    except ValueError:
        return 10.0


def _strength_denom() -> float:
    try:
        d = float(os.getenv("CHINA_PMI_STRENGTH_DENOM", "5.0"))
        return d if d > 0 else 5.0
    except ValueError:
        return 5.0


def _neutral_band() -> float:
    try:
        return float(os.getenv("CHINA_LAG_NEUTRAL_BAND", "0.3"))
    except ValueError:
        return 0.3


def _parse_ref_date(month: str) -> Optional[datetime]:
    """PMI 'month' is a reference-period date string like '2025-08-31'."""
    if not month or month == "N/A":
        return None
    try:
        return datetime.strptime(str(month)[:10], "%Y-%m-%d").replace(tzinfo=timezone.utc)
    except (ValueError, TypeError):
        return None


def _bias_from_pmi(mfg: float, non_mfg: float) -> Dict[str, Any]:
    """
    Resolve a directional bias + 0..1 strength from the manufacturing PMI, using
    non-manufacturing PMI as a confirmation tie-breaker.
    """
    band = _neutral_band()
    denom = _strength_denom()

    def dir_of(v: float) -> str:
        if v <= 0.0:
            return "neutral"  # 0.0 is the "no data" sentinel from fetch_china_pmi
        if v > 50.0 + band:
            return "bullish"
        if v < 50.0 - band:
            return "bearish"
        return "neutral"

    mfg_dir = dir_of(mfg)
    if mfg_dir == "neutral":
        return {"bias": "neutral", "strength": 0.0}

    strength = min(1.0, abs(mfg - 50.0) / denom)

    # Confirmation: if non-mfg has a reading and disagrees in direction, damp.
    non_dir = dir_of(non_mfg)
    if non_dir != "neutral" and non_dir != mfg_dir:
        strength *= 0.5  # mixed macro read → lower conviction

    return {"bias": mfg_dir, "strength": round(strength, 4)}


def compute_macro_lag(
    pmi: Dict[str, Any],
    media_lag_by_ticker: Dict[str, bool],
    now: Optional[datetime] = None,
) -> Dict[str, Any]:
    """
    Build the per-ADR macro-lag verdict payload.

    pmi                 — fetch_china_pmi() output: {month, manufacturing,
                          non_manufacturing, source, ...}. Empty dict = no data.
    media_lag_by_ticker — {TICKER: lag_open} from edgar_cn_lag.check_ticker_in_cn_media;
                          lag_open True means Chinese retail media has NOT yet
                          picked the name up (information hasn't propagated).
    now                 — injectable clock for testing; defaults to UTC now.

    A ticker is `fresh` (unpriced-edge open) only when BOTH the macro print is
    within the fresh window AND that ticker's media lag is still open. A missing
    media flag defaults to lag_open=True (absence of coverage evidence is not
    evidence of coverage) — never fabricates a closed lag.
    """
    now = now or datetime.now(tz=timezone.utc)
    tickers = _adr_tickers()

    if not pmi or (pmi.get("manufacturing", 0.0) <= 0.0 and
                   pmi.get("non_manufacturing", 0.0) <= 0.0):
        return {
            "generated_at": now.isoformat(),
            "pmi_source": pmi.get("source") if pmi else None,
            "reference_month": pmi.get("month") if pmi else None,
            "macro_bias": "neutral",
            "macro_strength": 0.0,
            "macro_fresh": False,
            "results": [],
            "data_gaps": ["china_pmi"],
            "complete": False,
        }

    mfg = float(pmi.get("manufacturing", 0.0))
    non_mfg = float(pmi.get("non_manufacturing", 0.0))
    verdict = _bias_from_pmi(mfg, non_mfg)

    ref = _parse_ref_date(pmi.get("month", ""))
    days_since = (now - ref).days if ref else None
    # No reference date → we cannot claim freshness; treat as stale (edge closed)
    # rather than guessing it's live.
    macro_fresh = (days_since is not None and days_since <= _fresh_days())

    release = "caixin_pmi" if str(pmi.get("source", "")).lower() == "caixin" else "nbs_pmi"

    results: List[Dict[str, Any]] = []
    for t in tickers:
        lag_open = media_lag_by_ticker.get(t, True)
        results.append({
            "ticker": t,
            "bias": verdict["bias"],
            "strength": verdict["strength"],
            # Per-ticker freshness: the macro print must be fresh AND this name's
            # media lag still open for the unpriced edge to apply to it.
            "fresh": bool(macro_fresh and lag_open),
            "release": release,
            "lag_open": bool(lag_open),
        })

    return {
        "generated_at": now.isoformat(),
        "pmi_source": pmi.get("source"),
        "reference_month": pmi.get("month"),
        "days_since_reference": days_since,
        "macro_bias": verdict["bias"],
        "macro_strength": verdict["strength"],
        "macro_fresh": bool(macro_fresh),
        "fresh_window_days": _fresh_days(),
        "results": results,
        "data_gaps": [],
        "complete": True,
    }
