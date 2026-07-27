import os
import time
import requests
import logging
from typing import Dict, Any, Optional

logger = logging.getLogger(__name__)

ALPACA_API_KEY = os.getenv("ALPACA_API_KEY", "")
ALPACA_SECRET_KEY = os.getenv("ALPACA_SECRET_KEY", "")
ALPACA_TRADING_URL = "https://paper-api.alpaca.markets"

def get_alpaca_headers() -> Dict[str, str]:
    return {
        "APCA-API-KEY-ID": ALPACA_API_KEY,
        "APCA-API-SECRET-KEY": ALPACA_SECRET_KEY,
        "accept": "application/json"
    }

def execute_alpaca_option_order(contract_symbol: str, action: str, qty: int, limit_price: float,
                                strategy_name: str, take_profit_mult: float = 1.3) -> bool:
    """
    Submits a limit order to Alpaca Paper Trading for an options contract.
    If it's a BUY order, attempts to submit a STC Take Profit order if supported,
    or relies on manual/external TP management if brackets aren't supported.
    """
    if not ALPACA_API_KEY:
        logger.warning(f"[{strategy_name}] Cannot execute {contract_symbol} - Missing Alpaca Credentials")
        return False

    url = f"{ALPACA_TRADING_URL}/v2/orders"
    
    # Place the Buy to Open order
    payload = {
        "symbol": contract_symbol,
        "qty": str(qty),
        "side": action.lower(),
        "type": "limit",
        "time_in_force": "day",
        "limit_price": str(round(limit_price, 2)),
        "extended_hours": False
    }

    try:
        resp = requests.post(url, json=payload, headers=get_alpaca_headers(), timeout=10)
        
        if resp.status_code in [200, 201]:
            order_data = resp.json()
            order_id = order_data.get("id")
            logger.info(f"[{strategy_name}] SUCCESS: Placed {action} order for {qty}x {contract_symbol} at ${limit_price} (Order ID: {order_id})")
            
            # Note on Options Brackets: Alpaca Paper doesn't natively support advanced OCO/Brackets
            # for options in the same way as equities yet on standard tier.
            # We log the intended Take Profit so the user is aware.
            if action.lower() == "buy" and take_profit_mult:
                tp_price = round(limit_price * take_profit_mult, 2)
                logger.info(f"[{strategy_name}] ACTION REQUIRED: Position opened. Manually place GTC Limit Sell at ${tp_price} (+{(take_profit_mult-1)*100:.0f}%) if bracket failed.")
            return True
        else:
            logger.error(f"[{strategy_name}] Alpaca Order Rejected: {resp.status_code} - {resp.text}")
            return False
            
    except Exception as e:
        logger.error(f"[{strategy_name}] Error communicating with Alpaca: {e}", exc_info=True)
        return False
