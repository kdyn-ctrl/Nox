import os
import sys
from datetime import datetime, timezone
from contextlib import asynccontextmanager
from typing import Any, Dict, List

from apscheduler.schedulers.background import BackgroundScheduler
from fastapi import FastAPI, HTTPException, Security, status
from fastapi.security import APIKeyHeader

from scrapers import (
    fetch_cailian_news,
    fetch_china_pmi,
    fetch_eastmoney_hot_board,
    fetch_pboc_lpr,
)

# ---------------------------------------------------------------------------
# RULE-009: Hard-abort on missing credentials — no silent degraded starts.
# ---------------------------------------------------------------------------
def _require_env(name: str) -> str:
    val = os.getenv(name)
    if not val:
        print(f"[FATAL] [DATA-ENGINE] Required env var '{name}' is not set. Refusing to start.", flush=True)
        sys.exit(1)
    return val

WEBHOOK_SECRET = _require_env("WEBHOOK_SECRET_TOKEN")
print("[INFO] [DATA-ENGINE] All required environment variables validated.", flush=True)

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
#
# Why in-memory rather than Redis / SQLite?
#   • Data is always refreshed from source every 15 minutes — persistence adds
#     no value because stale cached data is worse than a fresh scrape.
#   • Keeps the container fully stateless: no volume mounts, no migrations.
#   • Sub-millisecond reads — the heartbeat gets an instant response every time.
#
# Thread safety: APScheduler runs the refresh job on its own thread pool.
# Python's GIL ensures that dict key assignment is atomic, so partial writes
# to individual cache keys cannot produce a torn read. The entire cache is
# never swapped as a single object — each key is updated independently after
# its scraper succeeds, which means a failing scraper never wipes a previously
# good value.
# ---------------------------------------------------------------------------
_CACHE: Dict[str, Any] = {
    "hot_board":   [],   # List[Dict] — East Money top 10
    "pmi":         {},   # Dict       — NBS Manufacturing & Non-Mfg PMI
    "lpr":         {},   # Dict       — PBOC 1Y / 5Y Loan Prime Rate
    "news_cn":     [],   # List[Dict] — Cailian Press headlines
    "last_updated": None,  # ISO-8601 UTC timestamp of last successful cycle
}


def _refresh_cache() -> None:
    """
    Background worker — runs on startup and then every 15 minutes via APScheduler.

    Each scraper is called independently. A failure in one source (e.g., AkShare
    rate-limiting the PMI endpoint) does not prevent the others from updating.
    Existing cache values are only overwritten when a scraper returns non-empty
    data, so a transient failure preserves the last good value rather than
    replacing it with an empty payload.
    """
    print("[INFO] [DATA-ENGINE] Starting scheduled scrape cycle...", flush=True)

    hot_board = fetch_eastmoney_hot_board()
    if hot_board:
        _CACHE["hot_board"] = hot_board

    pmi = fetch_china_pmi()
    if pmi:
        _CACHE["pmi"] = pmi

    lpr = fetch_pboc_lpr()
    if lpr:
        _CACHE["lpr"] = lpr

    news_cn = fetch_cailian_news()
    if news_cn:
        _CACHE["news_cn"] = news_cn

    _CACHE["last_updated"] = datetime.now(tz=timezone.utc).isoformat()
    print(f"[INFO] [DATA-ENGINE] Scrape cycle complete. Cache updated at {_CACHE['last_updated']}.", flush=True)


# ---------------------------------------------------------------------------
# Lifespan — handles startup scrape + scheduler lifecycle cleanly.
# Using the modern asynccontextmanager form (FastAPI ≥ 0.93) rather than the
# deprecated on_event decorator so the scheduler is always shut down gracefully
# even if the process receives SIGTERM mid-cycle.
# ---------------------------------------------------------------------------
@asynccontextmanager
async def _lifespan(app: FastAPI):
    # Warm the cache immediately so the very first request after startup never
    # returns empty data due to the scheduler not having fired yet.
    _refresh_cache()

    scheduler = BackgroundScheduler(timezone="UTC")
    scheduler.add_job(_refresh_cache, "interval", minutes=15, id="cache_refresh")
    scheduler.start()
    print("[INFO] [DATA-ENGINE] APScheduler started — cache refresh every 15 minutes.", flush=True)

    yield  # Application runs here

    scheduler.shutdown(wait=False)
    print("[INFO] [DATA-ENGINE] APScheduler shut down cleanly.", flush=True)


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
app = FastAPI(
    title="Nox Quant Data Engine",
    description=(
        "Dedicated microservice for Chinese financial macro and sentiment data. "
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
    """
    Returns the container's liveness status and whether the cache has been
    populated at least once. Used by Docker healthchecks and depends_on probes.
    No authentication required — this must be reachable before the first
    scraped cycle completes.
    """
    return {
        "status":      "healthy",
        "cache_ready": _CACHE["last_updated"] is not None,
        "last_updated": _CACHE["last_updated"],
    }


@app.get(
    "/news/cn",
    summary="Latest Cailian Press (财联社) headlines",
    tags=["news"],
    dependencies=[Security(verify_token)],
)
def get_chinese_headlines() -> Dict[str, Any]:
    """
    Returns up to 10 real-time Chinese financial wire headlines from Cailian Press.
    Cache is refreshed every 15 minutes. Response is always instant — no live
    scrape is triggered by this endpoint.
    """
    return {
        "last_updated": _CACHE["last_updated"],
        "count":        len(_CACHE["news_cn"]),
        "news":         _CACHE["news_cn"],
    }


@app.get(
    "/macro/china",
    summary="China NBS PMI + PBOC LPR",
    tags=["macro"],
    dependencies=[Security(verify_token)],
)
def get_china_macro() -> Dict[str, Any]:
    """
    Returns the latest NBS Manufacturing / Non-Manufacturing PMI and PBOC
    Loan Prime Rate (1Y + 5Y). Both are the primary macro inputs for PBOC
    vs Fed divergence analysis and Chinese industrial health scoring.
    """
    return {
        "last_updated": _CACHE["last_updated"],
        "pmi":          _CACHE["pmi"],
        "lpr":          _CACHE["lpr"],
    }


@app.get(
    "/sentiment/china",
    summary="East Money (东方财富) hot board — top 10 A-share retail sentiment",
    tags=["sentiment"],
    dependencies=[Security(verify_token)],
)
def get_china_sentiment() -> Dict[str, Any]:
    """
    Returns the top 10 most-watched A-share tickers from East Money's retail
    sentiment ranking. High retail attention often leads institutional flows by
    1–2 sessions — useful as a cross-market leading indicator alongside VIX/SPY.
    """
    return {
        "last_updated": _CACHE["last_updated"],
        "count":        len(_CACHE["hot_board"]),
        "hot_board":    _CACHE["hot_board"],
    }
