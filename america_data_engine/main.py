import os
import sys
import time
from datetime import datetime, timezone
from contextlib import asynccontextmanager
from typing import Any, Dict, List
from zoneinfo import ZoneInfo

from apscheduler.schedulers.background import BackgroundScheduler
from apscheduler.triggers.cron import CronTrigger
from fastapi import FastAPI, HTTPException, Security, status
from fastapi.security import APIKeyHeader

from scrapers import (
    fetch_news_with_fallback,
    fetch_earnings_calendar,
    fetch_tradable_universe,
    fetch_price_snapshots,
)
from contradiction_vector import run_contradiction_check
from insider_cluster import detect_insider_clusters
from alt_macro import run_alt_macro_check
from fundamentals_scraper import get_fundamentals_raw
from fundamentals_calc import evaluate_fundamental_risk, evaluate_bullish_quality, Knobs as FundamentalsKnobs

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

# ─────────────────────────────────────────────────────────────────────────────
# Earnings calendar + insider clusters must cover every ticker actually
# traded, not just the curated news-sentiment WATCHLIST above. Found
# 2026-07-19: the options-trading watchlists grew (liquidity curation) to
# ~41 tickers while WATCHLIST stayed at ~27, leaving 12 real, actively-traded
# equities (e.g. SMCI, ORCL, ADBE) with zero rows in /earnings/calendar.
# That's not just a report gap — execution/OptionsSignalGenerator.hpp's
# pre-earnings warning AND post-earnings IV-crush buffer read this exact
# same cache; a ticker missing from the dict is indistinguishable from
# "confirmed no earnings" (calendar.valid stays true), so both real-money
# risk gates were silently inert for every one of those 12 names. Union in
# the three options watchlists so earnings/insider coverage matches what's
# actually traded; WATCHLIST itself stays untouched for news sentiment
# (RULE-D6: fix the class — same "curated vs. actual" scope-gap pattern
# already found once for fundamentals-risk, recurring here for earnings).
# ─────────────────────────────────────────────────────────────────────────────
_options_watchlist_raw = ",".join([
    os.getenv("OPTIONS_BOT_WATCHLIST", ""),
    os.getenv("OPTIONS_PERSONAL_WATCHLIST", ""),
    os.getenv("OPTIONS_BREAKOUT_WATCHLIST", ""),
])
_options_tickers = [t.strip() for t in _options_watchlist_raw.split(",") if t.strip()]
# 2026-07-22: user asked why LMT's earnings never showed up in Scout — root
# cause was that LMT isn't traded/news-tracked anywhere (not in WATCHLIST or
# any options watchlist), so it was never in this union at all — silently
# invisible, same "curated vs. actual" gap as the 2026-07-19 fix above, but
# for a ticker with no trading/news reason to be added to those lists. This
# lets someone watch a ticker's earnings calendar specifically without
# pulling it into news sentiment or options trading scope.
_earnings_extra_tickers = [t.strip() for t in os.getenv("NOX_EARNINGS_EXTRA_TICKERS", "").split(",") if t.strip()]
EARNINGS_INSIDER_WATCHLIST = sorted(set(WATCHLIST) | set(_options_tickers) | set(_earnings_extra_tickers))

# ─────────────────────────────────────────────────────────────────────────────
# Broad universe for the fundamentals-risk (Beneish/FCF) scan ONLY — decoupled
# from WATCHLIST so news/earnings keep scanning just the curated names while
# fundamentals-risk watches the whole tradable market (user's explicit "keep
# the trading watchlist small, build a separate broad tier" preference).
# ─────────────────────────────────────────────────────────────────────────────
FUNDAMENTALS_UNIVERSE_MIN_PRICE = float(os.getenv("FUNDAMENTALS_UNIVERSE_MIN_PRICE", "5.0"))
FUNDAMENTALS_UNIVERSE_MAX_TICKERS = int(os.getenv("FUNDAMENTALS_UNIVERSE_MAX_TICKERS", "12000"))
FUNDAMENTALS_SCAN_RATE_LIMIT_PER_SEC = float(os.getenv("FUNDAMENTALS_SCAN_RATE_LIMIT_PER_SEC", "4.0"))
# Fixed daily clock time (America/New_York) rather than an interval-since-boot —
# a broad scan this size should run once, after market close, so it catches the
# day's news/filings and doesn't compete with market-hours Alpaca traffic from
# the options/equity engines. An interval timer would drift with every restart
# (the same class of bug as incident_scanner_zero_signals: a redeploy after the
# fire time pushes the next run a full day later).
FUNDAMENTALS_SCAN_HOUR_ET = int(os.getenv("FUNDAMENTALS_SCAN_HOUR_ET", "20"))

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
    "fundamentals": {},         # Beneish M-Score + FCF runway per ticker (surface-only, bearish)
    "last_fundamentals_update": None,  # ISO-8601 UTC timestamp of last fundamentals scan
    "fundamentals_bullish": {},  # Piotroski F-Score per ticker (surface-only, bullish)
    "last_fundamentals_bullish_update": None,  # ISO-8601 UTC timestamp of last bullish scan
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

    if news_us is not None:
        _CACHE["last_updated"] = datetime.now(tz=timezone.utc).isoformat()
        print(f"[INFO] [AMERICA-DATA-ENGINE] News refresh complete at {_CACHE['last_updated']}.",
              flush=True)
    else:
        print("[WARN] [AMERICA-DATA-ENGINE] News refresh failed entirely — last_updated NOT stamped "
              f"(still {_CACHE['last_updated']}); freshness must reflect the last real success.",
              flush=True)


def _refresh_earnings_cache() -> None:
    """
    Background worker — runs on startup and then every 24 hours via APScheduler.
    Fetches earnings dates for all watchlisted tickers over the next 30 days.
    RULE-008: Uses (5, 10) timeout on all HTTP calls.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting 24-hour earnings calendar refresh...", flush=True)

    earnings = fetch_earnings_calendar(EARNINGS_INSIDER_WATCHLIST)
    # fetch_earnings_calendar always returns one key per ticker (None on a failed
    # fetch, [] or a real list on success) — so `if earnings:` is truthy even when
    # every single ticker failed. Freshness must be judged on real successes, not
    # dict-truthiness.
    succeeded = earnings and any(events is not None for events in earnings.values())
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

    if succeeded:
        _CACHE["last_earnings_update"] = datetime.now(tz=timezone.utc).isoformat()
        print(f"[INFO] [AMERICA-DATA-ENGINE] Earnings refresh complete at {_CACHE['last_earnings_update']}.",
              flush=True)
    else:
        print("[WARN] [AMERICA-DATA-ENGINE] Earnings refresh failed for every ticker — "
              f"last_earnings_update NOT stamped (still {_CACHE['last_earnings_update']}).", flush=True)


def _refresh_insider_cache() -> None:
    """
    Background worker — runs on startup and then every 6 hours via APScheduler.
    Scans the watchlist's SEC Form 4 filings for insider buy clusters (WS3).
    Form 4 scanning is SEC-rate-limited and filings are sporadic, so a 6-hour
    cadence is ample — far less frequent than the 15-minute news cycle.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting insider Form 4 cluster scan...", flush=True)
    try:
        _CACHE["insider_clusters"] = detect_insider_clusters(EARNINGS_INSIDER_WATCHLIST)
        _CACHE["last_insider_update"] = datetime.now(tz=timezone.utc).isoformat()
        print(f"[INFO] [AMERICA-DATA-ENGINE] Insider scan complete at {_CACHE['last_insider_update']}.",
              flush=True)
    except Exception as e:
        print(f"[WARN] [AMERICA-DATA-ENGINE] Insider cluster scan failed: {e} — "
              f"last_insider_update NOT stamped (still {_CACHE['last_insider_update']}).", flush=True)


def _refresh_alt_macro_cache() -> None:
    """
    Background worker — runs on startup and then every 2 hours via APScheduler.
    Fuses marine insurance + AIS tanker traffic against OFAC actions (WS2) to
    detect text-vs-physical-reality contradictions at maritime chokepoints.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting alternative-macro check...", flush=True)
    try:
        _CACHE["alt_macro"] = run_alt_macro_check()
        _CACHE["last_alt_macro_update"] = datetime.now(tz=timezone.utc).isoformat()
        print(f"[INFO] [AMERICA-DATA-ENGINE] Alt-macro check complete at {_CACHE['last_alt_macro_update']}.",
              flush=True)
    except Exception as e:
        print(f"[WARN] [AMERICA-DATA-ENGINE] Alt-macro check failed: {e} — "
              f"last_alt_macro_update NOT stamped (still {_CACHE['last_alt_macro_update']}).", flush=True)


def _build_fundamentals_universe() -> List[str]:
    """
    Broad ticker universe for the fundamentals-risk (Beneish/FCF) scan —
    deliberately decoupled from WATCHLIST (news/earnings keep scanning only
    the curated names; this is the "separate broad tier" that watches the
    whole tradable market for structural fragility, e.g. an IBM/LCID-style
    blowup neither the news watchlist nor the options trading watchlist
    would ever look at).

    Every active/tradable US equity from Alpaca, price-filtered to drop
    penny-stock/warrant noise, capped as a circuit breaker set above the
    observed ~10k-ticker universe rather than a routine limiter (Alpaca's
    asset order carries no liquidity/relevance signal, so a binding cap
    would drop names arbitrarily; no silent truncation either way — a drop
    is logged). Falls back to WATCHLIST if the Alpaca
    universe fetch itself comes back empty (e.g. Alpaca outage), so a
    transient failure degrades to the old 9-ticker behavior instead of
    scanning nothing.
    """
    universe = fetch_tradable_universe()
    if not universe:
        print("[WARN] [AMERICA-DATA-ENGINE] Broad universe fetch empty — "
              "falling back to WATCHLIST for this fundamentals scan.", flush=True)
        return WATCHLIST

    prices = fetch_price_snapshots(universe)
    filtered = [t for t in universe if prices.get(t, 0) >= FUNDAMENTALS_UNIVERSE_MIN_PRICE]

    if len(filtered) > FUNDAMENTALS_UNIVERSE_MAX_TICKERS:
        dropped = len(filtered) - FUNDAMENTALS_UNIVERSE_MAX_TICKERS
        print(f"[WARN] [AMERICA-DATA-ENGINE] Fundamentals universe capped at "
              f"{FUNDAMENTALS_UNIVERSE_MAX_TICKERS} tickers ({dropped} dropped).", flush=True)
        filtered = filtered[:FUNDAMENTALS_UNIVERSE_MAX_TICKERS]

    print(f"[INFO] [AMERICA-DATA-ENGINE] Fundamentals universe: {len(filtered)} tickers "
          f"(price >= ${FUNDAMENTALS_UNIVERSE_MIN_PRICE}).", flush=True)
    return filtered


def _refresh_fundamentals_cache() -> None:
    """
    Background worker — runs on startup and then every 24 hours via
    APScheduler. Scans the broad market universe (see
    _build_fundamentals_universe) for Beneish M-Score/FCF burn-runway
    (bearish/fragility) AND Piotroski F-Score (bullish/quality) — surface-
    only for this pass, no sizing/gating impact. SEC XBRL company-facts only
    changes when a new 10-K/10-Q is filed, far slower than the news cycle,
    so a daily cadence is ample even at this scale.

    Both screens are evaluated from the SAME get_fundamentals_raw() fetch per
    ticker rather than two separate SEC round-trips — a doubled per-ticker
    SEC call count is exactly the class of bug that caused the 2026-07-16 403
    cascade (see fundamentals_scraper.py).

    Paced at FUNDAMENTALS_SCAN_RATE_LIMIT_PER_SEC between tickers — SEC's
    fair-use guidance caps at ~10 req/sec and this loop has no other
    throttling, so at thousands of tickers an unpaced loop risks getting
    rate-limited or blocked outright.

    Per-ticker exceptions are caught individually so one bad filing/ticker
    never aborts the whole scan.
    """
    print("[INFO] [AMERICA-DATA-ENGINE] Starting fundamentals (Beneish/FCF + Piotroski) scan...", flush=True)
    knobs = FundamentalsKnobs.from_env()
    universe = _build_fundamentals_universe()
    delay_s = 1.0 / FUNDAMENTALS_SCAN_RATE_LIMIT_PER_SEC
    bearish_results: Dict[str, Any] = {}
    bullish_results: Dict[str, Any] = {}
    for i, ticker in enumerate(universe):
        try:
            raw = get_fundamentals_raw(ticker)
            bearish_results[ticker] = evaluate_fundamental_risk(raw, knobs)
            bullish_results[ticker] = evaluate_bullish_quality(raw, knobs)
        except Exception as e:
            print(f"[WARN] [AMERICA-DATA-ENGINE] Fundamentals scan failed for {ticker}: {e}", flush=True)
            bearish_results[ticker] = {
                "ticker": ticker, "data_quality": "INSUFFICIENT",
                "flags": {"beneish_manipulation_risk": False, "fcf_burn_risk": False},
                "fetch_error": str(e),
            }
            bullish_results[ticker] = {
                "ticker": ticker, "data_quality": "INSUFFICIENT",
                "flags": {"piotroski_high_quality": False},
                "fetch_error": str(e),
            }
        if i < len(universe) - 1:
            time.sleep(delay_s)
    _CACHE["fundamentals"] = bearish_results
    _CACHE["fundamentals_bullish"] = bullish_results
    # NOTE: evaluate_fundamental_risk()/evaluate_bullish_quality() always
    # include a "fetch_error" key (None on success, a string on failure) —
    # checking key absence here was always False, so this warning fired on
    # every run regardless of actual success rate. Check the value instead.
    succeeded = any(r.get("fetch_error") is None for r in bearish_results.values())
    now_iso = datetime.now(tz=timezone.utc).isoformat()
    if succeeded:
        _CACHE["last_fundamentals_update"] = now_iso
        _CACHE["last_fundamentals_bullish_update"] = now_iso
        print(f"[INFO] [AMERICA-DATA-ENGINE] Fundamentals scan complete at {now_iso}.", flush=True)
    else:
        print("[WARN] [AMERICA-DATA-ENGINE] Fundamentals scan failed for every ticker — "
              f"last_fundamentals_update NOT stamped (still {_CACHE['last_fundamentals_update']}).", flush=True)


# ---------------------------------------------------------------------------
# Lifespan — handles startup scrape + scheduler lifecycle cleanly.
# ---------------------------------------------------------------------------
@asynccontextmanager
async def _lifespan(app: FastAPI):
    # Startup: refresh news, earnings, and insider clusters immediately.
    _refresh_cache()
    _refresh_earnings_cache()
    _refresh_insider_cache()
    _refresh_alt_macro_cache()
    # Fundamentals deliberately does NOT run on startup (unlike the others
    # above) — it's a broad, multi-thousand-ticker scan that can take tens of
    # minutes, and the whole point of the fixed clock-time schedule below is
    # to run it once, after market close, not on every container restart.

    scheduler = BackgroundScheduler(timezone="UTC")
    # News: refresh every 15 minutes
    scheduler.add_job(_refresh_cache, "interval", minutes=15, id="cache_refresh_news")
    # Earnings: refresh once every 24 hours
    scheduler.add_job(_refresh_earnings_cache, "interval", hours=24, id="cache_refresh_earnings")
    # Insider Form 4 clusters: refresh every 6 hours
    scheduler.add_job(_refresh_insider_cache, "interval", hours=6, id="cache_refresh_insider")
    # Alternative macro (physical vs political): refresh every 2 hours
    scheduler.add_job(_refresh_alt_macro_cache, "interval", hours=2, id="cache_refresh_alt_macro")
    # Fundamentals (Beneish/FCF): fixed daily clock time, not interval-since-
    # boot (see FUNDAMENTALS_SCAN_HOUR_ET comment above). misfire_grace_time
    # covers a restart shortly after the fire time (e.g. a same-evening
    # redeploy) without reaching all the way back to "always run at boot".
    scheduler.add_job(
        _refresh_fundamentals_cache,
        CronTrigger(hour=FUNDAMENTALS_SCAN_HOUR_ET, minute=0, timezone=ZoneInfo("America/New_York")),
        id="cache_refresh_fundamentals", misfire_grace_time=4 * 3600,
    )
    scheduler.start()
    print("[INFO] [AMERICA-DATA-ENGINE] APScheduler started.", flush=True)
    print("  • News cache refresh: every 15 minutes", flush=True)
    print("  • Earnings calendar refresh: every 24 hours", flush=True)
    print("  • Insider Form 4 cluster scan: every 6 hours", flush=True)
    print("  • Alt-macro physical/political check: every 2 hours", flush=True)
    print(f"  • Fundamentals (Beneish/FCF) scan: daily at {FUNDAMENTALS_SCAN_HOUR_ET:02d}:00 ET", flush=True)

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


# Per-ticker news is on-demand (unlike news_us's 15-min background refresh)
# since which ticker gets asked about is unpredictable — a small TTL cache
# avoids hammering Alpaca/NewsAPI/Polygon if the same ticker is requested
# repeatedly in a short window (e.g. re-running a report), without needing
# a whole scheduled-refresh cycle for a long-tail per-ticker query.
_TICKER_NEWS_CACHE_TTL_SECONDS = int(os.getenv("TICKER_NEWS_CACHE_TTL_SECONDS", "600"))
_ticker_news_cache: Dict[str, Dict[str, Any]] = {}


@app.get(
    "/news/us/{ticker}",
    summary="Latest news headlines scoped to one ticker (best-effort — see per-source filtering notes)",
    tags=["news"],
    dependencies=[Security(verify_token)],
)
def get_ticker_headlines(ticker: str) -> Dict[str, Any]:
    ticker = ticker.upper().strip()
    now_ts = datetime.now(timezone.utc).timestamp()
    cached = _ticker_news_cache.get(ticker)
    if cached and (now_ts - cached["fetched_at"]) < _TICKER_NEWS_CACHE_TTL_SECONDS:
        return {"last_updated": cached["last_updated"], "count": len(cached["news"]), "news": cached["news"], "cached": True}

    news = fetch_news_with_fallback(ticker=ticker)
    if news is None:
        # All sources unreachable — serve the last good cache if we have one
        # rather than a hard failure, same fail-open spirit as the rest of
        # this service; otherwise report the gap honestly.
        if cached:
            return {"last_updated": cached["last_updated"], "count": len(cached["news"]), "news": cached["news"], "cached": True, "stale": True}
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=f"All news sources unreachable for {ticker}")

    last_updated = datetime.now(timezone.utc).isoformat()
    _ticker_news_cache[ticker] = {"news": news, "last_updated": last_updated, "fetched_at": now_ts}
    return {"last_updated": last_updated, "count": len(news), "news": news, "cached": False}


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


@app.get(
    "/risk/fundamentals",
    summary="Beneish M-Score + FCF burn-runway per watchlisted ticker (surface-only)",
    tags=["signals"],
    dependencies=[Security(verify_token)],
)
def get_fundamentals_all() -> Dict[str, Any]:
    """
    Returns the cached fundamental-risk scan for the whole watchlist.
    Refreshed once every 24 hours (SEC XBRL company-facts only changes on a
    new 10-K/10-Q filing). Surface-only: no sizing/gating impact yet.
    Named /risk/fundamentals rather than /risk/distress to leave room for a
    later, separate Altman Z-Score endpoint without a naming collision.
    """
    return {
        "last_updated": _CACHE["last_fundamentals_update"],
        "results": _CACHE["fundamentals"],
    }


@app.get(
    "/risk/fundamentals/{ticker}",
    summary="Beneish M-Score + FCF burn-runway for one ticker (surface-only)",
    tags=["signals"],
    dependencies=[Security(verify_token)],
)
def get_fundamentals_ticker(ticker: str) -> Dict[str, Any]:
    ticker = ticker.upper().strip()
    result = _CACHE["fundamentals"].get(ticker)
    if result is None:
        return {
            "ticker": ticker,
            "data_quality": "INSUFFICIENT",
            "note": "Not in watchlist or not yet scanned.",
        }
    return result


@app.get(
    "/risk/fundamentals-bullish",
    summary="Piotroski F-Score per watchlisted ticker (surface-only, bullish mirror)",
    tags=["signals"],
    dependencies=[Security(verify_token)],
)
def get_fundamentals_bullish_all() -> Dict[str, Any]:
    """
    Bullish mirror of /risk/fundamentals — same universe/cache/CIK machinery,
    same scan, opposite question (structural quality vs. fragility). Surface-
    only: no sizing/gating impact yet.
    """
    return {
        "last_updated": _CACHE["last_fundamentals_bullish_update"],
        "results": _CACHE["fundamentals_bullish"],
    }


@app.get(
    "/risk/fundamentals-bullish/{ticker}",
    summary="Piotroski F-Score for one ticker (surface-only, bullish mirror)",
    tags=["signals"],
    dependencies=[Security(verify_token)],
)
def get_fundamentals_bullish_ticker(ticker: str) -> Dict[str, Any]:
    ticker = ticker.upper().strip()
    result = _CACHE["fundamentals_bullish"].get(ticker)
    if result is None:
        return {
            "ticker": ticker,
            "data_quality": "INSUFFICIENT",
            "note": "Not in watchlist or not yet scanned.",
        }
    return result
