"""
Telemetry Watchdog for NOX Sandbox Trading Copilot
Monitors open equity positions in `personal_trades` and sends Telegram alerts for:
- Take Profit (+20%)
- Stop Loss (-15%)

Equity prices come from real IBKR quotes (ibkr_quotes.py, read-only reqMktData
via IB Gateway). Option positions are skipped: personal_trades/personal_signals
never store strike/expiration/right, so a contract cannot be identified from a
trade row, and there is no live option-pricing source yet (RULE-D5 — an
unpriced position gets skipped, never a fabricated/mock price). Pre-earnings
alerting is not wired to a real earnings calendar yet, so it's disabled below
rather than left returning a hardcoded value for one ticker.
"""

import os
import sqlite3
import logging
import urllib.request
import json

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")
TELEGRAM_BOT_TOKEN = os.getenv("TELEGRAM_BOT_TOKEN", "")
TELEGRAM_CHAT_ID = os.getenv("TELEGRAM_CHAT_ID", "")

# Telemetry Thresholds
TAKE_PROFIT_MIN_PCT = 0.20
STOP_LOSS_MAX_PCT = -0.15

# Strategy names that mean "this is an option position" even if asset_class
# wasn't populated on the row (older rows may predate that column).
_OPTION_STRATEGIES = {
    "LONG_CALL", "LONG_PUT", "BULL_CALL_SPREAD", "BEAR_PUT_SPREAD",
    "STRADDLE", "STRANGLE", "IRON_CONDOR", "REVERSE_IRON_CONDOR", "CSP", "CC",
}

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("telemetry_watchdog")


def fetch_open_positions() -> list:
    """Fetches open personal trades from SQLite."""
    if not os.path.exists(DB_PATH):
        return []
    with sqlite3.connect(DB_PATH) as conn:
        cursor = conn.cursor()
        cursor.execute(
            "SELECT id, ticker, strategy, direction, quantity, price, executed_at, asset_class "
            "FROM personal_trades "
            "WHERE closes_trade_id IS NULL AND action IN ('OPEN', 'BUY')"
        )
        return cursor.fetchall()


def _is_option_position(asset_class: str | None, strategy: str | None) -> bool:
    if asset_class == "OPTION":
        return True
    return bool(strategy) and strategy.upper() in _OPTION_STRATEGIES


def get_current_prices(tickers: list[str]) -> dict[str, float]:
    """
    Live equity underlying prices via IB Gateway. Returns {} (or a partial
    dict) on any connection/quote failure rather than raising or defaulting
    a price — callers must treat a missing ticker as "skip this cycle."
    """
    try:
        import ibkr_quotes
    except ImportError:
        logger.warning("telemetry_watchdog: ibkr_quotes module unavailable — skipping price fetch")
        return {}
    return ibkr_quotes.fetch_equity_quotes(tickers)


def check_earnings_date(ticker: str) -> bool:
    """
    Not yet wired to a real earnings calendar. Always False rather than a
    fabricated value for specific tickers — an unimplemented check must be a
    no-op, not a guess (RULE-D5).
    """
    return False


def dispatch_alert(message: str):
    """Sends Telegram alert."""
    if not TELEGRAM_BOT_TOKEN or not TELEGRAM_CHAT_ID:
        logger.info(f"[DRY-RUN ALERT]:\n{message}")
        return

    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
    payload = json.dumps({
        "chat_id": TELEGRAM_CHAT_ID,
        "text": message,
        "parse_mode": "HTML"
    }).encode("utf-8")

    req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req)
    except Exception as e:
        logger.error(f"Telegram dispatch failed: {e}")


def run_watchdog():
    logger.info("Running Telemetry Watchdog...")
    open_positions = fetch_open_positions()
    if not open_positions:
        logger.info("No open positions to monitor.")
        return

    equity_positions = [p for p in open_positions if not _is_option_position(p[7], p[2])]
    skipped_options = len(open_positions) - len(equity_positions)
    if skipped_options:
        logger.info(
            f"Telemetry Watchdog: skipping {skipped_options} option position(s) — "
            f"no live option-pricing source yet (contract detail not stored)."
        )

    equity_tickers = sorted({p[1] for p in equity_positions})
    live_prices = get_current_prices(equity_tickers) if equity_tickers else {}

    for pos in equity_positions:
        trade_id, ticker, strategy, direction, qty, entry_price, exec_at, asset_class = pos
        if entry_price is None or entry_price <= 0:
            continue

        current_price = live_prices.get(ticker)
        if current_price is None:
            logger.info(f"Telemetry Watchdog: no live price for {ticker} (trade #{trade_id}) — skipping this cycle.")
            continue

        pnl_pct = (current_price - entry_price) / entry_price
        if direction in ("SHORT", "BEARISH"):
            pnl_pct = -pnl_pct

        alert_triggered = False
        msg = f"🚨 <b>NOX WATCHDOG ALERT</b> 🚨\nTrade <code>#{trade_id}</code>: {ticker} {strategy}\n"

        if pnl_pct >= TAKE_PROFIT_MIN_PCT:
            msg += f"🎯 <b>Take Profit Reached:</b> +{pnl_pct*100:.1f}%\n"
            alert_triggered = True
        elif pnl_pct <= STOP_LOSS_MAX_PCT:
            msg += f"🛑 <b>Stop Loss Hit:</b> {pnl_pct*100:.1f}%\n"
            alert_triggered = True

        if check_earnings_date(ticker):
            msg += f"⚠️ <b>Earnings Imminent:</b> Print expected within 3 days. Consider closing to avoid IV crush.\n"
            alert_triggered = True

        if alert_triggered:
            msg += f"\nEntry: ${entry_price:.2f} | Current: ${current_price:.2f}"
            msg += f"\nClose trade: <code>/close {trade_id} {current_price:.2f}</code>"
            dispatch_alert(msg)

    logger.info("Watchdog cycle complete.")


if __name__ == "__main__":
    run_watchdog()
