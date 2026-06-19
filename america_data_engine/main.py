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
    "news_us":     [],   # List[Dict] — Alpaca news
    "last_updated": None,  # ISO-8601 UTC timestamp of last successful cycle
}


def _refresh_cache() -> None:
    """
    Background worker — runs on startup and then every 15 minutes via APScheduler.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting scheduled scrape cycle...", flush=True)

    news_us = fetch_alpaca_news()
    if news_us:
        _CACHE["news_us"] = news_us

    _CACHE["last_updated"] = datetime.now(tz=timezone.utc).isoformat()
    print(f"[INFO] [AMERICA-DATA-ENGINE] Scrape cycle complete. Cache updated at {_CACHE['last_updated']}.", flush=True)


# ---------------------------------------------------------------------------
# Lifespan — handles startup scrape + scheduler lifecycle cleanly.
# ---------------------------------------------------------------------------
@asynccontextmanager
async def _lifespan(app: FastAPI):
    _refresh_cache()

    scheduler = BackgroundScheduler(timezone="UTC")
    scheduler.add_job(_refresh_cache, "interval", minutes=15, id="cache_refresh")
    scheduler.start()
    print("[INFO] [AMERICA-DATA-ENGINE] APScheduler started — cache refresh every 15 minutes.", flush=True)

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
