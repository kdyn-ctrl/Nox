"""
WS3 — Insider Cluster Filter.

Consumes parsed SEC Form 4 filings and emits a signal only when conviction
clusters: ≥2 distinct officers/directors making OPEN-MARKET PURCHASES of the
same issuer within a rolling 48-hour window. Rule 10b5-1 pre-planned trades are
discarded upstream (scrapers._txn_is_10b5_1) — only discretionary buys count.

A lone insider buy is noise; a cluster is a coordinated vote of confidence.
"""

import os
from datetime import datetime, timezone, timedelta
from typing import Any, Dict, List

from scrapers import fetch_form4_filings

WINDOW_HOURS = int(os.getenv("INSIDER_CLUSTER_WINDOW_HOURS", "48"))
MIN_EXECS = int(os.getenv("INSIDER_MIN_EXECS", "2"))
BYPASS = os.getenv("INSIDER_CLUSTER_BYPASS", "false").lower() in ("true", "1", "yes")


def _parse_date(date_str: str):
    """Parse a Form 4 transactionDate (YYYY-MM-DD) to a UTC datetime, or None."""
    try:
        return datetime.strptime(date_str.strip()[:10], "%Y-%m-%d").replace(tzinfo=timezone.utc)
    except (ValueError, AttributeError):
        return None


def _collect_buys(filings: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """
    Flatten filings into individual qualifying buy events:
    open-market purchases by an officer or director, excluding 10b5-1 plans.
    """
    buys = []
    for f in filings:
        # Only insiders with real influence (officer or director) carry signal.
        if not (f.get("is_officer") or f.get("is_director")):
            continue
        for p in f.get("purchases", []):
            if p.get("is_planned"):
                continue  # discard Rule 10b5-1 pre-planned trades
            dt = _parse_date(p.get("date", ""))
            if dt is None or p.get("shares", 0) <= 0:
                continue
            buys.append({
                "owner": f.get("owner_name", "").strip(),
                "title": f.get("officer_title", "") or ("Director" if f.get("is_director") else ""),
                "date": dt,
                "shares": p["shares"],
                "price": p.get("price", 0.0),
                "value": p["shares"] * p.get("price", 0.0),
            })
    return buys


def _find_cluster(buys: List[Dict[str, Any]]) -> Dict[str, Any]:
    """
    Slide a WINDOW_HOURS window over buys (sorted by date) and find the window
    containing the most DISTINCT insiders. Returns the best cluster or {}.
    """
    if len(buys) < MIN_EXECS:
        return {}
    buys = sorted(buys, key=lambda b: b["date"])
    window = timedelta(hours=WINDOW_HOURS)
    best: Dict[str, Any] = {}

    for i, anchor in enumerate(buys):
        members = [b for b in buys[i:] if b["date"] - anchor["date"] <= window]
        distinct = {b["owner"] for b in members if b["owner"]}
        if len(distinct) >= MIN_EXECS and len(distinct) > len(best.get("insiders", [])):
            best = {
                "insiders": sorted(distinct),
                "buy_count": len(members),
                "total_value": round(sum(b["value"] for b in members), 2),
                "total_shares": int(sum(b["shares"] for b in members)),
                "window_start": anchor["date"].isoformat(),
                "window_end": max(b["date"] for b in members).isoformat(),
                "transactions": [
                    {
                        "owner": b["owner"], "title": b["title"],
                        "date": b["date"].date().isoformat(),
                        "shares": int(b["shares"]), "price": round(b["price"], 2),
                    }
                    for b in members
                ],
            }
    return best


def detect_insider_clusters(watchlist: List[str]) -> Dict[str, Any]:
    """
    Top-level entry point. Scans the watchlist for insider buy clusters and
    returns the signal JSON written to the shared data bus.

    `data_gaps` lists tickers whose SEC Form 4 feed could not be fetched
    after retries — these are excluded from the scan, so their absence from
    `signals` must NOT be read as "no insider activity".
    """
    signals = []
    data_gaps = []
    for ticker in watchlist:
        filings = fetch_form4_filings(ticker)
        if filings is None:
            data_gaps.append(ticker)
            continue
        if not filings:
            continue
        cluster = _find_cluster(_collect_buys(filings))
        if cluster:
            signals.append({
                "ticker": ticker.upper(),
                "action": "INSIDER_CLUSTER_BUY",
                "insider_count": len(cluster["insiders"]),
                **cluster,
            })

    payload = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "window_hours": WINDOW_HOURS,
        "min_execs": MIN_EXECS,
        "bypass": BYPASS,
        "signal_count": len(signals),
        "signals": signals,
        "data_gaps": data_gaps,
        "complete": not data_gaps,
    }
    print(
        f"[INFO] [INSIDER-CLUSTER] Scanned {len(watchlist)} ticker(s); "
        f"{len(signals)} cluster signal(s).",
        flush=True,
    )
    if data_gaps:
        print(
            f"[WARN] [INSIDER-CLUSTER] SEC Form 4 fetch failed for: {', '.join(data_gaps)} "
            f"— results are INCOMPLETE for these tickers.",
            flush=True,
        )
    return payload
