"""
Auto-detected fills from Interactive Brokers (IBKR) via Flex Web Service API.

Downloads execution reports / trade logs directly from IBKR's official Flex Web Service
using an API token and Flex Query ID.

Workflow:
1. Sends a SendRequest call to IBKR Flex Web Service with IBKR_FLEX_TOKEN and IBKR_FLEX_QUERY_ID.
2. Receives a ReferenceCode and statement download URL.
3. Polls/fetches GetStatement with ReferenceCode to get the execution XML.
4. Parses <Trade>, <TradeConfirm>, or <Execution> nodes into Nox's `imported_fills` schema.
5. Performs idempotent dedup on `source_transaction_id` in memory_bank.db.

Env Configuration:
- IBKR_FLEX_TOKEN: Flex Web Service token generated in IBKR Account Management.
- IBKR_FLEX_QUERY_ID: Flex Query ID configured for execution/trade reports.
- FILLS_IMPORTER_SOURCE: Set to 'ibkr' to enable scheduled polling.
- IBKR_POLL_INTERVAL_MINUTES: Polling frequency (default 30 min).
- IBKR_FILLS_LOOKBACK_DAYS: Lookback days for query (default 7 days).

Run manually or on schedule:
    python3 heartbeat/ibkr_fills_importer.py
"""

import os
import sqlite3
import sys
import time
import urllib.request
import urllib.parse
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
import logging

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("ibkr_fills_importer")

DB_PATH = os.getenv("MEMORY_BANK_PATH", "/app/data/memory_bank.db")
IBKR_FLEX_TOKEN = os.getenv("IBKR_FLEX_TOKEN", "").strip()
IBKR_FLEX_QUERY_ID = os.getenv("IBKR_FLEX_QUERY_ID", "").strip()
IBKR_FILLS_LOOKBACK_DAYS = int(os.getenv("IBKR_FILLS_LOOKBACK_DAYS", "7"))

SEND_REQUEST_URL = "https://www.interactivebrokers.com/Universal/servlet/FlexStatementService.SendRequest"
DEFAULT_GET_STATEMENT_URL = "https://www.interactivebrokers.com/Universal/servlet/FlexStatementService.GetStatement"

_ASSET_CLASS_MAP = {
    "STK": "EQUITY",
    "STOCK": "EQUITY",
    "EQUITY": "EQUITY",
    "OPT": "OPTION",
    "OPTION": "OPTION",
    "FUT": "FUTURES",
    "FUTURE": "FUTURES",
    "FUTURES": "FUTURES",
    "CASH": "FOREX",
    "CRYPTO": "CRYPTO",
}


def _format_date(raw_date_str: str) -> str:
    """Format YYYYMMDD, MM/DD/YYYY, or YYYY-MM-DD to YYYY-MM-DD."""
    if not raw_date_str:
        return datetime.now(timezone.utc).strftime("%Y-%m-%d")
    clean = raw_date_str.strip()
    if "/" in clean:
        parts = clean.split("/")
        if len(parts) == 3 and len(parts[2]) == 4:
            return f"{parts[2]}-{int(parts[0]):02d}-{int(parts[1]):02d}"
    clean = clean.replace("-", "")
    if len(clean) >= 8 and clean[:8].isdigit():
        return f"{clean[:4]}-{clean[4:6]}-{clean[6:8]}"
    return raw_date_str.strip()


def _format_time(raw_time_str: str) -> str:
    """Format HHMMSS or HH:MM:SS to HH:MM:SS."""
    if not raw_time_str:
        return "00:00:00"
    clean = raw_time_str.strip().replace(":", "")
    if ";" in clean:
        clean = clean.split(";")[1]
    if len(clean) >= 6 and clean[:6].isdigit():
        return f"{clean[:2]}:{clean[2:4]}:{clean[4:6]}"
    return raw_time_str.strip()


def request_flex_statement(token: str, query_id: str) -> tuple:
    """Initiates Flex Query request and returns tuple (ReferenceCode, statement_url)."""
    params = urllib.parse.urlencode({"t": token, "q": query_id, "v": "3"})
    url = f"{SEND_REQUEST_URL}?{params}"

    req = urllib.request.Request(url, headers={"User-Agent": "Nox/1.0 IBKRFlexImporter"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        xml_data = resp.read()

    root = ET.fromstring(xml_data)
    status = root.findtext("Status", default="").strip()

    if status.lower() != "success":
        error_code = root.findtext("ErrorCode", default="UNKNOWN")
        error_msg = root.findtext("ErrorMessage", default="No message")
        raise RuntimeError(f"IBKR Flex Request failed [Status={status}, ErrorCode={error_code}]: {error_msg}")

    ref_code = root.findtext("ReferenceCode", default="").strip()
    if not ref_code:
        raise ValueError("IBKR Flex Response missing ReferenceCode")
    
    statement_url = root.findtext("Url", default=DEFAULT_GET_STATEMENT_URL).strip()
    return ref_code, statement_url


def fetch_flex_statement(token: str, ref_code: str, statement_url: str = DEFAULT_GET_STATEMENT_URL, max_retries: int = 5, retry_delay: float = 3.0) -> bytes:
    """Polls statement_url until statement XML is ready."""
    params = urllib.parse.urlencode({"t": token, "q": ref_code, "v": "3"})
    url = f"{statement_url}?{params}" if statement_url else f"{DEFAULT_GET_STATEMENT_URL}?{params}"
    req = urllib.request.Request(url, headers={"User-Agent": "Nox/1.0 IBKRFlexImporter"})

    for attempt in range(1, max_retries + 1):
        with urllib.request.urlopen(req, timeout=30) as resp:
            xml_data = resp.read()

        try:
            root = ET.fromstring(xml_data)
            status = root.findtext("Status", default="")
            if status.lower() == "error":
                error_code = root.findtext("ErrorCode", default="")
                # Code 1019 = Statement generation in progress
                if error_code == "1019" and attempt < max_retries:
                    logger.info(f"IBKR Statement generating... waiting {retry_delay}s (attempt {attempt}/{max_retries})")
                    time.sleep(retry_delay)
                    continue
                error_msg = root.findtext("ErrorMessage", default="Unknown error")
                raise RuntimeError(f"IBKR Flex GetStatement error [{error_code}]: {error_msg}")
            # If root tag is FlexQueryResponse or FlexStatements, report is ready
            return xml_data
        except ET.ParseError:
            # If not XML or unexpected format
            return xml_data

    raise TimeoutError("Timed out waiting for IBKR Flex Statement generation")


def parse_ibkr_trades_xml(xml_content: bytes) -> list:
    """Parses IBKR Flex Query XML payload into a list of standardized fill dictionaries."""
    try:
        root = ET.fromstring(xml_content)
    except ET.ParseError as e:
        logger.error(f"Failed to parse IBKR Flex XML payload: {e}")
        return []

    fills = []
    # Find all potential trade nodes in the XML tree (<Trade>, <TradeConfirm>, <Execution>)
    for elem in root.iter():
        tag = elem.tag.split("}")[-1]  # strip namespace if any
        if tag not in ("Trade", "TradeConfirm", "Execution"):
            continue

        attr = elem.attrib
        tx_id = attr.get("transactionID") or attr.get("tradeID") or attr.get("executionID") or attr.get("confirmID")
        if not tx_id:
            continue

        symbol = (attr.get("symbol") or attr.get("underlyingSymbol") or "").upper().strip()
        if not symbol:
            continue

        raw_category = (attr.get("assetCategory") or attr.get("securityType") or "STK").upper().strip()
        asset_class = _ASSET_CLASS_MAP.get(raw_category, "EQUITY")

        raw_buysell = (attr.get("buySell") or attr.get("side") or "BUY").upper().strip()
        action = "BUY" if raw_buysell in ("BUY", "BOT", "B") else "SELL"

        open_close = (attr.get("openCloseIndicator") or attr.get("openClose") or "O").upper().strip()
        is_entry = 1 if open_close.startswith("O") else 0

        # Direction logic:
        # OPTION BUY -> LONG, OPTION SELL -> SHORT
        # EQUITY BUY -> LONG, EQUITY SELL -> (SHORT if is_entry else LONG)
        if asset_class == "OPTION":
            direction = "LONG" if action == "BUY" else "SHORT"
        else:
            direction = "SHORT" if (action == "SELL" and is_entry == 1) else "LONG"

        try:
            quantity = abs(float(attr.get("quantity") or attr.get("shares") or attr.get("qty") or 0))
            price = abs(float(attr.get("tradePrice") or attr.get("price") or 0))
            multiplier = 100.0 if asset_class == "OPTION" else 1.0
            amount = abs(float(attr.get("tradeMoney") or attr.get("netCash") or (quantity * price * multiplier)))
            fees = abs(float(attr.get("ibCommission") or attr.get("commission") or 0))
        except (ValueError, TypeError):
            logger.warning(f"Skipping trade {tx_id} due to invalid numeric fields")
            continue

        trade_date_str = _format_date(attr.get("tradeDate") or attr.get("reportDate") or attr.get("dateTime") or "")
        trade_time_str = _format_time(attr.get("tradeTime") or attr.get("dateTime") or "")

        source_tx_id = f"ibkr_trade_{tx_id}"

        fills.append({
            "source_transaction_id": source_tx_id,
            "source": "ibkr",
            "ticker": symbol,
            "action": action,
            "asset_class": asset_class,
            "quantity": quantity,
            "price": price,
            "amount": amount,
            "fees": fees,
            "trade_date": trade_date_str,
            "trade_time": trade_time_str,
            "direction": direction,
            "is_entry": is_entry,
        })

    return fills


def save_fills_to_db(db_path: str, fills: list) -> int:
    """Inserts fills into memory_bank.db idempotently based on source_transaction_id."""
    if not fills:
        return 0

    if not os.path.exists(os.path.dirname(db_path)) and os.path.dirname(db_path):
        os.makedirs(os.path.dirname(db_path), exist_ok=True)

    conn = sqlite3.connect(db_path)
    c = conn.cursor()

    c.execute("""
        CREATE TABLE IF NOT EXISTS imported_fills (
            id                       INTEGER PRIMARY KEY AUTOINCREMENT,
            imported_at              DATETIME DEFAULT CURRENT_TIMESTAMP,
            source_transaction_id    TEXT NOT NULL UNIQUE,
            source                   TEXT NOT NULL DEFAULT 'ibkr',
            ticker                   TEXT NOT NULL,
            action                   TEXT NOT NULL,
            asset_class              TEXT NOT NULL,
            quantity                 REAL,
            price                    REAL,
            amount                   REAL,
            fees                     REAL,
            trade_date               DATE,
            trade_time               TEXT,
            matched_signal_id        INTEGER,
            direction                TEXT,
            is_entry                 INTEGER DEFAULT 1
        )
    """)
    conn.commit()

    inserted_count = 0
    for f in fills:
        c.execute("SELECT 1 FROM imported_fills WHERE source_transaction_id = ?", (f["source_transaction_id"],))
        if c.fetchone() is not None:
            continue

        c.execute("""
            INSERT INTO imported_fills (
                source_transaction_id, source, ticker, action, asset_class,
                quantity, price, amount, fees, trade_date, trade_time,
                direction, is_entry
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            f["source_transaction_id"], f["source"], f["ticker"], f["action"], f["asset_class"],
            f["quantity"], f["price"], f["amount"], f["fees"], f["trade_date"], f["trade_time"],
            f["direction"], f["is_entry"]
        ))
        inserted_count += 1

    conn.commit()
    conn.close()
    return inserted_count


def poll_ibkr_fills() -> int:
    """Main polling entrypoint used by monitor.py and manual execution."""
    token = os.getenv("IBKR_FLEX_TOKEN", "").strip()
    query_id = os.getenv("IBKR_FLEX_QUERY_ID", "").strip()

    if not token or not query_id:
        logger.info("IBKR fills importer skipped: IBKR_FLEX_TOKEN or IBKR_FLEX_QUERY_ID not set")
        return 0

    logger.info(f"Polling IBKR Flex Query {query_id}...")
    try:
        ref_code, statement_url = request_flex_statement(token, query_id)
        xml_data = fetch_flex_statement(token, ref_code, statement_url=statement_url)
        fills = parse_ibkr_trades_xml(xml_data)
        new_count = save_fills_to_db(DB_PATH, fills)
        logger.info(f"IBKR fills poll complete: {len(fills)} trades parsed, {new_count} new fills saved to DB.")
        return new_count
    except Exception as e:
        logger.error(f"IBKR fills poll failed: {e}", exc_info=True)
        raise


if __name__ == "__main__":
    count = poll_ibkr_fills()
    print(f"Done. Imported {count} new IBKR fills.")
