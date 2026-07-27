import os
import re
import sys
import time
import json as _json
import requests
import xml.etree.ElementTree as ET
from datetime import datetime, timezone, timedelta
from typing import Dict, Any, List, Optional

from retry_utils import fetch_with_retry

# RULE-008: All HTTP calls use a (connect_timeout, read_timeout) tuple.
HTTP_TIMEOUT = (5, 10)

# SEC EDGAR requires a descriptive User-Agent or it returns 403. Match the
# convention already used by the heartbeat's EDGAR scraper.
SEC_USER_AGENT = os.getenv("SEC_USER_AGENT", "Nox/1.0 openclaw@vanhellsing.tech")

# ---------------------------------------------------------------------------
# WS1 — Headline sentiment scoring (Contradiction Vector input).
# ---------------------------------------------------------------------------
# Architecture constraint: the VPS only handles lightweight JSON responses and
# we avoid loading heavyweight NLP weights into the data-engine container. So
# sentiment here is a transparent, finance-tuned LEXICON scorer — deterministic,
# zero-dependency, and fast. It produces:
#   • a signed score in [-1, 1]      (negative = bearish, positive = bullish)
#   • a magnitude in [0, 1]          (confidence / strength of the signal)
#   • a category label matching the C++ SignalCategory enum used by WS4 decay
# A future upgrade can swap _lexicon_score() for an API/GGUF call without
# touching the contradiction logic that consumes this output.

# Signed sentiment lexicon (token -> weight). Tuned for market headlines.
_BULLISH_TERMS = {
    "beat": 1.0, "beats": 1.0, "surge": 1.0, "surges": 1.0, "soar": 1.0,
    "soars": 1.0, "rally": 0.9, "rallies": 0.9, "jump": 0.8, "jumps": 0.8,
    "gain": 0.6, "gains": 0.6, "rise": 0.5, "rises": 0.5, "upgrade": 0.9,
    "upgrades": 0.9, "outperform": 0.9, "record": 0.7, "strong": 0.6,
    "growth": 0.5, "profit": 0.5, "bullish": 1.0, "optimism": 0.6,
    "approval": 0.7, "approved": 0.7, "expands": 0.5, "boost": 0.6,
    "tops": 0.8, "rebound": 0.7, "recovery": 0.6, "breakthrough": 0.8,
}
_BEARISH_TERMS = {
    "miss": -1.0, "misses": -1.0, "plunge": -1.0, "plunges": -1.0,
    "crash": -1.0, "crashes": -1.0, "slump": -0.9, "slumps": -0.9,
    "fall": -0.6, "falls": -0.6, "drop": -0.7, "drops": -0.7,
    "decline": -0.6, "declines": -0.6, "downgrade": -0.9, "downgrades": -0.9,
    "underperform": -0.9, "warn": -0.8, "warning": -0.8, "warns": -0.8,
    "weak": -0.6, "loss": -0.6, "losses": -0.6, "bearish": -1.0,
    "fear": -0.7, "fears": -0.7, "probe": -0.6, "lawsuit": -0.6,
    "recall": -0.7, "cut": -0.5, "cuts": -0.5, "slowdown": -0.7,
    "default": -0.9, "bankruptcy": -1.0, "sanctions": -0.6, "selloff": -0.9,
}
_NEGATORS = {"not", "no", "without", "fails", "fail", "denies", "denied"}

# Category keyword routing → must match C++ SignalCategory tokens in WS4.
_CATEGORY_KEYWORDS = {
    "GEOPOLITICAL": ["war", "sanction", "sanctions", "tariff", "tariffs",
                     "conflict", "military", "strait", "hormuz", "ofac",
                     "geopolit", "missile", "invasion", "embargo"],
    "MACRO_ECONOMIC": ["fed", "fomc", "cpi", "inflation", "rate", "rates",
                       "jobs", "payroll", "gdp", "treasury", "yield",
                       "powell", "ecb", "recession", "pmi"],
    "EARNINGS": ["earnings", "revenue", "guidance", "eps", "quarter",
                 "profit", "forecast", "outlook", "results"],
    "TECHNICAL": ["breakout", "resistance", "support", "moving average",
                  "oversold", "overbought", "rsi", "trendline"],
}

_TOKEN_RE = re.compile(r"[a-z']+")
_TICKER_RE = re.compile(r"\b[A-Z]{1,5}\b")


# Pre-compile word-boundary matchers per category so "war" matches "war" but
# NOT "warn"/"warning"/"forward". Multiword keywords (e.g. "moving average")
# are matched as phrases. \b around the alternation handles both.
_CATEGORY_MATCHERS = {
    category: re.compile(
        r"\b(?:" + "|".join(re.escape(kw) for kw in keywords) + r")\b"
    )
    for category, keywords in _CATEGORY_KEYWORDS.items()
}


def _classify_category(text_lower: str) -> str:
    """Route a headline to a SignalCategory bucket by keyword presence."""
    for category, matcher in _CATEGORY_MATCHERS.items():
        if matcher.search(text_lower):
            return category
    return "GENERIC"


def score_headline_sentiment(headline: str, summary: str = "") -> Dict[str, Any]:
    """
    Lexicon sentiment for a single headline (+ optional summary).

    Returns:
        {
            "score":     float in [-1, 1]  (signed sentiment),
            "magnitude": float in [0, 1]   (confidence / strength),
            "category":  "GEOPOLITICAL" | "MACRO_ECONOMIC" | "EARNINGS"
                         | "TECHNICAL" | "GENERIC",
            "hits":      int               (number of sentiment tokens matched),
        }
    """
    text = f"{headline or ''} {summary or ''}"
    lower = text.lower()
    tokens = _TOKEN_RE.findall(lower)

    raw = 0.0
    hits = 0
    for i, tok in enumerate(tokens):
        weight = _BULLISH_TERMS.get(tok) or _BEARISH_TERMS.get(tok)
        if weight is None:
            continue
        # Flip polarity if the immediately-preceding token negates it
        # ("not strong", "fails to beat").
        if i > 0 and tokens[i - 1] in _NEGATORS:
            weight = -weight
        raw += weight
        hits += 1

    # Squash the unbounded sum into [-1, 1] with tanh so a single strong word
    # doesn't dominate and many mild words can still accumulate conviction.
    score = math_tanh(raw)
    magnitude = min(1.0, abs(raw) / 3.0)  # ~3 strong tokens → full confidence

    return {
        "score": round(score, 4),
        "magnitude": round(magnitude, 4),
        "category": _classify_category(lower),
        "hits": hits,
    }


def math_tanh(x: float) -> float:
    """Local tanh to avoid importing math just for one call."""
    # tanh(x) = (e^2x - 1) / (e^2x + 1); guard against overflow for large |x|.
    if x > 20:
        return 1.0
    if x < -20:
        return -1.0
    import math
    return math.tanh(x)


def should_include_article(headline: str, summary: str) -> bool:
    """
    Filters out low-signal topics (sports, entertainment, etc.) to reduce noise.
    Exclusion list configurable via EXCLUDE_TOPICS env var (comma-separated).
    """
    exclude_topics = os.getenv("EXCLUDE_TOPICS", "")
    if not exclude_topics:
        return True

    text = f"{headline} {summary}".lower()
    topics = [t.strip() for t in exclude_topics.split(",") if t.strip()]
    for topic in topics:
        if topic in text:
            return False
    return True


def score_news_batch(news: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """
    Attaches a `sentiment` block to each news item in place and returns the list.
    Tolerant of missing fields — never raises on a malformed item.
    """
    for item in news:
        try:
            item["sentiment"] = score_headline_sentiment(
                item.get("headline", ""), item.get("summary", "")
            )
        except Exception as e:  # never let one bad item break the batch
            print(f"[WARN] [SCRAPER] sentiment scoring failed: {e}", flush=True)
            item["sentiment"] = {
                "score": 0.0, "magnitude": 0.0, "category": "GENERIC", "hits": 0,
            }
    return news

def _require_env(name: str) -> str:
    val = os.getenv(name)
    if not val:
        raise EnvironmentError(f"Required env var '{name}' is not set.")
    return val

try:
    ALPACA_KEY = _require_env("ALPACA_API_KEY")
    ALPACA_SECRET = _require_env("ALPACA_SECRET_KEY")
except EnvironmentError as e:
    print(f"[FATAL] [AMERICA-DATA-ENGINE] {e}. Refusing to start.", flush=True)
    sys.exit(1)


ALPACA_BROKER_URL = os.getenv("ALPACA_BASE_URL", "https://paper-api.alpaca.markets")


def fetch_tradable_universe() -> List[str]:
    """
    Every active, tradable US-equity symbol Alpaca knows about (~6000-8000
    tickers) — the broad universe for the market-wide fundamentals-risk
    screen (see main.py's _build_fundamentals_universe). Filters out
    warrants/units/preferred shares (anything with non-alpha characters or
    >5 chars), same filter as heartbeat/monitor.py's fetch_market_universe().

    Fails open to [] on any error — a dead Alpaca feed degrades the
    fundamentals scan to its WATCHLIST fallback, never crashes it.
    """
    headers = {
        "APCA-API-KEY-ID": ALPACA_KEY,
        "APCA-API-SECRET-KEY": ALPACA_SECRET,
    }
    params = {"status": "active", "asset_class": "us_equity", "tradable": "true"}
    resp = fetch_with_retry(
        f"{ALPACA_BROKER_URL}/v2/assets", source="Alpaca asset list",
        headers=headers, params=params, timeout=HTTP_TIMEOUT,
    )
    if resp is None or resp.status_code != 200:
        print("[WARN] [SCRAPER] Alpaca asset list fetch failed.", flush=True)
        return []
    try:
        assets = resp.json()
        tickers = [
            a["symbol"] for a in assets
            if a.get("symbol", "").isalpha() and len(a["symbol"]) <= 5
        ]
        print(f"[INFO] [SCRAPER] Alpaca tradable universe: {len(tickers)} tickers.", flush=True)
        return tickers
    except Exception as e:
        print(f"[WARN] [SCRAPER] Alpaca asset list parse failed: {e}", flush=True)
        return []


def fetch_price_snapshots(tickers: List[str]) -> Dict[str, float]:
    """
    Batch last-trade price for many tickers via Alpaca's snapshots endpoint,
    chunked at 300 symbols/request (URL length limit — same chunking as
    heartbeat/monitor.py's fetch_batch_snapshots). Used only to price-filter
    the broad universe before the expensive per-ticker SEC fetch, so this
    returns just {ticker: price} rather than the fuller snapshot heartbeat's
    momentum scanner needs.
    """
    headers = {
        "APCA-API-KEY-ID": ALPACA_KEY,
        "APCA-API-SECRET-KEY": ALPACA_SECRET,
    }
    prices: Dict[str, float] = {}
    CHUNK = 300
    for i in range(0, len(tickers), CHUNK):
        chunk = tickers[i:i + CHUNK]
        resp = fetch_with_retry(
            "https://data.alpaca.markets/v2/stocks/snapshots",
            source=f"Alpaca snapshots chunk {i // CHUNK}",
            headers=headers, params={"symbols": ",".join(chunk), "feed": "iex"},
            timeout=HTTP_TIMEOUT,
        )
        if resp is None or resp.status_code != 200:
            continue
        try:
            for ticker, snap in resp.json().items():
                latest = snap.get("latestTrade") or {}
                daily = snap.get("dailyBar") or {}
                price = latest.get("p") or daily.get("c")
                if price:
                    prices[ticker] = price
        except Exception as e:
            print(f"[WARN] [SCRAPER] Alpaca snapshots chunk {i // CHUNK} parse failed: {e}", flush=True)
        time.sleep(0.5)  # respect rate limits between chunks
    return prices


def fetch_alpaca_news(ticker: Optional[str] = None) -> Optional[List[Dict[str, Any]]]:
    """
    Fetches the latest financial news from Alpaca's API. Pass `ticker` to
    scope results to one symbol (Alpaca's news endpoint natively supports a
    `symbols` filter) — used by the per-ticker digest endpoint; omitted for
    the existing global top-N headline feed.

    Returns None if the fetch failed after retries (distinct from a
    successful fetch that legitimately found zero articles, which returns
    []) so callers can tell "Alpaca is down" apart from "a quiet news day".
    """
    url = "https://data.alpaca.markets/v1beta1/news"
    headers = {
        "APCA-API-KEY-ID": ALPACA_KEY,
        "APCA-API-SECRET-KEY": ALPACA_SECRET,
    }
    params = {
        "limit": 10,
        "sort": "desc",
    }
    if ticker:
        params["symbols"] = ticker.upper()
    response = fetch_with_retry(url, source="Alpaca news", headers=headers, params=params, timeout=HTTP_TIMEOUT)
    if response is None:
        return None
    try:
        response.raise_for_status()
        news_data = response.json().get("news", [])

        result = []
        for item in news_data:
            result.append({
                "source":    item.get("source"),
                "headline":  item.get("headline"),
                "summary":   item.get("summary"),
                "url":       item.get("url"),
                "timestamp": item.get("created_at"),
                # WS1 — symbols let the Contradiction Vector map sentiment to the
                # ticker whose IV skew it should be cross-checked against.
                "symbols":   item.get("symbols", []),
            })

        # WS1 — attach lexicon sentiment to each headline so the Contradiction
        # Vector can cross-check text sentiment against live IV skew.
        score_news_batch(result)

        print(f"[INFO] [SCRAPER] Alpaca news fetched ({len(result)} articles, sentiment scored).", flush=True)
        return result
    except requests.RequestException as e:
        print(f"[ERROR] [SCRAPER] Alpaca news fetch failed: {e}", flush=True)
        return None


def fetch_newsapi_news(ticker: Optional[str] = None) -> Optional[List[Dict[str, Any]]]:
    """
    Fetches news from NewsAPI (free tier: 100 req/day, includes global + tech +
    finance). Filters for market-relevant keywords to avoid pure noise.
    Backup source used by fetch_news_with_fallback() when Alpaca fails.

    Pass `ticker` to scope the query to one symbol instead of the global
    keyword filter — NewsAPI has no dedicated ticker field, so this is a
    plain keyword search on the ticker string, best-effort by nature.

    Returns [] if the key is unset (source not configured — not a failure) or
    a legitimate empty result set. Returns None if the HTTP call itself failed
    after retries.
    """
    api_key = os.getenv("NEWSAPI_KEY")
    if not api_key:
        return []

    url = "https://newsapi.org/v2/everything"
    if ticker:
        q = ticker.upper()
    else:
        # Market-relevant keywords: earnings, inflation, Fed, tech, energy, geopolitics
        q = "(earnings OR inflation OR \"federal reserve\" OR fed OR nvda OR tsla OR aapl OR msft OR tech OR tariff OR sanctions OR oil OR energy) AND (market OR stock OR trading OR business)"
    params = {
        "q": q,
        "sortBy": "publishedAt",
        "language": "en",
        "pageSize": 20,
        "apiKey": api_key,
    }
    response = fetch_with_retry(url, source="NewsAPI news", params=params, timeout=HTTP_TIMEOUT)
    if response is None:
        return None
    try:
        response.raise_for_status()
        articles = response.json().get("articles", [])

        result = []
        for article in articles:
            headline = article.get("title", "")
            description = article.get("description", "")
            symbols = list(set(_TICKER_RE.findall(headline) + _TICKER_RE.findall(description)))

            result.append({
                "source": article.get("source", {}).get("name", "NewsAPI"),
                "headline": headline,
                "summary": description,
                "url": article.get("url"),
                "timestamp": article.get("publishedAt"),
                "symbols": symbols,
            })

        score_news_batch(result)
        print(f"[INFO] [SCRAPER] NewsAPI backup fetched ({len(result)} articles, sentiment scored).", flush=True)
        return result
    except requests.RequestException as e:
        print(f"[WARN] [SCRAPER] NewsAPI backup fetch failed: {e}", flush=True)
        return None


def fetch_polygon_news(ticker: Optional[str] = None) -> Optional[List[Dict[str, Any]]]:
    """
    Fetches news from Polygon.io free tier. Second backup source used by
    fetch_news_with_fallback() when both Alpaca and NewsAPI fail.

    Pass `ticker` to scope results to one symbol — Polygon's reference/news
    endpoint natively supports a `ticker` filter.

    Returns [] if the key is unset or a legitimate empty result set. Returns
    None if the HTTP call itself failed after retries.
    """
    api_key = os.getenv("POLYGON_API_KEY")
    if not api_key:
        return []

    url = "https://api.polygon.io/v2/reference/news"
    params = {
        "limit": 20,
        "sort": "-published_utc",
        "apiKey": api_key,
    }
    if ticker:
        params["ticker"] = ticker.upper()
    response = fetch_with_retry(url, source="Polygon news", params=params, timeout=HTTP_TIMEOUT)
    if response is None:
        return None
    try:
        response.raise_for_status()
        results = response.json().get("results", [])

        result = []
        for article in results:
            symbols = article.get("tickers", [])
            result.append({
                "source": "Polygon.io",
                "headline": article.get("title", ""),
                "summary": article.get("description", ""),
                "url": article.get("article_url"),
                "timestamp": article.get("published_utc"),
                "symbols": symbols,
            })

        score_news_batch(result)
        print(f"[INFO] [SCRAPER] Polygon backup fetched ({len(result)} articles, sentiment scored).", flush=True)
        return result
    except requests.RequestException as e:
        print(f"[WARN] [SCRAPER] Polygon backup fetch failed: {e}", flush=True)
        return None


# Free RSS feeds require no API key — last-resort fallback when Alpaca,
# NewsAPI, and Polygon have all failed or returned nothing.
_RSS_FEEDS = [
    ("Reuters Markets", "https://www.reutersagency.com/feed/?taxonomy=best-topics&post_type=best-news&storyline=market_news"),
    ("Reuters Business", "https://feeds.reuters.com/reuters/businessNews"),
    ("Reuters Tech", "https://feeds.reuters.com/reuters/technologyNews"),
    ("CNBC Top News", "https://feeds.cnbc.com/id/100003114/device/rss/rss.html"),
    ("DW Business", "https://www.dw.com/en/business/s-9097"),
]


def fetch_rss_news(ticker: Optional[str] = None) -> Optional[List[Dict[str, Any]]]:
    """
    Fetches news from free RSS feeds (Reuters, CNBC, DW News). Provides
    geopolitical + earnings + market news without an API key. Last-resort
    fallback source in fetch_news_with_fallback().

    Pass `ticker` to keep only items whose headline mentions it — RSS feeds
    have no native ticker filter, so this is a weak substring match, same
    "transparent about what it can't do well" spirit as the rest of this
    per-ticker path (Alpaca/Polygon's native filters are much stronger;
    this is deliberately the last resort, not the primary mechanism).

    Returns [] if every feed came back empty; returns None only if every
    single feed request failed outright (all sources down).
    """
    result: List[Dict[str, Any]] = []
    any_success = False

    for feed_name, feed_url in _RSS_FEEDS:
        response = fetch_with_retry(feed_url, source=f"RSS:{feed_name}", timeout=(5, 8))
        if response is None:
            print(f"[WARN] [SCRAPER] RSS feed '{feed_name}' failed after retries.", flush=True)
            continue
        try:
            response.raise_for_status()
            root = ET.fromstring(response.content)
            any_success = True

            # RSS 2.0 standard: items are under /rss/channel/item
            for item in root.findall(".//item")[:5]:  # limit to 5 per feed
                title = item.findtext("title", "")
                desc = item.findtext("description", "")
                pub_date = item.findtext("pubDate")
                link = item.findtext("link")
                symbols = _TICKER_RE.findall(title)

                result.append({
                    "source": feed_name,
                    "headline": title,
                    "summary": desc[:500] if desc else "",  # cap summary length
                    "url": link,
                    "timestamp": pub_date,
                    "symbols": list(set(symbols)),
                })
        except (requests.RequestException, ET.ParseError) as e:
            print(f"[WARN] [SCRAPER] RSS feed '{feed_name}' failed to parse: {e}", flush=True)
            continue

    if not any_success:
        return None

    if ticker:
        needle = ticker.upper()
        result = [item for item in result if needle in (item.get("headline") or "").upper()]

    score_news_batch(result)
    if result:
        print(f"[INFO] [SCRAPER] RSS feeds fetched ({len(result)} articles, sentiment scored).", flush=True)
    return result


def deduplicate_news(news_items: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """
    Deduplicates news items by headline hash to avoid double-scoring the same
    story from multiple sources. Keeps the first (most recent) occurrence.
    """
    import hashlib

    seen = set()
    deduped = []

    for item in news_items:
        headline = (item.get("headline") or "").lower().strip()
        if not headline:
            continue
        h = hashlib.md5(headline.encode()).hexdigest()
        if h not in seen:
            seen.add(h)
            deduped.append(item)

    return deduped


def fetch_news_with_fallback(ticker: Optional[str] = None) -> Optional[List[Dict[str, Any]]]:
    """
    Orchestrates multi-source news fetching with fallback logic.
    Tries sources in order: Alpaca (primary) -> NewsAPI -> Polygon -> RSS
    (last resort). Deduplicates, filters by topic, and scores all results
    uniformly.

    Pass `ticker` to scope every source to one symbol instead of the global
    feed — used by the per-ticker digest endpoint.

    Returns None only if ALL sources failed outright (network/API failures
    across the board) — distinct from a successful run that legitimately
    found zero qualifying articles, which returns []. Callers should treat
    None as a data gap the same way they treat fetch_alpaca_news() == None.
    """
    all_news: List[Dict[str, Any]] = []
    any_source_reachable = False

    news = fetch_alpaca_news(ticker=ticker)
    if news is not None:
        any_source_reachable = True
    if news:
        all_news.extend(news)
        print(f"[INFO] [SCRAPER] Primary (Alpaca) succeeded: {len(news)} items", flush=True)
    else:
        print("[WARN] [SCRAPER] Primary (Alpaca) failed or empty, trying backups...", flush=True)

        newsapi_items = fetch_newsapi_news(ticker=ticker)
        if newsapi_items is not None:
            any_source_reachable = True
        if newsapi_items:
            all_news.extend(newsapi_items)
            print(f"[INFO] [SCRAPER] Secondary (NewsAPI) succeeded: {len(newsapi_items)} items", flush=True)

        polygon_items = fetch_polygon_news(ticker=ticker)
        if polygon_items is not None:
            any_source_reachable = True
        if polygon_items:
            all_news.extend(polygon_items)
            print(f"[INFO] [SCRAPER] Secondary (Polygon) succeeded: {len(polygon_items)} items", flush=True)

        if not all_news:
            rss_items = fetch_rss_news(ticker=ticker)
            if rss_items is not None:
                any_source_reachable = True
            if rss_items:
                all_news.extend(rss_items)
                print(f"[INFO] [SCRAPER] Fallback (RSS) succeeded: {len(rss_items)} items", flush=True)

    if not any_source_reachable:
        print("[ERROR] [SCRAPER] All news sources failed outright — treating as data gap.", flush=True)
        return None

    deduped = deduplicate_news(all_news)

    filtered = [
        item for item in deduped
        if should_include_article(item.get("headline", ""), item.get("summary", ""))
    ]
    if len(filtered) < len(deduped):
        excluded = len(deduped) - len(filtered)
        print(f"[INFO] [SCRAPER] Topic filter excluded {excluded} article(s).", flush=True)

    print(f"[INFO] [SCRAPER] After dedup + filter: {len(filtered)} unique articles", flush=True)
    return filtered


FINNHUB_API_KEY = os.getenv("FINNHUB_API_KEY", "")


def fetch_earnings_calendar(tickers: List[str]) -> Dict[str, Optional[List[Dict[str, Any]]]]:
    """
    Queries Finnhub's free /calendar/earnings endpoint for scheduled earnings
    announcements over the next 30 days. Returns a dict mapping each ticker
    to a list of earnings dates.

    Format: {
        "AAPL": [{"date": "2026-07-30", "description": "Q3 2026 earnings (amc)"}, ...],
        "TSLA": [],
        ...
    }
    A ticker maps to None (not []) only if the whole-market fetch itself
    failed after retries, so a real "no earnings scheduled" is never confused
    with "the API call failed".

    2026-07-19: this used to call Alpaca's /v2/corporate-actions with
    types=earnings — confirmed live that Alpaca's real endpoint
    (data.alpaca.markets/v1/corporate-actions) 400s with "invalid ca type:
    earnings" (its supported types are splits/dividends/mergers/spin-offs/
    etc., never earnings), and the URL this code actually called
    (api.alpaca.markets/v2/corporate-actions) 404s outright — Alpaca has
    never supported earnings-date data at all, so this function had never
    returned a real earnings date in production for any ticker. Finnhub's
    free tier does carry real earnings dates (confirmed live) and its
    /calendar/earnings endpoint returns the WHOLE MARKET for a date range in
    one call, so this is now one request total instead of one per ticker —
    filtered down to `tickers` client-side.

    2026-07-22: confirmed live against the real key that a single 30-day-wide
    call silently truncates at exactly 1500 events (Finnhub free-tier response
    cap) — a 14-day window already hits 1500, and the events that survive the
    cap are NOT the nearest-term ones (a live 30-day call returned only
    August dates for our watchlist, dropping GOOGL's SAME-DAY and LMT's
    NEXT-DAY earnings entirely). That silently broke both the Scout report's
    "reports TODAY/in N days" section AND the C++ options engine's pre/post-
    earnings buffer gates (OptionsSignalGenerator.hpp's hasEarningsWithin5Days/
    hasRecentEarnings both read this same cache) — a real-money-adjacent
    correctness bug, not just a cosmetic report gap. Fixed by chunking the
    30-day window into 10-day slices (confirmed live at ~1361 events, safely
    under the 1500 cap) and merging results, so no sub-window can silently
    drop near-term dates the way one wide call did.

    RULE-008: Enforces (5, 10) timeout tuple on all HTTP calls.
    """
    if not FINNHUB_API_KEY:
        print("[WARN] [SCRAPER] FINNHUB_API_KEY not set — earnings calendar unavailable.", flush=True)
        return {t: None for t in tickers}

    now_utc = datetime.now(tz=timezone.utc)
    total_days = 30
    # 2026-07-22: 10-day chunks (~1361 events off-peak) still hit the 1500 cap
    # during a dense reporting week live (2026-08-02..2026-08-12: 1500,
    # confirmed truncated) — even a 5-day window got as high as 1462 events
    # in the same peak week. Single-day counts topped out at ~453 in that
    # same week, so 3-day chunks (10 calls/refresh, once per 24h — trivial
    # against Finnhub's free-tier rate limit) leave real margin instead of
    # chasing the cap with a slightly smaller number.
    chunk_days = 3
    ticker_set = set(tickers)
    result: Dict[str, Optional[List[Dict[str, Any]]]] = {t: [] for t in tickers}
    first_chunk_failed = False

    for offset in range(0, total_days, chunk_days):
        start_date = (now_utc + timedelta(days=offset)).date().isoformat()
        end_date = (now_utc + timedelta(days=min(offset + chunk_days, total_days))).date().isoformat()

        response = fetch_with_retry(
            "https://finnhub.io/api/v1/calendar/earnings",
            source="Finnhub earnings calendar",
            params={"from": start_date, "to": end_date, "token": FINNHUB_API_KEY},
            timeout=HTTP_TIMEOUT,
        )
        if response is None or response.status_code != 200:
            print(f"[WARN] [SCRAPER] Finnhub earnings calendar fetch failed for "
                  f"{start_date}..{end_date} (status={response.status_code if response else 'N/A'}).",
                  flush=True)
            if offset == 0:
                # The near-term chunk is the one Scout's "reports TODAY/in Nd"
                # section and the options engine's 5-day pre/post-earnings
                # gates actually depend on — treat its failure as a full
                # fetch failure (None for every ticker) rather than silently
                # rendering "no earnings" from an empty-but-not-None result.
                # A later, longer-horizon chunk failing is lower-stakes
                # (informational only) and just logs above.
                first_chunk_failed = True
            continue

        try:
            events = response.json().get("earningsCalendar", [])
        except Exception as e:
            print(f"[WARN] [SCRAPER] Finnhub earnings calendar parse failed for "
                  f"{start_date}..{end_date}: {e}", flush=True)
            if offset == 0:
                first_chunk_failed = True
            continue

        if len(events) >= 1500:
            print(f"[WARN] [SCRAPER] Finnhub earnings calendar chunk {start_date}..{end_date} "
                  f"hit {len(events)} events — may still be truncated; consider a smaller chunk_days.",
                  flush=True)
        for event in events:
            symbol = event.get("symbol")
            date = event.get("date")
            if symbol not in ticker_set or not date:
                continue
            hour = event.get("hour") or ""
            description = f"Q{event.get('quarter', '?')} {event.get('year', '?')} earnings"
            if hour:
                description += f" ({hour})"
            result[symbol].append({"date": date, "description": description})

    if first_chunk_failed:
        return {t: None for t in tickers}

    for ticker, earnings_list in result.items():
        if earnings_list:
            print(f"[INFO] [SCRAPER] Earnings found for {ticker}: {len(earnings_list)} event(s).", flush=True)

    return result


# ---------------------------------------------------------------------------
# WS3 — SEC EDGAR Form 4 (insider transactions) scraper.
# ---------------------------------------------------------------------------
# Form 4 reports an insider's change in beneficial ownership. We care about
# OPEN-MARKET PURCHASES (transaction code "P") by officers/directors, because
# discretionary buying is the highest-conviction insider signal. We explicitly
# DISCARD anything executed under a Rule 10b5-1 pre-arranged plan — those trades
# are scheduled in advance and carry no informational edge.
#
# Transaction codes of interest:
#   P = open-market / private purchase  (acquired)   ← the signal
#   S = open-market / private sale      (disposed)
#   A = grant/award, M = option exercise, G = gift, etc. (ignored — not a bet)

_SEC_HEADERS = {"User-Agent": SEC_USER_AGENT}


def _resolve_form4_xml_url(index_url: str) -> Optional[str]:
    """
    From a Form 4 filing index page, find the primary ownership XML document.
    Skips the XSL-rendered viewer links; returns the raw .xml href, or None.
    """
    resp = fetch_with_retry(index_url, source=f"SEC Form4 index:{index_url}", headers=_SEC_HEADERS, timeout=HTTP_TIMEOUT)
    if resp is None or resp.status_code != 200:
        return None
    # Index pages list document hrefs; the ownership doc ends in .xml and is
    # NOT under the /xslF345X0?/ rendering path.
    hrefs = re.findall(r'href="([^"]+\.xml)"', resp.text, flags=re.IGNORECASE)
    for href in hrefs:
        if "xsl" in href.lower():
            continue
        if href.startswith("/"):
            return f"https://www.sec.gov{href}"
        if href.startswith("http"):
            return href
        # Relative to the index page directory
        base = index_url.rsplit("/", 1)[0]
        return f"{base}/{href}"
    return None


def _txn_is_10b5_1(txn: ET.Element, footnotes: Dict[str, str]) -> bool:
    """
    True if a transaction references a Rule 10b5-1 plan, via either the modern
    explicit flag or a footnote referenced from within the transaction subtree.
    """
    # Modern Form 4 (post-2023) carries an explicit per-transaction flag.
    for tag in ("transactionCoding/aff10b5One", "aff10b5One"):
        v = txn.findtext(tag)
        if v and v.strip().lower() in ("1", "true"):
            return True
    # Footnote-based detection: collect footnoteId refs in this transaction.
    for fn in txn.iter("footnoteId"):
        fid = fn.get("id")
        if fid and "10b5-1" in footnotes.get(fid, "").lower():
            return True
    return False


def _parse_form4_xml(xml_bytes: bytes) -> Optional[Dict[str, Any]]:
    """
    Parse a single Form 4 ownership document into a structured dict:
        {
            "ticker", "owner_name", "is_officer", "is_director", "officer_title",
            "purchases": [{"date", "shares", "price", "is_planned"}],
        }
    Returns None if the document is unparseable. Only non-derivative open-market
    PURCHASES (code "P", acquired) are collected.
    """
    try:
        root = ET.fromstring(xml_bytes)
    except ET.ParseError:
        return None

    # Footnote text keyed by id, for 10b5-1 detection.
    footnotes = {
        fn.get("id", ""): (fn.text or "")
        for fn in root.findall("footnotes/footnote")
    }

    rel = root.find("reportingOwner/reportingOwnerRelationship")
    def _truthy(node, tag) -> bool:
        v = node.findtext(tag) if node is not None else None
        return bool(v) and v.strip().lower() in ("1", "true")

    info = {
        "ticker": (root.findtext("issuer/issuerTradingSymbol") or "").upper().strip(),
        "owner_name": root.findtext("reportingOwner/reportingOwnerId/rptOwnerName") or "",
        "is_officer": _truthy(rel, "isOfficer"),
        "is_director": _truthy(rel, "isDirector"),
        "officer_title": (rel.findtext("officerTitle") if rel is not None else "") or "",
        "purchases": [],
    }

    for txn in root.findall("nonDerivativeTable/nonDerivativeTransaction"):
        code = (txn.findtext("transactionCoding/transactionCode") or "").strip().upper()
        ad = (txn.findtext("transactionAmounts/transactionAcquiredDisposedCode/value") or "").strip().upper()
        if code != "P" or ad != "A":
            continue  # only open-market acquisitions are the signal
        try:
            shares = float(txn.findtext("transactionAmounts/transactionShares/value") or 0)
            price = float(txn.findtext("transactionAmounts/transactionPricePerShare/value") or 0)
        except ValueError:
            shares = price = 0.0
        info["purchases"].append({
            "date": txn.findtext("transactionDate/value") or "",
            "shares": shares,
            "price": price,
            "is_planned": _txn_is_10b5_1(txn, footnotes),
        })

    return info


def fetch_form4_filings(ticker: str, max_filings: int = 15) -> Optional[List[Dict[str, Any]]]:
    """
    Fetch and parse recent Form 4 filings for a ticker from SEC EDGAR.
    Returns a list of parsed filing dicts (see _parse_form4_xml), or None if
    the feed itself could not be fetched/parsed after retries — distinct from
    a successful fetch that legitimately found no qualifying filings ([]),
    so callers can flag the ticker as a data gap rather than "no signal".
    """
    feed_url = (
        "https://www.sec.gov/cgi-bin/browse-edgar"
        f"?action=getcompany&CIK={ticker}&type=4&dateb=&owner=include"
        f"&count={max_filings}&output=atom"
    )
    resp = fetch_with_retry(feed_url, source=f"SEC Form4 feed:{ticker}", headers=_SEC_HEADERS, timeout=HTTP_TIMEOUT)
    if resp is None:
        return None
    if resp.status_code != 200:
        print(f"[WARN] [SCRAPER] Form 4 feed HTTP {resp.status_code} for {ticker}.", flush=True)
        return None

    try:
        root = ET.fromstring(resp.content)
        ns = {"atom": "http://www.w3.org/2005/Atom"}
        filings: List[Dict[str, Any]] = []

        for entry in root.findall("atom:entry", ns):
            link_el = entry.find("atom:link", ns)
            if link_el is None:
                continue
            index_url = link_el.attrib.get("href", "")
            if not index_url:
                continue
            xml_url = _resolve_form4_xml_url(index_url)
            if not xml_url:
                continue
            doc = fetch_with_retry(xml_url, source=f"SEC Form4 doc:{ticker}", headers=_SEC_HEADERS, timeout=HTTP_TIMEOUT)
            if doc is None or doc.status_code != 200:
                print(f"[WARN] [SCRAPER] Could not fetch one Form 4 document for {ticker}; skipping that entry.", flush=True)
                continue
            parsed = _parse_form4_xml(doc.content)
            if parsed and parsed["purchases"]:
                # Stamp the ticker from the request if the XML omitted it.
                if not parsed["ticker"]:
                    parsed["ticker"] = ticker.upper()
                filings.append(parsed)

        print(f"[INFO] [SCRAPER] Form 4 for {ticker}: {len(filings)} filing(s) with open-market buys.", flush=True)
        return filings
    except ET.ParseError as e:
        print(f"[WARN] [SCRAPER] Form 4 feed for {ticker} failed to parse: {e}", flush=True)
        return None


# ---------------------------------------------------------------------------
# WS2 — Alternative Macro scrapers (physical-supply verification).
# ---------------------------------------------------------------------------
# These ingest PHYSICAL-reality signals — marine war-risk insurance premiums and
# AIS tanker-transit counts at maritime chokepoints — to cross-check against
# political TEXT (OFAC actions). Reliable AIS / Lloyd's-style insurance feeds are
# commercial, so each scraper supports three sources, in priority order:
#   1. A live HTTP provider endpoint (…_URL env) returning the documented JSON.
#   2. An inline manual feed (…_JSON env) — an analyst-maintained snapshot.
#   3. Graceful empty result (clearly flagged "unavailable") when neither is set.
# This keeps the pipeline buildable and testable today and drop-in ready for a
# paid feed later, without ever fabricating data.

def _load_alt_source(url_env: str, json_env: str, label: str) -> Optional[Any]:
    """Resolve an alt-data source from a provider URL or an inline JSON env var."""
    url = os.getenv(url_env)
    if url:
        resp = fetch_with_retry(url, source=f"alt-macro:{label}", timeout=HTTP_TIMEOUT)
        if resp is None:
            return None
        try:
            resp.raise_for_status()
            return resp.json()
        except (requests.RequestException, ValueError) as e:
            print(f"[WARN] [SCRAPER] {label} provider fetch failed: {e}", flush=True)
            return None
    raw = os.getenv(json_env)
    if raw:
        try:
            return _json.loads(raw)
        except ValueError as e:
            print(f"[WARN] [SCRAPER] {label} inline JSON parse failed: {e}", flush=True)
            return None
    return None


def fetch_marine_insurance_premiums() -> Dict[str, Any]:
    """
    War-risk insurance premiums (as % of hull value) per maritime region.

    Expected source shape:
        { "HORMUZ": {"war_risk_premium_pct": 0.7, "change_pct": 0.25, "as_of": "..."},
          "RED_SEA": {...}, ... }

    Returns {"available": bool, "regions": {...}, "source": "provider|manual|none"}.
    A rising premium = the insurance market pricing in higher physical risk.
    """
    data = _load_alt_source("ALT_INSURANCE_URL", "ALT_INSURANCE_JSON", "marine insurance")
    source = "provider" if os.getenv("ALT_INSURANCE_URL") else ("manual" if os.getenv("ALT_INSURANCE_JSON") else "none")
    if not isinstance(data, dict):
        return {"available": False, "regions": {}, "source": source}
    print(f"[INFO] [SCRAPER] Marine insurance premiums loaded ({source}): {len(data)} region(s).", flush=True)
    return {"available": True, "regions": data, "source": source}


def fetch_tanker_traffic() -> Dict[str, Any]:
    """
    AIS-derived tanker transit counts per chokepoint over a trailing window.

    Expected source shape:
        { "HORMUZ": {"transits_7d": 92, "baseline_transits": 130, "as_of": "..."},
          "RED_SEA": {...}, ... }

    Returns {"available": bool, "regions": {...}, "source": ...}, with a computed
    deviation_pct ((transits - baseline)/baseline) added per region. A large
    NEGATIVE deviation = traffic collapsing = physical disruption.
    """
    data = _load_alt_source("ALT_AIS_URL", "ALT_AIS_JSON", "AIS tanker traffic")
    source = "provider" if os.getenv("ALT_AIS_URL") else ("manual" if os.getenv("ALT_AIS_JSON") else "none")
    if not isinstance(data, dict):
        return {"available": False, "regions": {}, "source": source}

    for region, rec in data.items():
        try:
            transits = float(rec.get("transits_7d", 0))
            baseline = float(rec.get("baseline_transits", 0))
            rec["deviation_pct"] = round((transits - baseline) / baseline, 4) if baseline > 0 else None
        except (TypeError, ValueError, AttributeError):
            if isinstance(rec, dict):
                rec["deviation_pct"] = None
    print(f"[INFO] [SCRAPER] AIS tanker traffic loaded ({source}): {len(data)} region(s).", flush=True)
    return {"available": True, "regions": data, "source": source}


# OFAC recent-actions feed (public). Default to Treasury's recent actions RSS;
# overridable via env for testing or if Treasury changes the path.
OFAC_ACTIONS_URL = os.getenv(
    "OFAC_ACTIONS_URL",
    "https://ofac.treasury.gov/system/files/126/ofac.xml",
)

# Lexicon to classify a political action's supply impact.
_OFAC_TIGHTEN = ["sanction", "designat", "block", "freeze", "embargo", "restrict", "add to"]
_OFAC_EASE = ["license", "authoriz", "delist", "remov", "waiver", "ease", "lift", "general license"]


def fetch_ofac_actions(max_items: int = 25) -> Optional[List[Dict[str, Any]]]:
    """
    Fetch recent OFAC actions (sanctions designations, licenses, delistings) and
    classify each as supply-TIGHTENING (+1) or supply-EASING (-1) by keyword.

    Returns a list of {date, title, url, direction, keywords}, or None if the
    feed could not be fetched/parsed after retries — distinct from a
    successful fetch that legitimately found zero recent actions.
    A 'tightening' action constrains physical supply (bullish oil); an 'easing'
    action (e.g. a general license) releases supply (bearish oil).
    """
    resp = fetch_with_retry(OFAC_ACTIONS_URL, source="OFAC actions", headers={"User-Agent": SEC_USER_AGENT}, timeout=HTTP_TIMEOUT)
    if resp is None:
        return None
    if resp.status_code != 200:
        print(f"[WARN] [SCRAPER] OFAC feed HTTP {resp.status_code}.", flush=True)
        return None
    try:
        root = ET.fromstring(resp.content)
        actions: List[Dict[str, Any]] = []
        # Tolerate both RSS (<item>) and Atom (<entry>) feeds.
        items = root.iter("item")
        for item in items:
            title = (item.findtext("title") or "").strip()
            link = (item.findtext("link") or "").strip()
            date = (item.findtext("pubDate") or "").strip()
            if not title:
                continue
            low = title.lower()
            tighten = any(k in low for k in _OFAC_TIGHTEN)
            ease = any(k in low for k in _OFAC_EASE)
            direction = 0
            if tighten and not ease:
                direction = 1
            elif ease and not tighten:
                direction = -1
            actions.append({
                "date": date, "title": title, "url": link,
                "direction": direction,
                "keywords": [k for k in (_OFAC_TIGHTEN + _OFAC_EASE) if k in low],
            })
            if len(actions) >= max_items:
                break
        print(f"[INFO] [SCRAPER] OFAC actions fetched: {len(actions)}.", flush=True)
        return actions
    except ET.ParseError as e:
        print(f"[WARN] [SCRAPER] OFAC actions feed failed to parse: {e}", flush=True)
        return None
