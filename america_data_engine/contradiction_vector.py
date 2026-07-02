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
import math
from datetime import datetime, timezone
from typing import Any, Dict, List, Tuple

from retry_utils import fetch_with_retry

HTTP_TIMEOUT = (5, 10)
WEBHOOK_SECRET = os.getenv("WEBHOOK_SECRET_TOKEN", "")
HEARTBEAT_BASE = os.getenv("HEARTBEAT_BASE_URL", "http://heartbeat-monitor:8002")

# Tunable thresholds. Per the architecture constraint, the whole gate is
# bypassable via an explicit .env flag for backtesting.
# Defaults below are illustrative placeholders — production values are tuned
# via walk-forward validation and kept in a private .env, not in source.
SKEW_THRESHOLD = float(os.getenv("CONTRADICTION_SKEW_THRESHOLD", "0.05"))       # |skew_pct|
SENTIMENT_THRESHOLD = float(os.getenv("CONTRADICTION_SENTIMENT_THRESHOLD", "0.3"))
BYPASS = os.getenv("CONTRADICTION_BYPASS", "false").lower() in ("true", "1", "yes")

# Index proxy used for the market-wide check when sentiment isn't ticker-specific.
MARKET_PROXY = os.getenv("CONTRADICTION_MARKET_PROXY", "SPY")

# Half-life decay constants (hours) — sourced from DECAY_* in .env.
# Placeholder defaults; the tuned per-category values live in a private .env.
HALFLIFE_GEOPOLITICAL_HOURS = float(os.getenv("DECAY_GEO",      "24"))
HALFLIFE_MACRO_HOURS        = float(os.getenv("DECAY_MACRO",    "24"))
HALFLIFE_EARNINGS_HOURS     = float(os.getenv("DECAY_EARNINGS", "24"))
HALFLIFE_TECHNICAL_HOURS    = float(os.getenv("DECAY_TECHNICAL","24"))

# Anchor events — major structural news that act as reference points.
# Illustrative subset; the full curated keyword corpus is kept private.
_ANCHOR_KEYWORDS = [
    "sanction", "tariff", "war", "recession",
]

# Manipulation theme keywords — flag coordinated/artificial moves (use cautiously).
# Illustrative subset; the full curated list is kept private.
_MANIPULATION_THEMES = [
    "short squeeze", "pump and dump", "manipulation",
]


def _get_halflife_hours(category: str) -> float:
    """Map signal category to decay half-life in hours."""
    mapping = {
        "GEOPOLITICAL": HALFLIFE_GEOPOLITICAL_HOURS,
        "MACRO_ECONOMIC": HALFLIFE_MACRO_HOURS,
        "EARNINGS": HALFLIFE_EARNINGS_HOURS,
        "TECHNICAL": HALFLIFE_TECHNICAL_HOURS,
    }
    return mapping.get(category, HALFLIFE_TECHNICAL_HOURS)


def _is_anchor_event(headline: str, summary: str = "") -> bool:
    """
    Detects major structural news (Iran sanctions, Fed decisions, wars, etc.)
    that serve as reference points for sentiment evaluation.
    Anchor events should not be treated as noise even if unexpected.
    """
    text = f"{headline} {summary}".lower()
    return any(kw in text for kw in _ANCHOR_KEYWORDS)


def _detect_manipulation_theme(headline: str, summary: str = "") -> Tuple[bool, str]:
    """
    Flags articles describing artificial/coordinated moves (short squeezes, buybacks, etc.)
    Returns (is_manipulated, theme_tag).
    Manipulated signals should be weighted lower — real conviction comes from structural factors.
    """
    text = f"{headline} {summary}".lower()
    for theme in _MANIPULATION_THEMES:
        if theme in text:
            return True, theme
    return False, ""


def _compute_decay_weight(category: str, timestamp: Any) -> float:
    """
    Exponential decay: weight(t) = 2^(-t / halflife).
    Recent articles → weight ≈ 1.0. Old articles → weight → 0.
    If timestamp is missing, assume current time (full weight).
    """
    if not timestamp:
        return 1.0
    try:
        if isinstance(timestamp, str):
            # Try ISO format first, then fallback parsing
            if "T" in timestamp:
                article_time = datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
            else:
                article_time = datetime.fromisoformat(timestamp)
        else:
            article_time = timestamp

        # Ensure both are aware datetimes
        if article_time.tzinfo is None:
            article_time = article_time.replace(tzinfo=timezone.utc)
        now = datetime.now(tz=timezone.utc)

        age_hours = (now - article_time).total_seconds() / 3600.0
        if age_hours < 0:
            age_hours = 0  # future timestamp, treat as now
        halflife = _get_halflife_hours(category)
        weight = math.pow(2.0, -age_hours / halflife)
        return max(0.0, min(1.0, weight))  # clamp to [0, 1]
    except (ValueError, AttributeError, TypeError):
        return 1.0  # default to full weight on parse error


def fetch_iv_skew(ticker: str) -> Dict[str, Any]:
    """Query the heartbeat's internal IV skew endpoint for one ticker."""
    resp = fetch_with_retry(
        f"{HEARTBEAT_BASE}/iv/skew",
        source=f"IV skew:{ticker}",
        params={"ticker": ticker},
        headers={"X-Nox-Token": WEBHOOK_SECRET},
        timeout=HTTP_TIMEOUT,
    )
    if resp is None:
        return {"ticker": ticker, "method": "error", "error": "heartbeat unreachable after retries"}
    if resp.status_code == 200:
        return resp.json()
    return {"ticker": ticker, "method": "error", "error": f"HTTP {resp.status_code}"}


def aggregate_sentiment(news: List[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    """
    Aggregate per-headline sentiment into per-ticker buckets, plus a market-wide
    bucket keyed by MARKET_PROXY built from every scored headline.

    Applies:
    - Decay weighting (recent articles weighted higher, per-category half-life)
    - Per-source sentiment breakdown (see source disagreement)
    - Anchor event detection (major structural news)
    - Manipulation theme flagging (reduce weight on artificial moves)

    Returns {ticker: {"score", "magnitude", "category", "count", "latest_ts",
                      "sources": {...}, "anchor_event": bool,
                      "manipulation_theme": str|None}}
    plus a special "_anchor_events" key (List[str] of anchor headlines) that
    is NOT a per-ticker bucket — callers must pop/skip it before iterating.
    score is decay + magnitude + anchor-adjusted weighted mean.
    """
    buckets: Dict[str, Dict[str, Any]] = {}
    all_scores: List[float] = []
    all_weights: List[float] = []
    market_category_votes: Dict[str, int] = {}
    market_latest_ts = None
    anchor_events: List[str] = []  # store headlines of anchor events

    def _add(key: str, sent: Dict[str, Any], ts: Any, source: str, is_anchor: bool, manip_theme: str):
        b = buckets.setdefault(key, {
            "_score_w": 0.0, "_w": 0.0, "category_votes": {},
            "count": 0, "latest_ts": None, "sources": {}, "anchor_event": False, "manip_theme": None,
        })
        cat = sent.get("category", "GENERIC")
        decay = _compute_decay_weight(cat, ts)

        # Penalize manipulated signals: reduce weight if artificial
        manip_penalty = 0.5 if manip_theme else 1.0
        w = max(sent.get("magnitude", 0.0), 1e-3) * decay * manip_penalty  # floor so zero-mag still counts

        b["_score_w"] += sent.get("score", 0.0) * w
        b["_w"] += w
        b["count"] += 1
        b["category_votes"][cat] = b["category_votes"].get(cat, 0) + 1
        if ts and (b["latest_ts"] is None or str(ts) > str(b["latest_ts"])):
            b["latest_ts"] = ts
        if is_anchor:
            b["anchor_event"] = True
        if manip_theme:
            b["manip_theme"] = manip_theme

        # Track per-source sentiment for this ticker
        src = b["sources"].setdefault(source, {"_score_w": 0.0, "_w": 0.0, "count": 0})
        src["_score_w"] += sent.get("score", 0.0) * w
        src["_w"] += w
        src["count"] += 1

    for item in news:
        sent = item.get("sentiment")
        if not sent:
            continue
        ts = item.get("timestamp")
        cat = sent.get("category", "GENERIC")
        decay = _compute_decay_weight(cat, ts)
        source = item.get("source", "unknown")
        headline = item.get("headline", "")
        summary = item.get("summary", "")

        is_anchor = _is_anchor_event(headline, summary)
        is_manip, manip_theme = _detect_manipulation_theme(headline, summary)
        if is_anchor:
            anchor_events.append(headline)

        manip_penalty = 0.5 if is_manip else 1.0
        all_scores.append(sent.get("score", 0.0))
        all_weights.append(max(sent.get("magnitude", 0.0), 1e-3) * decay * manip_penalty)
        market_category_votes[cat] = market_category_votes.get(cat, 0) + 1
        if ts and (market_latest_ts is None or str(ts) > str(market_latest_ts)):
            market_latest_ts = ts
        for sym in (item.get("symbols") or []):
            _add(sym.upper(), sent, ts, source, is_anchor, manip_theme)

    # Finalise per-ticker weighted means and per-source breakdown.
    out: Dict[str, Dict[str, Any]] = {}
    for key, b in buckets.items():
        score = b["_score_w"] / b["_w"] if b["_w"] > 0 else 0.0
        dominant = max(b["category_votes"], key=b["category_votes"].get)

        sources_out = {}
        for src_name, src_data in b["sources"].items():
            src_score = src_data["_score_w"] / src_data["_w"] if src_data["_w"] > 0 else 0.0
            sources_out[src_name] = {"score": round(src_score, 4), "count": src_data["count"]}

        out[key] = {
            "score": round(score, 4),
            "magnitude": round(min(1.0, b["_w"] / 3.0), 4),
            "category": dominant,
            "count": b["count"],
            "latest_ts": b["latest_ts"],
            "sources": sources_out,
            "anchor_event": b["anchor_event"],
            "manipulation_theme": b.get("manip_theme"),
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
            "sources": {},
            "anchor_event": bool(anchor_events),
            "manipulation_theme": None,
        })

    # Stored separately (not a per-ticker bucket) — run_contradiction_check
    # pops this before iterating tickers.
    out["_anchor_events"] = anchor_events
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
    # "_anchor_events" is a List[str] side-channel, not a per-ticker bucket —
    # pop it before iterating tickers.
    anchor_events = sentiment.pop("_anchor_events", [])
    results = [evaluate_ticker(t, s) for t, s in sentiment.items()]
    # Surface contradictions first for quick human/LLM scanning.
    results.sort(key=lambda r: 0 if r.get("verdict", "").startswith("CONTRADICT") else 1)

    contradictions = [r for r in results if r.get("verdict", "").startswith("CONTRADICT")]
    data_gaps = [r["ticker"] for r in results if r.get("verdict") == "NO_DATA"]
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
        "anchor_events": anchor_events,
        "data_gaps": data_gaps,
        "complete": not data_gaps,
    }
    print(
        f"[INFO] [CONTRADICTION-VECTOR] Evaluated {len(results)} ticker(s); "
        f"{len(contradictions)} contradiction(s) flagged.",
        flush=True,
    )
    if data_gaps:
        print(
            f"[WARN] [CONTRADICTION-VECTOR] IV skew unavailable for: {', '.join(data_gaps)} "
            f"— verdicts for these tickers are NO_DATA, not a real signal.",
            flush=True,
        )
    return payload
