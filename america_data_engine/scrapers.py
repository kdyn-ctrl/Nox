import os
import sys
import requests
from datetime import datetime, timezone, timedelta
from typing import Dict, Any, List

# RULE-008: All HTTP calls use a (connect_timeout, read_timeout) tuple.
HTTP_TIMEOUT = (5, 10)

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


def fetch_alpaca_news() -> List[Dict[str, Any]]:
    """
    Fetches the latest financial news from Alpaca's API.
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
    try:
        response = requests.get(url, headers=headers, params=params, timeout=HTTP_TIMEOUT)
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
            })

        print(f"[INFO] [SCRAPER] Alpaca news fetched ({len(result)} articles).", flush=True)
        return result
    except requests.RequestException as e:
        print(f"[ERROR] [SCRAPER] Alpaca news fetch failed: {e}", flush=True)
        return []


def fetch_earnings_calendar(tickers: List[str]) -> Dict[str, List[Dict[str, Any]]]:
    """
    Queries Alpaca's corporate actions endpoint for scheduled earnings announcements.
    Returns a dict mapping each ticker to a list of earnings dates within the next 30 days.

    Format: {
        "AAPL": [{"date": "2026-07-15", "eps": 1.23, "eps_estimate": 1.20}, ...],
        "TSLA": [{"date": "2026-07-20"}, ...],
        ...
    }

    RULE-008: Enforces (5, 10) timeout tuple on all HTTP calls.
    """
    url = "https://api.alpaca.markets/v2/corporate-actions"
    headers = {
        "APCA-API-KEY-ID": ALPACA_KEY,
        "APCA-API-SECRET-KEY": ALPACA_SECRET,
    }

    result: Dict[str, List[Dict[str, Any]]] = {}

    # Compute the 30-day forward window from today (UTC)
    now_utc = datetime.now(tz=timezone.utc)
    start_date = now_utc.date().isoformat()
    end_date = (now_utc + timedelta(days=30)).date().isoformat()

    for ticker in tickers:
        try:
            params = {
                "symbols": ticker,
                "types": "earnings",
                "start": start_date,
                "end": end_date,
            }

            response = requests.get(url, headers=headers, params=params, timeout=HTTP_TIMEOUT)
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
            result[ticker] = []

    return result
