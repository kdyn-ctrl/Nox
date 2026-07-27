"""Utilities for checking US market trading days (skipping weekends and holidays)."""

import os
import requests
from datetime import datetime
from zoneinfo import ZoneInfo

# Fallback holidays (2024-2027) — used if Alpaca calendar fetch fails.
# IMPORTANT: Per FEEDBACK_HARDCODE_NOTHING_TUNABLE, override via TRADING_DAY_SKIP_DATES=YYYY-MM-DD,YYYY-MM-DD
FALLBACK_MARKET_HOLIDAYS = {
    # 2024
    "2024-01-01",  # New Year's Day
    "2024-01-15",  # MLK Day
    "2024-02-19",  # Presidents Day
    "2024-03-29",  # Good Friday
    "2024-05-27",  # Memorial Day
    "2024-06-19",  # Juneteenth
    "2024-07-04",  # Independence Day
    "2024-09-02",  # Labor Day
    "2024-11-28",  # Thanksgiving
    "2024-12-25",  # Christmas
    # 2025
    "2025-01-01",  # New Year's Day
    "2025-01-20",  # MLK Day
    "2025-02-17",  # Presidents Day
    "2025-04-18",  # Good Friday
    "2025-05-26",  # Memorial Day
    "2025-06-19",  # Juneteenth
    "2025-07-04",  # Independence Day
    "2025-09-01",  # Labor Day
    "2025-11-27",  # Thanksgiving
    "2025-12-25",  # Christmas
    # 2026
    "2026-01-01",  # New Year's Day
    "2026-01-19",  # MLK Day
    "2026-02-16",  # Presidents Day
    "2026-04-03",  # Good Friday
    "2026-05-25",  # Memorial Day
    "2026-06-19",  # Juneteenth
    "2026-07-03",  # Independence Day (observed on Fri; July 4 is Sat)
    "2026-09-07",  # Labor Day
    "2026-11-26",  # Thanksgiving
    "2026-12-25",  # Christmas
    # 2027
    "2027-01-01",  # New Year's Day
    "2027-01-18",  # MLK Day
    "2027-02-15",  # Presidents Day
    "2027-03-26",  # Good Friday
    "2027-05-31",  # Memorial Day
    "2027-06-18",  # Juneteenth
    "2027-07-05",  # Independence Day (observed, Mon)
    "2027-09-06",  # Labor Day
    "2027-11-25",  # Thanksgiving
    "2027-12-24",  # Christmas Eve (market closes at 13:00, treat as off)
    "2027-12-25",  # Christmas
}

_holidays_cache = None
_cache_date = None


def _fetch_alpaca_calendar() -> set | None:
    """
    Fetch US market holidays from Alpaca's official calendar endpoint.
    Returns a set of YYYY-MM-DD date strings, or None if fetch fails.

    Alpaca calendar is the authoritative source for market closure dates
    and handles holiday observances (e.g., when July 4 falls on Saturday).
    """
    try:
        alpaca_key = os.getenv("ALPACA_API_KEY")
        alpaca_secret = os.getenv("ALPACA_SECRET_KEY")

        if not alpaca_key or not alpaca_secret:
            return None

        headers = {
            "APCA-API-KEY-ID": alpaca_key,
            "APCA-API-SECRET-KEY": alpaca_secret,
        }

        # Alpaca /v1/calendar returns trading days and early closes for the year
        # We invert it to get non-trading days (weekends + holidays)
        resp = requests.get(
            "https://paper-api.alpaca.markets/v1/calendar",
            headers=headers,
            timeout=(5, 10),
        )

        if resp.status_code != 200:
            print(f"[WARN] [TRADING_DAY_UTILS] Alpaca calendar fetch failed: HTTP {resp.status_code}")
            return None

        calendar_data = resp.json()
        if not calendar_data:
            return None

        # Extract all dates marked as non-trading days
        trading_dates = {item["date"] for item in calendar_data}

        # Get current year and next few years to identify all market holidays
        et = ZoneInfo("America/New_York")
        now = datetime.now(et)
        year = now.year

        holidays = set()
        # Generate all dates in the current year + next 2 years
        for check_year in [year, year + 1, year + 2]:
            from datetime import date, timedelta
            start = date(check_year, 1, 1)
            end = date(check_year, 12, 31)
            current = start
            while current <= end:
                date_str = current.strftime("%Y-%m-%d")
                # If it's a weekday (Mon-Fri) and NOT in trading dates, it's a holiday
                if current.weekday() < 5 and date_str not in trading_dates:
                    holidays.add(date_str)
                current += timedelta(days=1)

        print(f"[INFO] [TRADING_DAY_UTILS] Loaded {len(holidays)} market holidays from Alpaca calendar.")
        return holidays

    except Exception as e:
        print(f"[WARN] [TRADING_DAY_UTILS] Alpaca calendar fetch failed: {e}")
        return None


def get_market_holidays():
    """
    Return set of market holidays (YYYY-MM-DD format).

    Strategy:
      1. Attempt to fetch from Alpaca calendar API (authoritative, always current)
      2. Fall back to hardcoded list on failure
      3. Augment with env var overrides (TRADING_DAY_SKIP_DATES=YYYY-MM-DD,...)
    """
    global _holidays_cache, _cache_date

    et = ZoneInfo("America/New_York")
    today = datetime.now(et).strftime("%Y-%m-%d")

    # Cache holidays for the day (avoid repeated API calls)
    if _holidays_cache is not None and _cache_date == today:
        return _holidays_cache

    # Try to fetch from Alpaca first
    holidays = _fetch_alpaca_calendar()

    # Fall back to hardcoded list on failure
    if holidays is None:
        holidays = set(FALLBACK_MARKET_HOLIDAYS)
        print("[INFO] [TRADING_DAY_UTILS] Using fallback hardcoded market holidays.")

    # Allow env var override: TRADING_DAY_SKIP_DATES=YYYY-MM-DD,YYYY-MM-DD
    env_skip = os.getenv("TRADING_DAY_SKIP_DATES", "")
    if env_skip:
        for date_str in env_skip.split(","):
            date_str = date_str.strip()
            if date_str:
                holidays.add(date_str)
                print(f"[INFO] [TRADING_DAY_UTILS] Added env override: {date_str}")

    # Cache for the day
    _holidays_cache = holidays
    _cache_date = today

    return holidays


def is_trading_day() -> bool:
    """
    Return True if today is a US market trading day (not a weekend or holiday).

    This gates options signals, equity signals, scout protocol, market scanner, and IV collection.
    To temporarily disable all signal generation on weekdays, set TRADING_DAYS_ENABLED=false in .env
    """
    # Check env var override
    if os.getenv("TRADING_DAYS_ENABLED", "true").lower() == "false":
        return False

    et = ZoneInfo("America/New_York")
    now = datetime.now(et)

    # Skip weekends (0=Mon, 6=Sun)
    if now.weekday() >= 5:  # Saturday or Sunday
        return False

    # Skip holidays
    today = now.strftime("%Y-%m-%d")
    if today in get_market_holidays():
        return False

    return True
