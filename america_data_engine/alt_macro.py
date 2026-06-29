"""
WS2 — Alternative Macro Pipeline (physical-supply verification).

The Skeptic premise applied to macro: a political announcement (OFAC license,
sanctions relief) is TEXT; tanker AIS counts and war-risk insurance premiums are
PHYSICAL REALITY. When a headline says "supply is returning" while ships have
stopped moving and underwriters are repricing risk upward, the physical data
wins. This module fuses both into a per-chokepoint verdict.

Sign convention (everything normalized to oil-price bias):
    +1  → supply CONSTRAINED  → BULLISH oil
    -1  → supply RELEASED     → BEARISH oil
"""

import math
import os
from datetime import datetime, timezone
from typing import Any, Dict, List

from scrapers import (
    fetch_marine_insurance_premiums,
    fetch_tanker_traffic,
    fetch_ofac_actions,
)

# Significance thresholds (env-tunable); below these a signal is treated as noise.
PHYSICAL_MIN = float(os.getenv("ALT_MACRO_PHYSICAL_MIN", "0.30"))
POLITICAL_MIN = float(os.getenv("ALT_MACRO_POLITICAL_MIN", "0.30"))
BYPASS = os.getenv("ALT_MACRO_BYPASS", "false").lower() in ("true", "1", "yes")

# Maritime chokepoints → affected instruments + the entity keywords that link an
# OFAC action to this region.
CHOKEPOINTS: Dict[str, Dict[str, Any]] = {
    "HORMUZ": {
        "label": "Strait of Hormuz",
        "commodity": "crude oil",
        "tickers": ["USO", "XLE", "CL"],
        "entities": ["iran", "hormuz", "persian gulf", "irgc"],
    },
    "RED_SEA": {
        "label": "Bab-el-Mandeb / Red Sea",
        "commodity": "crude & container shipping",
        "tickers": ["USO", "ZIM", "FTAI"],
        "entities": ["houthi", "yemen", "red sea", "bab-el-mandeb"],
    },
    "BLACK_SEA": {
        "label": "Black Sea",
        "commodity": "grain & crude",
        "tickers": ["WEAT", "USO"],
        "entities": ["russia", "ukraine", "black sea", "novorossiysk"],
    },
    "VENEZUELA": {
        "label": "Venezuela crude exports",
        "commodity": "heavy crude",
        "tickers": ["USO", "XLE"],
        "entities": ["venezuela", "pdvsa", "maduro"],
    },
}


def _clamp_tanh(x: float) -> float:
    if x > 20:
        return 1.0
    if x < -20:
        return -1.0
    return math.tanh(x)


def _physical_stress(insurance_rec: Dict[str, Any], traffic_rec: Dict[str, Any]) -> Dict[str, Any]:
    """
    Combine insurance-premium change and tanker-traffic deviation into a single
    physical-stress score in [-1, 1]. Positive = supply constrained (bullish oil).
    """
    signals = []
    detail = {}

    if insurance_rec:
        try:
            change = float(insurance_rec.get("change_pct", 0.0))
            # +25% premium move → strong stress; scale so 0.25 → ~0.64.
            sig = _clamp_tanh(change * 3.0)
            signals.append(sig)
            detail["insurance_change_pct"] = change
            detail["insurance_premium_pct"] = insurance_rec.get("war_risk_premium_pct")
        except (TypeError, ValueError):
            pass

    if traffic_rec and traffic_rec.get("deviation_pct") is not None:
        try:
            dev = float(traffic_rec["deviation_pct"])
            # Traffic DOWN (negative deviation) = disruption = positive stress.
            sig = _clamp_tanh(-dev * 3.0)
            signals.append(sig)
            detail["traffic_deviation_pct"] = dev
        except (TypeError, ValueError):
            pass

    if not signals:
        return {"score": None, "detail": detail}
    return {"score": round(sum(signals) / len(signals), 4), "detail": detail}


def _political_signal(ofac_actions: List[Dict[str, Any]], entities: List[str]) -> Dict[str, Any]:
    """
    Net political supply signal for a region from OFAC actions matching its
    entities. Positive = tightening (bullish oil), negative = easing.
    """
    matched = []
    net = 0.0
    for act in ofac_actions:
        low = act.get("title", "").lower()
        if any(e in low for e in entities):
            net += act.get("direction", 0)
            matched.append({"title": act.get("title"), "direction": act.get("direction")})
    if not matched:
        return {"score": None, "matched": []}
    return {"score": round(_clamp_tanh(net * 0.7), 4), "matched": matched}


def _bias_label(score: float) -> str:
    return "BULLISH_OIL" if score > 0 else "BEARISH_OIL"


def _evaluate_region(key: str, cfg: Dict[str, Any],
                     insurance: Dict[str, Any], traffic: Dict[str, Any],
                     ofac: List[Dict[str, Any]]) -> Dict[str, Any]:
    phys = _physical_stress(insurance.get("regions", {}).get(key, {}),
                            traffic.get("regions", {}).get(key, {}))
    pol = _political_signal(ofac, cfg["entities"])

    p_score = phys["score"]
    t_score = pol["score"]

    result = {
        "region": key,
        "label": cfg["label"],
        "commodity": cfg["commodity"],
        "tickers": cfg["tickers"],
        "physical_stress": p_score,
        "political_signal": t_score,
        "physical_detail": phys["detail"],
        "political_matched": pol["matched"],
    }

    have_phys = p_score is not None and abs(p_score) >= PHYSICAL_MIN
    have_pol = t_score is not None and abs(t_score) >= POLITICAL_MIN

    if not have_phys and not have_pol:
        result.update({"verdict": "NO_DATA", "bias": None,
                        "reason": "No significant physical or political signal."})
        return result

    if have_phys and have_pol:
        # Sign disagreement = the headline contradicts the ships. Trust physical.
        if (p_score > 0) != (t_score > 0):
            result.update({
                "verdict": "TEXT_CONTRADICTS_PHYSICAL",
                "bias": _bias_label(p_score),
                "reason": ("Political narrative implies " + _bias_label(t_score) +
                           " but physical supply data implies " + _bias_label(p_score) +
                           " — trusting physical reality."),
            })
        else:
            result.update({
                "verdict": "CONFIRM",
                "bias": _bias_label(p_score),
                "reason": "Physical supply data and political signal agree.",
            })
        return result

    if have_phys:
        result.update({"verdict": "PHYSICAL_ONLY", "bias": _bias_label(p_score),
                        "reason": "Physical stress present; no matching political action."})
    else:
        result.update({"verdict": "POLITICAL_ONLY", "bias": _bias_label(t_score),
                        "reason": "Political action present; physical data unavailable/quiet."})
    return result


def run_alt_macro_check() -> Dict[str, Any]:
    """
    Top-level entry point invoked by the data-engine scheduler. Produces the
    JSON written to the shared data bus / signal pipeline.
    """
    insurance = fetch_marine_insurance_premiums()
    traffic = fetch_tanker_traffic()
    ofac = fetch_ofac_actions()

    regions = [_evaluate_region(k, cfg, insurance, traffic, ofac)
               for k, cfg in CHOKEPOINTS.items()]
    # Surface contradictions first.
    regions.sort(key=lambda r: 0 if r["verdict"] == "TEXT_CONTRADICTS_PHYSICAL" else 1)

    contradictions = [r for r in regions if r["verdict"] == "TEXT_CONTRADICTS_PHYSICAL"]
    payload = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "bypass": BYPASS,
        "sources": {
            "insurance": insurance.get("source", "none"),
            "ais": traffic.get("source", "none"),
            "ofac_actions": len(ofac),
        },
        "physical_min": PHYSICAL_MIN,
        "political_min": POLITICAL_MIN,
        "contradiction_count": len(contradictions),
        "regions": regions,
    }
    print(
        f"[INFO] [ALT-MACRO] Evaluated {len(regions)} chokepoint(s); "
        f"{len(contradictions)} text-vs-physical contradiction(s).",
        flush=True,
    )
    return payload
