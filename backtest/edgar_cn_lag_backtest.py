#!/usr/bin/env python3
"""
EDGAR → Chinese Retail Media Information Lag Backtest

Event study: for each 6-K filing by a Chinese ADR, measure the abnormal
return (vs MCHI) over 1, 2, and 5 trading sessions. Tests whether material
SEC filings produce a statistically significant return before Chinese retail
media covers them — the core premise of WS7 (Information Lag signal).

Usage:
    python edgar_cn_lag_backtest.py
    python edgar_cn_lag_backtest.py --start 2022-01-01 --end 2025-12-31
    python edgar_cn_lag_backtest.py --tickers BABA JD --start 2023-01-01
    python edgar_cn_lag_backtest.py --material-only

Outputs:
    data/lag_backtest_results.csv   — one row per 6-K filing
    data/lag_backtest_summary.json  — aggregated stats + t-stats per window
"""

import argparse
import csv
import json
import math
import os
import re
import time
from datetime import date, datetime, timedelta
from typing import Any, Dict, List, Optional, Tuple

import requests
import yfinance as yf
from scipy import stats

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
EDGAR_HEADERS = {"User-Agent": "Nox/1.0 fairydestroyasaur856@gmail.com"}
BENCHMARK_TICKER = "MCHI"
DEFAULT_START = "2022-01-01"
DEFAULT_END = "2025-12-31"
MATERIALITY_THRESHOLD = 0.25  # magnitude >= this → HIGH materiality

# CIKs zero-padded to 10 digits for EDGAR submissions API
CN_ADR_CIKS: Dict[str, str] = {
    "BABA": "0001577552",
    "JD":   "0001549802",
    "PDD":  "0001629119",
    "BIDU": "0001116132",
    "NIO":  "0001724517",
}

# HK dual-listed equivalents for cross-reference (informational only)
HK_DUALS: Dict[str, str] = {
    "BABA": "9988.HK",
    "JD":   "9618.HK",
    "BIDU": "9888.HK",
    "NIO":  "9866.HK",
}

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "..", "data")
RESULTS_CSV = os.path.join(OUTPUT_DIR, "lag_backtest_results.csv")
SUMMARY_JSON = os.path.join(OUTPUT_DIR, "lag_backtest_summary.json")

# ---------------------------------------------------------------------------
# Sentiment scoring (ported from america_data_engine/scrapers.py)
# ---------------------------------------------------------------------------
_BULLISH: Dict[str, float] = {
    "beat": 1.0, "beats": 1.0, "surge": 1.0, "soar": 1.0, "rally": 0.9,
    "jump": 0.8, "gain": 0.6, "rise": 0.5, "upgrade": 0.9, "record": 0.7,
    "strong": 0.6, "growth": 0.5, "profit": 0.5, "approval": 0.7,
    "expands": 0.5, "boost": 0.6, "tops": 0.8, "rebound": 0.7,
    "recovery": 0.6, "breakthrough": 0.8,
}
_BEARISH: Dict[str, float] = {
    "miss": -1.0, "misses": -1.0, "plunge": -1.0, "crash": -1.0,
    "slump": -0.9, "fall": -0.6, "drop": -0.7, "decline": -0.6,
    "downgrade": -0.9, "warn": -0.8, "weak": -0.6, "loss": -0.6,
    "bearish": -1.0, "probe": -0.6, "lawsuit": -0.6, "recall": -0.7,
    "cut": -0.5, "slowdown": -0.7, "default": -0.9, "bankruptcy": -1.0,
    "sanctions": -0.6, "selloff": -0.9,
}
_NEGATORS = {"not", "no", "without", "fails", "fail", "denies", "denied"}
_EARNINGS_KW = {"earnings", "revenue", "guidance", "eps", "quarter",
                "profit", "forecast", "outlook", "results"}
_MACRO_KW    = {"fed", "fomc", "inflation", "rate", "gdp", "recession", "pmi"}
_TOKEN_RE    = re.compile(r"[a-z']+")


def _score_text(text: str) -> Tuple[float, float, str]:
    """Returns (score ∈ [-1,1], magnitude ∈ [0,1], category) for filing text."""
    lower = text.lower()
    tokens = _TOKEN_RE.findall(lower)
    raw = 0.0
    hits = 0
    for i, tok in enumerate(tokens):
        w = _BULLISH.get(tok) or _BEARISH.get(tok)
        if w is None:
            continue
        if i > 0 and tokens[i - 1] in _NEGATORS:
            w = -w
        raw += w
        hits += 1

    score = math.tanh(raw)
    magnitude = min(1.0, abs(raw) / 3.0)

    if any(kw in lower for kw in _EARNINGS_KW):
        category = "EARNINGS"
    elif any(kw in lower for kw in _MACRO_KW):
        category = "MACRO_ECONOMIC"
    else:
        category = "GENERIC"

    return round(score, 4), round(magnitude, 4), category


# ---------------------------------------------------------------------------
# EDGAR data fetching
# ---------------------------------------------------------------------------
def _get_submissions_page(url: str) -> Optional[Dict]:
    """Fetch one EDGAR submissions JSON page with rate-limit awareness."""
    time.sleep(0.12)  # ~8 req/s — well within EDGAR's 10 req/s limit
    try:
        r = requests.get(url, headers=EDGAR_HEADERS, timeout=(5, 30))
        r.raise_for_status()
        return r.json()
    except Exception as e:
        print(f"[WARN] EDGAR fetch failed ({url}): {e}")
        return None


def get_6k_filings(ticker: str, start_date: str, end_date: str) -> List[Dict]:
    """
    Fetch all 6-K filing metadata for a ticker from the EDGAR submissions API.
    Returns list of {ticker, date, accession, primary_doc}.
    """
    cik = CN_ADR_CIKS[ticker]
    url = f"https://data.sec.gov/submissions/CIK{cik}.json"
    data = _get_submissions_page(url)
    if not data:
        return []

    start = date.fromisoformat(start_date)
    end   = date.fromisoformat(end_date)
    results: List[Dict] = []

    def _extract_filings(bucket: Dict) -> None:
        forms    = bucket.get("form", [])
        dates    = bucket.get("filingDate", [])
        accs     = bucket.get("accessionNumber", [])
        pri_docs = bucket.get("primaryDocument", [])
        for form, fd_str, acc, pri in zip(forms, dates, accs, pri_docs):
            if form != "6-K":
                continue
            try:
                fd = date.fromisoformat(fd_str)
            except ValueError:
                continue
            if fd < start or fd > end:
                continue
            results.append({
                "ticker":      ticker,
                "date":        fd_str,
                "accession":   acc,
                "primary_doc": pri,
            })

    _extract_filings(data.get("filings", {}).get("recent", {}))

    # Paginate through older filing pages if any
    for file_info in data.get("filings", {}).get("files", []):
        page_url = f"https://data.sec.gov/submissions/{file_info['name']}"
        page = _get_submissions_page(page_url)
        if page:
            _extract_filings(page)

    print(f"[INFO] {ticker}: found {len(results)} 6-K filings in range")
    return results


def fetch_filing_snippet(ticker: str, accession: str, primary_doc: str,
                         max_chars: int = 4000) -> str:
    """
    Fetch a text snippet from the primary 6-K document for materiality scoring.
    Returns empty string on failure.
    """
    cik_raw = CN_ADR_CIKS[ticker].lstrip("0")
    acc_flat = accession.replace("-", "")
    doc_url = (
        f"https://www.sec.gov/Archives/edgar/data/{cik_raw}/{acc_flat}/{primary_doc}"
    )
    time.sleep(0.12)
    try:
        r = requests.get(doc_url, headers=EDGAR_HEADERS, timeout=(5, 20))
        if r.status_code != 200:
            return ""
        # Strip HTML if present
        if "<html" in r.text[:200].lower() or "<HTML" in r.text[:200]:
            from bs4 import BeautifulSoup
            soup = BeautifulSoup(r.text, "html.parser")
            for el in soup(["script", "style", "table"]):
                el.extract()
            text = soup.get_text(separator=" ")
        else:
            text = r.text
        return " ".join(text.split())[:max_chars]
    except Exception as e:
        print(f"[WARN] Could not fetch filing doc for {ticker} {accession}: {e}")
        return ""


# ---------------------------------------------------------------------------
# Market data
# ---------------------------------------------------------------------------
def download_price_history(tickers: List[str], start: str, end: str) -> Dict[str, Any]:
    """
    Download daily OHLCV for all tickers + benchmark via yfinance.
    Returns {ticker: DataFrame with DatetimeIndex}.
    """
    all_tickers = list(set(tickers + [BENCHMARK_TICKER]))
    print(f"[INFO] Downloading price history for {all_tickers} ({start} → {end})...")

    # Extend end by 10 trading days to cover post-filing windows
    end_extended = (datetime.fromisoformat(end) + timedelta(days=14)).strftime("%Y-%m-%d")

    price_data: Dict[str, Any] = {}
    for t in all_tickers:
        try:
            df = yf.download(t, start=start, end=end_extended, progress=False, auto_adjust=True)
            if df.empty:
                print(f"[WARN] No price data for {t}")
                continue
            # yfinance may return MultiIndex columns when downloading one ticker
            if hasattr(df.columns, "levels"):
                df.columns = df.columns.droplevel(1)
            price_data[t] = df
        except Exception as e:
            print(f"[WARN] yfinance failed for {t}: {e}")

    return price_data


def _get_close_prices(df: Any, reference_date: str) -> List[Optional[float]]:
    """
    Return close prices at T+0, T+1, T+2, T+5 trading sessions
    starting from the first session on or after reference_date.
    """
    ref = date.fromisoformat(reference_date)
    trading_days = sorted([
        d for d in df.index
        if hasattr(d, "date") and d.date() >= ref
    ])
    closes: List[Optional[float]] = []
    for offset in [0, 1, 2, 5]:
        if offset < len(trading_days):
            try:
                val = float(df.loc[trading_days[offset], "Close"])
                closes.append(val)
            except Exception:
                closes.append(None)
        else:
            closes.append(None)
    return closes  # [T0, T1, T2, T5]


def compute_abnormal_returns(
    ticker: str,
    filing_date: str,
    price_data: Dict[str, Any],
) -> Dict[str, Optional[float]]:
    """
    Compute abnormal returns at T+1, T+2, T+5 sessions vs MCHI benchmark.
    AR = ticker_return - benchmark_return over the same window.
    All returns measured close-to-close (T+0 close → T+N close).
    """
    ticker_df = price_data.get(ticker)
    bench_df  = price_data.get(BENCHMARK_TICKER)
    result: Dict[str, Optional[float]] = {"AR_1d": None, "AR_2d": None, "AR_5d": None}

    if ticker_df is None or bench_df is None:
        return result

    t_closes = _get_close_prices(ticker_df, filing_date)
    b_closes = _get_close_prices(bench_df,  filing_date)

    for label, idx in [("AR_1d", 1), ("AR_2d", 2), ("AR_5d", 5)]:
        t0, tn = t_closes[0], t_closes[idx] if idx < len(t_closes) else None
        b0, bn = b_closes[0], b_closes[idx] if idx < len(b_closes) else None
        if None in (t0, tn, b0, bn) or t0 == 0 or b0 == 0:
            continue
        t_ret = (tn - t0) / t0
        b_ret = (bn - b0) / b0
        result[label] = round(t_ret - b_ret, 6)

    return result


# ---------------------------------------------------------------------------
# Statistical analysis
# ---------------------------------------------------------------------------
def summarise(ars: List[Optional[float]], label: str) -> Dict[str, Any]:
    """T-test of abnormal returns against zero + descriptive stats."""
    vals = [x for x in ars if x is not None]
    n = len(vals)
    if n < 5:
        return {"window": label, "n": n, "mean": None, "median": None,
                "t_stat": None, "p_value": None, "significant": False}

    t_stat, p_value = stats.ttest_1samp(vals, popmean=0.0)
    return {
        "window":      label,
        "n":           n,
        "mean":        round(sum(vals) / n, 6),
        "median":      round(sorted(vals)[n // 2], 6),
        "std":         round(float(stats.tstd(vals)), 6),
        "t_stat":      round(float(t_stat), 4),
        "p_value":     round(float(p_value), 4),
        "significant": abs(float(t_stat)) >= 2.0 and float(p_value) < 0.05,
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tickers", nargs="+", default=list(CN_ADR_CIKS.keys()),
                   help="CN ADR tickers to include (default: all)")
    p.add_argument("--start", default=DEFAULT_START, help="Start date YYYY-MM-DD")
    p.add_argument("--end",   default=DEFAULT_END,   help="End date YYYY-MM-DD")
    p.add_argument("--material-only", action="store_true",
                   help="Only include HIGH materiality filings in summary stats")
    p.add_argument("--skip-text", action="store_true",
                   help="Skip filing text fetch (faster; all filings marked ROUTINE)")
    return p.parse_args()


def main() -> None:
    args = parse_args()

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    tickers = [t for t in args.tickers if t in CN_ADR_CIKS]
    if not tickers:
        print(f"[ERROR] No valid tickers in {args.tickers}. Valid: {list(CN_ADR_CIKS)}")
        return

    # --- 1. Collect all 6-K filings from EDGAR ---
    all_filings: List[Dict] = []
    for ticker in tickers:
        filings = get_6k_filings(ticker, args.start, args.end)
        all_filings.extend(filings)

    if not all_filings:
        print("[ERROR] No 6-K filings found. Check date range and network access.")
        return

    print(f"[INFO] Total 6-K filings collected: {len(all_filings)}")

    # --- 2. Score materiality ---
    if not args.skip_text:
        print("[INFO] Scoring filing materiality (this may take a few minutes)...")
    for filing in all_filings:
        if args.skip_text:
            filing["materiality"] = "UNKNOWN"
            filing["sentiment_score"] = 0.0
            filing["sentiment_magnitude"] = 0.0
            filing["category"] = "GENERIC"
        else:
            snippet = fetch_filing_snippet(
                filing["ticker"], filing["accession"], filing["primary_doc"]
            )
            score, magnitude, category = _score_text(snippet)
            filing["sentiment_score"]     = score
            filing["sentiment_magnitude"] = magnitude
            filing["category"]            = category
            filing["materiality"] = "HIGH" if magnitude >= MATERIALITY_THRESHOLD else "ROUTINE"

    # --- 3. Download price history ---
    price_data = download_price_history(tickers, args.start, args.end)
    if BENCHMARK_TICKER not in price_data:
        print(f"[ERROR] Could not download benchmark ({BENCHMARK_TICKER}). Aborting.")
        return

    # --- 4. Compute abnormal returns for each filing ---
    print("[INFO] Computing abnormal returns...")
    rows: List[Dict] = []
    for filing in all_filings:
        ar = compute_abnormal_returns(filing["ticker"], filing["date"], price_data)
        row = {
            "ticker":               filing["ticker"],
            "date":                 filing["date"],
            "accession":            filing["accession"],
            "materiality":          filing.get("materiality", "UNKNOWN"),
            "category":             filing.get("category", "GENERIC"),
            "sentiment_score":      filing.get("sentiment_score", 0.0),
            "sentiment_magnitude":  filing.get("sentiment_magnitude", 0.0),
            "AR_1d":                ar["AR_1d"],
            "AR_2d":                ar["AR_2d"],
            "AR_5d":                ar["AR_5d"],
        }
        rows.append(row)

    # --- 5. Write CSV ---
    csv_fields = ["ticker", "date", "accession", "materiality", "category",
                  "sentiment_score", "sentiment_magnitude", "AR_1d", "AR_2d", "AR_5d"]
    with open(RESULTS_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=csv_fields)
        w.writeheader()
        w.writerows(rows)
    print(f"[INFO] Results written to {RESULTS_CSV}")

    # --- 6. Aggregate + t-test ---
    def _filter(rows: List[Dict], material_only: bool) -> List[Dict]:
        if material_only:
            return [r for r in rows if r["materiality"] == "HIGH"]
        return rows

    filtered = _filter(rows, args.material_only)
    label_suffix = " [HIGH only]" if args.material_only else " [all filings]"

    summary = {
        "meta": {
            "tickers":       tickers,
            "start":         args.start,
            "end":           args.end,
            "total_filings": len(rows),
            "filtered_n":    len(filtered),
            "material_only": args.material_only,
            "benchmark":     BENCHMARK_TICKER,
        },
        "windows": [
            summarise([r["AR_1d"] for r in filtered], f"1-session AR{label_suffix}"),
            summarise([r["AR_2d"] for r in filtered], f"2-session AR{label_suffix}"),
            summarise([r["AR_5d"] for r in filtered], f"5-session AR{label_suffix}"),
        ],
        "by_ticker": {},
        "by_category": {},
    }

    # Per-ticker breakdowns
    for t in tickers:
        t_rows = [r for r in filtered if r["ticker"] == t]
        summary["by_ticker"][t] = {
            "n": len(t_rows),
            "AR_1d": summarise([r["AR_1d"] for r in t_rows], "1d")["mean"],
            "AR_2d": summarise([r["AR_2d"] for r in t_rows], "2d")["mean"],
            "AR_5d": summarise([r["AR_5d"] for r in t_rows], "5d")["mean"],
        }

    # Per-category breakdowns
    for cat in ["EARNINGS", "MACRO_ECONOMIC", "GENERIC", "UNKNOWN"]:
        c_rows = [r for r in filtered if r["category"] == cat]
        if c_rows:
            summary["by_category"][cat] = {
                "n":    len(c_rows),
                "AR_1d": summarise([r["AR_1d"] for r in c_rows], "1d")["mean"],
                "AR_2d": summarise([r["AR_2d"] for r in c_rows], "2d")["mean"],
                "AR_5d": summarise([r["AR_5d"] for r in c_rows], "5d")["mean"],
            }

    with open(SUMMARY_JSON, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"[INFO] Summary written to {SUMMARY_JSON}")

    # --- 7. Console report ---
    print("\n" + "=" * 60)
    print("EDGAR → CN RETAIL MEDIA LAG — BACKTEST RESULTS")
    print("=" * 60)
    print(f"Tickers : {', '.join(tickers)}")
    print(f"Period  : {args.start} → {args.end}")
    print(f"Filings : {len(rows)} total | {len(filtered)} in analysis set")
    print(f"Filter  : {'HIGH materiality only' if args.material_only else 'all filings'}")
    print()
    for w in summary["windows"]:
        sig = " *** SIGNIFICANT ***" if w["significant"] else ""
        mean_pct = f"{w['mean'] * 100:.2f}%" if w["mean"] is not None else "N/A"
        t = w["t_stat"] if w["t_stat"] is not None else "N/A"
        p = w["p_value"] if w["p_value"] is not None else "N/A"
        print(f"  {w['window']:<30} mean AR={mean_pct:>8}  t={t}  p={p}{sig}")
    print()
    any_sig = any(w["significant"] for w in summary["windows"])
    if any_sig:
        print("VERDICT: Edge supported by data — at least one window shows")
        print("         statistically significant abnormal returns (|t|>=2, p<0.05).")
        print("         Proceed to WS7 live signal implementation.")
    else:
        print("VERDICT: Edge NOT supported in this sample. Review --material-only")
        print("         and date range before deciding on live implementation.")
    print("=" * 60)


if __name__ == "__main__":
    main()
