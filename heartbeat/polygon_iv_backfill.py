"""
Polygon.io 52-week historical IV backfill — CLAUDE.md Phase 2, item C.

calculate_iv_rank() in monitor.py already implements the correct 52-week
percentile formula, but it can only use what's actually in the
`historical_volatility` table — and until now the ONLY writer was
collect_eod_iv_snapshots(), one row per ticker per day. That means a fresh
deployment (or a new ticker added to the watchlist) needs 30 trading days
before iv_rank even leaves "snapshot_relative" fallback, and a full year
before it's a true 52-week rank.

This script closes that gap by reconstructing the last ~12 months of daily
IV directly from Polygon's historical options data instead of waiting for it
to accumulate organically:

  1. Pull the underlying's daily closes for the backfill window (one call).
  2. For each month in the window, pick ONE representative contract — the
     near-the-money call with ~POLYGON_IV_TARGET_DTE days to expiry as of
     that month's anchor date (Polygon's contracts-reference endpoint
     supports `as_of` on already-expired contracts, so this works
     retroactively).
  3. Pull that contract's daily OHLC for its coverage window (one call).
  4. Invert Black-Scholes on each day's (contract close, underlying close,
     strike, days-to-expiry) to recover the implied vol, and write it into
     the SAME historical_volatility table monitor.py already owns via
     store_iv_snapshot() — so calculate_iv_rank() picks it up with zero
     changes on that side.

Run manually (needs POLYGON_API_KEY):
    python3 polygon_iv_backfill.py --tickers SPY,QQQ,AAPL

Note: Polygon.io rebranded to Massive.com on 2025-10-30. `api.polygon.io` and
POLYGON_API_KEY still work unchanged (the vendor is running both domains in
parallel indefinitely), so nothing below needed to change for this to work —
but new integrations, or a future migration, should look at api.massive.com /
MASSIVE_API_KEY instead.

Chaining one representative contract per month means the reconstructed
series is an approximation (a single monthly ATM-call proxy, not a
continuously-tracked specific contract) — the same tradeoff VIX itself makes
by rolling a representative basket. That's an acceptable proxy for a rank
computed over a 252-day range; it is NOT precise enough to feed live pricing
(fetchIVData's live snapshot stays the source for iv_level, only iv_rank
reads this backfilled history).
"""

import argparse
import math
import os
import sqlite3
import sys
import time
from datetime import datetime, timedelta, timezone

from retry_utils import fetch_with_retry

POLYGON_BASE_URL = "https://api.polygon.io"

# Deliberately NOT importing from monitor.py: that module's top-level code
# hard-aborts (require_env) unless Telegram/Anthropic/Alpaca/webhook secrets
# are all set, and eagerly constructs live Telegram + Anthropic clients on
# import — side effects this pure data-backfill script has no business
# triggering. Instead this owns its own handle to the SAME memory_bank.db
# file and the SAME historical_volatility schema, mirroring the pattern the
# C++ engine already uses for order_ledger/daily_ledger (independent writers,
# one shared table) rather than cross-importing a monolithic script.
DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")

_us_raw = os.getenv("NOX_WATCHLIST_US", "AAPL,TSLA,NVDA,MSFT")
_cn_raw = os.getenv("NOX_WATCHLIST_CN", "BABA,JD,PDD,BIDU,NIO")
WATCHLIST = [t.strip() for t in (_us_raw + "," + _cn_raw).split(",") if t.strip()]


def store_iv_snapshot(ticker: str, iv: float, date_str: str) -> bool:
    """Writes one IV snapshot to historical_volatility — same table/schema
    monitor.py's collect_eod_iv_snapshots() writes, so calculate_iv_rank()
    and the C++ IvRankStore both pick up backfilled rows with no changes."""
    try:
        with sqlite3.connect(DB_PATH) as conn:
            conn.execute(
                "CREATE TABLE IF NOT EXISTS historical_volatility ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT NOT NULL, "
                "date DATE NOT NULL, implied_volatility REAL NOT NULL, "
                "snapshot_timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
                "UNIQUE(ticker, date))"
            )
            conn.execute(
                "INSERT OR REPLACE INTO historical_volatility "
                "(ticker, date, implied_volatility, snapshot_timestamp) "
                "VALUES (?, ?, ?, CURRENT_TIMESTAMP)",
                (ticker, date_str, iv),
            )
            conn.commit()
        return True
    except sqlite3.Error as e:
        print(f"[ERROR] [POLYGON_IV_BACKFILL] Failed to store {ticker}/{date_str}: {e}", file=sys.stderr)
        return False

# Tuned-but-generic financial constants — env-sourced per project convention
# (nothing tunable stays a bare literal), even though none of these are
# proprietary/curated values.
RISK_FREE_RATE       = float(os.getenv("POLYGON_IV_RISK_FREE_RATE", "0.04"))
TARGET_DTE           = int(os.getenv("POLYGON_IV_TARGET_DTE", "35"))
DTE_TOLERANCE        = int(os.getenv("POLYGON_IV_DTE_TOLERANCE", "10"))
BACKFILL_MONTHS      = int(os.getenv("POLYGON_IV_BACKFILL_MONTHS", "12"))
RATE_LIMIT_DELAY_SEC = float(os.getenv("POLYGON_RATE_LIMIT_DELAY_SECONDS", "13"))


# ── Black-Scholes pricing + implied-volatility inversion ───────────────────
# No numpy/scipy dependency (heartbeat's image doesn't carry them) — stdlib
# math.erf gives an exact normal CDF, and BS call price is monotonically
# increasing in sigma, so a plain bisection IV solver is both correct and
# simple to reason about (no derivative/Vega computation, no divergence risk
# near the money that Newton-Raphson can hit).

def _norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def black_scholes_price(S: float, K: float, T: float, r: float, sigma: float,
                        option_type: str = "call") -> float:
    """Black-Scholes European option price. T in years, sigma annualized decimal."""
    if T <= 0 or sigma <= 0 or S <= 0 or K <= 0:
        return max(0.0, (S - K) if option_type == "call" else (K - S))
    d1 = (math.log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * math.sqrt(T))
    d2 = d1 - sigma * math.sqrt(T)
    if option_type == "call":
        return S * _norm_cdf(d1) - K * math.exp(-r * T) * _norm_cdf(d2)
    return K * math.exp(-r * T) * _norm_cdf(-d2) - S * _norm_cdf(-d1)


def implied_volatility_from_price(price: float, S: float, K: float, T: float, r: float,
                                  option_type: str = "call",
                                  lo: float = 1e-4, hi: float = 5.0,
                                  tol: float = 1e-4, max_iter: int = 100) -> float | None:
    """
    Bisection solve for sigma such that black_scholes_price(...) == price.
    Returns None if the observed price falls outside what any sigma in
    [lo, hi] can produce (bad/stale data) rather than returning a garbage edge value.
    """
    if T <= 0 or price <= 0 or S <= 0 or K <= 0:
        return None
    price_lo = black_scholes_price(S, K, T, r, lo, option_type)
    price_hi = black_scholes_price(S, K, T, r, hi, option_type)
    if price < price_lo or price > price_hi:
        return None

    for _ in range(max_iter):
        mid = (lo + hi) / 2.0
        p_mid = black_scholes_price(S, K, T, r, mid, option_type)
        if abs(p_mid - price) < tol:
            return mid
        if p_mid < price:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


# ── Polygon.io client ────────────────────────────────────────────────────────

def _polygon_get(path: str, api_key: str, params: dict | None = None, source: str = "Polygon"):
    params = dict(params or {})
    params["apiKey"] = api_key
    resp = fetch_with_retry(POLYGON_BASE_URL + path, source=source, params=params)
    if resp is None or resp.status_code != 200:
        return None
    try:
        return resp.json()
    except ValueError:
        return None


def fetch_daily_closes(ticker: str, from_date: str, to_date: str, api_key: str) -> dict:
    """Returns {date_str: close_price} for `ticker` (underlying or options ticker)."""
    body = _polygon_get(
        f"/v2/aggs/ticker/{ticker}/range/1/day/{from_date}/{to_date}",
        api_key, params={"adjusted": "true", "sort": "asc", "limit": 50000},
        source=f"Polygon aggs:{ticker}",
    )
    time.sleep(RATE_LIMIT_DELAY_SEC)
    if not body or body.get("resultsCount", 0) == 0:
        return {}
    out = {}
    for bar in body.get("results", []):
        date_str = datetime.fromtimestamp(bar["t"] / 1000, tz=timezone.utc).strftime("%Y-%m-%d")
        out[date_str] = bar["c"]
    return out


def find_representative_contract(underlying: str, as_of_date: str, underlying_price: float,
                                 api_key: str) -> dict | None:
    """
    Picks the near-the-money call, ~TARGET_DTE days out as of `as_of_date`,
    from Polygon's options-contracts reference (works retroactively via
    `as_of` even for already-expired contracts). Returns
    {"ticker": options_ticker, "strike": float, "expiration_date": "YYYY-MM-DD"}
    or None if nothing in range was found.
    """
    anchor = datetime.strptime(as_of_date, "%Y-%m-%d")
    exp_gte = (anchor + timedelta(days=TARGET_DTE - DTE_TOLERANCE)).strftime("%Y-%m-%d")
    exp_lte = (anchor + timedelta(days=TARGET_DTE + DTE_TOLERANCE)).strftime("%Y-%m-%d")

    body = _polygon_get(
        "/v3/reference/options/contracts", api_key,
        params={
            "underlying_ticker": underlying,
            "contract_type": "call",
            "as_of": as_of_date,
            "expiration_date.gte": exp_gte,
            "expiration_date.lte": exp_lte,
            # NOT "expired": "true" — verified empirically (research/
            # premium_selling_test.py, 2026-07-16) that Polygon's `expired`
            # filter means "expired as of now", not "was tradable as of
            # as_of". Combined with a past `as_of` it always returns zero
            # results, so this backfill has been a silent no-op in
            # production since it shipped.
            "limit": 1000,
            "sort": "strike_price",
            "order": "asc",
        },
        source=f"Polygon contracts:{underlying}@{as_of_date}",
    )
    time.sleep(RATE_LIMIT_DELAY_SEC)
    results = (body or {}).get("results", [])
    if not results:
        return None

    best = min(results, key=lambda c: abs(c.get("strike_price", 0.0) - underlying_price))
    return {
        "ticker": best["ticker"],
        "strike": best["strike_price"],
        "expiration_date": best["expiration_date"],
    }


# ── Orchestration ────────────────────────────────────────────────────────────

def _month_anchors(months: int) -> list[str]:
    """Trailing `months` monthly anchor dates (1st of each month, oldest first)."""
    today = datetime.now(timezone.utc)
    anchors = []
    for i in range(months, 0, -1):
        # Step back i months from the first of the current month.
        year = today.year
        month = today.month - i
        while month <= 0:
            month += 12
            year -= 1
        anchors.append(datetime(year, month, 1).strftime("%Y-%m-%d"))
    return anchors


def backfill_ticker(ticker: str, api_key: str, months: int = BACKFILL_MONTHS) -> dict:
    """
    Reconstructs and stores ~`months` months of daily IV for `ticker` via
    Polygon historical options data. Returns a summary dict for logging.
    Best-effort per month: one bad month (missing contract, thin data) never
    aborts the rest of the backfill.
    """
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    year_ago = (datetime.now(timezone.utc) - timedelta(days=months * 31)).strftime("%Y-%m-%d")

    underlying_closes = fetch_daily_closes(ticker, year_ago, today, api_key)
    if not underlying_closes:
        return {"ticker": ticker, "days_written": 0, "error": "no underlying price history"}

    anchors = _month_anchors(months)
    days_written = 0
    months_failed = 0

    for idx, anchor_date in enumerate(anchors):
        anchor_price = underlying_closes.get(anchor_date)
        # The anchor itself may be a non-trading day — fall back to the
        # nearest date we do have a close for.
        if anchor_price is None:
            candidates = sorted(d for d in underlying_closes if d >= anchor_date)
            if not candidates:
                months_failed += 1
                continue
            anchor_date = candidates[0]
            anchor_price = underlying_closes[anchor_date]

        contract = find_representative_contract(ticker, anchor_date, anchor_price, api_key)
        if not contract:
            months_failed += 1
            continue

        coverage_end = anchors[idx + 1] if idx + 1 < len(anchors) else today
        coverage_end = min(coverage_end, contract["expiration_date"])
        contract_closes = fetch_daily_closes(contract["ticker"], anchor_date, coverage_end, api_key)

        expiry = datetime.strptime(contract["expiration_date"], "%Y-%m-%d")
        for date_str, contract_close in contract_closes.items():
            underlying_close = underlying_closes.get(date_str)
            if underlying_close is None or contract_close <= 0:
                continue
            T = (expiry - datetime.strptime(date_str, "%Y-%m-%d")).days / 365.0
            iv = implied_volatility_from_price(
                contract_close, underlying_close, contract["strike"], T, RISK_FREE_RATE, "call")
            if iv is None or iv <= 0 or iv > 5.0:
                continue
            if store_iv_snapshot(ticker, iv, date_str):
                days_written += 1

    return {"ticker": ticker, "days_written": days_written, "months_failed": months_failed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tickers", type=str, default=None,
                        help="Comma-separated tickers (default: the full WATCHLIST)")
    parser.add_argument("--months", type=int, default=BACKFILL_MONTHS,
                        help=f"Months of history to reconstruct (default: {BACKFILL_MONTHS})")
    args = parser.parse_args()

    api_key = os.getenv("POLYGON_API_KEY")
    if not api_key:
        print("[ERROR] POLYGON_API_KEY not set — cannot backfill.", file=sys.stderr)
        sys.exit(1)

    tickers = args.tickers.split(",") if args.tickers else WATCHLIST
    for ticker in tickers:
        ticker = ticker.strip()
        result = backfill_ticker(ticker, api_key, months=args.months)
        print(f"[BACKFILL] {result}")


if __name__ == "__main__":
    main()
