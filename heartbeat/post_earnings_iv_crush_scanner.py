import os
import json
import logging
import urllib.request
import urllib.parse
from datetime import datetime, timedelta, timezone
from typing import List, Dict, Any, Optional

import pandas as pd
import yfinance as yf
from trading_day_utils import is_trading_day

# Reusing components from existing scanners
from squeeze_pead_scanner import (
    OptionSignalCandidate,
    save_signal_to_db,
    SANDBOX_BALANCE,
    _http_get_json,
    FINNHUB_API_KEY
)

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("post_earnings_iv_crush_scanner")

TELEGRAM_BOT_TOKEN = os.getenv("TELEGRAM_BOT_TOKEN", "")
TELEGRAM_CHAT_ID = os.getenv("TELEGRAM_CHAT_ID", "")

# The ultra-liquid names where Iron Condors/Short Straddles have tight enough spreads
ULTRA_LIQUID_TICKERS = ["AAPL", "NVDA", "TSLA", "AMD", "AMZN", "META", "MSFT", "GOOGL"]
TARGET_ENTRY_DAYS_BEFORE = 1

def _fetch_future_earnings_yf(watchlist: List[str], lookahead_days: int = 7) -> Dict[str, Dict[str, Any]]:
    """Cross-verify future earnings dates using yfinance consensus calendar."""
    results = {}
    today = datetime.now(timezone.utc).date()
    end_date = today + timedelta(days=lookahead_days)
    for ticker in watchlist:
        try:
            t = yf.Ticker(ticker)
            ed = t.get_earnings_dates(limit=4)
            if ed is None or ed.empty:
                continue
            for idx, row in ed.iterrows():
                event_date = idx.date()
                if today <= event_date <= end_date:
                    results[ticker] = {
                        "symbol": ticker,
                        "date": event_date.strftime("%Y-%m-%d"),
                        "hour": "amc" if idx.hour >= 12 else "bmo",
                        "epsEstimate": float(row["EPS Estimate"]) if pd.notna(row.get("EPS Estimate")) else None,
                    }
        except Exception as e:
            logger.debug(f"yfinance future earnings lookup failed for {ticker}: {e}")
    return results


def fetch_future_earnings(watchlist: List[str], lookahead_days: int = 7) -> List[Dict[str, Any]]:
    """Fetch future earnings dates cross-validated between Finnhub and yfinance calendars."""
    events_map = {}
    today = datetime.now(timezone.utc).date()
    end_date = today + timedelta(days=lookahead_days)

    if FINNHUB_API_KEY:
        url = "https://finnhub.io/api/v1/calendar/earnings"
        params = {"from": today.isoformat(), "to": end_date.isoformat(), "token": FINNHUB_API_KEY}
        data = _http_get_json(url, params=params)
        if data and "earningsCalendar" in data:
            for item in data["earningsCalendar"]:
                t = item.get("symbol")
                if t in watchlist:
                    events_map[t] = item

    # Cross-verify and fill missing/discrepant events using yfinance
    yf_events = _fetch_future_earnings_yf(watchlist, lookahead_days=lookahead_days)
    for ticker, yf_item in yf_events.items():
        if ticker not in events_map:
            logger.info(f"IV Crush: Added missing earnings event for {ticker} on {yf_item['date']} via yfinance cross-check.")
            events_map[ticker] = yf_item
        else:
            fh_date = events_map[ticker].get("date")
            if fh_date != yf_item["date"]:
                logger.info(f"IV Crush: Date discrepancy for {ticker} (Finnhub={fh_date}, yfinance={yf_item['date']}). Correcting to yfinance.")
                events_map[ticker]["date"] = yf_item["date"]

    return list(events_map.values())


def count_trading_days_between(start_date, end_date) -> int:
    """Count trading days between two dates (exclusive of start_date, inclusive of end_date)."""
    current = start_date + timedelta(days=1)
    count = 0
    while current <= end_date:
        if is_trading_day(current):
            count += 1
        current += timedelta(days=1)
    return count


from trading_day_utils import is_trading_day, is_signal_dispatch_allowed


def dispatch_iv_crush_telegram(candidate: OptionSignalCandidate, signal_id: int) -> bool:
    """Custom format emphasizing manual execution for IV Crush."""
    if not is_signal_dispatch_allowed(is_open_execution=True):
        logger.info(f"[TELEGRAM_DISPATCH] IV Crush signal s:{signal_id} saved, but Telegram alert suppressed (outside market/pre-open dispatch hours).")
        return True
    
    msg_text = (
        f"📡 <b>SELLING VEGA (IV CRUSH)</b> (<code>s:{signal_id}</code>)\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"<b>Ticker:</b> {candidate.ticker} | <b>Strategy:</b> {candidate.strategy}\n"
        f"<b>Spot Price:</b> ~${candidate.spot_price:.2f} | <b>Entry Date:</b> TODAY\n"
        f"<b>Earnings Date:</b> {candidate.announcement_date} (Exactly 1 trading day away)\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"⚠️ <b>MANUAL EXECUTION REQUIRED</b>\n"
        f"<i>(Alpaca Paper Trading restricts automated short naked options/spreads)</i>\n\n"
        f"⚙️ <b>EXECUTION BLUEPRINT</b>\n"
        f"• Structure: Iron Condor or Short Straddle\n"
        f"• Expiration: Nearest weekly expiration (closest to 3-7 DTE)\n"
        f"• Entry: Sell to Open just before market close today.\n"
        f"• Exit: Buy to Close the morning after earnings once IV crushes.\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"📝 <b>Log Entry:</b> <code>/trade s:{signal_id} [qty] core</code>"
    )

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
                logger.info(f"Dispatched Telegram alert for IV Crush signal s:{signal_id}")
                return True
    except Exception as e:
        logger.error(f"Telegram dispatch failed: {e}")
    return False


def run_scanner(dry_run: bool = False):
    logger.info(f"Starting IV Crush (Selling Vega) scanner for {ULTRA_LIQUID_TICKERS}")
    
    events = fetch_future_earnings(ULTRA_LIQUID_TICKERS, lookahead_days=7)
    today = datetime.now(timezone.utc).date()
    
    results = []
    
    for event in events:
        ticker = event.get("symbol")
        ann_date_str = event.get("date")
        if not ticker or not ann_date_str:
            continue
            
        try:
            ann_date = datetime.strptime(ann_date_str, "%Y-%m-%d").date()
        except ValueError:
            continue
            
        # Count trading days to earnings
        if ann_date <= today:
            continue
            
        trading_days_left = count_trading_days_between(today, ann_date)
        
        if trading_days_left == TARGET_ENTRY_DAYS_BEFORE:
            logger.info(f"🎯 Match: {ticker} earnings on {ann_date} is exactly {TARGET_ENTRY_DAYS_BEFORE} trading day away.")
            
            # Fetch current price via yf
            try:
                t = yf.Ticker(ticker)
                hist = t.history(period="1d")
                if not hist.empty:
                    spot = hist['Close'].iloc[-1]
                else:
                    spot = 0.0
            except Exception:
                spot = 0.0
                
            candidate = OptionSignalCandidate(
                ticker=ticker,
                strategy="IRON_CONDOR", # Default suggested strategy
                direction="NEUTRAL",
                spot_price=spot,
                strike=spot, 
                strike2=None,
                dte=7, # Short DTE for crush
                delta=0.0, 
                net_debit=0.0,
                max_gain=0.0,
                max_loss=0.0,
                prob_win=0.50,
                expected_value=0.0,
                engine_source="IV_CRUSH",
                iv_rank=0.90, # Proxy assumption that IV is maxed before earnings
                quality_score=0.99,
                reason=f"Selling Vega: Earnings 1 trading day away. Manual execution required.",
                announcement_date=ann_date_str
            )
            
            if not dry_run:
                signal_id = save_signal_to_db(candidate, suggested_qty=1)
                dispatch_iv_crush_telegram(candidate, signal_id)
                # NO ALPACA EXECUTION HERE due to short-selling restrictions.
            else:
                signal_id = 999
                logger.info(f"[DRY-RUN] Approved signal: {candidate.ticker} IV_CRUSH")
                
            results.append(candidate)
            
    logger.info(f"Scan completed. Generated IV Crush signals: {len(results)}")
    return results

if __name__ == "__main__":
    import sys
    dry_run_mode = "--dry-run" in sys.argv
    run_scanner(dry_run=dry_run_mode)
