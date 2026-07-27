import os
import requests
import akshare as ak
from datetime import datetime
from typing import Dict, Any, List

from retry_utils import call_with_retry

# The official NBS PMI series akshare exposes (macro_china_pmi_yearly /
# macro_china_non_man_pmi) is scraped from a Sina Finance page that has been
# observed to stop updating for months at a time while still returning HTTP
# 200 with a non-empty dataframe — so a plain fetch failure check doesn't catch
# it. If the newest row is older than this many days, treat it as stale and
# fall back to the Caixin (财新) PMI series, which tracks the live NBS release
# schedule closely and has been confirmed current in practice.
PMI_STALENESS_DAYS = int(os.getenv("PMI_STALENESS_DAYS", "45"))

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
        df = call_with_retry(func, source=f"EastMoney hot board:{func_name}",
                              is_failure=lambda d: d is None or d.empty,
                              timeout_seconds=20)
        if df is None:
            print(f"[WARN] [SCRAPER] {func_name} returned empty dataframe after retries, trying next.", flush=True)
            continue
        try:
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
                if not ticker and not name:
                    # Every expected column name missed (akshare schema drift) —
                    # this isn't a real row, it's an all-defaults placeholder.
                    # Skip it rather than rendering a zero-filled fake entry.
                    continue
                result.append({
                    "rank":          int(rank),
                    "ticker":        str(ticker),
                    "name":          str(name),
                    "price":         float(price),
                    "change_pct":    float(chg),
                    "turnover_rate": float(turn),
                })
            if not result:
                print(f"[WARN] [SCRAPER] {func_name} returned rows but none had a resolvable "
                      f"ticker/name (schema drift?), trying next.", flush=True)
                continue
            print(f"[INFO] [SCRAPER] East Money hot board fetched ({len(result)} rows).", flush=True)
            return result
        except Exception as e:
            print(f"[WARN] [SCRAPER] {func_name} failed: {e}. Trying next candidate.", flush=True)
            continue

    print("[ERROR] [SCRAPER] All East Money hot board candidates failed.", flush=True)
    return []


def _days_stale(date_str: str) -> float:
    """Returns days between date_str (YYYY-MM-DD) and now, or inf if unparsable."""
    try:
        return (datetime.utcnow() - datetime.strptime(date_str, "%Y-%m-%d")).days
    except (ValueError, TypeError):
        return float("inf")


def _fetch_caixin_pmi() -> Dict[str, Any]:
    """
    Fallback source: Caixin (财新) Manufacturing/Services/Composite PMI.
    A privately-compiled index (distinct methodology from the official NBS
    survey) but tracks the same monthly release cadence and, unlike the
    akshare NBS endpoints, has been confirmed to stay current.
    """
    # NOTE: manufacturing/non_manufacturing start as None, not 0.0 — a failed
    # parse must stay indistinguishable-from-missing, never render as a real
    # (impossible) 0.0 PMI reading downstream.
    result = {
        "month":                  "N/A",
        "manufacturing":          None,
        "manufacturing_yoy":      0.0,
        "non_manufacturing":      None,
        "non_manufacturing_yoy":  0.0,
        "source":                 "Caixin",
    }

    df_mfg = call_with_retry(ak.index_pmi_man_cx, source="Caixin Manufacturing PMI",
                              is_failure=lambda d: d is None or d.empty,
                              timeout_seconds=20)
    if df_mfg is not None:
        try:
            latest = df_mfg.iloc[-1]
            mfg_val, mfg_date = latest.get('制造业PMI'), latest.get('日期')
            if mfg_val is not None:
                result["manufacturing"] = float(mfg_val)
            if mfg_date is not None:
                result["month"] = str(mfg_date)
            print(f"[INFO] [SCRAPER] Caixin Manufacturing PMI fetched ({mfg_val}) for {mfg_date}.", flush=True)
        except Exception as e:
            print(f"[ERROR] [SCRAPER] Caixin Manufacturing PMI parse failed: {e}", flush=True)

    df_com = call_with_retry(ak.index_pmi_com_cx, source="Caixin Composite PMI",
                              is_failure=lambda d: d is None or d.empty,
                              timeout_seconds=20)
    if df_com is not None:
        try:
            latest = df_com.iloc[-1]
            com_val = latest.get('综合PMI')
            if com_val is not None:
                # Composite is the closest Caixin analogue to the official
                # non-manufacturing print (Caixin has no standalone
                # non-manufacturing series) — labelled accordingly downstream.
                result["non_manufacturing"] = float(com_val)
            print(f"[INFO] [SCRAPER] Caixin Composite PMI fetched ({com_val}).", flush=True)
        except Exception as e:
            print(f"[ERROR] [SCRAPER] Caixin Composite PMI parse failed: {e}", flush=True)

    if (result["manufacturing"] is not None or result["non_manufacturing"] is not None) \
            and _days_stale(result["month"]) <= PMI_STALENESS_DAYS:
        return result
    return {}


def fetch_china_pmi() -> Dict[str, Any]:
    """
    Pulls China Manufacturing & Non-Manufacturing PMI.

    PMI > 50 = expansion, < 50 = contraction.
    The single most-watched leading indicator for Chinese industrial output
    and a direct input to global supply chain models.

    Tries the official NBS series first (via akshare's macro_china_pmi_yearly /
    macro_china_non_man_pmi). That series is scraped from a Sina Finance page
    which has been observed to silently stop updating (still returns a
    non-empty dataframe, just with no new rows) for months at a time. If the
    newest official row is older than PMI_STALENESS_DAYS, falls back to the
    Caixin PMI series instead of silently serving a stale official print.

    Returns a dict with normalised English keys and a "source" field
    ("NBS" or "Caixin"). Empty dict on total failure.
    """
    # NOTE: manufacturing/non_manufacturing start as None, not 0.0 — a failed
    # parse must stay indistinguishable-from-missing, never render as a real
    # (impossible) 0.0 PMI reading downstream. See _fetch_caixin_pmi for the
    # same pattern (RULE-D6: fix the class).
    result = {
        "month":                  "N/A",
        "manufacturing":          None,
        "manufacturing_yoy":      0.0, # Retained for schema backwards compatibility
        "non_manufacturing":      None,
        "non_manufacturing_yoy":  0.0, # Retained for schema backwards compatibility
        "source":                 "NBS",
    }

    # 1. Fetch Official Manufacturing PMI
    df_mfg = call_with_retry(ak.macro_china_pmi_yearly, source="China Manufacturing PMI",
                              is_failure=lambda d: d is None or d.empty,
                              timeout_seconds=20)
    if df_mfg is not None:
        try:
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
            print(f"[ERROR] [SCRAPER] China Manufacturing PMI parse failed: {e}", flush=True)

    # 2. Fetch Official Non-Manufacturing PMI
    df_non_mfg = call_with_retry(ak.macro_china_non_man_pmi, source="China Non-Manufacturing PMI",
                                  is_failure=lambda d: d is None or d.empty,
                                  timeout_seconds=20)
    if df_non_mfg is not None:
        try:
            latest = df_non_mfg.iloc[-1]
            non_mfg_val = latest.get('今值')
            if non_mfg_val is not None:
                result["non_manufacturing"] = float(non_mfg_val)
            print(f"[INFO] [SCRAPER] Non-Manufacturing PMI fetched ({non_mfg_val}).", flush=True)
        except Exception as e:
            print(f"[ERROR] [SCRAPER] China Non-Manufacturing PMI parse failed: {e}", flush=True)

    # If the official series is too stale (source went dark but keeps
    # returning old rows with HTTP 200), fall back to Caixin instead of
    # silently re-serving the same old official print.
    nbs_stale = result["manufacturing"] is not None and _days_stale(result["month"]) > PMI_STALENESS_DAYS
    if nbs_stale:
        print(f"[WARN] [SCRAPER] Official NBS PMI stale ({result['month']}, "
              f">{PMI_STALENESS_DAYS}d old) — falling back to Caixin PMI.", flush=True)
        caixin = _fetch_caixin_pmi()
        if caixin:
            return caixin
        print("[ERROR] [SCRAPER] Caixin fallback also stale or unavailable — "
              "refusing to re-serve the stale NBS print.", flush=True)
        return {}

    # Return results if we succeeded in getting at least one of the indicators.
    # A partial failure (e.g. manufacturing parsed, non_manufacturing didn't)
    # still returns the dict, but the failed field stays None so the consumer
    # renders "N/A" for it instead of a fake 0.0 reading.
    if result["manufacturing"] is not None or result["non_manufacturing"] is not None:
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
    df = call_with_retry(ak.macro_china_lpr, source="PBOC LPR", is_failure=lambda d: d is None or d.empty,
                          timeout_seconds=20)
    if df is None:
        print("[ERROR] [SCRAPER] PBOC LPR fetch failed after retries.", flush=True)
        return {}
    try:
        latest = df.iloc[-1]
        result = {
            "date":   str(latest.iloc[0]),
            "lpr_1y": float(latest.iloc[1]),
            "lpr_5y": float(latest.iloc[2]),
        }
        print(f"[INFO] [SCRAPER] PBOC LPR fetched for {result['date']}.", flush=True)
        return result
    except Exception as e:
        print(f"[ERROR] [SCRAPER] PBOC LPR parse failed: {e}", flush=True)
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
        df = call_with_retry(lambda t=ticker: ak.stock_news_em(symbol=t), source=f"stock_news_em:{ticker}",
                              is_failure=lambda d: d is None or d.empty,
                              timeout_seconds=20)
        if df is None:
            print(f"[WARN] [SCRAPER] stock_news_em returned empty for {ticker} after retries.", flush=True)
            continue
        try:
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
            print(f"[WARN] [SCRAPER] stock_news_em parse failed for {ticker}: {e}", flush=True)
            continue

    if result:
        print(f"[INFO] [SCRAPER] Cailian/EM news fetched ({len(result)} headlines "
              f"across {len(_CN_NEWS_TICKERS)} tickers).", flush=True)
    else:
        print("[ERROR] [SCRAPER] All Cailian/EM news candidates failed.", flush=True)
    return result