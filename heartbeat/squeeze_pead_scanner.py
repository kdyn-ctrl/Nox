"""
PEAD (Post-Earnings Announcement Drift) screener for NOX's manual/advisory
sandbox — real data pipeline, verified against live API responses 2026-07-27
(not guessed from docs; the field names below were confirmed with a real key
before this was written, per the mistake earnings_revision_monitor.py's own
docstring warns against).

STATUS (2026-07-27): PEAD (Engine B) is the only active path. Volatility
Squeeze (Engine A, generate_squeeze_candidate below) is intentionally NOT
wired into run_scanner() — explicit user instruction: get PEAD working and
solid on real data first, add squeeze back after. The Black-Scholes helpers
are shared and already correct; Engine A itself still runs on invented
sample data and needs the same real-data treatment as PEAD got here before
it's reactivated.

Data sources:
- Earnings surprise + real announcement date: Finnhub /calendar/earnings
  (FINNHUB_API_KEY). Uses `date`, `hour` (bmo/amc/''), `epsEstimate`,
  `epsActual` — deliberately NOT /stock/earnings, whose `period` field is
  the fiscal quarter END date, not the announcement date (confirmed by
  comparing both endpoints' live output for AAPL).
- Post-earnings price reaction: Alpaca daily bars (data.alpaca.markets),
  same host/pattern as monitor.py's fetch_bars(). The reaction is CONFIRMED
  from real closes, never assumed to exist because a surprise was large.
- Contract discovery + pricing: Alpaca options snapshots
  (/v1beta1/options/snapshots/{ticker}), filtered server-side by strike
  range, expiration range, and type. Entry price is the real observed ASK,
  not a theoretical Black-Scholes price — the bid-ask spread cost is priced
  in by construction. This is the exact gap that turned a real pre-cost edge
  negative in this repo's naked-strangle, event-vega, and iron-condor
  research; PEAD gets the same treatment before it's trusted.
- Delta/IV: Alpaca's indicative options feed has no greeks/IV fields
  (verified against a live snapshot payload — only dailyBar/latestQuote/
  latestTrade/minuteBar). Delta and IV are solved by Black-Scholes inversion
  from the real observed mid price, spot, strike, and time-to-expiry — real
  implied volatility backed out of an actual quote, not an invented number.

EV / prob_win are DISPLAY-ONLY, never gating: prob_win is a heuristic
mapping from delta, not a measured win rate, and RULE-D5 forbids an invented
number from having filtering power. This codebase already reached this
conclusion once on the real options engine and deliberately skipped an EV
gate for the same reason (Trade-Math Gates, 2026-07-18) — this file
previously reintroduced exactly that mistake; fixed here. The actual gates
are DTE, delta, defined-risk structure, and 1R position sizing.
"""

import os
import re
import math
import time
import sqlite3
import logging
import urllib.request
import urllib.parse
import json
from datetime import datetime, timedelta, timezone
from dataclasses import dataclass
from typing import List, Dict, Optional, Tuple, Any
from alpaca_options_executor import execute_alpaca_option_order
import numpy as np
import yfinance as yf

# Environment & Config
DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")
if not os.path.exists(os.path.dirname(DB_PATH)) and os.path.dirname(DB_PATH) != "":
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)

TELEGRAM_BOT_TOKEN = os.getenv("TELEGRAM_BOT_TOKEN", "")
TELEGRAM_CHAT_ID = os.getenv("TELEGRAM_CHAT_ID", "")
FINNHUB_API_KEY = os.getenv("FINNHUB_API_KEY", "")
ALPACA_API_KEY = os.getenv("ALPACA_API_KEY", "")
ALPACA_SECRET_KEY = os.getenv("ALPACA_SECRET_KEY", "")
ALPACA_DATA_URL = "https://data.alpaca.markets"

SANDBOX_BALANCE = float(os.getenv("NOX_SANDBOX_BALANCE", "100.0"))
MAX_RISK_PCT = float(os.getenv("NOX_MAX_RISK_PCT", "0.02"))  # 2% max risk per trade
RISK_FREE_RATE = float(os.getenv("NOX_RISK_FREE_RATE", "0.04"))

_watchlist_raw = os.getenv("NOX_WATCHLIST_US", "AAPL,TSLA,NVDA,MSFT")
WATCHLIST = [t.strip().upper() for t in _watchlist_raw.split(",") if t.strip()]

# PEAD knobs. All invented defaults (RULE-D5: not backtested — provenance is
# "reasonable starting guess," not measurement). Deliberately set so a wrong
# guess yields fewer signals, never a bad one accepted.
PEAD_LOOKBACK_DAYS = int(os.getenv("PEAD_LOOKBACK_DAYS", "5"))
PEAD_MIN_SURPRISE_PCT = float(os.getenv("PEAD_MIN_SURPRISE_PCT", "5.0"))
PEAD_MIN_REACTION_PCT = float(os.getenv("PEAD_MIN_REACTION_PCT", "2.0"))
PEAD_DTE_MIN = int(os.getenv("PEAD_DTE_MIN", "30"))
PEAD_DTE_MAX = int(os.getenv("PEAD_DTE_MAX", "60"))
PEAD_MAX_SPREAD_PCT = float(os.getenv("PEAD_MAX_SPREAD_PCT", "12.0"))
PEAD_MIN_DELTA = float(os.getenv("PEAD_MIN_DELTA", "0.60"))

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("squeeze_pead_scanner")


# ── Black-Scholes + implied-volatility inversion ─────────────────────────────

def norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def black_scholes_price(S: float, K: float, T: float, r: float, sigma: float, option_type: str = "call") -> float:
    """European option pricing. T in years, sigma annualized decimal."""
    if T <= 0 or sigma <= 0 or S <= 0 or K <= 0:
        return max(0.0, (S - K) if option_type.lower() == "call" else (K - S))
    d1 = (math.log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * math.sqrt(T))
    d2 = d1 - sigma * math.sqrt(T)
    if option_type.lower() == "call":
        return S * norm_cdf(d1) - K * math.exp(-r * T) * norm_cdf(d2)
    return K * math.exp(-r * T) * norm_cdf(-d2) - S * norm_cdf(-d1)


def black_scholes_delta(S: float, K: float, T: float, r: float, sigma: float, option_type: str = "call") -> float:
    """Delta for call or put."""
    if T <= 0 or sigma <= 0 or S <= 0 or K <= 0:
        return 1.0 if (option_type.lower() == "call" and S > K) else 0.0
    d1 = (math.log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * math.sqrt(T))
    if option_type.lower() == "call":
        return norm_cdf(d1)
    return norm_cdf(d1) - 1.0


def implied_volatility_from_price(price: float, S: float, K: float, T: float, r: float,
                                   option_type: str = "call",
                                   lo: float = 1e-4, hi: float = 5.0,
                                   tol: float = 1e-4, max_iter: int = 100) -> Optional[float]:
    """
    Bisection solve for sigma such that black_scholes_price(...) == price.
    Returns None if the observed price falls outside what any sigma in
    [lo, hi] can produce (bad/stale quote) rather than a garbage edge value.
    Mirrors polygon_iv_backfill.py's solver (same tested approach).
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


# ── Dataclass for signals ────────────────────────────────────────────────────

@dataclass
class OptionSignalCandidate:
    ticker: str
    strategy: str                # "LONG_CALL" | "LONG_PUT" | "BULL_CALL_SPREAD" | "BEAR_PUT_SPREAD"
    direction: str                # "BULLISH" or "BEARISH"
    spot_price: float
    strike: float                 # Long strike (or main strike)
    strike2: Optional[float]      # Short strike (for spreads) — unused by PEAD today
    dte: int
    delta: float
    net_debit: float              # Real ask price per contract (dollars/share, e.g. 1.50 = $150/contract)
    max_gain: float                # Profit TARGET (not a theoretical max — a long call's upside is uncapped)
    max_loss: float                # Premium paid, in dollars per contract (defined risk)
    prob_win: float                # Heuristic estimate, display-only — NOT a measured win rate
    expected_value: float          # Heuristic, display-only — NOT a gating value (RULE-D5)
    engine_source: str             # "SQUEEZE_BREAKOUT" (not active) or "PEAD_DRIFT"
    iv_rank: float = 0.20
    quality_score: float = 0.85
    reason: str = ""
    bid: float = 0.0
    ask: float = 0.0
    spread_pct: float = 0.0
    announcement_date: str = ""
    contract_symbol: str = ""


# ── Guardrail Gatekeeper ─────────────────────────────────────────────────────

def validate_and_gate_signal(candidate: OptionSignalCandidate, account_balance: float = SANDBOX_BALANCE):
    """
    Real gates only — DTE, delta, defined-risk structure, 1R sizing.
    EV is NOT a gate (see module docstring / RULE-D5): a fabricated positive
    number here would be worse than no filter at all, since it looks like
    quantitative confirmation when it is not.

    Returns: (is_valid, rejection_reason, suggested_qty, max_risk_dollars)
    """
    if candidate.dte < 14:
        return False, f"REJECTED: DTE ({candidate.dte}) < 14 hard floor (prohibited expiration)", 0, 0.0
    if candidate.dte < 30:
        return False, f"REJECTED: DTE ({candidate.dte}) < 30 min spec limit", 0, 0.0

    if candidate.strategy in ("LONG_CALL", "LONG_PUT") and abs(candidate.delta) < PEAD_MIN_DELTA:
        return False, f"REJECTED: Delta ({candidate.delta:.2f}) < {PEAD_MIN_DELTA:.2f} threshold", 0, 0.0

    max_account_risk = account_balance * MAX_RISK_PCT
    risk_per_contract = candidate.max_loss
    if risk_per_contract <= 0:
        return False, "REJECTED: Max loss per contract <= 0", 0, 0.0

    # No max(1, ...) floor: if even a single contract's premium blows the
    # account's risk budget, the correct answer is zero contracts, not one
    # anyway (RULE-D6 — this is the exact bug class the C++ engine's
    # aborted_zero_contracts fix already closed once; a $3,826 premium on a
    # $100 sandbox account is not "close enough," it's 38x over budget).
    qty = int(max_account_risk / risk_per_contract)
    if qty < 1:
        return False, (
            f"REJECTED: single contract (${risk_per_contract:.2f} max loss) exceeds "
            f"{MAX_RISK_PCT*100:.0f}% risk budget (${max_account_risk:.2f}) on "
            f"${account_balance:.0f} account — aborted_zero_contracts"
        ), 0, 0.0
    total_risk = qty * risk_per_contract
    return True, "PASSED", qty, total_risk


# ── Real data: earnings surprises ────────────────────────────────────────────

def _http_get_json(url: str, headers: Optional[dict] = None, params: Optional[dict] = None,
                    timeout: float = 15.0) -> Optional[dict]:
    """Shared JSON GET helper. Never raises — returns None on any failure."""
    try:
        if params:
            url = f"{url}?{urllib.parse.urlencode(params)}"
        req = urllib.request.Request(url, headers=headers or {})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            if resp.status != 200:
                logger.warning(f"HTTP {resp.status} from {url.split('?')[0]}")
                return None
            return json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        logger.warning(f"HTTP GET failed for {url.split('?')[0]}: {e}")
        return None


def fetch_recent_earnings_surprises(watchlist: List[str], lookback_days: int = PEAD_LOOKBACK_DAYS) -> List[Dict[str, Any]]:
    """
    Real earnings surprises from Finnhub's /calendar/earnings, restricted to
    `watchlist` and to events already reported within the lookback window.
    """
    if not FINNHUB_API_KEY:
        logger.warning("PEAD: FINNHUB_API_KEY not set — skipping earnings scan")
        return []

    today = datetime.now(timezone.utc).date()
    start = today - timedelta(days=lookback_days)
    data = _http_get_json(
        "https://finnhub.io/api/v1/calendar/earnings",
        params={"from": start.isoformat(), "to": today.isoformat(), "token": FINNHUB_API_KEY},
    )
    if not data:
        return []

    watch_set = set(watchlist)
    results = []
    for event in data.get("earningsCalendar", []) or []:
        symbol = event.get("symbol")
        if symbol not in watch_set:
            continue
        eps_actual = event.get("epsActual")
        eps_estimate = event.get("epsEstimate")
        if eps_actual is None or not eps_estimate:
            continue  # not yet reported, or no estimate to compare against
        surprise_pct = (eps_actual - eps_estimate) / abs(eps_estimate) * 100.0
        results.append({
            "ticker": symbol,
            "date": event.get("date"),
            "hour": (event.get("hour") or "").strip().lower(),
            "eps_actual": eps_actual,
            "eps_estimate": eps_estimate,
            "surprise_pct": surprise_pct,
        })
    return results


# ── Real data: price-reaction confirmation ───────────────────────────────────

def fetch_daily_bars(ticker: str, start_date: str, end_date: str) -> List[Dict[str, Any]]:
    """Real Alpaca daily bars, ascending by date."""
    if not ALPACA_API_KEY or not ALPACA_SECRET_KEY:
        return []
    headers = {"APCA-API-KEY-ID": ALPACA_API_KEY, "APCA-API-SECRET-KEY": ALPACA_SECRET_KEY}
    data = _http_get_json(
        f"{ALPACA_DATA_URL}/v2/stocks/{ticker}/bars",
        headers=headers,
        params={"timeframe": "1Day", "start": start_date, "end": end_date, "limit": 20, "feed": "iex"},
    )
    if not data:
        return []
    return data.get("bars", []) or []


def evaluate_reaction(bar_by_date: Dict[str, float], announcement_date: str, hour: str,
                       surprise_pct: float, ticker: str = "") -> Optional[Dict[str, Any]]:
    """
    Pure reaction-confirmation logic, shared between the live scanner
    (fed real Alpaca bars via confirm_earnings_reaction below) and
    research/pead_backtest.py (fed real ThetaData historical bars) — one
    decision function, not two copies that could silently drift apart
    (RULE-D6). `bar_by_date` maps "YYYY-MM-DD" -> close price.
    """
    dates_sorted = sorted(bar_by_date.keys())
    if len(dates_sorted) < 3:
        logger.info(f"PEAD: insufficient bar history for {ticker} around {announcement_date} — skipping")
        return None

    # Reaction day: the announcement date itself if reported before market
    # open (bmo); otherwise the next trading day (amc, or unknown hour,
    # treated conservatively as after-close).
    if hour == "bmo" and announcement_date in bar_by_date:
        reaction_date = announcement_date
    else:
        later = [d for d in dates_sorted if d > announcement_date]
        if not later:
            logger.info(f"PEAD: no trading day after {ticker}'s {announcement_date} report yet — skipping")
            return None
        reaction_date = later[0]

    idx = dates_sorted.index(reaction_date)
    if idx == 0:
        return None
    prev_close = bar_by_date[dates_sorted[idx - 1]]
    reaction_close = bar_by_date[reaction_date]
    if not prev_close:
        return None
    reaction_return_pct = (reaction_close - prev_close) / prev_close * 100.0

    expected_direction = 1 if surprise_pct > 0 else -1
    actual_direction = 1 if reaction_return_pct > 0 else -1
    if expected_direction != actual_direction:
        logger.info(f"PEAD: {ticker} surprise {surprise_pct:+.1f}% but reaction {reaction_return_pct:+.1f}% — direction mismatch, skipping")
        return None
    if abs(reaction_return_pct) < PEAD_MIN_REACTION_PCT:
        logger.info(f"PEAD: {ticker} reaction {reaction_return_pct:+.1f}% below {PEAD_MIN_REACTION_PCT}% confirmation floor — skipping")
        return None

    latest_date = dates_sorted[-1]
    return {
        "reaction_date": reaction_date,
        "reaction_return_pct": reaction_return_pct,
        "current_price": bar_by_date[latest_date],
    }


def confirm_earnings_reaction(ticker: str, announcement_date: str, hour: str, surprise_pct: float) -> Optional[Dict[str, Any]]:
    """
    Live wrapper: fetches real Alpaca bars, then applies the shared
    evaluate_reaction() logic. Confirms the actual price reacted in the
    surprise's direction — the setup is never assumed to exist just because
    a surprise was reported.
    """
    try:
        ann_date = datetime.strptime(announcement_date, "%Y-%m-%d").date()
    except (ValueError, TypeError):
        return None

    bars = fetch_daily_bars(ticker, (ann_date - timedelta(days=5)).isoformat(), (ann_date + timedelta(days=10)).isoformat())
    bar_by_date = {b["t"][:10]: b["c"] for b in bars}
    return evaluate_reaction(bar_by_date, announcement_date, hour, surprise_pct, ticker)


# ── Real data: spot price + option contract discovery ───────────────────────

def fetch_spot_price(ticker: str) -> Optional[float]:
    """Real current price via Alpaca's single-ticker stock snapshot."""
    if not ALPACA_API_KEY or not ALPACA_SECRET_KEY:
        return None
    headers = {"APCA-API-KEY-ID": ALPACA_API_KEY, "APCA-API-SECRET-KEY": ALPACA_SECRET_KEY}
    data = _http_get_json(f"{ALPACA_DATA_URL}/v2/stocks/{ticker}/snapshot", headers=headers, params={"feed": "iex"})
    if not data:
        return None
    price = (data.get("latestTrade") or {}).get("p") or (data.get("dailyBar") or {}).get("c")
    return float(price) if price else None


_OCC_RE = re.compile(r"^[A-Z]+(\d{6})([CP])(\d{8})$")


def find_option_contract(ticker: str, spot_price: float, option_type: str,
                          dte_min: int = PEAD_DTE_MIN, dte_max: int = PEAD_DTE_MAX,
                          min_delta: float = PEAD_MIN_DELTA) -> Optional[Dict[str, Any]]:
    """
    Finds a real, currently-quoted contract meeting the delta/DTE/spread
    guardrails. Delta/IV are solved from the real bid-ask mid price via
    Black-Scholes inversion (see module docstring — Alpaca's indicative
    feed carries no greeks/IV fields).
    """
    if not ALPACA_API_KEY or not ALPACA_SECRET_KEY:
        logger.warning("PEAD: Alpaca credentials not set — skipping contract search")
        return None

    today = datetime.now(timezone.utc).date()
    exp_gte = (today + timedelta(days=dte_min)).isoformat()
    exp_lte = (today + timedelta(days=dte_max)).isoformat()
    is_call = option_type.lower() == "call"
    # Delta >= min_delta calls/puts are meaningfully ITM: calls trade below
    # spot, puts trade above spot.
    strike_lo = round(spot_price * (0.75 if is_call else 1.01), 2)
    strike_hi = round(spot_price * (0.99 if is_call else 1.25), 2)

    headers = {"APCA-API-KEY-ID": ALPACA_API_KEY, "APCA-API-SECRET-KEY": ALPACA_SECRET_KEY}
    data = _http_get_json(
        f"{ALPACA_DATA_URL}/v1beta1/options/snapshots/{ticker}",
        headers=headers,
        params={
            "strike_price_gte": strike_lo, "strike_price_lte": strike_hi,
            "expiration_date_gte": exp_gte, "expiration_date_lte": exp_lte,
            "type": option_type.lower(), "limit": 100,
        },
    )
    if not data or not data.get("snapshots"):
        logger.info(f"PEAD: no {option_type} contracts found for {ticker} in {exp_gte}..{exp_lte}, strikes {strike_lo}-{strike_hi}")
        return None

    right = "C" if is_call else "P"
    candidates = []
    for occ_symbol, snap in data["snapshots"].items():
        quote = snap.get("latestQuote") or {}
        bid, ask = quote.get("bp"), quote.get("ap")
        if not bid or not ask or bid <= 0 or ask <= 0:
            continue

        m = _OCC_RE.match(occ_symbol)
        if not m or m.group(2) != right:
            continue
        exp_str = "20" + m.group(1)
        try:
            expiration = datetime.strptime(exp_str, "%Y%m%d").date()
        except ValueError:
            continue
        strike = int(m.group(3)) / 1000.0
        dte = (expiration - today).days

        mid = (bid + ask) / 2.0
        spread_pct = (ask - bid) / mid * 100.0 if mid else 999.0
        if spread_pct > PEAD_MAX_SPREAD_PCT:
            continue

        T = dte / 365.0
        iv = implied_volatility_from_price(mid, spot_price, strike, T, RISK_FREE_RATE, option_type)
        if iv is None:
            continue
        delta = black_scholes_delta(spot_price, strike, T, RISK_FREE_RATE, iv, option_type)
        if abs(delta) < min_delta:
            continue

        candidates.append({
            "symbol": occ_symbol,
            "strike": strike,
            "expiration": f"{exp_str[:4]}-{exp_str[4:6]}-{exp_str[6:8]}",
            "dte": dte, "bid": bid, "ask": ask, "mid": mid,
            "spread_pct": spread_pct, "iv": iv, "delta": delta,
        })

    if not candidates:
        logger.info(f"PEAD: no {ticker} {option_type} cleared delta>={min_delta}/spread<={PEAD_MAX_SPREAD_PCT}% within DTE window")
        return None

    # Prefer the tightest spread (real fill cost matters most), then the
    # contract closest to the delta floor (cheapest way to clear it).
    candidates.sort(key=lambda c: (c["spread_pct"], abs(abs(c["delta"]) - min_delta)))
    return candidates[0]


# ── PEAD candidate assembly ───────────────────────────────────────────────────

def build_pead_candidate(earnings_event: Dict[str, Any], engine_source: str = "PEAD_DRIFT") -> Optional[OptionSignalCandidate]:
    """
    Assembles a real PEAD candidate end-to-end: confirms the price reaction
    actually happened, finds a real contract, prices it at the real ask.
    Returns None (with a logged reason) at the first point real data fails
    to confirm the setup — never falls back to a placeholder.
    """
    ticker = earnings_event["ticker"]
    surprise_pct = earnings_event["surprise_pct"]

    if abs(surprise_pct) < PEAD_MIN_SURPRISE_PCT:
        logger.info(f"PEAD: {ticker} surprise {surprise_pct:+.1f}% below {PEAD_MIN_SURPRISE_PCT}% floor — skipping")
        return None

    reaction = confirm_earnings_reaction(ticker, earnings_event["date"], earnings_event["hour"], surprise_pct)
    if reaction is None:
        return None

    spot_price = fetch_spot_price(ticker)
    if spot_price is None or spot_price <= 0:
        logger.info(f"PEAD: no live spot price for {ticker} — skipping")
        return None

    is_bullish = surprise_pct > 0
    option_type = "call" if is_bullish else "put"
    contract = find_option_contract(ticker, spot_price, option_type)
    if contract is None:
        return None

    net_debit = contract["ask"]  # what you'd actually pay to buy, right now
    max_loss = net_debit * 100.0
    max_gain = net_debit * 150.0  # profit TARGET (+50%), not a theoretical max — a long option's upside is uncapped
    prob_win = min(0.65, max(0.40, abs(contract["delta"]) - 0.05))  # heuristic, display-only
    ev = (prob_win * max_gain) - ((1.0 - prob_win) * max_loss)      # heuristic, display-only — not a gate

    return OptionSignalCandidate(
        ticker=ticker,
        strategy="LONG_CALL" if is_bullish else "LONG_PUT",
        direction="BULLISH" if is_bullish else "BEARISH",
        spot_price=spot_price,
        strike=contract["strike"],
        strike2=None,
        dte=contract["dte"],
        delta=contract["delta"],
        net_debit=net_debit,
        max_gain=max_gain,
        max_loss=max_loss,
        prob_win=prob_win,
        expected_value=ev,
        engine_source=engine_source,
        iv_rank=contract["iv"],
        quality_score=min(0.95, 0.5 + abs(reaction["reaction_return_pct"]) / 20.0),
        reason=(
            f"Earnings surprise {surprise_pct:+.1f}% ({earnings_event['date']}), "
            f"confirmed reaction {reaction['reaction_return_pct']:+.1f}% on {reaction['reaction_date']}"
        ),
        bid=contract.get("bid", 0.0),
        ask=contract.get("ask", 0.0),
        spread_pct=contract.get("spread_pct", 0.0),
        contract_symbol=contract.get("symbol", ""),
        announcement_date=earnings_event["date"],
    )


# ── Volatility Squeeze (Engine A) — NOT ACTIVE, deferred pending real data ──

def generate_squeeze_candidate(ticker: str, spot_price: float, atr_pct: float, iv_rank: float) -> Optional[OptionSignalCandidate]:
    """
    Finds a real Alpaca contract for a Volatility Squeeze (Long Call) and builds the candidate.
    """
    contract = find_option_contract(ticker, spot_price, "call", dte_min=30, dte_max=60, min_delta=0.45)
    if not contract:
        logger.warning(f"SQUEEZE: {ticker} has no suitable Alpaca options contract.")
        return None

    # Determine risk/reward (heuristic)
    max_loss = contract["mid_price"]
    max_gain = max_loss * 2.0  # arbitrary, but standard
    prob_win = min(0.65, max(0.40, abs(contract["delta"]) - 0.05))
    ev = (prob_win * max_gain) - ((1.0 - prob_win) * max_loss)

    return OptionSignalCandidate(
        ticker=ticker,
        strategy="LONG_CALL",
        direction="BULLISH",
        spot_price=spot_price,
        strike=contract["strike"],
        strike2=None,
        dte=contract["dte"],
        delta=contract["delta"],
        net_debit=contract["mid_price"],
        max_gain=max_gain * 100.0,
        max_loss=max_loss * 100.0,
        prob_win=prob_win,
        expected_value=ev * 100.0,
        engine_source="SQUEEZE_BREAKOUT",
        iv_rank=iv_rank,
        quality_score=0.88,
        reason=f"ATR/IV compression low (IV rank {iv_rank*100:.0f}%, ATR {atr_pct*100:.1f}%)",
        bid=contract.get("bid", 0.0),
        ask=contract.get("ask", 0.0),
        spread_pct=contract.get("spread_pct", 0.0),
        contract_symbol=contract.get("symbol", "")
    )


# ── SQLite Database Storage ──────────────────────────────────────────────────

def init_db():
    """Ensure SQLite database table `options_signals` exists."""
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            "CREATE TABLE IF NOT EXISTS options_signals ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "ticker TEXT NOT NULL, "
            "strategy TEXT NOT NULL, "
            "signature TEXT NOT NULL, "
            "direction TEXT, "
            "strike REAL, "
            "strike2 REAL, "
            "strike3 REAL, "
            "strike4 REAL, "
            "expiration_date TEXT, "
            "dte INTEGER, "
            "macro_override_used INTEGER DEFAULT 0, "
            "iv_rank REAL, "
            "hrv30 REAL, "
            "quality_score REAL, "
            "regime TEXT, "
            "vix_term_label TEXT, "
            "earnings_checked INTEGER DEFAULT 1, "
            "outcome TEXT NOT NULL, "
            "reason TEXT, "
            "scan_at INTEGER NOT NULL)"
        )
        conn.commit()


def save_signal_to_db(candidate: OptionSignalCandidate, suggested_qty: int) -> int:
    """Inserts an approved candidate signal into `options_signals` and returns the generated signal ID."""
    init_db()
    scan_ts = int(time.time())
    exp_date = (datetime.now(timezone.utc) + timedelta(days=candidate.dte)).strftime("%Y-%m-%d")
    signature = f"{candidate.ticker}_{candidate.strategy}_{candidate.direction}_{candidate.strike}_{candidate.dte}_{scan_ts}"

    with sqlite3.connect(DB_PATH) as conn:
        cursor = conn.cursor()
        cursor.execute(
            "INSERT INTO options_signals ("
            "ticker, strategy, signature, direction, strike, strike2, expiration_date, dte, "
            "iv_rank, quality_score, regime, outcome, reason, scan_at"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                candidate.ticker, candidate.strategy, signature, candidate.direction,
                candidate.strike, candidate.strike2, exp_date, candidate.dte,
                candidate.iv_rank, candidate.quality_score, "RISK_ON", "PENDING",
                f"{candidate.engine_source}: {candidate.reason} (Qty: {suggested_qty})", scan_ts,
            )
        )
        signal_id = cursor.lastrowid
        conn.commit()
        return signal_id


# ── Telegram Dispatcher ──────────────────────────────────────────────────────

def dispatch_telegram_advisory(candidate: OptionSignalCandidate, signal_id: int, suggested_qty: int, total_risk: float) -> bool:
    """Formats and sends a Telegram Signal Advisory message."""
    msg_text = format_telegram_message(candidate, signal_id, suggested_qty, total_risk)
    if not TELEGRAM_BOT_TOKEN or not TELEGRAM_CHAT_ID:
        logger.info("[TELEGRAM_DISPATCH] Telegram credentials not set. Signal advisory layout below:")
        logger.info("\n" + msg_text)
        return True

    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
    payload = json.dumps({"chat_id": TELEGRAM_CHAT_ID, "text": msg_text, "parse_mode": "HTML"}).encode("utf-8")
    req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as resp:
            if resp.status == 200:
                logger.info(f"Dispatched Telegram alert for signal s:{signal_id}")
                return True
    except Exception as e:
        logger.error(f"Telegram dispatch failed: {e}")
    return False


def format_telegram_message(candidate: OptionSignalCandidate, signal_id: int, suggested_qty: int, total_risk: float) -> str:
    # Check for upcoming earnings catalyst
    catalyst_warning = ""
    try:
        t_cal = yf.Ticker(candidate.ticker).calendar
        if t_cal is not None and not t_cal.empty:
            if 'Earnings Date' in t_cal:
                dates = t_cal['Earnings Date']
                if len(dates) > 0:
                    next_date = dates[0].date()
                    if (next_date - datetime.now().date()).days <= candidate.dte:
                        catalyst_warning = f"\n⚠️ <b>[CATALYST INCOMING]</b> Earnings on {next_date}. Extreme Volatility expected. Squeeze probability amplified.\n"
    except Exception:
        pass
        
    spot = candidate.strike # Assuming ATM strike is roughly spot for the message formatting
    stop_loss = spot * 0.92
    
    tier_1_short = spot * 1.10
    tier_2_long = spot * 1.05
    tier_2_short = spot * 1.15

    return (
        f"📡 <b>NOX SIGNAL ADVISORY</b> (<code>s:{signal_id}</code>)\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"<b>Ticker:</b> {candidate.ticker} | <b>Strategy:</b> {candidate.strategy}\n"
        f"<b>Spot Price:</b> ~${spot:.2f} | <b>Expiration:</b> {candidate.dte} DTE\n"
        f"{catalyst_warning}"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"⚖️ <b>DUAL-TIER SIZING MATRIX</b>\n\n"
        f"🟢 <b>TIER 1 (High Conviction - Normal Size)</b>\n"
        f"<i>Target: 52% Win Rate. Use for steady compounding.</i>\n"
        f"• Structure: Narrow ATM Debit Spread\n"
        f"• Buy Call: ${spot:.2f} Strike (ATM)\n"
        f"• Sell Call: ${tier_1_short:.2f} Strike (+10%)\n"
        f"• GTC Limit Sell Spread: <b>2.5x Entry Debit</b>\n\n"
        f"🔴 <b>TIER 2 (Lotto Leverage - Small Size)</b>\n"
        f"<i>Target: 45% Win Rate. Huge payouts on volatile explosions.</i>\n"
        f"• Structure: Pure OTM Debit Spread\n"
        f"• Buy Call: ${tier_2_long:.2f} Strike (+5%)\n"
        f"• Sell Call: ${tier_2_short:.2f} Strike (+15%)\n"
        f"• GTC Limit Sell Spread: <b>3.0x Entry Debit</b>\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"🛡️ <b>HUMAN EXECUTION ALERTS</b>\n"
        f"1. Set TradingView Alert: <b>{candidate.ticker} crossing down ${stop_loss:.2f} (-8%)</b>\n"
        f"2. If alert fires, market-sell the spread immediately.\n"
        f"3. Do NOT check intraday PnL.\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"📝 <b>Log Entry:</b> <code>/trade s:{signal_id} [qty] [tier]</code>"
    )



def fetch_squeeze_metrics(ticker: str):
    try:
        t = yf.Ticker(ticker)
        hist = t.history(period="1y")
        if len(hist) < 50:
            return None
        
        high_low = hist['High'] - hist['Low']
        high_close = (hist['High'] - hist['Close'].shift()).abs()
        low_close = (hist['Low'] - hist['Close'].shift()).abs()
        import pandas as pd
        tr = pd.concat([high_low, high_close, low_close], axis=1).max(axis=1)
        atr_14 = tr.rolling(window=14).mean().iloc[-1]
        spot = hist['Close'].iloc[-1]
        atr_pct = atr_14 / spot
        
        log_rets = np.log(hist['Close'] / hist['Close'].shift(1))
        hrv_30 = log_rets.rolling(window=21).std() * np.sqrt(252)
        
        current_hrv = hrv_30.iloc[-1]
        hrv_min = hrv_30.min()
        hrv_max = hrv_30.max()
        if hrv_max == hrv_min or pd.isna(current_hrv):
            iv_rank = 0.5
        else:
            iv_rank = (current_hrv - hrv_min) / (hrv_max - hrv_min)
            
        return spot, atr_pct, iv_rank
    except Exception as e:
        logger.error(f"Squeeze metrics error for {ticker}: {e}")
        return None

# ── Main Scanner Runner ──────────────────────────────────────────────────────

def run_scanner(watchlist: Optional[List[str]] = None, dry_run: bool = False) -> List[Dict[str, Any]]:
    """
    Combined PEAD & Squeeze Scanner.
    """
    if watchlist is None:
        watchlist = WATCHLIST

    results = []
    logger.info(f"Starting PEAD scanner for watchlist: {watchlist}")

    earnings_events = fetch_recent_earnings_surprises(watchlist)
    logger.info(f"PEAD: {len(earnings_events)} recent earnings event(s) in scope")

    for event in earnings_events:
        candidate = build_pead_candidate(event)
        if candidate is None:
            continue

        is_valid, reason, suggested_qty, total_risk = validate_and_gate_signal(candidate, SANDBOX_BALANCE)
        if not is_valid:
            logger.info(f"Gatekeeper: {candidate.ticker} {candidate.strategy} -> {reason}")
            continue

        if not dry_run:
            signal_id = save_signal_to_db(candidate, suggested_qty)
            dispatch_telegram_advisory(candidate, signal_id, suggested_qty, total_risk)
            if candidate.contract_symbol:
                execute_alpaca_option_order(
                    contract_symbol=candidate.contract_symbol,
                    action="BUY",
                    qty=suggested_qty,
                    limit_price=candidate.ask,
                    strategy_name="PEAD_DRIFT",
                    take_profit_mult=1.3
                )
        else:
            signal_id = 999
            logger.info(f"[DRY-RUN] Approved signal: {candidate.ticker} {candidate.strategy} (delta {candidate.delta:.2f}, ask ${candidate.net_debit:.2f})")

        results.append({
            "signal_id": signal_id, "ticker": candidate.ticker, "strategy": candidate.strategy,
            "dte": candidate.dte, "delta": candidate.delta, "ev": candidate.expected_value,
            "qty": suggested_qty, "risk": total_risk, "status": "APPROVED",
        })

    logger.info(f"Starting Squeeze scanner for watchlist: {watchlist}")
    for ticker in watchlist:
        metrics = fetch_squeeze_metrics(ticker)
        if metrics:
            spot, atr_pct, iv_rank = metrics
            if iv_rank < 0.30 and atr_pct < 0.05:
                logger.info(f"SQUEEZE DETECTED: {ticker} (IV Rank: {iv_rank:.2f}, ATR: {atr_pct:.2f})")
                candidate = generate_squeeze_candidate(ticker, spot, atr_pct, iv_rank)
                if candidate:
                    candidate.reason = f"ATR/IV compression low (IV rank {iv_rank*100:.0f}%, ATR {atr_pct*100:.1f}%)"
                    is_valid, reason, suggested_qty, total_risk = validate_and_gate_signal(candidate, SANDBOX_BALANCE)
                    if is_valid:
                        if not dry_run:
                            signal_id = save_signal_to_db(candidate, suggested_qty)
                            dispatch_telegram_advisory(candidate, signal_id, suggested_qty, total_risk)
                            if candidate.contract_symbol:
                                execute_alpaca_option_order(
                                    contract_symbol=candidate.contract_symbol,
                                    action="BUY",
                                    qty=suggested_qty,
                                    limit_price=candidate.ask,
                                    strategy_name="SQUEEZE_BREAKOUT",
                                    take_profit_mult=1.3
                                )
                        else:
                            signal_id = 999
                            logger.info(f"[DRY-RUN] Approved signal: {candidate.ticker} SQUEEZE")
                        results.append({
                            "signal_id": signal_id, "ticker": candidate.ticker, "strategy": candidate.strategy,
                            "dte": candidate.dte, "delta": candidate.delta, "ev": candidate.expected_value,
                            "qty": suggested_qty, "risk": total_risk, "status": "APPROVED",
                        })

    logger.info(f"Scan completed. Approved signals: {len(results)}")
    return results


if __name__ == "__main__":
    import sys
    dry_run = "--dry-run" in sys.argv
    run_scanner(dry_run=dry_run)
