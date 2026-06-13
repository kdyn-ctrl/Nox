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

    High retail attention in China often leads institutional flows by 1–2 sessions,
    making this a useful leading sentiment indicator for cross-market analysis.

    Returns a list of dicts with normalised English keys. Empty list on failure.
    """
    try:
        df = ak.stock_hot_rank_em()
        # Columns: 排名, 代码, 股票名称, 最新价, 涨跌幅, 换手率
        top = df.head(10)
        result = []
        for _, row in top.iterrows():
            result.append({
                "rank":          int(row.get('排名', 0)),
                "ticker":        str(row.get('代码', '')),
                "name":          str(row.get('股票名称', '')),
                "price":         float(row.get('最新价', 0.0))   if row.get('最新价')   is not None else 0.0,
                "change_pct":    float(row.get('涨跌幅', 0.0))   if row.get('涨跌幅')   is not None else 0.0,
                "turnover_rate": float(row.get('换手率', 0.0))   if row.get('换手率')   is not None else 0.0,
            })
        print(f"[INFO] [SCRAPER] East Money hot board fetched ({len(result)} rows).", flush=True)
        return result
    except Exception as e:
        print(f"[ERROR] [SCRAPER] East Money hot board failed: {e}", flush=True)
        return []


def fetch_china_pmi() -> Dict[str, Any]:
    """
    Pulls China NBS Manufacturing & Non-Manufacturing PMI (国家统计局 制造业PMI).

    PMI > 50 = expansion, < 50 = contraction.
    The single most-watched leading indicator for Chinese industrial output
    and a direct input to global supply chain models.

    Returns a dict with normalised English keys. Empty dict on failure.
    """
    try:
        df = ak.macro_china_pmi_yearly()
        # Columns: 月份, 制造业-指数, 制造业-同比增长, 非制造业-指数, 非制造业-同比增长
        if df.empty:
            print("[WARN] [SCRAPER] China PMI dataframe was empty.", flush=True)
            return {}
        latest = df.iloc[-1]
        result = {
            "month":                  str(latest.get('月份', 'N/A')),
            "manufacturing":          float(latest.get('制造业-指数', 0.0)),
            "manufacturing_yoy":      float(latest.get('制造业-同比增长', 0.0)),
            "non_manufacturing":      float(latest.get('非制造业-指数', 0.0)),
            "non_manufacturing_yoy":  float(latest.get('非制造业-同比增长', 0.0)),
        }
        print(f"[INFO] [SCRAPER] China PMI fetched for {result['month']}.", flush=True)
        return result
    except Exception as e:
        print(f"[ERROR] [SCRAPER] China PMI fetch failed: {e}", flush=True)
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


def fetch_cailian_news() -> List[Dict[str, str]]:
    """
    Pulls real-time financial telegraph headlines from Cailian Press (财联社电报).

    Cailian is China's closest equivalent to a real-time wire service — headlines
    move A-share prices within seconds of publication. This is the highest-velocity
    Chinese-language news source available via AkShare.

    Returns a list of dicts with normalised English keys. Empty list on failure.
    """
    try:
        df = ak.stock_telegraph_cls()
        if df.empty:
            print("[WARN] [SCRAPER] Cailian Press dataframe was empty.", flush=True)
            return []
        latest = df.head(10)
        result = []
        for _, row in latest.iterrows():
            result.append({
                "time":    str(row.get('发布时间', '')),
                "title":   str(row.get('标题', '')),
                "content": str(row.get('内容', '')),
            })
        print(f"[INFO] [SCRAPER] Cailian Press fetched ({len(result)} headlines).", flush=True)
        return result
    except Exception as e:
        print(f"[ERROR] [SCRAPER] Cailian Press fetch failed: {e}", flush=True)
        return []
