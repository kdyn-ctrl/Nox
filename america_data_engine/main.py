import asyncio
import os
import sys
from datetime import datetime, timezone
from contextlib import asynccontextmanager
from typing import Any, Dict, List

from apscheduler.schedulers.background import BackgroundScheduler
from fastapi import FastAPI, HTTPException, Security, status
from fastapi.security import APIKeyHeader

from scrapers import (
    fetch_alpaca_news,
    fetch_earnings_calendar,
)

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
# Watchlist for earnings monitoring
# RULE-008: Earnings calendar is cached in-memory and refreshed once every 24h.
# ─────────────────────────────────────────────────────────────────────────────
WATCHLIST = ["AAPL", "TSLA", "NVDA", "MSFT", "BABA", "JD", "PDD", "BIDU", "NIO"]

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
    "news_us":           [],   # List[Dict] — Alpaca news
    "earnings_calendar": {},   # Dict[str, List[Dict]] — ticker -> earnings dates
    "last_updated":      None, # ISO-8601 UTC timestamp of last successful news cycle
    "last_earnings_update": None, # ISO-8601 UTC timestamp of last earnings refresh
}


def _refresh_cache() -> None:
    """
    Background worker — runs on startup and then every 15 minutes via APScheduler.
    Refreshes news only (lightweight); earnings are refreshed separately every 24h.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting scheduled news scrape...", flush=True)

    # Guard the whole cycle: a failing cycle must log and return, never raise,
    # so the recurring APScheduler job is not killed by a single bad run.
    try:
        news_us = fetch_alpaca_news()
        if news_us:
            _CACHE["news_us"] = news_us

        _CACHE["last_updated"] = datetime.now(tz=timezone.utc).isoformat()
        print(f"[INFO] [AMERICA-DATA-ENGINE] News refresh complete at {_CACHE['last_updated']}.",
              flush=True)
    except Exception as e:
        print(f"[ERROR] [AMERICA-DATA-ENGINE] News refresh failed: {e}", flush=True)
        return


def _refresh_earnings_cache() -> None:
    """
    Background worker — runs on startup and then every 24 hours via APScheduler.
    Fetches earnings dates for all watchlisted tickers over the next 30 days.
    RULE-008: Uses (5, 10) timeout on all HTTP calls.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting 24-hour earnings calendar refresh...", flush=True)

    try:
        earnings = fetch_earnings_calendar(WATCHLIST)
        if earnings:
            _CACHE["earnings_calendar"] = earnings
            total_events = sum(len(events) for events in earnings.values())
            print(f"[INFO] [AMERICA-DATA-ENGINE] Earnings calendar updated: {total_events} event(s) found.",
                  flush=True)

        _CACHE["last_earnings_update"] = datetime.now(tz=timezone.utc).isoformat()
        print(f"[INFO] [AMERICA-DATA-ENGINE] Earnings refresh complete at {_CACHE['last_earnings_update']}.",
              flush=True)
    except Exception as e:
        print(f"[ERROR] [AMERICA-DATA-ENGINE] Earnings refresh failed: {e}", flush=True)
        return


# ---------------------------------------------------------------------------
# Lifespan — handles startup scrape + scheduler lifecycle cleanly.
# ---------------------------------------------------------------------------
@asynccontextmanager
async def _lifespan(app: FastAPI):
    # Startup: refresh both news and earnings immediately. Run the (slow,
    # blocking) scrapes in a worker thread so we don't block the event loop
    # while the very first request waits on startup.
    await asyncio.to_thread(_refresh_cache)
    await asyncio.to_thread(_refresh_earnings_cache)

    scheduler = BackgroundScheduler(timezone="UTC")
    # News: refresh every 15 minutes
    scheduler.add_job(_refresh_cache, "interval", minutes=15, id="cache_refresh_news")
    # Earnings: refresh once every 24 hours
    scheduler.add_job(_refresh_earnings_cache, "interval", hours=24, id="cache_refresh_earnings")
    scheduler.start()
    print("[INFO] [AMERICA-DATA-ENGINE] APScheduler started.", flush=True)
    print("  • News cache refresh: every 15 minutes", flush=True)
    print("  • Earnings calendar refresh: every 24 hours", flush=True)

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
