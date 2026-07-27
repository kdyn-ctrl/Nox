"""
Read-only IBKR market-data quote fetcher.

Connects to IB Gateway/TWS (reqMktData via ib_async, never places an order)
to fetch live equity underlying prices for telemetry_watchdog.py. Options are
explicitly out of scope here: personal_trades/personal_signals never store
strike/expiration/right, so an option contract cannot be identified from a
trade row alone — callers must not fall back to a mock/estimated premium for
those (RULE-D5: an invented number gets no alerting power).

Env:
- IBKR_QUOTES_HOST (default "ib-gateway", the docker-compose service name)
- IBKR_QUOTES_PORT (default 4004 — gnzsnz/ib-gateway's socat-republished
  paper port, reachable container-to-container; NOT the raw 4002 the image
  binds to 127.0.0.1 internally. 4003 is the equivalent live port.)
- IBKR_QUOTES_CLIENT_ID (default 7 — must not collide with any other IB API
  client connecting to the same gateway)
"""

import logging
import os

logger = logging.getLogger("ibkr_quotes")

IBKR_QUOTES_HOST = os.getenv("IBKR_QUOTES_HOST", "ib-gateway").strip()
IBKR_QUOTES_PORT = int(os.getenv("IBKR_QUOTES_PORT", "4004"))
IBKR_QUOTES_CLIENT_ID = int(os.getenv("IBKR_QUOTES_CLIENT_ID", "7"))
CONNECT_TIMEOUT_SECONDS = 10.0


def fetch_equity_quotes(tickers: list[str]) -> dict[str, float]:
    """
    Fetch live market prices for equity tickers via IB Gateway.

    Returns {ticker: price} only for tickers that resolved to a real quote.
    A ticker absent from the result means "no live price available" — the
    gateway may be unreachable, the contract may not qualify, or there may be
    no active market-data subscription for it. Callers must skip that
    position's check rather than substitute a placeholder price.
    """
    if not tickers:
        return {}

    try:
        from ib_async import IB, Stock
    except ImportError:
        logger.warning("ibkr_quotes: ib_async not installed — skipping quote fetch")
        return {}

    ib = IB()
    try:
        ib.connect(
            IBKR_QUOTES_HOST,
            IBKR_QUOTES_PORT,
            clientId=IBKR_QUOTES_CLIENT_ID,
            timeout=CONNECT_TIMEOUT_SECONDS,
            readonly=True,
        )
    except Exception as e:
        logger.warning(f"ibkr_quotes: could not connect to {IBKR_QUOTES_HOST}:{IBKR_QUOTES_PORT}: {e}")
        return {}

    prices: dict[str, float] = {}
    try:
        contracts = [Stock(t, "SMART", "USD") for t in tickers]
        qualified = ib.qualifyContracts(*contracts)
        if not qualified:
            logger.warning(f"ibkr_quotes: no contracts qualified for {tickers}")
            return {}

        live_tickers = ib.reqTickers(*qualified)
        for t in live_tickers:
            price = t.marketPrice()
            if price and price == price:  # excludes NaN ("no data")
                prices[t.contract.symbol] = float(price)
            else:
                logger.info(f"ibkr_quotes: no live price for {t.contract.symbol} (unsubscribed or no data)")
    except Exception as e:
        logger.warning(f"ibkr_quotes: quote fetch failed: {e}")
    finally:
        try:
            ib.disconnect()
        except Exception:
            pass

    return prices
