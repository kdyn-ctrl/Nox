"""
WS1 — Contradiction Vector.

Cross-checks headline sentiment (text) against live options-market IV skew
(price of fear). The premise: when the words and the money disagree, trust the
money. A bullish headline while puts are bid up (positive skew) is a CONTRADICTION
— the signal is suppressed (IGNORE) rather than acted on.

Data flow:
    scrapers.score_news_batch()  →  per-headline sentiment
    heartbeat /iv/skew           →  per-ticker put_iv - call_iv
    run_contradiction_check()    →  verdict JSON on the shared data bus

The output also carries WS4-compatible sentiment records (category + emission
timestamp) so the analyst's HalfLifeDecay can age them — this is the live data
source WS4's decay hook was built for.
"""

import os
import statistics
from datetime import datetime, timezone
from typing import Any, Dict, List

import requests

HTTP_TIMEOUT = (5, 10)
WEBHOOK_SECRET = os.getenv("WEBHOOK_SECRET_TOKEN", "")
HEARTBEAT_BASE = os.getenv("HEARTBEAT_BASE_URL", "http://heartbeat-monitor:8002")

# Tunable thresholds. Per the architecture constraint, the whole gate is
# bypassable via an explicit .env flag for backtesting.
SKEW_THRESHOLD = float(os.getenv("CONTRADICTION_SKEW_THRESHOLD", "0.05"))       # |skew_pct|
SENTIMENT_THRESHOLD = float(os.getenv("CONTRADICTION_SENTIMENT_THRESHOLD", "0.3"))
BYPASS = os.getenv("CONTRADICTION_BYPASS", "false").lower() in ("true", "1", "yes")

# Index proxy used for the market-wide check when sentiment isn't ticker-specific.
MARKET_PROXY = os.getenv("CONTRADICTION_MARKET_PROXY", "SPY")


def fetch_iv_skew(ticker: str) -> Dict[str, Any]:
    """Query the heartbeat's internal IV skew endpoint for one ticker."""
    try:
        resp = requests.get(
            f"{HEARTBEAT_BASE}/iv/skew",
            params={"ticker": ticker},
            headers={"X-Nox-Token": WEBHOOK_SECRET},
            timeout=HTTP_TIMEOUT,
        )
        if resp.status_code == 200:
            return resp.json()
        return {"ticker": ticker, "method": "error", "error": f"HTTP {resp.status_code}"}
    except requests.RequestException as e:
        return {"ticker": ticker, "method": "error", "error": str(e)}


def aggregate_sentiment(news: List[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    """
    Aggregate per-headline sentiment into per-ticker buckets, plus a market-wide
    bucket keyed by MARKET_PROXY built from every scored headline.

    Returns {ticker: {"score", "magnitude", "category", "count", "latest_ts"}}.
    score is the magnitude-weighted mean sentiment for that ticker.
    """
    buckets: Dict[str, Dict[str, Any]] = {}
    all_scores: List[float] = []
    all_weights: List[float] = []
    market_category_votes: Dict[str, int] = {}
    market_latest_ts = None

    def _add(key: str, sent: Dict[str, Any], ts: Any):
        b = buckets.setdefault(key, {
            "_score_w": 0.0, "_w": 0.0, "category_votes": {},
            "count": 0, "latest_ts": None,
        })
        w = max(sent.get("magnitude", 0.0), 1e-3)  # floor so zero-mag still counts
        b["_score_w"] += sent.get("score", 0.0) * w
        b["_w"] += w
        b["count"] += 1
        cat = sent.get("category", "GENERIC")
        b["category_votes"][cat] = b["category_votes"].get(cat, 0) + 1
        if ts and (b["latest_ts"] is None or str(ts) > str(b["latest_ts"])):
            b["latest_ts"] = ts

    for item in news:
        sent = item.get("sentiment")
        if not sent:
            continue
        ts = item.get("timestamp")
        all_scores.append(sent.get("score", 0.0))
        all_weights.append(max(sent.get("magnitude", 0.0), 1e-3))
        cat = sent.get("category", "GENERIC")
        market_category_votes[cat] = market_category_votes.get(cat, 0) + 1
        if ts and (market_latest_ts is None or str(ts) > str(market_latest_ts)):
            market_latest_ts = ts
        for sym in (item.get("symbols") or []):
            _add(sym.upper(), sent, ts)

    # Finalise per-ticker weighted means.
    out: Dict[str, Dict[str, Any]] = {}
    for key, b in buckets.items():
        score = b["_score_w"] / b["_w"] if b["_w"] > 0 else 0.0
        dominant = max(b["category_votes"], key=b["category_votes"].get)
        out[key] = {
            "score": round(score, 4),
            "magnitude": round(min(1.0, b["_w"] / 3.0), 4),
            "category": dominant,
            "count": b["count"],
            "latest_ts": b["latest_ts"],
        }

    # Market-wide bucket (used when no ticker-specific sentiment is available).
    if all_scores:
        wsum = sum(all_weights)
        market_score = sum(s * w for s, w in zip(all_scores, all_weights)) / wsum
        dominant = max(market_category_votes, key=market_category_votes.get)
        out.setdefault(MARKET_PROXY, {
            "score": round(market_score, 4),
            "magnitude": round(min(1.0, wsum / 6.0), 4),
            "category": dominant,
            "count": len(all_scores),
            "latest_ts": market_latest_ts,
        })
    return out


def _classify(sentiment_score: float, skew_pct: float) -> Dict[str, str]:
    """
    Resolve the verdict from sentiment vs skew.

    Positive skew_pct = puts bid up = bearish options positioning.
    """
    bullish_text = sentiment_score >= SENTIMENT_THRESHOLD
    bearish_text = sentiment_score <= -SENTIMENT_THRESHOLD
    bearish_skew = skew_pct >= SKEW_THRESHOLD
    bullish_skew = skew_pct <= -SKEW_THRESHOLD

    if bullish_text and bearish_skew:
        return {"verdict": "CONTRADICT_BULLISH", "action": "IGNORE",
                "reason": "Bullish headlines but puts are bid up (bearish IV skew)."}
    if bearish_text and bullish_skew:
        return {"verdict": "CONTRADICT_BEARISH", "action": "IGNORE",
                "reason": "Bearish headlines but calls are bid up (bullish IV skew)."}
    if (bullish_text and bullish_skew) or (bearish_text and bearish_skew):
        return {"verdict": "CONFIRM", "action": "PROCEED",
                "reason": "Text sentiment and IV skew agree."}
    return {"verdict": "NEUTRAL", "action": "NEUTRAL",
            "reason": "No strong agreement or disagreement."}


def evaluate_ticker(ticker: str, sent: Dict[str, Any]) -> Dict[str, Any]:
    """Run the full contradiction check for a single ticker."""
    skew = fetch_iv_skew(ticker)
    base = {
        "ticker": ticker,
        "sentiment_score": sent["score"],
        "sentiment_magnitude": sent["magnitude"],
        "category": sent["category"],
        "headline_count": sent["count"],
        "emitted_at": sent.get("latest_ts"),
    }
    if skew.get("method") != "live_chain":
        base.update({
            "skew": None, "skew_pct": None, "put_call_oi_ratio": None,
            "verdict": "NO_DATA", "action": "NEUTRAL",
            "reason": f"IV skew unavailable: {skew.get('error', 'unknown')}",
        })
        return base

    decision = _classify(sent["score"], skew["skew_pct"])
    base.update({
        "skew": skew["skew"],
        "skew_pct": skew["skew_pct"],
        "put_call_oi_ratio": skew.get("put_call_oi_ratio"),
        **decision,
    })
    # In bypass mode (backtest) we still record the verdict but never suppress.
    if BYPASS and base["action"] == "IGNORE":
        base["action"] = "PROCEED"
        base["reason"] += " [BYPASS: suppression disabled]"
    return base


def to_sentiment_scores(results: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """
    Project results into WS4 HalfLifeDecay input records:
        {category, score_0, emitted_at}
    The analyst computes age_hours against 'now' and decays score_0.
    Contradicted (IGNORE) signals are dropped — they carry no usable weight.
    """
    out = []
    for r in results:
        if r.get("action") == "IGNORE":
            continue
        out.append({
            "category": r["category"],
            "score_0": r["sentiment_score"],
            "emitted_at": r.get("emitted_at"),
        })
    return out


def run_contradiction_check(news: List[Dict[str, Any]]) -> Dict[str, Any]:
    """
    Top-level entry point invoked by the data-engine scheduler.
    Produces the JSON written to the shared data bus.
    """
    sentiment = aggregate_sentiment(news)
    results = [evaluate_ticker(t, s) for t, s in sentiment.items()]
    # Surface contradictions first for quick human/LLM scanning.
    results.sort(key=lambda r: 0 if r.get("verdict", "").startswith("CONTRADICT") else 1)

    contradictions = [r for r in results if r.get("verdict", "").startswith("CONTRADICT")]
    payload = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "bypass": BYPASS,
        "skew_threshold": SKEW_THRESHOLD,
        "sentiment_threshold": SENTIMENT_THRESHOLD,
        "tickers_evaluated": len(results),
        "contradiction_count": len(contradictions),
        "results": results,
        # WS4 feed: aged-decay input for the analyst.
        "sentiment_scores": to_sentiment_scores(results),
    }
    print(
        f"[INFO] [CONTRADICTION-VECTOR] Evaluated {len(results)} ticker(s); "
        f"{len(contradictions)} contradiction(s) flagged.",
        flush=True,
    )
    return payload
