"""
WS7 — EDGAR → Chinese Retail Media Lag: cache-check helper

Pure, stateless function that inspects the china-data-engine's in-memory
cache to determine whether a US ADR ticker has been picked up by Chinese
retail media. Designed to be called from the /lag/check FastAPI endpoint.

Window tracking (SQLite) and Telegram alerts live in heartbeat, which calls
this service via HTTP. This keeps china-data-engine fully stateless.
"""

from typing import Any, Dict, List

# Chinese name aliases for US ADRs — Cailian headlines use Chinese names;
# East Money A-share hot board shows Chinese stock codes (not US ADR symbols)
# so we match on company name substrings instead.
# Illustrative example only — the full curated ADR watchlist is kept private.
_ADR_CN_NAMES: Dict[str, List[str]] = {
    "BABA": ["阿里巴巴", "阿里", "alibaba"],
}


def check_ticker_in_cn_media(
    ticker: str,
    hot_board: List[Dict[str, Any]],
    news_cn: List[Dict[str, Any]],
) -> Dict[str, Any]:
    """
    Check whether a US ADR ticker appears in the cached Chinese retail media.

    hot_board entries: {rank, ticker (A-share code), name (Chinese)}
    news_cn entries:   {title, content, source}

    Returns:
        {
            "is_on_hot_board": bool,
            "is_in_cailian":   bool,
            "lag_open":        bool,   # True = neither source covers it yet
        }
    """
    ticker_upper = ticker.upper()
    aliases = _ADR_CN_NAMES.get(ticker_upper, [ticker.lower()])

    # Hot board uses Chinese company names — match on name field, not A-share code
    is_on_hot_board = any(
        any(alias in str(entry.get("name", "")).lower() for alias in aliases)
        for entry in hot_board
    )

    cailian_corpus = " ".join(
        f"{item.get('title', '')} {item.get('content', '')}".lower()
        for item in news_cn
    )
    is_in_cailian = any(alias in cailian_corpus for alias in aliases)

    return {
        "is_on_hot_board": is_on_hot_board,
        "is_in_cailian":   is_in_cailian,
        "lag_open":        not (is_on_hot_board or is_in_cailian),
    }
