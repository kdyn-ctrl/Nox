import os
import sys
from datetime import datetime, timezone
from contextlib import asynccontextmanager
from typing import Any, Dict, List

from apscheduler.schedulers.background import BackgroundScheduler
from fastapi import FastAPI, HTTPException, Security, status
from fastapi.security import APIKeyHeader

from scrapers import (
    fetch_news_with_fallback,
    fetch_earnings_calendar,
)
from contradiction_vector import run_contradiction_check
from insider_cluster import detect_insider_clusters
from alt_macro import run_alt_macro_check

# ---------------------------------------------------------------------------
# RULE-009: Hard-abort on missing credentials — no silent degraded starts.
# ---------------------------------------------------------------------------
def _require_env(name: str) -> str:
    val = os.getenv(name)
    if not val:
        print(f"[FATAL] [AMERICA-DATA-ENGINE] Required env var '{name}' is not set. Refusing to start.", flush=True)
        sys.exit(1)
    return val

WEBHOOK_SECRET = _require_env("WEBHOOK_SECRET_TOKEN")
print("[INFO] [AMERICA-DATA-ENGINE] All required environment variables validated.", flush=True)

# ─────────────────────────────────────────────────────────────────────────────
# Watchlist for earnings monitoring and news sentiment.
# Driven by NOX_WATCHLIST_US (domestic) + NOX_WATCHLIST_CN (Chinese ADRs) so
# that all services draw from the same source-of-truth in .env / docker-compose.
# RULE-008: Earnings calendar is cached in-memory and refreshed once every 24h.
# ─────────────────────────────────────────────────────────────────────────────
_us_raw = os.getenv("NOX_WATCHLIST_US", "AAPL,TSLA,NVDA,MSFT")
_cn_raw = os.getenv("NOX_WATCHLIST_CN", "BABA,JD,PDD,BIDU,NIO")
WATCHLIST = [t.strip() for t in (_us_raw + "," + _cn_raw).split(",") if t.strip()]

# ---------------------------------------------------------------------------
# Auth gate — RULE-004 style: every internal endpoint requires the shared
# secret in the X-Nox-Token header. Callers (heartbeat) supply it via env.
# auto_error=True means FastAPI returns 403 automatically for missing headers
# before our validator even runs.
# ---------------------------------------------------------------------------
_api_key_header = APIKeyHeader(name="X-Nox-Token", auto_error=True)

def verify_token(api_key: str = Security(_api_key_header)) -> None:
    """Rejects any request whose X-Nox-Token does not match WEBHOOK_SECRET_TOKEN."""
    if api_key != WEBHOOK_SECRET:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Forbidden: invalid token",
        )

# ---------------------------------------------------------------------------
# In-memory cache — all scrapers write here; all endpoints read from here.
# ---------------------------------------------------------------------------
_CACHE: Dict[str, Any] = {
    "news_us":           [],   # List[Dict] — Alpaca news (sentiment-scored)
    "earnings_calendar": {},   # Dict[str, List[Dict]] — ticker -> earnings dates
    "contradiction":     {},   # WS1 — latest Contradiction Vector evaluation
    "insider_clusters":  {},   # WS3 — latest insider cluster-buy signals
    "alt_macro":         {},   # WS2 — latest physical-vs-political macro check
    "last_updated":      None, # ISO-8601 UTC timestamp of last successful news cycle
    "last_earnings_update": None, # ISO-8601 UTC timestamp of last earnings refresh
    "last_insider_update": None,  # ISO-8601 UTC timestamp of last Form 4 scan
    "last_alt_macro_update": None, # ISO-8601 UTC timestamp of last alt-macro scan
    "news_volume_history": [],  # rolling window of article counts (for spike detection)
    "volume_spike_detected": False,  # flag if current count is anomalous
}


def _detect_volume_spike(current_count: int, window_size: int = 4) -> "tuple[bool, float, float]":
    """
    Detects if current article count is anomalously high.
    Returns (is_spike, mean, stddev) where is_spike=True if current > mean + 2sigma.
    Maintains a rolling window of recent counts.
    """
    history = _CACHE["news_volume_history"]
    history.append(current_count)
    if len(history) > window_size:
        history.pop(0)

    if len(history) < 2:
        return False, float(current_count), 0.0

    mean = sum(history[:-1]) / len(history[:-1])  # exclude current
    if len(history[:-1]) > 1:
        variance = sum((x - mean) ** 2 for x in history[:-1]) / len(history[:-1])
        stddev = variance ** 0.5
    else:
        stddev = 0.0

    is_spike = current_count > (mean + 2.0 * stddev) if stddev > 0 else False
    return is_spike, mean, stddev


def _refresh_cache() -> None:
    """
    Background worker — runs on startup and then every 15 minutes via APScheduler.
    Refreshes news via the multi-source fallback chain (Alpaca -> NewsAPI ->
    Polygon -> RSS), applies volume-spike detection, and runs the Contradiction
    Vector. Earnings are refreshed separately every 24h.

    fetch_news_with_fallback() returns None only if every source failed
    outright (data gap — keep the previous cache, do not overwrite with
    nothing) and [] if sources succeeded but legitimately found no articles
    (a quiet news day — still a valid, if empty, refresh).
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting scheduled news scrape with fallback logic...", flush=True)

    news_us = fetch_news_with_fallback()
    if news_us is None:
        print("[WARN] [AMERICA-DATA-ENGINE] All news sources failed after retries; keeping previous cache.", flush=True)
        _CACHE["volume_spike_detected"] = False
    elif news_us:
        _CACHE["news_us"] = news_us

        # Detect volume spikes (potential noise / breaking-event flood).
        is_spike, mean, stddev = _detect_volume_spike(len(news_us))
        _CACHE["volume_spike_detected"] = is_spike
        if is_spike:
            print(f"[WARN] [AMERICA-DATA-ENGINE] Volume spike detected: {len(news_us)} articles "
                  f"(mean: {mean:.1f}, sigma: {stddev:.1f})", flush=True)

        # WS1 — cross-check headline sentiment against live IV skew. Wrapped so a
        # heartbeat outage never blocks the (more critical) news cache refresh.
        try:
            _CACHE["contradiction"] = run_contradiction_check(news_us)
        except Exception as e:
            print(f"[WARN] [AMERICA-DATA-ENGINE] Contradiction check failed: {e}", flush=True)
    else:
        # Legitimate empty result — all reachable sources agreed there's
        # nothing new. Still record it as "up to date", just with zero items.
        _CACHE["news_us"] = news_us
        _CACHE["volume_spike_detected"] = False

    _CACHE["last_updated"] = datetime.now(tz=timezone.utc).isoformat()
    print(f"[INFO] [AMERICA-DATA-ENGINE] News refresh complete at {_CACHE['last_updated']}.",
          flush=True)


def _refresh_earnings_cache() -> None:
    """
    Background worker — runs on startup and then every 24 hours via APScheduler.
    Fetches earnings dates for all watchlisted tickers over the next 30 days.
    RULE-008: Uses (5, 10) timeout on all HTTP calls.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting 24-hour earnings calendar refresh...", flush=True)

    earnings = fetch_earnings_calendar(WATCHLIST)
    if earnings:
        _CACHE["earnings_calendar"] = earnings
        failed_tickers = [t for t, events in earnings.items() if events is None]
        total_events = sum(len(events) for events in earnings.values() if events)
        print(f"[INFO] [AMERICA-DATA-ENGINE] Earnings calendar updated: {total_events} event(s) found.",
              flush=True)
        if failed_tickers:
            print(
                f"[WARN] [AMERICA-DATA-ENGINE] Earnings fetch failed for: {', '.join(failed_tickers)} "
                f"— their earnings data is stale/missing this cycle.",
                flush=True,
            )

    _CACHE["last_earnings_update"] = datetime.now(tz=timezone.utc).isoformat()
    print(f"[INFO] [AMERICA-DATA-ENGINE] Earnings refresh complete at {_CACHE['last_earnings_update']}.",
          flush=True)


def _refresh_insider_cache() -> None:
    """
    Background worker — runs on startup and then every 6 hours via APScheduler.
    Scans the watchlist's SEC Form 4 filings for insider buy clusters (WS3).
    Form 4 scanning is SEC-rate-limited and filings are sporadic, so a 6-hour
    cadence is ample — far less frequent than the 15-minute news cycle.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting insider Form 4 cluster scan...", flush=True)
    try:
        _CACHE["insider_clusters"] = detect_insider_clusters(WATCHLIST)
    except Exception as e:
        print(f"[WARN] [AMERICA-DATA-ENGINE] Insider cluster scan failed: {e}", flush=True)

    _CACHE["last_insider_update"] = datetime.now(tz=timezone.utc).isoformat()
    print(f"[INFO] [AMERICA-DATA-ENGINE] Insider scan complete at {_CACHE['last_insider_update']}.",
          flush=True)


def _refresh_alt_macro_cache() -> None:
    """
    Background worker — runs on startup and then every 2 hours via APScheduler.
    Fuses marine insurance + AIS tanker traffic against OFAC actions (WS2) to
    detect text-vs-physical-reality contradictions at maritime chokepoints.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting alternative-macro check...", flush=True)
    try:
        _CACHE["alt_macro"] = run_alt_macro_check()
    except Exception as e:
        print(f"[WARN] [AMERICA-DATA-ENGINE] Alt-macro check failed: {e}", flush=True)

    _CACHE["last_alt_macro_update"] = datetime.now(tz=timezone.utc).isoformat()
    print(f"[INFO] [AMERICA-DATA-ENGINE] Alt-macro check complete at {_CACHE['last_alt_macro_update']}.",
          flush=True)


# ---------------------------------------------------------------------------
# Lifespan — handles startup scrape + scheduler lifecycle cleanly.
# ---------------------------------------------------------------------------
@asynccontextmanager
async def _lifespan(app: FastAPI):
    # Startup: refresh news, earnings, and insider clusters immediately
    _refresh_cache()
    _refresh_earnings_cache()
    _refresh_insider_cache()
    _refresh_alt_macro_cache()

    scheduler = BackgroundScheduler(timezone="UTC")
    # News: refresh every 15 minutes
    scheduler.add_job(_refresh_cache, "interval", minutes=15, id="cache_refresh_news")
    # Earnings: refresh once every 24 hours
    scheduler.add_job(_refresh_earnings_cache, "interval", hours=24, id="cache_refresh_earnings")
    # Insider Form 4 clusters: refresh every 6 hours
    scheduler.add_job(_refresh_insider_cache, "interval", hours=6, id="cache_refresh_insider")
    # Alternative macro (physical vs political): refresh every 2 hours
    scheduler.add_job(_refresh_alt_macro_cache, "interval", hours=2, id="cache_refresh_alt_macro")
    scheduler.start()
    print("[INFO] [AMERICA-DATA-ENGINE] APScheduler started.", flush=True)
    print("  • News cache refresh: every 15 minutes", flush=True)
    print("  • Earnings calendar refresh: every 24 hours", flush=True)
    print("  • Insider Form 4 cluster scan: every 6 hours", flush=True)
    print("  • Alt-macro physical/political check: every 2 hours", flush=True)

    yield  # Application runs here

    scheduler.shutdown(wait=False)
    print("[INFO] [AMERICA-DATA-ENGINE] APScheduler shut down cleanly.", flush=True)


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
app = FastAPI(
    title="Nox Quant America Data Engine",
    description=(
        "Dedicated microservice for US financial macro and sentiment data. "
        "All endpoints are authenticated via X-Nox-Token. "
        "Cache is refreshed every 15 minutes by an internal APScheduler job."
    ),
    version="1.0.0",
    lifespan=_lifespan,
)


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------

@app.get(
    "/health",
    summary="Liveness probe — no auth required",
    tags=["ops"],
)
def health_check() -> Dict[str, Any]:
    return {
        "status":      "healthy",
        "cache_ready": _CACHE["last_updated"] is not None,
        "last_updated_utc": _CACHE["last_updated"],
    }


@app.get(
    "/news/us",
    summary="Latest Alpaca news headlines",
    tags=["news"],
    dependencies=[Security(verify_token)],
)
def get_us_headlines() -> Dict[str, Any]:
    return {
        "last_updated": _CACHE["last_updated"],
        "count":        len(_CACHE["news_us"]),
        "news":         _CACHE["news_us"],
    }


@app.get(
    "/contradiction/us",
    summary="Latest Contradiction Vector evaluation (sentiment vs IV skew)",
    tags=["signals"],
    dependencies=[Security(verify_token)],
)
def get_contradiction_vector() -> Dict[str, Any]:
    """
    Returns the most recent Contradiction Vector result: per-ticker verdicts
    (CONFIRM / CONTRADICT_* / NEUTRAL / NO_DATA) plus WS4-ready decayed-sentiment
    records. Refreshed every 15 minutes alongside the news cache.
    """
    return _CACHE["contradiction"] or {
        "generated_at": None,
        "results": [],
        "sentiment_scores": [],
        "note": "Contradiction Vector has not run yet.",
    }


@app.get(
    "/macro/alt",
    summary="Alternative macro: physical supply vs political text (WS2)",
    tags=["signals"],
    dependencies=[Security(verify_token)],
)
def get_alt_macro() -> Dict[str, Any]:
    """
    Returns the latest alternative-macro evaluation: per-chokepoint verdicts
    fusing marine war-risk insurance + AIS tanker traffic against OFAC actions,
    flagging where political narrative contradicts physical reality. Refreshed
    every 2 hours.
    """
    return _CACHE["alt_macro"] or {
        "generated_at": None,
        "regions": [],
        "note": "Alt-macro check has not run yet.",
    }


@app.get(
    "/insider/clusters",
    summary="Latest insider Form 4 buy-cluster signals (WS3)",
    tags=["signals"],
    dependencies=[Security(verify_token)],
)
def get_insider_clusters() -> Dict[str, Any]:
    """
    Returns the most recent insider cluster scan: tickers where ≥2 distinct
    officers/directors made open-market purchases within a 48-hour window
    (Rule 10b5-1 pre-planned trades excluded). Refreshed every 6 hours.
    """
    return _CACHE["insider_clusters"] or {
        "generated_at": None,
        "signals": [],
        "note": "Insider cluster scan has not run yet.",
    }


@app.get(
    "/earnings/calendar",
    summary="Earnings calendar for next 30 days",
    tags=["earnings"],
    dependencies=[Security(verify_token)],
)
def get_earnings_calendar() -> Dict[str, Any]:
    """
    Returns the cached earnings calendar for all watchlisted tickers.
    Calendar is refreshed once every 24 hours.

    Response format:
    {
        "last_updated": "2026-06-25T14:30:00+00:00",
        "earnings_calendar": {
            "AAPL": [
                {"date": "2026-07-15", "description": "Q3 Earnings Announcement"}
            ],
            "TSLA": [...]
        }
    }
    """
    return {
        "last_updated": _CACHE["last_earnings_update"],
        "earnings_calendar": _CACHE["earnings_calendar"],
    }
