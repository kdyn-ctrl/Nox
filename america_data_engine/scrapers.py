import os
import re
import sys
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


def fetch_alpaca_news() -> Optional[List[Dict[str, Any]]]:
    """
    Fetches the latest financial news from Alpaca's API.

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


def fetch_earnings_calendar(tickers: List[str]) -> Dict[str, Optional[List[Dict[str, Any]]]]:
    """
    Queries Alpaca's corporate actions endpoint for scheduled earnings announcements.
    Returns a dict mapping each ticker to a list of earnings dates within the next 30 days.

    Format: {
        "AAPL": [{"date": "2026-07-15", "eps": 1.23, "eps_estimate": 1.20}, ...],
        "TSLA": [{"date": "2026-07-20"}, ...],
        ...
    }
    A ticker maps to None (not []) if its fetch failed after retries, so a
    real "no earnings scheduled" is never confused with "the API call failed".

    RULE-008: Enforces (5, 10) timeout tuple on all HTTP calls.
    """
    url = "https://api.alpaca.markets/v2/corporate-actions"
    headers = {
        "APCA-API-KEY-ID": ALPACA_KEY,
        "APCA-API-SECRET-KEY": ALPACA_SECRET,
    }

    result: Dict[str, Optional[List[Dict[str, Any]]]] = {}

    # Compute the 30-day forward window from today (UTC)
    now_utc = datetime.now(tz=timezone.utc)
    start_date = now_utc.date().isoformat()
    end_date = (now_utc + timedelta(days=30)).date().isoformat()

    for ticker in tickers:
        params = {
            "symbols": ticker,
            "types": "earnings",
            "start": start_date,
            "end": end_date,
        }
        response = fetch_with_retry(
            url, source=f"Alpaca earnings:{ticker}", headers=headers, params=params, timeout=HTTP_TIMEOUT
        )
        if response is None:
            result[ticker] = None
            continue
        try:
            response.raise_for_status()
            data = response.json()
            events = data.get("corporate_actions", [])

            earnings_list = []
            for event in events:
                # Each event has: id, symbol, date, type, description, etc.
                if event.get("type") == "earnings":
                    earnings_list.append({
                        "date": event.get("date"),
                        "description": event.get("description"),
                    })

            result[ticker] = earnings_list
            if earnings_list:
                print(f"[INFO] [SCRAPER] Earnings found for {ticker}: {len(earnings_list)} event(s).",
                      flush=True)

        except requests.RequestException as e:
            print(f"[WARN] [SCRAPER] Could not fetch earnings for {ticker}: {e}", flush=True)
            result[ticker] = None

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
