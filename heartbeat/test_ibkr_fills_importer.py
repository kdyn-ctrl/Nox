import os
import sqlite3
import pytest
import xml.etree.ElementTree as ET
from heartbeat.ibkr_fills_importer import (
    _format_date,
    _format_time,
    parse_ibkr_trades_xml,
    save_fills_to_db,
)


def test_date_and_time_formatters():
    assert _format_date("20260726") == "2026-07-26"
    assert _format_date("2026-07-26") == "2026-07-26"
    assert _format_time("143000") == "14:30:00"
    assert _format_time("14:30:00") == "14:30:00"


def test_parse_ibkr_trades_xml_equity_and_option():
    sample_xml = b"""<?xml version="1.0" encoding="UTF-8"?>
<FlexQueryResponse queryId="1583989" type="AF">
    <FlexStatements count="1">
        <FlexStatement accountId="U1234567" fromDate="20260720" toDate="20260726">
            <Trades>
                <Trade accountId="U1234567" currency="USD" assetCategory="OPT" symbol="SPY" description="SPY 21AUG26 500 C" buySell="BUY" quantity="2" tradePrice="5.25" tradeMoney="1050" ibCommission="-2.00" tradeDate="20260726" tradeTime="143000" transactionID="TX1001" openCloseIndicator="O" />
                <Trade accountId="U1234567" currency="USD" assetCategory="STK" symbol="AAPL" description="APPLE INC" buySell="SELL" quantity="10" tradePrice="220.50" tradeMoney="2205" ibCommission="-1.00" tradeDate="20260726" tradeTime="151200" transactionID="TX1002" openCloseIndicator="C" />
            </Trades>
        </FlexStatement>
    </FlexStatements>
</FlexQueryResponse>
"""
    fills = parse_ibkr_trades_xml(sample_xml)
    assert len(fills) == 2

    fill_opt = fills[0]
    assert fill_opt["source_transaction_id"] == "ibkr_trade_TX1001"
    assert fill_opt["ticker"] == "SPY"
    assert fill_opt["action"] == "BUY"
    assert fill_opt["asset_class"] == "OPTION"
    assert fill_opt["quantity"] == 2.0
    assert fill_opt["price"] == 5.25
    assert fill_opt["amount"] == 1050.0
    assert fill_opt["fees"] == 2.0
    assert fill_opt["trade_date"] == "2026-07-26"
    assert fill_opt["trade_time"] == "14:30:00"
    assert fill_opt["direction"] == "LONG"
    assert fill_opt["is_entry"] == 1

    fill_stk = fills[1]
    assert fill_stk["source_transaction_id"] == "ibkr_trade_TX1002"
    assert fill_stk["ticker"] == "AAPL"
    assert fill_stk["action"] == "SELL"
    assert fill_stk["asset_class"] == "EQUITY"
    assert fill_stk["quantity"] == 10.0
    assert fill_stk["price"] == 220.50
    assert fill_stk["direction"] == "LONG"
    assert fill_stk["is_entry"] == 0


def test_save_fills_to_db_idempotency(tmp_path):
    db_file = str(tmp_path / "test_memory_bank.db")

    sample_fills = [
        {
            "source_transaction_id": "ibkr_trade_TX999",
            "source": "ibkr",
            "ticker": "QQQ",
            "action": "BUY",
            "asset_class": "EQUITY",
            "quantity": 5.0,
            "price": 480.0,
            "amount": 2400.0,
            "fees": 1.5,
            "trade_date": "2026-07-26",
            "trade_time": "10:00:00",
            "direction": "LONG",
            "is_entry": 1,
        }
    ]

    first_insert = save_fills_to_db(db_file, sample_fills)
    assert first_insert == 1

    second_insert = save_fills_to_db(db_file, sample_fills)
    assert second_insert == 0

    conn = sqlite3.connect(db_file)
    c = conn.cursor()
    c.execute("SELECT ticker, action, quantity, price FROM imported_fills WHERE source_transaction_id = 'ibkr_trade_TX999'")
    row = c.fetchone()
    conn.close()

    assert row is not None
    assert row[0] == "QQQ"
    assert row[1] == "BUY"
    assert row[2] == 5.0
    assert row[3] == 480.0
