import os
import requests
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
