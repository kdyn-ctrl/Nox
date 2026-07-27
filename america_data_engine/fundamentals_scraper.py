"""
SEC EDGAR XBRL company-facts scraper — fundamentals data for the Beneish
M-Score + FCF burn-runway screens (see fundamentals_calc.py).

Structurally different SEC surface from scrapers.py's Form 4 scraper: XBRL
company-facts is a JSON API (data.sec.gov) rather than the HTML EDGAR browse
endpoint (www.sec.gov) scrapers.py already hits, so this lives in its own
file rather than growing that one further. Both hosts require the same
descriptive User-Agent per SEC's fair-use policy — reused here, not
duplicated.

Fail-open at every layer, per RULE-008: an unresolved ticker, an unreachable
host, a malformed body, or a missing XBRL tag each degrade to None/empty
rather than raising — a dead SEC feed must never crash the fundamentals scan.
"""
import os
from datetime import date, datetime, timedelta, timezone
from typing import Any, Dict, List, Optional

from retry_utils import fetch_with_retry
from scrapers import SEC_USER_AGENT, HTTP_TIMEOUT

SEC_TICKERS_URL = "https://www.sec.gov/files/company_tickers.json"
SEC_COMPANYFACTS_URL = "https://data.sec.gov/api/xbrl/companyfacts/CIK{cik}.json"

FUNDAMENTALS_CIK_CACHE_TTL_HOURS = float(os.getenv("FUNDAMENTALS_CIK_CACHE_TTL_HOURS", "168"))
# Separate, much shorter cooldown for a FAILED fetch attempt (e.g. SEC 403s
# the ticker-map request). Without this, a single failure never "warms" the
# cache — the freshness check below requires a non-empty map — so every
# subsequent resolve_cik() call for the rest of the scan (one per ticker,
# thousands per run) re-attempts the same request against SEC's already-
# blocking endpoint. Confirmed in production 2026-07-16: a first-request 403
# turned into ~10,168 repeated requests to the same endpoint in one scan.
FUNDAMENTALS_CIK_RETRY_COOLDOWN_HOURS = float(os.getenv("FUNDAMENTALS_CIK_RETRY_COOLDOWN_HOURS", "1"))

# Each Beneish/FCF input maps to a list of candidate us-gaap XBRL tags, tried
# in order — companies tag the same concept inconsistently year to year.
BENEISH_TAGS: Dict[str, List[str]] = {
    "receivables":         ["AccountsReceivableNetCurrent", "ReceivablesNetCurrent"],
    "revenue":             ["Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax"],
    "cogs":                ["CostOfGoodsAndServicesSold", "CostOfRevenue", "CostOfGoodsSold"],
    "current_assets":      ["AssetsCurrent"],
    "ppe_net":             ["PropertyPlantAndEquipmentNet"],
    "securities":          ["ShortTermInvestments"],  # optional; treated as 0 if absent
    "total_assets":        ["Assets"],
    "depreciation":        ["DepreciationDepletionAndAmortization", "Depreciation"],
    "sga":                 ["SellingGeneralAndAdministrativeExpense"],  # see _SGA_COMPOSITE_TAGS fallback
    "total_liabilities":   ["Liabilities"],
    "current_liabilities": ["LiabilitiesCurrent"],
    "ltd":                 ["LongTermDebtNoncurrent"],
    "net_income":          ["NetIncomeLoss"],
    "cfo":                 ["NetCashProvidedByUsedInOperatingActivities"],
}
FCF_TAGS: Dict[str, List[str]] = {
    "cash":  ["CashAndCashEquivalentsAtCarryingValue",
              "CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents"],
    "cfo_q": ["NetCashProvidedByUsedInOperatingActivities"],
    "capex": ["PaymentsToAcquirePropertyPlantAndEquipment"],
}
# Piotroski F-Score's one input not already covered by BENEISH_TAGS (the rest
# of its 9 criteria reuse net_income/total_assets/cfo/ltd/current_assets/
# current_liabilities/revenue/cogs, all already extracted above) — dilution
# check needs shares outstanding, which XBRL tags in "shares" units rather
# than "USD" like every other field here.
PIOTROSKI_EXTRA_TAGS: Dict[str, List[str]] = {
    "shares_outstanding": ["CommonStockSharesOutstanding", "CommonStockSharesIssued"],
}
_ANNUAL_UNITS: Dict[str, str] = {"shares_outstanding": "shares"}  # default "USD" otherwise
_ANNUAL_TAGS = set(BENEISH_TAGS.keys()) | set(PIOTROSKI_EXTRA_TAGS.keys())

# Some filers (confirmed live: Microsoft) never report the combined SG&A tag
# at all — they report General & Administrative and Selling & Marketing as
# two separate line items instead. A single alternate-tag fallback (like
# every other BENEISH_TAGS field uses) doesn't apply here since neither tag
# alone IS SG&A — this needs a sum of both, not a substitute for one.
_SGA_COMPOSITE_TAGS: List[str] = ["GeneralAndAdministrativeExpense", "SellingAndMarketingExpense"]
_QUARTERLY_TAGS = set(FCF_TAGS.keys())

_ticker_to_cik: Dict[str, str] = {}
_ticker_to_cik_fetched_at: Optional[datetime] = None
_ticker_to_cik_last_attempt_at: Optional[datetime] = None


def _sec_headers() -> Dict[str, str]:
    return {"User-Agent": SEC_USER_AGENT}


def _load_ticker_cik_map(force: bool = False) -> None:
    global _ticker_to_cik, _ticker_to_cik_fetched_at, _ticker_to_cik_last_attempt_at
    now = datetime.now(timezone.utc)
    if not force:
        if (
            _ticker_to_cik
            and _ticker_to_cik_fetched_at
            and (now - _ticker_to_cik_fetched_at) < timedelta(hours=FUNDAMENTALS_CIK_CACHE_TTL_HOURS)
        ):
            return
        # No successful map yet, but don't hammer SEC's already-403ing
        # endpoint once per ticker — back off for the shorter cooldown
        # instead of the full success TTL.
        if (
            _ticker_to_cik_last_attempt_at
            and (now - _ticker_to_cik_last_attempt_at) < timedelta(hours=FUNDAMENTALS_CIK_RETRY_COOLDOWN_HOURS)
        ):
            return

    _ticker_to_cik_last_attempt_at = now
    resp = fetch_with_retry(
        SEC_TICKERS_URL,
        source="SEC ticker->CIK map",
        headers=_sec_headers(),
        timeout=HTTP_TIMEOUT,
    )
    if resp is None or resp.status_code != 200:
        return  # fail-open: keep whatever mapping (possibly empty) we already had

    try:
        body = resp.json()
    except Exception:
        return

    # SEC's real company_tickers.json is a dict keyed by stringified index
    # ({"0": {...}, "1": {...}, ...}), NOT a JSON array — confirmed live
    # 2026-07-16. Iterating a dict directly yields its string keys, and
    # str.get() doesn't exist, so this previously raised AttributeError on
    # every real successful (200) response — masked until now because every
    # prior production run either got the SEC_USER_AGENT-403 (returns above,
    # before this code ever runs) or crashed here uncaught. Either way,
    # resolve_cik() has never actually resolved a ticker against live data.
    entries = body.values() if isinstance(body, dict) else body

    mapping: Dict[str, str] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        ticker = str(entry.get("ticker", "")).upper().strip()
        cik_raw = entry.get("cik_str")
        if not ticker or cik_raw is None:
            continue
        mapping[ticker] = f"{int(cik_raw):010d}"

    if mapping:
        _ticker_to_cik = mapping
        _ticker_to_cik_fetched_at = now


def resolve_cik(ticker: str) -> Optional[str]:
    """Ticker -> 10-digit zero-padded CIK. None on not-found or fetch failure."""
    ticker = ticker.upper().strip()
    _load_ticker_cik_map()
    return _ticker_to_cik.get(ticker)


def fetch_company_facts(cik: str) -> Optional[Dict[str, Any]]:
    """GET SEC EDGAR's companyfacts JSON for this CIK. None on any failure."""
    resp = fetch_with_retry(
        SEC_COMPANYFACTS_URL.format(cik=cik),
        source=f"SEC XBRL companyfacts:{cik}",
        headers=_sec_headers(),
        timeout=HTTP_TIMEOUT,
    )
    if resp is None or resp.status_code != 200:
        return None
    try:
        return resp.json()
    except Exception:
        return None


def _units_for_tag(facts: Dict[str, Any], tag: str, unit: str = "USD") -> List[Dict[str, Any]]:
    try:
        us_gaap = facts["facts"]["us-gaap"]
    except (KeyError, TypeError):
        return []
    tag_data = us_gaap.get(tag)
    if not tag_data:
        return []
    # Nearly everything relevant here is USD-denominated; shares_outstanding
    # is the one field tagged in "shares" units instead.
    return tag_data.get("units", {}).get(unit, [])


def _annual_periods(facts: Dict[str, Any], tag: str, unit: str = "USD") -> List[Dict[str, Any]]:
    """Most-recent-first list of annual (FY, form 10-K) datapoints for `tag`."""
    entries = [
        e for e in _units_for_tag(facts, tag, unit)
        if e.get("form") == "10-K" and e.get("fp") == "FY" and e.get("end")
    ]
    entries.sort(key=lambda e: e["end"], reverse=True)
    return entries


def _quarterly_periods(facts: Dict[str, Any], tag: str) -> List[Dict[str, Any]]:
    """Most-recent-first list of 10-Q datapoints for `tag`."""
    entries = [
        e for e in _units_for_tag(facts, tag)
        if e.get("form") == "10-Q" and e.get("end")
    ]
    entries.sort(key=lambda e: e["end"], reverse=True)
    return entries


def _extract_annual(facts: Dict[str, Any], tag_candidates: List[str], period_index: int,
                     unit: str = "USD") -> Optional[float]:
    """`period_index=0` -> most recent FY, `1` -> the FY before that."""
    for tag in tag_candidates:
        periods = _annual_periods(facts, tag, unit)
        if len(periods) > period_index:
            try:
                return float(periods[period_index]["val"])
            except (KeyError, TypeError, ValueError):
                continue
    return None


def _extract_annual_sga_composite(facts: Dict[str, Any], period_index: int) -> Optional[float]:
    """Sums _SGA_COMPOSITE_TAGS for one annual period. None if either half
    is missing — a partial sum (just G&A, or just Selling/Marketing) isn't
    the real SG&A figure and would understate it silently."""
    parts = [_extract_annual(facts, [tag], period_index) for tag in _SGA_COMPOSITE_TAGS]
    if any(p is None for p in parts):
        return None
    return sum(parts)


# A single fiscal quarter is ~91 days; allow slack for 13-week retail calendars
# and reporting drift. Anything materially longer is a cumulative year-to-date
# figure that must be de-cumulated (Q2 10-Q reports ~182d/6mo, Q3 ~273d/9mo).
_SINGLE_QUARTER_MAX_DAYS = 100


def _period_days(entry: Dict[str, Any]) -> Optional[int]:
    """Duration in days of an XBRL *duration* datapoint (a flow concept like
    CFO/capex), or None for an instantaneous balance concept (e.g. cash), which
    carries no `start`."""
    s, e = entry.get("start"), entry.get("end")
    if not s or not e:
        return None
    try:
        sy, sm, sd = (int(x) for x in str(s).split("-"))
        ey, em, ed = (int(x) for x in str(e).split("-"))
        return (date(ey, em, ed) - date(sy, sm, sd)).days
    except (ValueError, TypeError):
        return None


def _single_quarter_flow(periods: List[Dict[str, Any]]) -> Optional[float]:
    """Isolate the most recent SINGLE-QUARTER value of a flow concept (CFO,
    capex) from 10-Q datapoints (audit Phase 4.2).

    SEC 10-Q cash-flow statements report cumulative year-to-date figures — a Q2
    10-Q shows 6 months, a Q3 10-Q shows 9 months — so taking the latest
    datapoint verbatim (the old behavior) overstates a single quarter's
    magnitude 2-3x. For a cash-burning company that understates FCF runway and
    fires false `fcf_burn_risk` alerts. Strategy:
      1. Among datapoints sharing the most-recent end date, prefer the SHORTEST
         duration — XBRL commonly carries a discrete 3-month frame alongside the
         cumulative one, and the short one is already the single quarter.
      2. If that frame is ~one quarter, use it as-is.
      3. Otherwise de-cumulate: subtract the prior YTD datapoint that shares the
         same fiscal-year `start` (Q2_6mo − Q1_3mo, Q3_9mo − Q2_6mo).
      4. If it can't be isolated cleanly, return None (→ insufficient_data)
         rather than emit a 2-3x-wrong number that trips a false alert.

    `periods` is most-recent-end-first (see _quarterly_periods)."""
    if not periods:
        return None
    latest_end = periods[0].get("end")
    same_end = [p for p in periods if p.get("end") == latest_end]

    # Prefer the shortest-duration frame at the latest end date; unknown
    # durations sort last so a real 3-month frame always wins.
    def _dur_key(p: Dict[str, Any]) -> int:
        d = _period_days(p)
        return d if d is not None else 10 ** 9

    chosen = min(same_end, key=_dur_key)
    try:
        chosen_val = float(chosen["val"])
    except (KeyError, TypeError, ValueError):
        return None

    dur = _period_days(chosen)
    if dur is not None and dur <= _SINGLE_QUARTER_MAX_DAYS:
        return chosen_val

    # Cumulative frame only: de-cumulate against the prior YTD sharing the same
    # fiscal-year start (all YTD periods begin at the FY start, so `start`
    # identifies the chain). ISO date strings compare correctly lexically.
    start = chosen.get("start")
    if start:
        priors = [p for p in periods
                  if p.get("start") == start and str(p.get("end", "")) < str(chosen.get("end", ""))]
        if priors:
            prior = max(priors, key=lambda p: str(p.get("end", "")))
            try:
                return chosen_val - float(prior["val"])
            except (KeyError, TypeError, ValueError):
                return None
    return None  # cumulative but un-isolable → insufficient, not a wrong number


def _extract_quarterly(facts: Dict[str, Any], tag_candidates: List[str]) -> Optional[float]:
    for tag in tag_candidates:
        periods = _quarterly_periods(facts, tag)
        if not periods:
            continue
        # Balance concepts (e.g. cash) are instantaneous point-in-time snapshots
        # with no `start` — the latest is correct as-is. Flow concepts (CFO,
        # capex) carry a `start` and are cumulative YTD in 10-Qs, so isolate the
        # single quarter.
        is_flow = any(p.get("start") for p in periods)
        if is_flow:
            val = _single_quarter_flow(periods)
        else:
            try:
                val = float(periods[0]["val"])
            except (KeyError, TypeError, ValueError):
                val = None
        if val is not None:
            return val
    return None


def get_fundamentals_raw(ticker: str) -> Dict[str, Any]:
    """
    Top-level entry point. Resolves ticker->CIK, pulls company-facts, and
    extracts the raw Beneish (two most recent annual periods) and FCF
    (most recent quarterly period) inputs. Fails open at every layer —
    each field degrades to None individually rather than the whole call
    raising, since fundamentals_calc.py's insufficient_data handling is
    designed around per-field gaps, not an all-or-nothing fetch.
    """
    ticker = ticker.upper().strip()
    annual_tags = {**BENEISH_TAGS, **PIOTROSKI_EXTRA_TAGS}
    result: Dict[str, Any] = {
        "ticker": ticker,
        "cik": None,
        "resolved": False,
        "current_period": {k: None for k in annual_tags},
        "prior_period": {k: None for k in annual_tags},
        "quarterly": {k: None for k in FCF_TAGS},
        "fetch_error": None,
    }

    cik = resolve_cik(ticker)
    if not cik:
        result["fetch_error"] = "cik_not_found"
        return result
    result["cik"] = cik
    result["resolved"] = True

    facts = fetch_company_facts(cik)
    if facts is None:
        result["fetch_error"] = "companyfacts_fetch_failed"
        return result

    # One companyfacts fetch serves both Beneish/FCF (bearish) and Piotroski
    # (bullish) — deliberately not two separate SEC round-trips per ticker,
    # since a doubled per-ticker SEC call count is exactly the class of bug
    # that caused the 2026-07-16 403 cascade (fundamentals_scraper.py's own
    # cooldown comment above).
    result["current_period"] = {
        field: _extract_annual(facts, tags, 0, unit=_ANNUAL_UNITS.get(field, "USD"))
        for field, tags in annual_tags.items()
    }
    result["prior_period"] = {
        field: _extract_annual(facts, tags, 1, unit=_ANNUAL_UNITS.get(field, "USD"))
        for field, tags in annual_tags.items()
    }
    if result["current_period"]["sga"] is None:
        result["current_period"]["sga"] = _extract_annual_sga_composite(facts, 0)
    if result["prior_period"]["sga"] is None:
        result["prior_period"]["sga"] = _extract_annual_sga_composite(facts, 1)
    result["quarterly"] = {
        field: _extract_quarterly(facts, tags) for field, tags in FCF_TAGS.items()
    }
    return result
