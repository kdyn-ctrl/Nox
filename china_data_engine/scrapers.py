import requests
import akshare as ak
from typing import Dict, Any, List

# RULE-008: All HTTP calls use a (connect_timeout, read_timeout) tuple.
# A scalar timeout=10 only sets the read timeout — the connection can still
# block indefinitely. The tuple form enforces both independently.
HTTP_TIMEOUT = (5, 10)


def fetch_eastmoney_hot_board() -> List[Dict[str, Any]]:
    """
    Pulls the top 10 most-watched retail sentiment stocks on the A-share market
    via East Money (东方财富人气榜).

    Uses stock_hot_rank_latest_em as the primary source — it is the real-time
    snapshot endpoint and is more reliably available than stock_hot_rank_em,
    which polls a heavier historical feed that intermittently returns empty JSON.

    Returns a list of dicts with normalised English keys. Empty list on failure.
    """
    # Two candidate functions, tried in order. stock_hot_rank_latest_em is the
    # lightweight real-time endpoint; stock_hot_rank_em is the heavier fallback.
    CANDIDATES = [
        ("stock_hot_rank_latest_em", ak.stock_hot_rank_latest_em),
        ("stock_hot_rank_em",        ak.stock_hot_rank_em),
    ]
    for func_name, func in CANDIDATES:
        try:
            df = func()
            if df is None or df.empty:
                print(f"[WARN] [SCRAPER] {func_name} returned empty dataframe, trying next.", flush=True)
                continue

            print(f"[INFO] [SCRAPER] East Money hot board — using {func_name}. "
                  f"Columns: {df.columns.tolist()}", flush=True)

            top = df.head(10)
            result = []
            for _, row in top.iterrows():
                # Gracefully resolve column names — different endpoints use
                # slightly different Chinese headers for the same fields.
                rank   = row.get('排名')   or row.get('序号')   or 0
                ticker = row.get('代码')   or row.get('股票代码') or ''
                name   = row.get('股票名称') or row.get('名称')   or ''
                price  = row.get('最新价')  or row.get('现价')   or 0.0
                chg    = row.get('涨跌幅')  or row.get('涨跌额')  or 0.0
                turn   = row.get('换手率')  or 0.0
                result.append({
                    "rank":          int(rank),
                    "ticker":        str(ticker),
                    "name":          str(name),
                    "price":         float(price),
                    "change_pct":    float(chg),
                    "turnover_rate": float(turn),
                })
            print(f"[INFO] [SCRAPER] East Money hot board fetched ({len(result)} rows).", flush=True)
            return result
        except Exception as e:
            print(f"[WARN] [SCRAPER] {func_name} failed: {e}. Trying next candidate.", flush=True)
            continue

    print("[ERROR] [SCRAPER] All East Money hot board candidates failed.", flush=True)
    return []


def fetch_china_pmi() -> Dict[str, Any]:
    """
    Pulls China NBS Manufacturing & Non-Manufacturing PMI (国家统计局 制造业PMI).

    PMI > 50 = expansion, < 50 = contraction.
    The single most-watched leading indicator for Chinese industrial output
    and a direct input to global supply chain models.

    Under modern akshare (v1.18+), these are retrieved from two separate endpoints
    representing the official Manufacturing and Non-Manufacturing series.

    Returns a dict with normalised English keys. Empty dict on failure.
    """
    result = {
        "month":                  "N/A",
        "manufacturing":          0.0,
        "manufacturing_yoy":      0.0, # Retained for schema backwards compatibility
        "non_manufacturing":      0.0,
        "non_manufacturing_yoy":  0.0  # Retained for schema backwards compatibility
    }

    # 1. Fetch Official Manufacturing PMI
    try:
        df_mfg = ak.macro_china_pmi_yearly()
        if not df_mfg.empty:
            latest = df_mfg.iloc[-1]
            mfg_val = latest.get('今值')
            mfg_date = latest.get('日期')
            if mfg_val is not None:
                result["manufacturing"] = float(mfg_val)
            if mfg_date is not None:
                # Store the date string (e.g. "2025-08-31") as the month/reference date
                result["month"] = str(mfg_date)
            print(f"[INFO] [SCRAPER] Manufacturing PMI fetched ({mfg_val}) for {mfg_date}.", flush=True)
    except Exception as e:
        print(f"[ERROR] [SCRAPER] China Manufacturing PMI fetch failed: {e}", flush=True)

    # 2. Fetch Official Non-Manufacturing PMI
    try:
        df_non_mfg = ak.macro_china_non_man_pmi()
        if not df_non_mfg.empty:
            latest = df_non_mfg.iloc[-1]
            non_mfg_val = latest.get('今值')
            if non_mfg_val is not None:
                result["non_manufacturing"] = float(non_mfg_val)
            print(f"[INFO] [SCRAPER] Non-Manufacturing PMI fetched ({non_mfg_val}).", flush=True)
    except Exception as e:
        print(f"[ERROR] [SCRAPER] China Non-Manufacturing PMI fetch failed: {e}", flush=True)

    # Return results if we succeeded in getting at least one of the indicators
    if result["manufacturing"] > 0.0 or result["non_manufacturing"] > 0.0:
        return result
    return {}


def fetch_pboc_lpr() -> Dict[str, Any]:
    """
    Pulls the latest PBOC Loan Prime Rate — 1-Year and 5-Year (贷款市场报价利率).

    Rate cuts = stimulus signal; holds or hikes = tightening.
    A divergence between PBOC and Fed policy is a direct USD/CNY pressure signal
    and affects the pricing of cross-listed stocks (H-shares and ADRs).

    Returns a dict with normalised English keys. Empty dict on failure.
    """
    try:
        df = ak.macro_china_lpr()
        if df.empty:
            print("[WARN] [SCRAPER] PBOC LPR dataframe was empty.", flush=True)
            return {}
        latest = df.iloc[-1]
        result = {
            "date":   str(latest.iloc[0]),
            "lpr_1y": float(latest.iloc[1]),
            "lpr_5y": float(latest.iloc[2]),
        }
        print(f"[INFO] [SCRAPER] PBOC LPR fetched for {result['date']}.", flush=True)
        return result
    except Exception as e:
        print(f"[ERROR] [SCRAPER] PBOC LPR fetch failed: {e}", flush=True)
        return {}


# Tickers to query for Chinese market news via East Money's news feed.
# These are a mix of major Chinese ADRs and cross-listed names that generate
# the highest volume of 财联社-sourced headlines on the East Money platform.
_CN_NEWS_TICKERS = ["BABA", "JD", "PDD", "BIDU", "NIO"]


def fetch_cailian_news() -> List[Dict[str, str]]:
    """
    Pulls Chinese financial market headlines via East Money's stock news feed
    (stock_news_em), replacing the defunct stock_telegraph_cls endpoint.

    Why stock_news_em?
      • stock_telegraph_cls / stock_cls_alerts_cls / stock_info_global_cls were
        all removed or made unreliable in akshare >= 1.10. stock_info_global_cls
        hangs indefinitely — confirmed in production.
      • stock_news_em is confirmed working in v1.18.64 and its articles are
        predominantly sourced from 财联社 (Cailian) and 财中社, making it a
        direct functional replacement for the old telegraph feed.
      • It accepts a symbol parameter, so we query a basket of Chinese ADRs
        and deduplicate by headline to avoid repetition.

    Returns a list of up to 10 dicts with normalised English keys.
    Empty list on total failure.
    """
    seen_titles: set = set()
    result: List[Dict[str, str]] = []

    for ticker in _CN_NEWS_TICKERS:
        if len(result) >= 10:
            break
        try:
            df = ak.stock_news_em(symbol=ticker)
            if df is None or df.empty:
                print(f"[WARN] [SCRAPER] stock_news_em returned empty for {ticker}.", flush=True)
                continue

            for _, row in df.head(5).iterrows():
                title = str(row.get('新闻标题', '')).strip()
                if not title or title in seen_titles:
                    continue
                seen_titles.add(title)
                result.append({
                    "time":    str(row.get('发布时间', '')),
                    "title":   title,
                    "content": str(row.get('新闻内容', '')).strip(),
                    "source":  str(row.get('文章来源', '')),
                    "url":     str(row.get('新闻链接', '')),
                })

        except Exception as e:
            print(f"[WARN] [SCRAPER] stock_news_em failed for {ticker}: {e}", flush=True)
            continue

    if result:
        print(f"[INFO] [SCRAPER] Cailian/EM news fetched ({len(result)} headlines "
              f"across {len(_CN_NEWS_TICKERS)} tickers).", flush=True)
    else:
        print("[ERROR] [SCRAPER] All Cailian/EM news candidates failed.", flush=True)
    return result