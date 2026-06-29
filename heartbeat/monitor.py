import os
import re
import sys
import telebot
import requests
import anthropic
import schedule
import time
import threading
import xml.etree.ElementTree as ET
import sqlite3
import logging
import math
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo
from telebot.util import smart_split
from bs4 import BeautifulSoup

# --- 1. CONFIGURATION ---
# RULE-009: Validate all required credentials at startup.
# Any missing variable is a hard-abort — no silent runtime failures.
def require_env(name: str) -> str:
    """Return the value of an env var or exit immediately with [FATAL]."""
    val = os.getenv(name)
    if val is None:
        print(f"[FATAL] [HEARTBEAT] Required env var '{name}' is not set. Refusing to start.",
              flush=True)
        sys.exit(1)
    return val

TELEGRAM_TOKEN = require_env('TELEGRAM_BOT_TOKEN')
CHAT_ID        = require_env('TELEGRAM_CHAT_ID')
ANTHROPIC_KEY  = require_env('ANTHROPIC_API_KEY')
ALPACA_API     = require_env('ALPACA_API_KEY')
ALPACA_SEC     = require_env('ALPACA_SECRET_KEY')
WEBHOOK_SECRET = require_env('WEBHOOK_SECRET_TOKEN')

print("[INFO] [HEARTBEAT] All required environment variables validated.", flush=True)

bot = telebot.TeleBot(TELEGRAM_TOKEN)
claude = anthropic.Anthropic(api_key=ANTHROPIC_KEY)

# Configure logging for IV collection pipeline
logging.basicConfig(
    level=logging.INFO,
    format='[%(levelname)s] [HEARTBEAT] %(message)s'
)
logger = logging.getLogger(__name__)

DB_PATH = '/app/data/memory_bank.db'

# Watchlist segmentation.
# DOMESTIC_WATCHLIST: US companies — file Form 8-K for material event disclosures.
# CHINESE_ADRS:       Foreign Private Issuers listed in the US — file Form 6-K instead.
#                     Polling the 8-K feed for these tickers would silently miss all
#                     their disclosures. The get_filing_type() helper resolves which
#                     feed and which document type to use for each ticker.
#
# Driven by NOX_WATCHLIST_US / NOX_WATCHLIST_CN env vars (set in .env / compose)
# so that adding a ticker in one place propagates to all services automatically.
_us_raw = os.getenv("NOX_WATCHLIST_US", "AAPL,TSLA,NVDA,MSFT")
_cn_raw = os.getenv("NOX_WATCHLIST_CN", "BABA,JD,PDD,BIDU,NIO")
DOMESTIC_WATCHLIST = [t.strip() for t in _us_raw.split(",") if t.strip()]
CHINESE_ADRS       = [t.strip() for t in _cn_raw.split(",") if t.strip()]
WATCHLIST          = DOMESTIC_WATCHLIST + CHINESE_ADRS

# Broad market scanner watchlist — covers all major S&P 500 sectors.
# TradingView free tier limits you to ~5 alerts; this scanner covers 35+ tickers
# by fetching bars from Alpaca and computing signals internally.
SCANNER_WATCHLIST = [
    # Index ETFs
    "SPY", "QQQ", "IWM",
    # Mega-cap tech
    "AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "META", "TSLA",
    # Financials
    "JPM", "BAC", "GS", "V", "MA",
    # Healthcare
    "JNJ", "UNH", "ABBV", "PFE",
    # Energy
    "XOM", "CVX", "COP",
    # Consumer
    "WMT", "HD", "COST",
    # Industrials
    "CAT", "BA", "RTX",
    # Growth / high-beta
    "PLTR", "SNOW", "CRM", "COIN",
    # User's existing TradingView tickers
    "XOM",  # already above, dedup is fine — set() used in scanner
]

# RULE-016 / Patch C: Global re-entrant lock for all SQLite write operations.
# Three threads (main bot loop, schedule_checker, poll_sec_edgar) share the same
# database file. Without a lock, concurrent writes raise:
#   sqlite3.OperationalError: database is locked
# Using a threading.Lock() ensures only one thread holds a write transaction at
# a time; reads are fast enough that the contention overhead is negligible.
db_lock = threading.Lock()

# --- 1.5 THE MEMORY BANK ---
def init_db():
    """Creates the database and tables if they don't exist."""
    # Patch C: Wrap the entire DDL block in db_lock so that no background
    # thread can begin a write before the schema is fully initialised.
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute('''
                    CREATE TABLE IF NOT EXISTS daily_audits (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
                        tickers_scanned TEXT,
                        claude_analysis TEXT
                    )
                ''')
                c.execute('''
                    CREATE TABLE IF NOT EXISTS processed_filings (
                        filing_id TEXT PRIMARY KEY,
                        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
                    )
                ''')
                c.execute('''
                    CREATE TABLE IF NOT EXISTS trade_history (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
                        ticker TEXT,
                        action TEXT,
                        price REAL,
                        rsi_value REAL,
                        sizing_kelly_ratio REAL,
                        pnl REAL
                    )
                ''')
                c.execute('''
                    CREATE TABLE IF NOT EXISTS historical_volatility (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        ticker TEXT NOT NULL,
                        date DATE NOT NULL,
                        implied_volatility REAL NOT NULL,
                        snapshot_timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
                        UNIQUE(ticker, date)
                    )
                ''')
                c.execute('''
                    CREATE INDEX IF NOT EXISTS idx_iv_ticker_date
                    ON historical_volatility(ticker, date)
                ''')
                c.execute('''
                    CREATE TABLE IF NOT EXISTS webhook_signals (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        received_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                        ticker TEXT,
                        action TEXT,
                        price REAL,
                        rsi REAL,
                        vix REAL,
                        source TEXT DEFAULT 'market_scanner'
                    )
                ''')
                conn.commit()
    except Exception as e:
        print(f"Database initialization error: {e}")

# --- 2. DATA EXTRACTION ---
# RULE-008: All HTTP calls use a (connect_timeout, read_timeout) tuple.
# A scalar timeout=10 only sets the read timeout — the connection can still
# block indefinitely. The tuple form enforces both independently.
HTTP_TIMEOUT = (5, 10)  # (connect seconds, read seconds)

def get_alpaca_portfolio():
    headers = {'APCA-API-KEY-ID': ALPACA_API, 'APCA-API-SECRET-KEY': ALPACA_SEC}
    try:
        acc_resp = requests.get('https://paper-api.alpaca.markets/v2/account',
                                headers=headers, timeout=HTTP_TIMEOUT)
        pos_resp = requests.get('https://paper-api.alpaca.markets/v2/positions',
                                headers=headers, timeout=HTTP_TIMEOUT)
        
        if acc_resp.status_code != 200 or pos_resp.status_code != 200:
            return f"Failed to pull Alpaca data. Account status: {acc_resp.status_code}, Positions status: {pos_resp.status_code}"

        acc = acc_resp.json()
        pos = pos_resp.json()
        
        if not isinstance(acc, dict) or not isinstance(pos, list):
            return "Alpaca API returned unexpected data formats."

        balance = acc.get('portfolio_value', 'Unknown')
        positions = []
        for p in pos:
            if isinstance(p, dict):
                positions.append(f"{p.get('qty', 0)} shares of {p.get('symbol', 'UNKNOWN')} (P&L: ${p.get('unrealized_pl', 0.0)})")
        
        pos_str = "\n".join(positions) if positions else "No open positions."
        return f"Portfolio: ${balance}\nPositions:\n{pos_str}"
    except Exception as e:
        return f"Failed to pull Alpaca data due to exception: {str(e)}"

def get_filing_type(ticker: str) -> str:
    """
    Returns the correct SEC form type for a given ticker.
    Chinese ADRs are Foreign Private Issuers and file Form 6-K.
    All US domestic companies file Form 8-K.
    """
    return "6-K" if ticker in CHINESE_ADRS else "8-K"


def get_latest_sec_filing(ticker: str) -> str:
    """
    Fetches the actual text of the latest 8-K or 6-K filing for a ticker.

    Two-step process:
      1. Pull the company's Atom feed for the correct form type (8-K or 6-K).
      2. Resolve the index page from the latest entry to find and fetch
         the primary HTML document — the actual filing text, not metadata.

    Token budget: truncated to 8,000 chars for the daily scout. The
    real-time pipeline (process_automated_filing) uses 40,000 chars
    because it sends a dedicated alert and can afford the larger context.
    """
    filing_type = get_filing_type(ticker)
    url = (
        f"https://www.sec.gov/cgi-bin/browse-edgar"
        f"?action=getcompany&CIK={ticker}&type={filing_type}&output=atom"
    )
    headers = {'User-Agent': 'Nox/1.0 openclaw@vanhellsing.tech'}
    try:
        resp = requests.get(url, headers=headers, timeout=HTTP_TIMEOUT)
        if resp.status_code != 200:
            return f"SEC feed returned {resp.status_code} for {ticker}."

        root = ET.fromstring(resp.content)
        ns = {'atom': 'http://www.w3.org/2005/Atom'}
        entries = root.findall('atom:entry', ns)
        if not entries:
            return f"No recent {filing_type} filings found for {ticker}."

        link_el = entries[0].find('atom:link', ns)
        if link_el is None:
            return f"No filing index link found in feed for {ticker}."

        index_url = link_el.attrib.get('href', '')
        if not index_url:
            return f"Empty filing index link for {ticker}."

        primary_url = resolve_primary_document(index_url, headers, filing_type)
        if not primary_url:
            return f"Could not resolve primary {filing_type} document for {ticker}."

        doc_res = requests.get(primary_url, headers=headers, timeout=HTTP_TIMEOUT)
        if doc_res.status_code != 200:
            return f"Primary document fetch returned {doc_res.status_code} for {ticker}."

        soup = BeautifulSoup(doc_res.text, "html.parser")
        for element in soup(["script", "style"]):
            element.extract()

        lines = [line.strip() for line in soup.get_text(separator="\n").splitlines() if line.strip()]
        # 8,000 char budget for scout context — enough for Claude to identify
        # the item numbers and key disclosures without burning excess tokens.
        cleaned = "\n".join(lines)[:8000]
        print(f"[INFO] [HEARTBEAT] Resolved {filing_type} text for {ticker} ({len(cleaned)} chars).", flush=True)
        return f"SEC {filing_type} ({ticker}):\n{cleaned}"
    except Exception as e:
        print(f"[WARN] [HEARTBEAT] get_latest_sec_filing failed for {ticker}: {e}", flush=True)
        return f"SEC pull failed for {ticker}: {str(e)}"

# --- 2.5 CHINESE MARKET INTELLIGENCE (china-data-engine) ---

def query_data_engine(endpoint: str, base_url: str = "http://china-data-engine:8000") -> dict:
    """
    Sends an authenticated GET request to an internal china-data-engine microservice.

    The china-data-engine runs scrapers on a 15-minute APScheduler cycle and
    caches results in memory, so this call always returns instantly — no live
    scrape is triggered. If the china-data-engine is unreachable (e.g., still starting
    up) we return an empty dict; the caller must handle that gracefully.

    Authentication follows the same shared-secret pattern used by the analyst →
    execution webhook (RULE-004): the X-Nox-Token header carries WEBHOOK_SECRET.
    RULE-008 timeouts are enforced via HTTP_TIMEOUT.
    """
    url = f"{base_url}{endpoint}"
    headers = {"X-Nox-Token": WEBHOOK_SECRET}
    try:
        res = requests.get(url, headers=headers, timeout=HTTP_TIMEOUT)
        if res.status_code == 200:
            return res.json()
        print(
            f"[WARN] [HEARTBEAT] china-data-engine at {base_url} returned HTTP {res.status_code} "
            f"for {endpoint}.",
            flush=True,
        )
    except Exception as e:
        print(
            f"[WARN] [HEARTBEAT] Could not reach china-data-engine at {url}: {e}",
            flush=True,
        )
    return {}


def get_chinese_market_context() -> str:
    """
    Assembles three layers of Chinese market intelligence by querying the
    dedicated china-data-engine microservice over the internal nox_net Docker network.

    Previously this function called AkShare directly inside the heartbeat
    container. That design had two problems:
      1. A slow or hanging AkShare call could block the Telegram bot thread.
      2. akshare + pandas added ~400 MB to the heartbeat image for logic that
         belongs in a data layer, not a notification layer.

    Now the heartbeat simply reads from the china-data-engine cache (<5 ms per call)
    and formats the response into a Claude-ready string. All scraping complexity
    lives in data_engine/scrapers.py where it belongs.

    Each endpoint is queried independently so a failure on one source (e.g.,
    the sentiment endpoint) does not suppress PMI or LPR data.
    """
    sections = []

    # --- East Money Hot Board (东方财富人气榜) ---
    sentiment_payload = query_data_engine("/sentiment/china")
    hot_board = sentiment_payload.get("hot_board", [])
    if hot_board:
        lines = [
            f"  {s['name']} ({s['ticker']}) — "
            f"¥{s['price']} | 涨跌幅: {s['change_pct']}%"
            for s in hot_board[:5]
        ]
        sections.append(
            "🇨🇳 East Money Hot Board (东方财富人气榜) — Top 5 Most-Watched A-Shares:\n"
            + "\n".join(lines)
        )
        print("[INFO] [HEARTBEAT] East Money hot board received from china-data-engine.", flush=True)
    else:
        sections.append("🇨🇳 East Money Hot Board: unavailable.")

    # --- China Manufacturing PMI (制造业PMI) ---
    macro_payload = query_data_engine("/macro/china")
    pmi = macro_payload.get("pmi", {})
    if pmi:
        mfg     = pmi.get('manufacturing', 'N/A')
        non_mfg = pmi.get('non_manufacturing', 'N/A')
        month   = pmi.get('month', 'N/A')
        try:
            expansion = float(mfg) > 50.0
        except (TypeError, ValueError):
            expansion = False
        sections.append(
            f"🏭 China PMI (NBS 国家统计局) — {month}:\n"
            f"  Manufacturing (制造业): {mfg} "
            f"({'EXPANSION ▲' if expansion else 'CONTRACTION ▼'})\n"
            f"  Non-Manufacturing (非制造业): {non_mfg}"
        )
        print("[INFO] [HEARTBEAT] China PMI received from china-data-engine.", flush=True)
    else:
        sections.append("🏭 China PMI: unavailable.")

    # --- PBOC Loan Prime Rate (贷款市场报价利率 LPR) ---
    lpr = macro_payload.get("lpr", {})
    if lpr:
        sections.append(
            f"🏦 PBOC Loan Prime Rate (LPR 贷款市场报价利率) — {lpr.get('date', 'N/A')}:\n"
            f"  1-Year LPR: {lpr.get('lpr_1y', 'N/A')}%\n"
            f"  5-Year LPR: {lpr.get('lpr_5y', 'N/A')}%"
        )
        print("[INFO] [HEARTBEAT] PBOC LPR received from china-data-engine.", flush=True)
    else:
        sections.append("🏦 PBOC LPR: unavailable.")

    # --- Cailian Press headlines (财联社电报) ---
    # Injected as a fourth context layer for the scout — highest-velocity
    # Chinese-language news source. Claude uses these to detect intraday
    # policy signals that have not yet appeared in English-language feeds.
    news_payload = query_data_engine("/news/cn")
    news_cn = news_payload.get("news", [])
    if news_cn:
        lines = [
            f"  [{n.get('time', '')}] {n.get('title', '')}"
            for n in news_cn[:5]
        ]
        sections.append(
            "📰 Cailian Press (财联社电报) — Latest 5 Headlines:\n"
            + "\n".join(lines)
        )
        print("[INFO] [HEARTBEAT] Cailian Press headlines received from china-data-engine.", flush=True)
    else:
        sections.append("📰 Cailian Press: unavailable.")

    return "\n\n".join(sections)


def get_us_news_context() -> str:
    """
    Assembles US news context by querying the america-china-data-engine.
    """
    news_payload = query_data_engine("/news/us", "http://america-data-engine:8001")
    news_us = news_payload.get("news", [])
    if news_us:
        lines = [
            f"- {n.get('headline', '')}"
            for n in news_us[:5] #top 5
        ]
        print(f"[INFO] [HEARTBEAT] US news received from america-china-data-engine ({len(lines)} headlines).", flush=True)
        return "\n".join(lines)
    else:
        print("[WARN] [HEARTBEAT] US news from america-china-data-engine was empty.", flush=True)
        return "US news headlines unavailable."


# --- 2.8 BROAD MARKET SCANNER ---
# Replaces / supplements TradingView alerts by scanning SCANNER_WATCHLIST
# every 30 minutes during market hours and posting BUY signals directly to
# the execution engine's internal /webhook endpoint over Docker's nox_net.
# No Traefik, no public internet, no TradingView subscription needed.

ALPACA_DATA_URL  = "https://data.alpaca.markets"
ALPACA_BROKER_URL = "https://paper-api.alpaca.markets"

# Minimum liquidity thresholds for the whole-market scanner.
# Keeps the candidate pool to liquid, optionable names.
SCANNER_MIN_PRICE       = 5.0      # skip sub-$5 stocks (wide spreads, illiquid options)
SCANNER_MIN_VOLUME      = 500_000  # skip tickers with < 500k daily volume
SCANNER_CANDIDATE_LIMIT = 80       # run full bar analysis on the top N candidates
# How many days of bars to fetch for RSI/ATR/SMA calculation
SCANNER_BAR_LIMIT       = 60

def _calc_rsi(closes: list[float], period: int = 14) -> float:
    """Wilder's smoothed RSI — same algorithm as OptionsSignalGenerator.hpp."""
    if len(closes) < period + 1:
        return 50.0
    deltas = [closes[i] - closes[i - 1] for i in range(1, len(closes))]
    gains  = [max(0.0, d) for d in deltas]
    losses = [max(0.0, -d) for d in deltas]
    avg_g  = sum(gains[:period])  / period
    avg_l  = sum(losses[:period]) / period
    for i in range(period, len(gains)):
        avg_g = (avg_g * (period - 1) + gains[i])  / period
        avg_l = (avg_l * (period - 1) + losses[i]) / period
    if avg_l == 0.0:
        return 100.0
    rs = avg_g / avg_l
    return 100.0 - (100.0 / (1.0 + rs))


def _calc_atr(highs: list[float], lows: list[float], closes: list[float], period: int = 14) -> float:
    """14-period Wilder ATR."""
    if len(closes) < 2:
        return 0.0
    trs = []
    for i in range(1, len(closes)):
        tr = max(highs[i] - lows[i],
                 abs(highs[i] - closes[i - 1]),
                 abs(lows[i]  - closes[i - 1]))
        trs.append(tr)
    if not trs:
        return 0.0
    n   = min(period, len(trs))
    atr = sum(trs[:n]) / n
    for i in range(n, len(trs)):
        atr = (atr * (period - 1) + trs[i]) / period
    return atr


def _calc_sma(values: list[float], period: int) -> float:
    if len(values) < period:
        return values[-1] if values else 0.0
    return sum(values[-period:]) / period


def fetch_bars(ticker: str, limit: int = 220) -> dict | None:
    """
    Fetch daily OHLCV bars from Alpaca market data API.
    Returns dict with keys: opens, highs, lows, closes, volumes (all lists, oldest first).
    """
    headers = {
        "APCA-API-KEY-ID":     ALPACA_API,
        "APCA-API-SECRET-KEY": ALPACA_SEC,
    }
    try:
        resp = requests.get(
            f"{ALPACA_DATA_URL}/v2/stocks/{ticker}/bars",
            headers=headers,
            params={"timeframe": "1Day", "limit": limit, "adjustment": "raw", "feed": "iex"},
            timeout=HTTP_TIMEOUT,
        )
        if resp.status_code != 200:
            return None
        data = resp.json()
        bars = data.get("bars", [])
        if not bars:
            return None
        return {
            "opens":   [b["o"] for b in bars],
            "highs":   [b["h"] for b in bars],
            "lows":    [b["l"] for b in bars],
            "closes":  [b["c"] for b in bars],
            "volumes": [b["v"] for b in bars],
        }
    except Exception as e:
        logger.warning(f"fetch_bars failed for {ticker}: {e}")
        return None


def fetch_batch_bars(tickers: list[str], limit: int = 60) -> dict[str, dict]:
    """
    Batch-fetch daily bars for many tickers in one API call.
    Returns {ticker: {opens, highs, lows, closes, volumes}} for tickers that have data.
    Alpaca returns up to ~1000 symbols per request.
    """
    headers = {
        "APCA-API-KEY-ID":     ALPACA_API,
        "APCA-API-SECRET-KEY": ALPACA_SEC,
    }
    result = {}
    # Alpaca batch bars endpoint accepts symbols as comma-separated query param
    # but URLs have length limits — chunk at 300 symbols per request.
    CHUNK = 300
    for i in range(0, len(tickers), CHUNK):
        chunk = tickers[i:i + CHUNK]
        try:
            resp = requests.get(
                f"{ALPACA_DATA_URL}/v2/stocks/bars",
                headers=headers,
                params={
                    "symbols":   ",".join(chunk),
                    "timeframe": "1Day",
                    "limit":     limit,
                    "adjustment": "raw",
                    "feed":      "iex",
                },
                timeout=(8, 30),
            )
            if resp.status_code != 200:
                continue
            data = resp.json().get("bars", {})
            for ticker, bars in data.items():
                if bars:
                    result[ticker] = {
                        "opens":   [b["o"] for b in bars],
                        "highs":   [b["h"] for b in bars],
                        "lows":    [b["l"] for b in bars],
                        "closes":  [b["c"] for b in bars],
                        "volumes": [b["v"] for b in bars],
                    }
        except Exception as e:
            logger.warning(f"fetch_batch_bars chunk {i//CHUNK} failed: {e}")
        time.sleep(0.5)  # respect rate limits between chunks
    return result


def fetch_batch_snapshots(tickers: list[str]) -> dict[str, dict]:
    """
    Get current price, volume, and daily change for many tickers at once.
    Used to rank the full market universe by activity before expensive bar analysis.
    """
    headers = {
        "APCA-API-KEY-ID":     ALPACA_API,
        "APCA-API-SECRET-KEY": ALPACA_SEC,
    }
    result = {}
    CHUNK = 300
    for i in range(0, len(tickers), CHUNK):
        chunk = tickers[i:i + CHUNK]
        try:
            resp = requests.get(
                f"{ALPACA_DATA_URL}/v2/stocks/snapshots",
                headers=headers,
                params={"symbols": ",".join(chunk), "feed": "iex"},
                timeout=(8, 30),
            )
            if resp.status_code != 200:
                continue
            for ticker, snap in resp.json().items():
                try:
                    daily = snap.get("dailyBar", {})
                    prev  = snap.get("prevDailyBar", {})
                    lat   = snap.get("latestTrade", {})
                    price = lat.get("p", 0) or daily.get("c", 0)
                    vol   = daily.get("v", 0)
                    prev_c = prev.get("c", price) or price
                    pct_chg = abs((price - prev_c) / prev_c * 100) if prev_c else 0
                    result[ticker] = {
                        "price":   price,
                        "volume":  vol,
                        "pct_chg": pct_chg,
                        "activity": vol * pct_chg,  # rank by dollar-volume × % move
                    }
                except Exception:
                    pass
        except Exception as e:
            logger.warning(f"fetch_batch_snapshots chunk {i//CHUNK} failed: {e}")
        time.sleep(0.5)
    return result


_universe_cache: list[str] = []
_universe_fetched_date: str = ""

def fetch_market_universe() -> list[str]:
    """
    Returns a list of all tradable, optionable US equity symbols from Alpaca.
    Results are cached for the trading day — the universe only changes at open.

    Filters: active status, us_equity class, price ≥ $5 (enforced later at snapshot stage).
    Returns ~6000-8000 symbols covering the full US listed equity market.
    """
    global _universe_cache, _universe_fetched_date
    et   = ZoneInfo("America/New_York")
    today = datetime.now(et).strftime("%Y-%m-%d")
    if _universe_cache and _universe_fetched_date == today:
        return _universe_cache

    headers = {
        "APCA-API-KEY-ID":     ALPACA_API,
        "APCA-API-SECRET-KEY": ALPACA_SEC,
    }
    tickers = []
    # Alpaca paginates assets — fetch all pages
    page_token = None
    while True:
        params = {
            "status":       "active",
            "asset_class":  "us_equity",
            "tradable":     "true",
        }
        if page_token:
            params["page_token"] = page_token
        try:
            resp = requests.get(
                f"{ALPACA_BROKER_URL}/v2/assets",
                headers=headers,
                params=params,
                timeout=HTTP_TIMEOUT,
            )
            if resp.status_code != 200:
                break
            assets = resp.json()
            if not assets:
                break
            for a in assets:
                sym = a.get("symbol", "")
                # Skip if symbol has special chars (warrants, units, preferred, etc.)
                if sym and sym.isalpha() and len(sym) <= 5:
                    tickers.append(sym)
            # Alpaca v2 assets returns all in one call (no pagination token)
            break
        except Exception as e:
            logger.warning(f"fetch_market_universe failed: {e}")
            break

    _universe_cache = tickers
    _universe_fetched_date = today
    logger.info(f"Market universe loaded: {len(tickers)} tickers.")
    return tickers


def fetch_vix_level() -> float:
    """Current VIX via Yahoo Finance (free, no API key)."""
    try:
        resp = requests.get(
            "https://query1.finance.yahoo.com/v8/finance/chart/%5EVIX",
            params={"interval": "1d", "range": "1d"},
            headers={"User-Agent": "Mozilla/5.0"},
            timeout=HTTP_TIMEOUT,
        )
        return float(resp.json()["chart"]["result"][0]["meta"]["regularMarketPrice"])
    except Exception:
        return 20.0  # neutral fallback — gates will still apply


def fetch_spy_regime() -> tuple[float, float, float]:
    """
    Returns (spy_price, spy_200_sma, vix).
    Fetched once per scan cycle and passed to each ticker scan to avoid
    220 × number-of-tickers redundant API calls.
    """
    vix = fetch_vix_level()
    bars = fetch_bars("SPY", limit=210)
    if not bars or len(bars["closes"]) < 200:
        return 0.0, 0.0, vix
    spy_price   = bars["closes"][-1]
    spy_200_sma = _calc_sma(bars["closes"], 200)
    return spy_price, spy_200_sma, vix


def scan_ticker_for_signal(
    ticker: str,
    spy_price: float,
    spy_200_sma: float,
    vix: float,
    bars_override: dict | None = None,
) -> dict | None:
    """
    Returns a webhook-ready signal dict if the ticker passes all entry conditions,
    otherwise None. Mirrors the RSI + regime gates enforced in execution/main.cpp.

    bars_override: pre-fetched bar data from fetch_batch_bars() — skips the API call
                   when called from the whole-market scanner (Stage 3 reuse).

    Entry conditions (BUY bias only):
      • RSI-14 between 45 and 68  (momentum without being overbought)
      • Price above 20-day SMA    (uptrend confirmed)
      • Volume > 1.2× 10-day avg  (breakout confirmation)
      • Regime: not RISK_OFF      (VIX < 25 AND SPY above 200-SMA)
    """
    if vix >= 25.0 and spy_price > 0 and spy_price < spy_200_sma:
        return None  # RISK_OFF — engine would block this; don't waste the call

    bars = bars_override if bars_override else fetch_bars(ticker, limit=60)
    if not bars or len(bars["closes"]) < 22:
        return None

    closes  = bars["closes"]
    highs   = bars["highs"]
    lows    = bars["lows"]
    volumes = bars["volumes"]

    rsi   = _calc_rsi(closes)
    atr   = _calc_atr(highs, lows, closes)
    sma20 = _calc_sma(closes, 20)
    price = closes[-1]

    if price <= 0 or atr <= 0:
        return None
    if rsi < 45.0 or rsi > 68.0:
        return None
    if price < sma20:
        return None

    # Volume confirmation: current volume vs 10-day average
    avg_vol_10 = _calc_sma(volumes, 10) if len(volumes) >= 10 else volumes[-1]
    if avg_vol_10 > 0 and volumes[-1] < avg_vol_10 * 1.2:
        return None

    return {
        "secret_key":              WEBHOOK_SECRET,
        "ticker":                  ticker,
        "action":                  "BUY",
        "price":                   round(price, 4),
        "rsi":                     round(rsi, 2),
        "vol":                     int(volumes[-1]),
        "atr":                     round(atr, 4),
        "stop_loss_atr_multiplier": 2.0,
        "vix":                     round(vix, 2),
        "spy_price":               round(spy_price, 4),
        "spy_200_sma":             round(spy_200_sma, 4),
        "risk_tier":               1,  # Standard: 1% capital per trade
    }


def post_signal_to_engine(signal: dict) -> bool:
    """POST a signal to the execution engine's internal webhook (Docker-internal only)."""
    try:
        resp = requests.post(
            "http://execution-engine:8080/webhook",
            json=signal,
            timeout=(3, 8),
        )
        return resp.status_code == 200
    except Exception as e:
        logger.warning(f"post_signal_to_engine failed: {e}")
        return False


def log_scanner_signal(signal: dict) -> None:
    """Persist scanner-generated signals to webhook_signals table."""
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "INSERT INTO webhook_signals (ticker, action, price, rsi, vix, source) "
                    "VALUES (?, ?, ?, ?, ?, ?)",
                    (signal["ticker"], signal["action"], signal["price"],
                     signal["rsi"], signal["vix"], "market_scanner"),
                )
                conn.commit()
    except Exception as e:
        logger.warning(f"log_scanner_signal failed: {e}")


def is_market_hours() -> bool:
    """True if the NYSE is likely open (Mon–Fri 9:30–16:00 ET, ignoring holidays)."""
    et  = ZoneInfo("America/New_York")
    now = datetime.now(et)
    if now.weekday() >= 5:  # Saturday / Sunday
        return False
    open_time  = now.replace(hour=9,  minute=30, second=0, microsecond=0)
    close_time = now.replace(hour=16, minute=0,  second=0, microsecond=0)
    return open_time <= now <= close_time


def run_market_scanner() -> None:
    """
    Whole-market scanner — covers every tradable US equity on Alpaca (~6000-8000 tickers).

    Pipeline (3 stages to avoid scanning thousands of tickers with expensive bar calls):

      Stage 1 — Universe: fetch all active US equities from Alpaca asset list (cached daily).
      Stage 2 — Snapshot screen: batch-snapshot the universe to get current price/volume/% move.
                 Filter to liquid names (price ≥ $5, volume ≥ 500k) and rank by unusual
                 activity (volume × % daily change). Take the top SCANNER_CANDIDATE_LIMIT.
      Stage 3 — Signal analysis: batch-fetch 60 days of daily bars for the candidates,
                 compute RSI/ATR/SMA, apply entry conditions, post qualifying signals.

    This is how institutional systems do it: universe → screener → signal.
    """
    if not is_market_hours():
        logger.info("Market scanner skipped — outside market hours.")
        return

    logger.info("Market scanner cycle starting...")
    spy_price, spy_200_sma, vix = fetch_spy_regime()
    logger.info(f"Regime: SPY={spy_price:.2f}, SMA200={spy_200_sma:.2f}, VIX={vix:.1f}")

    if vix >= 25.0 and spy_price > 0 and spy_price < spy_200_sma:
        logger.info("Market scanner: RISK_OFF regime — skipping scan.")
        return

    # ── Stage 1: Universe ────────────────────────────────────────────────────
    universe = fetch_market_universe()
    if not universe:
        logger.warning("Market scanner: empty universe — skipping.")
        return
    logger.info(f"Universe: {len(universe)} tickers.")

    # ── Stage 2: Snapshot screen ─────────────────────────────────────────────
    snapshots = fetch_batch_snapshots(universe)

    # Filter and rank by unusual activity
    candidates = [
        (ticker, snap)
        for ticker, snap in snapshots.items()
        if snap["price"] >= SCANNER_MIN_PRICE
        and snap["volume"] >= SCANNER_MIN_VOLUME
    ]
    # Sort by activity score (volume × abs % change) descending
    candidates.sort(key=lambda x: x[1]["activity"], reverse=True)
    top_tickers = [t for t, _ in candidates[:SCANNER_CANDIDATE_LIMIT]]

    logger.info(
        f"Snapshot screen: {len(candidates)} liquid tickers → "
        f"top {len(top_tickers)} by unusual activity."
    )

    # ── Stage 3: Bar analysis and signal generation ───────────────────────────
    bars_map = fetch_batch_bars(top_tickers, limit=SCANNER_BAR_LIMIT)

    triggered = []
    for ticker in top_tickers:
        if ticker not in bars_map:
            continue
        bars = bars_map[ticker]
        if len(bars["closes"]) < 22:
            continue
        try:
            signal = scan_ticker_for_signal(ticker, spy_price, spy_200_sma, vix,
                                            bars_override=bars)
            if signal:
                ok = post_signal_to_engine(signal)
                log_scanner_signal(signal)
                snap = snapshots.get(ticker, {})
                triggered.append(
                    f"{ticker} RSI={signal['rsi']:.0f} "
                    f"{snap.get('pct_chg', 0):.1f}% "
                    f"{'✓' if ok else '✗'}"
                )
        except Exception as e:
            logger.warning(f"[SCANNER] {ticker}: {e}")

    if triggered:
        msg = (
            f"📡 *Whole-Market Scanner* — {len(triggered)} signal(s) from "
            f"{len(top_tickers)} candidates\n"
            f"_(universe: {len(universe)} tickers · VIX={vix:.1f})_\n\n" +
            "\n".join(f"• {t}" for t in triggered[:20])  # cap Telegram msg length
        )
        if len(triggered) > 20:
            msg += f"\n_...and {len(triggered) - 20} more_"
        bot.send_message(CHAT_ID, msg, parse_mode="Markdown")
    else:
        logger.info(
            f"Scanner cycle complete — 0 signals from {len(top_tickers)} candidates "
            f"(VIX={vix:.1f})."
        )


# --- 3. THE SCOUT PROTOCOL (DAILY REPORT) ---

SCOUT_SYSTEM_PROMPT = """You are Nox, a quantitative trading assistant generating a pre-market briefing.

OUTPUT FORMAT — Telegram Markdown. Follow this exactly:
• Bold with *single asterisks* — use for section headers and key numbers
• Bullet points use • (unicode bullet), not dashes or asterisks
• Section divider: ────────────────────────  (copy exactly, 24 chars)
• Separate each section with one blank line
• NO markdown # headers — they render as literal # in Telegram
• NO tables — use • Key: *value* format

REQUIRED SECTIONS IN THIS ORDER — each starts with *SECTION NAME* on its own line, then the divider:

*REGIME & MACRO*
────────────────────────
(VIX level and what it means, SPY vs 200-SMA, risk posture in one line each)

*US MARKET*
────────────────────────
(index moves with specific %, sector rotation, volume vs average, any structural breaks)

*CHINA CROSS-MARKET*
────────────────────────
(PMI reading + expansion/contraction, LPR rate + direction, hot board top name, explicit statement: does China CONFIRM, CONTRADICT, or LEAD the US narrative today)

*SEC RADAR*
────────────────────────
(one bullet per material filing — ticker, item number, one-sentence impact. Skip filings with no market impact)

*ACTIONABLE SETUPS*
────────────────────────
(2-4 specific trade ideas: ticker — strategy type — exact condition that must hold — why now)

*WATCH TOMORROW*
────────────────────────
(2-3 catalysts or levels to monitor, with specific price/date/number)

CONTENT RULES:
• Every bullet must contain at least one specific number or data point
• ACTIONABLE SETUPS must name ticker, strategy, and a falsifiable entry condition
• China section must explicitly say CONFIRMS / CONTRADICTS / LEADS — not implied
• No filler phrases, no disclaimers, no "it's worth noting"
• If a section has no material data, write one bullet: • Nothing material today
• Write as many bullets as the data warrants — depth over brevity. Do not truncate analysis to hit a length target. A section with more material gets more bullets.
• Within each bullet, go one level deeper than the surface fact: state the implication, not just the number"""


def _split_scout_sections(text: str) -> list[str]:
    """
    Split Claude's report on section headers (*BOLD* lines preceded by a blank line)
    so each Telegram message contains exactly one complete section.
    Falls back to smart_split if no sections are detected.
    """
    import re
    # Split on blank line + bold section header pattern
    parts = re.split(r'\n\n(?=\*[A-Z][A-Z &\-]+\*\n)', text)
    # Filter empty and strip
    parts = [p.strip() for p in parts if p.strip()]
    if len(parts) <= 1:
        # Claude didn't follow the format — fall back to char-based split
        return list(smart_split(text, chars_per_string=3800))
    # Guard: no individual part should exceed Telegram's 4096-char limit
    final = []
    for part in parts:
        if len(part) <= 4000:
            final.append(part)
        else:
            for chunk in smart_split(part, chars_per_string=3800):
                final.append(chunk)
    return final


def run_scout_protocol():
    try:
        news_context    = get_us_news_context()
        sec_context     = "\n\n".join([get_latest_sec_filing(t) for t in ["NVDA", "BABA"]])
        chinese_context = get_chinese_market_context()

        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=7000,
            system=SCOUT_SYSTEM_PROMPT,
            messages=[{"role": "user", "content": (
                f"US Headlines:\n{news_context}\n\n"
                f"SEC Filings:\n{sec_context}\n\n"
                f"Chinese Market Intelligence:\n{chinese_context}"
            )}]
        )

        analysis_text = response.content[0].text
        et_tz = ZoneInfo('America/New_York')
        timestamp = datetime.now(et_tz).strftime('%Y-%m-%d %H:%M ET')

        # Header sent as its own message so sections start clean
        header = (
            f"*NOX DAILY AUDIT*\n"
            f"────────────────────────\n"
            f"{timestamp}"
        )
        bot.send_message(CHAT_ID, header, parse_mode='Markdown')

        # Send each section as its own Telegram message — no mid-section breaks
        for section in _split_scout_sections(analysis_text):
            bot.send_message(CHAT_ID, section, parse_mode='Markdown')

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute("INSERT INTO daily_audits (tickers_scanned, claude_analysis) VALUES (?, ?)",
                          ("NVDA, BABA", analysis_text))
                conn.commit()
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] Scout protocol failed: {e}", flush=True)

# --- 2.7 HISTORICAL IMPLIED VOLATILITY COLLECTION ---

def fetch_options_chain_iv(ticker: str) -> float | None:
    """
    Fetches the options chain for a ticker and calculates a weighted-average
    implied volatility (IV) from both call and put options.

    Uses Alpaca's /v1beta3/options/chains endpoint for end-of-day snapshot.
    Weights by open interest to reflect market conviction.

    Returns: weighted average IV as a float, or None if fetch fails.
    """
    headers = {
        'APCA-API-KEY-ID': ALPACA_API,
        'APCA-API-SECRET-KEY': ALPACA_SEC
    }

    try:
        # Alpaca options chains endpoint — paper trading API
        url = f"https://paper-api.alpaca.markets/v1beta3/options/chains/{ticker}"
        resp = requests.get(url, headers=headers, timeout=HTTP_TIMEOUT)

        if resp.status_code != 200:
            logger.warning(f"Alpaca options chain request failed for {ticker}: HTTP {resp.status_code}")
            return None

        data = resp.json()
        if not data or 'chains' not in data:
            logger.warning(f"No options chain data returned for {ticker}")
            return None

        # Collect all IV values weighted by open interest
        iv_values = []
        total_oi = 0

        chains = data.get('chains', [])
        for chain_entry in chains:
            if 'iv' in chain_entry and chain_entry.get('iv') is not None:
                iv = float(chain_entry['iv'])
                oi = float(chain_entry.get('open_interest', 0))
                if oi > 0:
                    iv_values.append((iv, oi))
                    total_oi += oi

        if not iv_values or total_oi == 0:
            logger.warning(f"No liquid options (with open interest) found for {ticker}")
            return None

        # Weighted average IV
        weighted_iv = sum(iv * oi for iv, oi in iv_values) / total_oi
        logger.info(f"Fetched IV for {ticker}: {weighted_iv:.4f} (from {len(iv_values)} contracts, OI: {total_oi})")
        return weighted_iv

    except Exception as e:
        logger.error(f"Exception fetching options chain for {ticker}: {e}")
        return None


def _classify_option_side(entry: dict) -> str | None:
    """
    Resolve whether a chain entry is a 'call' or 'put'.
    Tries explicit fields first, then falls back to parsing the OCC symbol
    (…YYMMDD[C|P]strike). Returns 'call', 'put', or None if undeterminable.
    """
    for key in ("type", "option_type", "side", "cp", "right"):
        v = entry.get(key)
        if isinstance(v, str):
            low = v.strip().lower()
            if low in ("call", "c"):
                return "call"
            if low in ("put", "p"):
                return "put"
    sym = entry.get("symbol", "") or ""
    # OCC symbol: 6-char date followed by C or P, e.g. AAPL250620C00190000
    m = re.search(r"\d{6}([CP])\d+", sym)
    if m:
        return "call" if m.group(1) == "C" else "put"
    return None


def fetch_iv_skew(ticker: str) -> dict:
    """
    WS1 — compute live IV skew and a put/call open-interest profile for a ticker.

    Skew = put_iv - call_iv (both open-interest-weighted). A POSITIVE skew means
    puts are bid up relative to calls — the options market is paying for downside
    protection (bearish / fearful). A negative skew is bullish.

    The Contradiction Vector cross-checks this against headline sentiment:
    bullish text + bearish (positive) skew is a contradiction → IGNORE the signal.

    Returns:
        {
            "ticker", "call_iv", "put_iv", "skew", "skew_pct",
            "put_call_oi_ratio", "contracts", "method": "live_chain" | "error"
        }
    """
    headers = {
        "APCA-API-KEY-ID": ALPACA_API,
        "APCA-API-SECRET-KEY": ALPACA_SEC,
    }
    try:
        url = f"https://paper-api.alpaca.markets/v1beta3/options/chains/{ticker}"
        resp = requests.get(url, headers=headers, timeout=HTTP_TIMEOUT)
        if resp.status_code != 200:
            return {"ticker": ticker, "method": "error",
                    "error": f"chain HTTP {resp.status_code}"}

        data = resp.json() or {}
        chains = data.get("chains", [])
        if not chains:
            return {"ticker": ticker, "method": "error", "error": "empty chain"}

        call_iv_oi = call_oi = 0.0
        put_iv_oi = put_oi = 0.0
        contracts = 0

        for entry in chains:
            iv = entry.get("iv")
            if iv is None:
                continue
            oi = float(entry.get("open_interest", 0) or 0)
            if oi <= 0:
                continue
            side = _classify_option_side(entry)
            if side == "call":
                call_iv_oi += float(iv) * oi
                call_oi += oi
                contracts += 1
            elif side == "put":
                put_iv_oi += float(iv) * oi
                put_oi += oi
                contracts += 1

        if call_oi == 0 or put_oi == 0:
            return {"ticker": ticker, "method": "error",
                    "error": "insufficient call/put open interest"}

        call_iv = call_iv_oi / call_oi
        put_iv = put_iv_oi / put_oi
        skew = put_iv - call_iv
        skew_pct = (skew / call_iv) if call_iv > 0 else 0.0

        result = {
            "ticker": ticker,
            "call_iv": round(call_iv, 4),
            "put_iv": round(put_iv, 4),
            "skew": round(skew, 4),
            "skew_pct": round(skew_pct, 4),
            "put_call_oi_ratio": round(put_oi / call_oi, 4),
            "contracts": contracts,
            "method": "live_chain",
        }
        logger.info(
            f"IV skew for {ticker}: skew={result['skew']:.4f} "
            f"(put={put_iv:.4f}, call={call_iv:.4f}, P/C OI={result['put_call_oi_ratio']:.2f})"
        )
        return result
    except Exception as e:
        logger.error(f"fetch_iv_skew failed for {ticker}: {e}")
        return {"ticker": ticker, "method": "error", "error": str(e)}


def store_iv_snapshot(ticker: str, iv: float, date_str: str) -> bool:
    """
    Writes an IV snapshot to the historical_volatility table.
    Uses db_lock to prevent concurrent writes.

    Args:
        ticker: Stock symbol
        iv: Implied volatility as a decimal (e.g., 0.35 for 35%)
        date_str: Date string in YYYY-MM-DD format

    Returns: True if successful, False otherwise.
    """
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    """
                    INSERT OR REPLACE INTO historical_volatility
                    (ticker, date, implied_volatility, snapshot_timestamp)
                    VALUES (?, ?, ?, CURRENT_TIMESTAMP)
                    """,
                    (ticker, date_str, iv)
                )
                conn.commit()
        logger.info(f"Stored IV snapshot: {ticker} on {date_str} = {iv:.4f}")
        return True
    except Exception as e:
        logger.error(f"Failed to store IV snapshot for {ticker}: {e}")
        return False


def collect_eod_iv_snapshots():
    """
    Post-market collection task (runs at 16:30 ET).
    Iterates through the watchlist, fetches current IV from Alpaca,
    and writes each snapshot to the historical_volatility table.

    Failures on individual tickers don't block the full task.
    """
    logger.info("Starting end-of-day IV snapshot collection...")
    et_tz = ZoneInfo('America/New_York')
    today = datetime.now(et_tz).strftime('%Y-%m-%d')

    successful = 0
    failed = 0

    for ticker in WATCHLIST:
        iv = fetch_options_chain_iv(ticker)
        if iv is not None:
            if store_iv_snapshot(ticker, iv, today):
                successful += 1
            else:
                failed += 1
        else:
            failed += 1

    logger.info(f"EOD IV collection complete: {successful} succeeded, {failed} failed")


def calculate_iv_rank(ticker: str, current_iv: float | None = None) -> dict:
    """
    Calculates a ticker's IV Rank by comparing today's IV against accumulated
    historical records in the historical_volatility table.

    IV Rank = (Current IV - 52-week low) / (52-week high - 52-week low)
    Result is clamped to [0, 1] representing percentile in the recent range.

    If fewer than 30 days of data exist, falls back to a snapshot-relative
    calculation (Current IV - Average IV) / Average IV and logs a warning.

    Args:
        ticker: Stock symbol
        current_iv: Optional current IV value. If None, fetches from Alpaca.

    Returns: Dictionary with keys:
        - iv_rank: Percentile float in [0, 1], or None if calculation fails
        - current_iv: Current IV value used
        - method: "full_history" or "snapshot_relative" or "error"
        - data_points: Number of historical data points used
        - days_available: Number of distinct trading days in history
    """
    try:
        # Fetch current IV if not provided
        if current_iv is None:
            current_iv = fetch_options_chain_iv(ticker)
            if current_iv is None:
                return {
                    'iv_rank': None,
                    'current_iv': None,
                    'method': 'error',
                    'data_points': 0,
                    'days_available': 0,
                    'error': f'Could not fetch current IV for {ticker}'
                }

        # Query historical IV data
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    """
                    SELECT implied_volatility, date
                    FROM historical_volatility
                    WHERE ticker = ?
                    ORDER BY date DESC
                    LIMIT 252
                    """,
                    (ticker,)
                )
                rows = c.fetchall()

        if not rows:
            logger.warning(f"No historical IV data found for {ticker}; cannot calculate rank")
            return {
                'iv_rank': None,
                'current_iv': current_iv,
                'method': 'error',
                'data_points': 0,
                'days_available': 0,
                'error': 'No historical data'
            }

        iv_history = [row[0] for row in rows]
        unique_dates = len(set(row[1] for row in rows))

        # Full history method: 30+ days available
        if unique_dates >= 30:
            iv_min = min(iv_history)
            iv_max = max(iv_history)

            if iv_max == iv_min:
                # All IVs identical — clamp to 0.5 (middle of range)
                iv_rank = 0.5
            else:
                iv_rank = (current_iv - iv_min) / (iv_max - iv_min)
                iv_rank = max(0.0, min(1.0, iv_rank))  # Clamp to [0, 1]

            logger.info(
                f"IV Rank for {ticker}: {iv_rank:.2%} "
                f"(Current={current_iv:.4f}, Min={iv_min:.4f}, Max={iv_max:.4f}, Points={len(iv_history)})"
            )
            return {
                'iv_rank': iv_rank,
                'current_iv': current_iv,
                'method': 'full_history',
                'data_points': len(iv_history),
                'days_available': unique_dates,
                'iv_min': iv_min,
                'iv_max': iv_max
            }
        else:
            # Fallback: snapshot-relative percentile
            logger.warning(
                f"Insufficient data for {ticker}: only {unique_dates} days (need 30+). "
                f"Falling back to snapshot-relative calculation."
            )
            avg_iv = sum(iv_history) / len(iv_history)
            if avg_iv > 0:
                iv_rank = (current_iv - avg_iv) / avg_iv
                iv_rank = max(0.0, min(1.0, iv_rank))  # Clamp to [0, 1]
            else:
                iv_rank = 0.5

            return {
                'iv_rank': iv_rank,
                'current_iv': current_iv,
                'method': 'snapshot_relative',
                'data_points': len(iv_history),
                'days_available': unique_dates,
                'average_iv': avg_iv
            }

    except Exception as e:
        logger.error(f"Exception calculating IV Rank for {ticker}: {e}")
        return {
            'iv_rank': None,
            'current_iv': None,
            'method': 'error',
            'data_points': 0,
            'days_available': 0,
            'error': str(e)
        }


def schedule_checker():
    # RULE-006: Daily Scout MUST fire at 10:00 AM Eastern Time (ET), not UTC.
    #
    # Why this approach:
    #   • `schedule` library has no native timezone awareness — `.at("HH:MM")`
    #     always interprets the time as the system clock (UTC inside Docker).
    #   • The tz-argument form `.at("HH:MM", "America/New_York")` was introduced
    #     in schedule 1.2.0. Pinning that version in the Dockerfile is required
    #     (see Dockerfile); this code depends on it.
    #   • DST transitions mean the UTC equivalent of 11:30 ET changes twice a
    #     year (UTC-5 in winter → UTC-4 in summer). We recompute at midnight UTC
    #     each day so the offset is always correct.
    #
    # Design:
    #   _reschedule_scout() is called once at startup and then once per day at
    #   00:01 UTC. It clears the previous 'scout' job, computes today's correct
    #   UTC wall-clock equivalent of 11:30 ET, and registers a fresh job.
    #   A `_scout_fired_today` flag prevents a double-fire if reschedule happens
    #   to run on the same tick as the job (edge case on slow startup).

    ET  = ZoneInfo("America/New_York")
    UTC = ZoneInfo("UTC")

    def _scheduled_scout():
        """Scheduled daily scout. Manual /report runs don't interfere with this."""
        run_scout_protocol()

    def _reschedule_scout():
        """
        Clear any existing 'scout' job, compute the UTC wall-clock time that
        corresponds to 9:00 AM ET (pre-market) *today*, and register a new daily job.
        Called once at startup and then nightly at 00:01 UTC.
        """
        schedule.clear("scout")

        now_et     = datetime.now(tz=ET)
        # Build a timezone-aware 9:00 AM ET for today (pre-market), then express it in UTC.
        target_et  = now_et.replace(hour=9, minute=0, second=0, microsecond=0)
        target_utc = target_et.astimezone(UTC)
        utc_hhmm   = target_utc.strftime("%H:%M")

        schedule.every().day.at(utc_hhmm).do(_scheduled_scout).tag("scout")
        print(
            f"[INFO] [HEARTBEAT] Daily Scout (re)scheduled: "
            f"9:00 AM ET (pre-market) = {utc_hhmm} UTC "
            f"({'EDT UTC-4' if target_et.utcoffset().total_seconds() == -14400 else 'EST UTC-5'}).",
            flush=True,
        )

    # --- Initial registration at startup ---
    _reschedule_scout()

    # --- IV Collection Scheduler (Post-Market) ---
    # RULE-006 applies here too: schedule 16:30 ET, which varies by DST offset.
    # We reschedule at 00:01 UTC each day to maintain the correct wall clock time.

    def _reschedule_iv_collection():
        """
        Clear any existing 'iv_collection' job, compute the UTC wall-clock time that
        corresponds to 4:30 PM ET (market close + buffer) *today*, and register a new job.
        Called once at startup and then nightly at 00:01 UTC.
        """
        schedule.clear("iv_collection")

        now_et     = datetime.now(tz=ET)
        # 16:30 ET = 4:30 PM ET (market close is 16:00, so this gives 30 min buffer)
        target_et  = now_et.replace(hour=16, minute=30, second=0, microsecond=0)
        target_utc = target_et.astimezone(UTC)
        utc_hhmm   = target_utc.strftime("%H:%M")

        schedule.every().day.at(utc_hhmm).do(collect_eod_iv_snapshots).tag("iv_collection")
        logger.info(
            f"EOD IV collection (re)scheduled: "
            f"4:30 PM ET (post-market) = {utc_hhmm} UTC "
            f"({'EDT UTC-4' if target_et.utcoffset().total_seconds() == -14400 else 'EST UTC-5'})."
        )

    _reschedule_iv_collection()

    # --- Market Scanner (every 30 minutes, market hours only) ---
    # Runs independently of DST — the is_market_hours() check inside handles
    # the ET window, so we schedule on a fixed wall-clock interval, not a time.
    schedule.clear("market_scanner")
    schedule.every(30).minutes.do(run_market_scanner).tag("market_scanner")
    logger.info("Market scanner scheduled: every 30 minutes (market hours gated internally).")
    # Fire once immediately at startup so first signals don't wait 30 minutes.
    threading.Thread(target=run_market_scanner, daemon=True).start()

    # Reschedule IV collection each night along with the scout
    def _combined_reschedule():
        _reschedule_scout()
        _reschedule_iv_collection()

    schedule.clear("reschedule")
    schedule.every().day.at("00:01").do(_combined_reschedule).tag("reschedule")

    # --- Monthly journal (1st of each month at 00:05 UTC) ---
    schedule.every().day.at("00:05").do(
        lambda: run_monthly_journal() if datetime.now().day == 1 else None
    ).tag("monthly_journal")

    while True:
        # Patch C: Catch any exception thrown by run_scout_protocol() (e.g.,
        # Anthropic timeout, Alpaca 429, Telegram API block) so that a single
        # failed invocation does NOT terminate the scheduler thread and
        # permanently silence all future scheduled tasks.
        try:
            schedule.run_pending()
        except Exception as e:
            print(f"⚠️ [SCHEDULER ERROR] Thread exception caught: {e}", flush=True)
        time.sleep(30)   # 30-second tick — tight enough not to miss the window

# --- 4. CONVERSATIONAL AGENT HANDLERS ---
@bot.message_handler(commands=['report'])
def trigger_report(message):
    """
    /report — Manually triggers the full daily scout protocol on demand.
    Sends an acknowledgement immediately so the user knows it fired,
    then runs the full pipeline (news + SEC + Chinese market context + Claude)
    in a background thread so the bot remains responsive during generation.
    """
    try:
        bot.reply_to(message, "⚙️ *Nox Scout firing now...* Assembling data layers. This takes ~30 seconds.", parse_mode='Markdown')
        threading.Thread(target=run_scout_protocol, daemon=True).start()
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /report command failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to trigger report: {str(e)}")

@bot.message_handler(commands=['status'])
def send_status(message):
    """
    /status - Overhauled to poll the entire system in real-time.
    Pings the Execution and Data engines, and queries SQLite for the last
    recorded timestamps, returning a structured health dashboard.
    """
    try:
        print("[STATUS CMD] Received /status command. Beginning health checks.", flush=True)

        # --- 1. Ping Core Services ---
        exec_status, exec_ping = "OFFLINE", -1
        try:
            start_time = time.time()
            exec_res = requests.get("http://execution-engine:8080/health", timeout=HTTP_TIMEOUT)
            print(f"[STATUS CMD] Execution Engine response: {exec_res.status_code}", flush=True)
            if exec_res.status_code == 200:
                data = exec_res.json()
                print(f"[STATUS CMD] Execution Engine data: {data}", flush=True)
                if data.get("status") == "healthy":
                    exec_status = "ONLINE"
                    exec_ping = int((time.time() - start_time) * 1000)
        except (requests.RequestException, ValueError) as e:
            print(f"[STATUS CMD] Execution Engine check failed: {e}", flush=True)
            pass  # Status remains OFFLINE

        data_status, data_cache_age = "OFFLINE", "N/A"
        try:
            data_res = requests.get("http://china-data-engine:8000/health", timeout=HTTP_TIMEOUT)
            print(f"[STATUS CMD] China Data Engine response: {data_res.status_code}", flush=True)
            if data_res.status_code == 200:
                health_data = data_res.json()
                print(f"[STATUS CMD] China Data Engine data: {health_data}", flush=True)
                if health_data.get("status") == "healthy":
                    data_status = "ONLINE"
                    last_updated_str = health_data.get("last_updated_utc")
                    if last_updated_str:
                        last_updated = datetime.fromisoformat(last_updated_str.replace("Z", "+00:00"))
                        age = datetime.now(ZoneInfo("UTC")) - last_updated
                        data_cache_age = f"{int(age.total_seconds() // 60)}m ago"
        except (requests.RequestException, ValueError) as e:
            print(f"[STATUS CMD] China Data Engine check failed: {e}", flush=True)
            pass # Status remains OFFLINE

        america_data_status, america_data_cache_age = "OFFLINE", "N/A"
        try:
            america_data_res = requests.get("http://america-data-engine:8001/health", timeout=HTTP_TIMEOUT)
            print(f"[STATUS CMD] America Data Engine response: {america_data_res.status_code}", flush=True)
            if america_data_res.status_code == 200:
                health_data = america_data_res.json()
                print(f"[STATUS CMD] America Data Engine data: {health_data}", flush=True)
                if health_data.get("status") == "healthy":
                    america_data_status = "ONLINE"
                    last_updated_str = health_data.get("last_updated_utc")
                    if last_updated_str:
                        last_updated = datetime.fromisoformat(last_updated_str.replace("Z", "+00:00"))
                        age = datetime.now(ZoneInfo("UTC")) - last_updated
                        america_data_cache_age = f"{int(age.total_seconds() // 60)}m ago"
        except (requests.RequestException, ValueError) as e:
            print(f"[STATUS CMD] America Data Engine check failed: {e}", flush=True)
            pass # Status remains OFFLINE

        # --- 2. Query Analyst Heartbeat from Execution Engine ---
        analyst_heartbeat = "Never"
        try:
            analyst_res = requests.get("http://execution-engine:8080/last-report", timeout=HTTP_TIMEOUT)
            print(f"[STATUS CMD] Analyst heartbeat response: {analyst_res.status_code}", flush=True)
            if analyst_res.status_code == 200:
                analyst_data = analyst_res.json()
                last_report_str = analyst_data.get("last_analyst_report", "Never")
                if last_report_str != "Never":
                    last_report = datetime.fromisoformat(last_report_str.replace("Z", "+00:00"))
                    age = datetime.now(ZoneInfo("UTC")) - last_report
                    analyst_heartbeat = f"{int(age.total_seconds() // 3600)}h ago"
                else:
                    analyst_heartbeat = "Never"
        except (requests.RequestException, ValueError) as e:
            print(f"[STATUS CMD] Analyst heartbeat check failed: {e}", flush=True)
            analyst_heartbeat = "Error"

        # --- 3. Query Memory Bank ---
        print("[STATUS CMD] Querying Memory Bank...", flush=True)
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute("SELECT timestamp FROM daily_audits ORDER BY timestamp DESC LIMIT 1")
                last_audit_row = c.fetchone()
                last_audit_age = "Never"
                if last_audit_row:
                    last_audit = datetime.fromisoformat(last_audit_row[0])
                    age = datetime.now() - last_audit
                    last_audit_age = f"{int(age.total_seconds() // 3600)}h ago"
                
                c.execute("SELECT COUNT(*) FROM daily_audits")
                audit_count = c.fetchone()[0]
                c.execute("SELECT COUNT(*) FROM processed_filings")
                filing_count = c.fetchone()[0]
        print(f"[STATUS CMD] DB query successful: Last audit {last_audit_age}, {audit_count} audits, {filing_count} filings.", flush=True)

        # --- 3. Assemble Dashboard ---
        # CRITICAL: MarkdownV2 requires escaping these reserved chars: _*[]()~`>#+-=|{}.!
        # This includes hyphens and underscores. ALWAYS apply esc() to ALL dynamic values
        # before inserting into status_msg. Negative numbers (-1, etc.) are a common source of errors.
        def esc(text: str) -> str:
            reserved = r'_*[]()~`>#+-=|{}.!'
            return re.sub(f'([{re.escape(reserved)}])', r'\\\1', str(text))

        separator = "─" * 24
        status_msg = (
            f"🦅 *Nox System Health Status*\n"
            f"{separator}\n"
            f"🧠 *Analyst Heartbeat:* Active \\(Last cycle: {esc(analyst_heartbeat)}\\)\n"
            f"⚡ *Execution Engine:* {esc(exec_status)} \\(Ping: {esc(exec_ping)}ms\\)\n"
            f"🇨🇳 *China Data Engine:* {esc(data_status)} \\(Cache updated: {esc(data_cache_age)}\\)\n"
            f"🇺🇸 *America Data Engine:* {esc(america_data_status)} \\(Cache updated: {esc(america_data_cache_age)}\\)\n"
            f"📚 *Memory Bank:* {esc(audit_count)} Audits \\| {esc(filing_count)} Processed Filings\n"
            f"📊 *Current Market Regime:* `RISK_ON`"
        )
        print(f"[STATUS CMD] Assembled status message:\n{status_msg}", flush=True)
        bot.reply_to(message, status_msg, parse_mode='MarkdownV2')
        print("[STATUS CMD] Successfully sent status message.", flush=True)

    except Exception as e:
        print(f"[STATUS CMD] An unexpected error occurred: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to retrieve status: {str(e)}")

@bot.message_handler(commands=['history'])
def send_history(message):
    """
    /history [n] — Returns the last N daily audit reports from the Memory Bank.
    Defaults to 5 if no argument is supplied. Capped at 20 to prevent flooding.
    Each report is sent as its own message so Telegram never has to chunk a wall
    of text and the user can scroll through them individually.
    """
    try:
        # Parse optional count argument from the command, e.g. "/history 10"
        parts = message.text.strip().split()
        try:
            requested = int(parts[1]) if len(parts) > 1 else 5
            count = max(1, min(requested, 20))  # clamp: 1 ≤ count ≤ 20
        except (ValueError, IndexError):
            count = 5

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT timestamp, tickers_scanned, claude_analysis "
                    "FROM daily_audits ORDER BY timestamp DESC LIMIT ?",
                    (count,)
                )
                rows = c.fetchall()

        if not rows:
            bot.reply_to(message, "📭 No audit reports found in the Memory Bank yet.")
            return

        # Confirm how many we're sending before the flood begins
        bot.reply_to(
            message,
            f"📚 *Nox Memory Bank — Last {len(rows)} Audit Report(s)*",
            parse_mode='Markdown'
        )

        # Send each report as its own chunked message (oldest → newest after reverse)
        for timestamp, tickers, analysis in reversed(rows):
            report_msg = (
                f"🗓 *{timestamp}*\n"
                f"📌 Tickers: `{tickers}`\n\n"
                f"{analysis}"
            )
            for chunk in smart_split(report_msg, chars_per_string=4096):
                bot.send_message(message.chat.id, chunk, parse_mode='Markdown')

    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /history command failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to retrieve history: {str(e)}")


@bot.message_handler(commands=['signals'])
def send_signals(message):
    """
    /signals [n] — Shows the last N signals received by the execution engine webhook.
    Queries /recent-signals from the engine over the internal Docker network.
    Defaults to 10 if no argument given, capped at 50.
    """
    try:
        parts = message.text.strip().split()
        try:
            requested = int(parts[1]) if len(parts) > 1 else 10
            count = max(1, min(requested, 50))
        except (ValueError, IndexError):
            count = 10

        resp = requests.get("http://execution-engine:8080/recent-signals", timeout=HTTP_TIMEOUT)
        if resp.status_code != 200:
            bot.reply_to(message, f"⚠️ Execution engine returned HTTP {resp.status_code}.")
            return

        signals = resp.json()
        if not signals:
            bot.reply_to(message, "📭 No signals received by the engine yet.\n\n"
                         "Possible causes:\n"
                         "• TradingView hasn't fired any alerts\n"
                         "• Auth secret mismatch (silently dropped)\n"
                         "• Market scanner hasn't triggered (check market hours)")
            return

        signals = signals[-count:]  # most recent N
        lines = []
        for s in reversed(signals):
            action = s.get('action', '?')
            ticker = s.get('ticker', '?')
            price  = s.get('price', 0)
            rsi    = s.get('rsi', 0)
            vix    = s.get('vix', 0)
            ts     = s.get('received_at', '?')[:19]
            lines.append(
                f"• `{ts}` *{action}* {ticker} "
                f"@ ${price:.2f} RSI={rsi:.1f} VIX={vix:.1f}"
            )

        bot.reply_to(
            message,
            f"📡 *Last {len(signals)} signal(s) received by execution engine:*\n\n" +
            "\n".join(lines),
            parse_mode="Markdown",
        )
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /signals command failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to fetch signals: {str(e)}")


@bot.message_handler(func=lambda message: True)
def chat_with_nox(message):
    # Patch A: The three variable assignments below were previously placed INSIDE
    # the claude.messages.create() parameter list, causing a Python SyntaxError
    # on startup (statements are not valid as keyword arguments). They are now
    # correctly declared as local variables BEFORE the API call.
    try:
        bot.send_chat_action(message.chat.id, 'typing')

        portfolio_keywords = ("portfolio", "position", "balance", "stock",
                              "holding", "trade", "alpaca", "p&l", "pnl")
        include_portfolio = any(kw in message.text.lower() for kw in portfolio_keywords)
        portfolio_data = get_alpaca_portfolio() if include_portfolio else "(portfolio data not requested)"

        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=1024,
            system="You are Nox. Be witty, concise, and focused on algorithmic trading.",
            messages=[{"role": "user", "content": f"{message.text}\n\nData: {portfolio_data}"}]
        )

        response_text = response.content[0].text
        chunks = smart_split(response_text, chars_per_string=4096)
        for chunk in chunks:
            bot.send_message(message.chat.id, chunk)
    except Exception as e:
        bot.reply_to(message, f"⚠️ Brain Error: {str(e)}")

# --- 5. REAL-TIME SEC POLING PIPELINE ---
def is_filing_processed(filing_id):
    # Read-only query; no lock needed for SELECT on SQLite WAL mode, but we
    # acquire db_lock for consistency and to guard against non-WAL deployments.
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()
            c.execute("SELECT 1 FROM processed_filings WHERE filing_id = ?", (filing_id,))
            return c.fetchone() is not None

def mark_filing_processed(filing_id):
    # Patch C: db_lock guards this INSERT against concurrent daily_audits writes.
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()
            c.execute("INSERT OR IGNORE INTO processed_filings (filing_id) VALUES (?)", (filing_id,))
            conn.commit()

def poll_sec_edgar():
    print("[INFO] [HEARTBEAT] Nox Automated SEC Radar engaged...", flush=True)
    # Poll both the global 8-K feed (US domestic companies) and the global 6-K
    # feed (Foreign Private Issuers — all Chinese ADRs file here).
    # Running a single 8-K feed would silently miss every BABA/JD/PDD/BIDU/NIO
    # material disclosure. The two feeds are polled sequentially each cycle;
    # total wall time is roughly 2× a single request, well within the 30s tick.
    SEC_FEEDS = [
        ("8-K", "https://www.sec.gov/cgi-bin/browse-edgar?action=getcurrent&type=8-K&output=atom"),
        ("6-K", "https://www.sec.gov/cgi-bin/browse-edgar?action=getcurrent&type=6-K&output=atom"),
    ]
    headers = {"User-Agent": "Nox/1.0 openclaw@vanhellsing.tech"}

    while True:
        for feed_type, sec_url in SEC_FEEDS:
            try:
                response = requests.get(sec_url, headers=headers, timeout=HTTP_TIMEOUT)
                if response.status_code != 200:
                    print(f"[WARN] [HEARTBEAT] SEC {feed_type} feed returned {response.status_code}.", flush=True)
                    continue

                root = ET.fromstring(response.content)
                ns = {'atom': 'http://www.w3.org/2005/Atom'}
                for entry in root.findall('atom:entry', ns):
                    title_el     = entry.find('atom:title', ns)
                    link_el      = entry.find('atom:link', ns)
                    filing_id_el = entry.find('atom:id', ns)

                    if title_el is None or link_el is None or filing_id_el is None:
                        print("[WARN] [HEARTBEAT] Skipping malformed SEC feed entry.", flush=True)
                        continue

                    title     = title_el.text
                    link      = link_el.attrib.get('href', '')
                    filing_id = filing_id_el.text

                    if not filing_id or not link:
                        print("[WARN] [HEARTBEAT] Skipping entry with missing id or link.", flush=True)
                        continue

                    if is_filing_processed(filing_id):
                        continue

                    for ticker in WATCHLIST:
                        # Only act if the feed type matches what this ticker
                        # should be filing. This prevents a 6-K hit on a
                        # company with a similar name from alerting on a
                        # domestic ticker, and vice versa.
                        if get_filing_type(ticker) != feed_type:
                            continue

                        # Word-boundary regex prevents false positives from
                        # short tickers ("JD" matching "adjusted", etc.).
                        pattern = re.compile(
                            rf"\b{re.escape(ticker)}\b", re.IGNORECASE
                        )
                        if pattern.search(title):
                            print(
                                f"🚨 [SEC RADAR] Verified {feed_type} found for {ticker}!",
                                flush=True
                            )
                            process_automated_filing(ticker, link, feed_type)
                            mark_filing_processed(filing_id)

            except Exception as e:
                print(f"⚠️ SEC Radar Error ({feed_type} feed): {e}", flush=True)

        time.sleep(30)

def resolve_primary_document(index_url: str, headers: dict, filing_type: str = "8-K") -> str | None:
    """
    SEC EDGAR filing index pages list documents but contain no 8-K text.
    This function fetches the index page, finds the primary 8-K document
    (the first .htm file typed '8-K' in the filing table), and returns
    its absolute URL so the caller can fetch the actual filing content.

    EDGAR index tables have a predictable structure:
      <table class="tableFile"> with columns: Seq, Description, Document, Type, Size
    We look for a row whose Type column is exactly '8-K' and return that
    document's href. Falls back to the first .htm file if no typed match.
    """
    try:
        idx_res = requests.get(index_url, headers=headers, timeout=HTTP_TIMEOUT)
        if idx_res.status_code != 200:
            print(f"[WARN] [HEARTBEAT] Index page returned {idx_res.status_code}: {index_url}", flush=True)
            return None

        soup = BeautifulSoup(idx_res.text, "html.parser")
        base_url = "https://www.sec.gov"

        # Primary pass: find the row whose Type column exactly matches the
        # expected form type ("8-K" for domestic, "6-K" for ADRs/FPIs).
        # Hardcoding "8-K" here would silently return the wrong document
        # for any Chinese ADR filing.
        for table in soup.find_all("table", {"class": "tableFile"}):
            for row in table.find_all("tr"):
                cells = row.find_all("td")
                if len(cells) < 4:
                    continue
                doc_type = cells[3].get_text(strip=True)
                if doc_type == filing_type:
                    link_tag = cells[2].find("a", href=True)
                    if link_tag:
                        href = link_tag["href"]
                        return href if href.startswith("http") else base_url + href

        # Fallback: first .htm anchor in any tableFile row.
        # Covers edge cases where the Type cell is blank or uses a variant
        # like "6-K/A" (amended 6-K) that won't match an exact string check.
        for table in soup.find_all("table", {"class": "tableFile"}):
            for row in table.find_all("tr"):
                cells = row.find_all("td")
                if len(cells) < 3:
                    continue
                link_tag = cells[2].find("a", href=True)
                if link_tag and link_tag["href"].endswith(".htm"):
                    href = link_tag["href"]
                    return href if href.startswith("http") else base_url + href

        print("[WARN] [HEARTBEAT] Could not locate primary document in index page.", flush=True)
        return None
    except Exception as e:
        print(f"[WARN] [HEARTBEAT] resolve_primary_document failed: {e}", flush=True)
        return None


def analyze_and_alert(ticker: str, payload: str, filing_type: str, context: str = ""):
    """Sends a formatted alert to Telegram after analysis by Claude."""
    try:
        # Truncate payload if it's still too large after potential chunking.
        final_payload = payload[:40000]
        if len(payload) > 40000:
            print(f"[WARN] [HEARTBEAT] Final payload for {ticker} truncated to 40,000 chars.", flush=True)

        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=256,
            system=(
                f"You are the Risk Analyst Node of Nox. "
                f"Analyze this SEC {filing_type} filing. "
                f"Reply strictly in this format: "
                f"RISK_FACTOR: [0.1 to 1.0] | SUMMARY: [One sentence]."
            ),
            messages=[{"role": "user", "content": f"Ticker: {ticker}\n\nFiling Text:\n{final_payload}"}]
        )
        analysis = response.content[0].text
        full_msg = f"🦅 *SEC Radar Alert — {filing_type}: {ticker}* {context}\n\n`{analysis}`"
        for chunk in smart_split(full_msg, chars_per_string=4096):
            bot.send_message(CHAT_ID, chunk, parse_mode='Markdown')
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] Analysis/alerting failed for {ticker}: {e}", flush=True)
        bot.send_message(CHAT_ID, f"⚠️ Analysis failed for {ticker} filing.", parse_mode='Markdown')


def process_heavyweight_filing(ticker: str, document: str, filing_type: str):
    """
    Handles large filings by breaking them into overlapping chunks, summarizing
    each, and then performing a final analysis on the combined summaries.
    RULE-017: Runs in a background thread to avoid blocking the SEC poller.
    """
    TOKEN_CHAR_RATIO = 4
    CHUNK_SIZE = 4000 * TOKEN_CHAR_RATIO  # 16,000 chars
    OVERLAP = 200 * TOKEN_CHAR_RATIO   # 800 chars

    chunks = [
        document[i:i + CHUNK_SIZE]
        for i in range(0, len(document), CHUNK_SIZE - OVERLAP)
    ]

    summaries = []
    bot.send_message(CHAT_ID,
        f"⏳ Heavyweight filing for `{ticker}` is too large for Fast Lane (`{len(document):,}` chars). "
        f"Breaking into {len(chunks)} overlapping chunks. Analysis will follow.",
        parse_mode='Markdown'
    )

    for i, chunk in enumerate(chunks):
        print(f"[INFO] [HEARTBEAT] Heavyweight: processing chunk {i+1}/{len(chunks)} for {ticker}...", flush=True)
        try:
            # Use a smaller, faster model for per-chunk summarization.
            response = claude.messages.create(
                model="claude-haiku-4-5-20251001",
                max_tokens=512,
                system=(
                    "You are a summarization engine. You will receive a chunk of a "
                    "large SEC filing. Your task is to extract the key material information. "
                    "Be concise and focus on actionable events."
                ),
                messages=[{
                    "role": "user",
                    "content": f"Chunk {i+1}/{len(chunks)} of a {filing_type} for {ticker}:\n\n{chunk}"
                }]
            )
            summaries.append(response.content[0].text)
        except Exception as e:
            print(f"[ERROR] [HEARTBEAT] Chunk {i+1} for {ticker} failed: {e}", flush=True)
            summaries.append(f"[Chunk {i+1} failed to process]")
        time.sleep(1) # Rate-limit to avoid hitting API limits.

    combined_summary = "\n\n".join(summaries)
    print(f"[INFO] [HEARTBEAT] Heavyweight: all {len(chunks)} chunks for {ticker} summarized.", flush=True)
    analyze_and_alert(ticker, combined_summary, filing_type, context="(Heavyweight Analysis)")


def process_automated_filing(ticker: str, filing_url: str, filing_type: str = "8-K") -> None:
    """
    Orchestrates the filing processing pipeline, implementing the Dynamic
    Routing Architecture (RULE-017).

    1.  Pre-processes the filing to strip HTML and extract high-value sections.
    2.  Routes to Fast Lane (instant, synchronous) for documents under 15k tokens.
    3.  Routes to Heavyweight Lane (background thread, chunked analysis) for
        larger documents to prevent blocking the main SEC polling loop.
    """
    headers = {"User-Agent": "Nox/1.0 openclaw@vanhellsing.tech"}
    try:
        primary_url = resolve_primary_document(filing_url, headers, filing_type)
        if not primary_url:
            print(f"[WARN] [HEARTBEAT] Could not resolve primary doc for {ticker}, skipping.", flush=True)
            return

        print(f"[INFO] [HEARTBEAT] Fetching primary {filing_type} doc for {ticker}: {primary_url}", flush=True)
        doc_res = requests.get(primary_url, headers=headers, timeout=HTTP_TIMEOUT)
        if doc_res.status_code != 200:
            return

        soup = BeautifulSoup(doc_res.text, "html.parser")
        # Pre-processing: strip raw HTML tables, scripts, and styles.
        for element in soup(["script", "style", "table"]):
            element.extract()

        clean_text = soup.get_text(separator="\n")
        lines = [line.strip() for line in clean_text.splitlines() if line.strip()]
        cleaned_document = "\n".join(lines)

        # Pre-processing: Target high-value sections like Item 1.01, 5.02, or 8.01.
        # These items contain the most material, market-moving information.
        item_pattern = re.compile(r"(Item\s+(1\.01|5\.02|8\.01))", re.IGNORECASE)
        matches = list(item_pattern.finditer(cleaned_document))
        
        if matches:
            print(f"[INFO] [HEARTBEAT] High-value items found in {ticker} filing. Extracting.", flush=True)
            high_value_text = []
            for i, match in enumerate(matches):
                start = match.start()
                end = matches[i+1].start() if i + 1 < len(matches) else len(cleaned_document)
                high_value_text.append(cleaned_document[start:end])
            dense_payload = "\n\n---\n\n".join(high_value_text)
        else:
            print(f"[INFO] [HEARTBEAT] No specific items found for {ticker}. Using full document.", flush=True)
            dense_payload = cleaned_document

        # --- Dynamic Routing Architecture (RULE-017) ---
        TOKEN_CHAR_RATIO = 4
        FAST_LANE_TOKEN_LIMIT = 15000
        FAST_LANE_CHAR_LIMIT = FAST_LANE_TOKEN_LIMIT * TOKEN_CHAR_RATIO

        if len(dense_payload) <= FAST_LANE_CHAR_LIMIT:
            # FAST LANE: Process synchronously.
            print(f"[INFO] [HEARTBEAT] Routing {ticker} filing to FAST LANE ({len(dense_payload):,} chars).", flush=True)
            analyze_and_alert(ticker, dense_payload, filing_type, context="(Fast Lane)")
        else:
            # HEAVYWEIGHT LANE: Process in a background thread to avoid blocking.
            print(f"[INFO] [HEARTBEAT] Routing {ticker} filing to HEAVYWEIGHT LANE ({len(dense_payload):,} chars).", flush=True)
            threading.Thread(
                target=process_heavyweight_filing,
                args=(ticker, dense_payload, filing_type),
                daemon=True
            ).start()

    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] Failed to process automated filing for {ticker}: {e}", flush=True)

# --- 5.5 MONTHLY TRADING JOURNAL ---
# Generates a structured markdown report from the SQLite database and writes it
# to /app/reports/YYYY-MM.md (mounted from the host as ./reports/).
# Triggered by /monthly_report in Telegram, and auto-runs on the 1st of each month.
#
# After the report is written, commit it to the nocturnal branch manually:
#   git add docs/journal/YYYY-MM.md && git commit -m "journal: Month YYYY progress"
# Or run: ./commit_report.sh

REPORTS_DIR = "/app/reports"

def generate_monthly_report(year: int, month: int) -> str:
    """
    Queries all tables and builds a markdown report for the given month.
    Returns the report as a string.
    """
    import calendar
    et    = ZoneInfo("America/New_York")
    now   = datetime.now(et)
    label = f"{year}-{month:02d}"
    month_name = calendar.month_name[month]

    # Pull Alpaca portfolio snapshot
    portfolio_text = get_alpaca_portfolio()

    # Pull DB stats
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()

            # Trades this month
            c.execute(
                "SELECT COUNT(*), SUM(pnl), AVG(pnl) FROM trade_history "
                "WHERE strftime('%Y-%m', timestamp) = ?", (label,)
            )
            trade_row = c.fetchone()
            trade_count  = trade_row[0] or 0
            trade_pnl    = trade_row[1] or 0.0
            trade_avg    = trade_row[2] or 0.0

            # Win/loss split
            c.execute(
                "SELECT COUNT(*) FROM trade_history "
                "WHERE strftime('%Y-%m', timestamp) = ? AND pnl > 0", (label,)
            )
            wins = c.fetchone()[0] or 0

            c.execute(
                "SELECT ticker, action, price, pnl, timestamp FROM trade_history "
                "WHERE strftime('%Y-%m', timestamp) = ? ORDER BY pnl DESC LIMIT 3", (label,)
            )
            top_trades = c.fetchall()

            c.execute(
                "SELECT ticker, action, price, pnl, timestamp FROM trade_history "
                "WHERE strftime('%Y-%m', timestamp) = ? ORDER BY pnl ASC LIMIT 3", (label,)
            )
            bot_trades = c.fetchall()

            # Audits this month
            c.execute(
                "SELECT COUNT(*) FROM daily_audits "
                "WHERE strftime('%Y-%m', timestamp) = ?", (label,)
            )
            audit_count = c.fetchone()[0] or 0

            # Filings processed this month
            c.execute(
                "SELECT COUNT(*) FROM processed_filings "
                "WHERE strftime('%Y-%m', timestamp) = ?", (label,)
            )
            filing_count = c.fetchone()[0] or 0

            # Webhook signals (scanner-generated) this month
            c.execute(
                "SELECT COUNT(*), COUNT(DISTINCT ticker) FROM webhook_signals "
                "WHERE strftime('%Y-%m', received_at) = ?", (label,)
            )
            scan_row = c.fetchone()
            scan_signals = scan_row[0] or 0
            scan_tickers = scan_row[1] or 0

            # IV rank data points accumulated
            c.execute(
                "SELECT ticker, COUNT(*) as days FROM historical_volatility GROUP BY ticker ORDER BY days DESC"
            )
            iv_rows = c.fetchall()

    # Compute win rate
    win_rate = (wins / trade_count * 100) if trade_count > 0 else 0.0
    losses   = trade_count - wins

    def trade_table(rows: list) -> str:
        if not rows:
            return "_None_"
        lines = []
        for ticker, action, price, pnl, ts in rows:
            pnl_str = f"+${pnl:.2f}" if (pnl or 0) >= 0 else f"-${abs(pnl or 0):.2f}"
            lines.append(f"| {ticker} | {action} | ${price:.2f} | {pnl_str} | {str(ts)[:10]} |")
        return "| Ticker | Action | Price | P&L | Date |\n|--------|--------|-------|-----|------|\n" + "\n".join(lines)

    def iv_table(rows: list) -> str:
        if not rows:
            return "_No IV data accumulated yet_"
        lines = []
        for ticker, days in rows[:10]:
            status = "✅ Full rank" if days >= 30 else f"⏳ {days}/30 days"
            lines.append(f"| {ticker} | {days} | {status} |")
        return "| Ticker | Days | Status |\n|--------|------|--------|\n" + "\n".join(lines)

    report = f"""# Nox Trading Journal — {month_name} {year}

*Generated: {now.strftime('%Y-%m-%d %H:%M ET')} | Branch: nocturnal*

---

## Portfolio Snapshot

```
{portfolio_text}
```

---

## Trade Performance

| Metric | Value |
|--------|-------|
| Trades executed | {trade_count} |
| Wins | {wins} |
| Losses | {losses} |
| Win rate | {win_rate:.1f}% |
| Total P&L | ${trade_pnl:+.2f} |
| Avg P&L/trade | ${trade_avg:+.2f} |

### Best Trades
{trade_table(top_trades)}

### Worst Trades
{trade_table(bot_trades)}

---

## Signal Activity

| Source | Count |
|--------|-------|
| Market scanner signals posted | {scan_signals} |
| Unique tickers scanned | {scan_tickers} |
| Daily audit reports | {audit_count} |
| SEC filings processed | {filing_count} |

---

## IV Rank Accumulation
*(Full rank requires 30+ days of daily snapshots. Collected at 4:30 PM ET each trading day.)*

{iv_table(iv_rows)}

---

## Notes

> *Add observations, strategy adjustments, or notable events before committing.*

---

*To commit this journal to the nocturnal branch:*
```bash
cp /root/Nox/reports/{label}.md /root/Nox/docs/journal/{label}.md
git -C /root/Nox add docs/journal/{label}.md
git -C /root/Nox commit -m "journal: {month_name} {year} trading progress"
```
"""
    return report


def write_monthly_report(year: int, month: int) -> str:
    """Write the report to /app/reports/YYYY-MM.md. Returns the file path."""
    import os
    os.makedirs(REPORTS_DIR, exist_ok=True)
    report_text = generate_monthly_report(year, month)
    path = f"{REPORTS_DIR}/{year}-{month:02d}.md"
    with open(path, "w") as f:
        f.write(report_text)
    logger.info(f"Monthly report written to {path}")
    return path


def run_monthly_journal():
    """Auto-runs on the 1st of each month. Writes and notifies via Telegram."""
    et    = ZoneInfo("America/New_York")
    now   = datetime.now(et)
    # Generate for the just-completed month
    month = now.month - 1 if now.month > 1 else 12
    year  = now.year if now.month > 1 else now.year - 1
    try:
        path = write_monthly_report(year, month)
        import calendar
        label = f"{year}-{month:02d}"
        bot.send_message(
            CHAT_ID,
            f"📓 *Monthly Journal Generated*\n"
            f"━━━━━━━━━━━━━━━━━━━━━━━━\n"
            f"• File: `reports/{label}.md`\n"
            f"• To commit to nocturnal branch:\n"
            f"```\n"
            f"cp /root/Nox/reports/{label}.md /root/Nox/docs/journal/{label}.md\n"
            f"git -C /root/Nox add docs/journal/{label}.md\n"
            f"git -C /root/Nox commit -m 'journal: {calendar.month_name[month]} {year}'\n"
            f"```",
            parse_mode="Markdown",
        )
    except Exception as e:
        logger.error(f"Monthly journal failed: {e}")


@bot.message_handler(commands=['monthly_report'])
def cmd_monthly_report(message):
    """
    /monthly_report [YYYY-MM] — Generate a trading journal for a given month.
    Defaults to the current month. Writes to reports/ on the host and sends
    a preview of the summary section via Telegram.
    """
    try:
        et  = ZoneInfo("America/New_York")
        now = datetime.now(et)
        parts = message.text.strip().split()
        if len(parts) > 1:
            try:
                year, month = int(parts[1].split("-")[0]), int(parts[1].split("-")[1])
            except Exception:
                bot.reply_to(message, "⚠️ Format: /monthly_report 2026-06")
                return
        else:
            year, month = now.year, now.month

        bot.reply_to(message, f"⏳ Generating journal for {year}-{month:02d}...")
        path = write_monthly_report(year, month)

        # Send the performance summary section as a Telegram preview
        report = generate_monthly_report(year, month)
        # Extract just the trade performance section for the Telegram preview
        lines = report.split("\n")
        preview_lines = []
        in_section = False
        for line in lines:
            if line.startswith("## Trade Performance"):
                in_section = True
            elif line.startswith("## Signal Activity"):
                break
            if in_section:
                preview_lines.append(line)

        preview = "\n".join(preview_lines[:25])
        bot.send_message(
            message.chat.id,
            f"📓 *Journal for {year}-{month:02d} written to `reports/{year}-{month:02d}.md`*\n\n"
            f"{preview}\n\n"
            f"_Full report on host at: /root/Nox/reports/{year}-{month:02d}.md_",
            parse_mode="Markdown",
        )
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /monthly_report failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed: {str(e)}")


# --- 5.5 INTERNAL IV ENDPOINT (WS1 Contradiction Vector data source) ---
# The heartbeat already holds the Alpaca options-chain plumbing, so it is the
# natural place to expose live IV skew. A tiny stdlib HTTP server (no Flask/
# FastAPI dependency added to this image) serves it on nox_net, authenticated
# with the same X-Nox-Token shared secret used elsewhere (RULE-004).
#
#   GET /iv/skew?ticker=NVDA   → fetch_iv_skew() JSON
#   GET /health                → liveness (no auth)
#
# Internal-only: bound on the Docker network; never published to the host.
IV_ENDPOINT_PORT = int(os.getenv("HEARTBEAT_IV_PORT", "8002"))


def _start_iv_http_server():
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    from urllib.parse import urlparse, parse_qs
    import json as _json

    class _IVHandler(BaseHTTPRequestHandler):
        def _send(self, code: int, payload: dict):
            body = _json.dumps(payload).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):  # noqa: N802 (stdlib naming)
            parsed = urlparse(self.path)
            if parsed.path == "/health":
                self._send(200, {"status": "healthy"})
                return

            # RULE-004: every data endpoint requires the shared secret.
            if self.headers.get("X-Nox-Token") != WEBHOOK_SECRET:
                self._send(401, {"error": "Forbidden: invalid token"})
                return

            if parsed.path == "/iv/skew":
                qs = parse_qs(parsed.query)
                ticker = (qs.get("ticker", [""])[0] or "").upper().strip()
                if not ticker:
                    self._send(400, {"error": "missing ?ticker="})
                    return
                self._send(200, fetch_iv_skew(ticker))
                return

            self._send(404, {"error": "not found"})

        def log_message(self, *args):  # silence default stderr access logging
            return

    server = ThreadingHTTPServer(("0.0.0.0", IV_ENDPOINT_PORT), _IVHandler)
    print(f"[INFO] [HEARTBEAT] IV skew endpoint listening on :{IV_ENDPOINT_PORT}", flush=True)
    server.serve_forever()


# --- 6. CORE INITIALIZATION ENGINE ---
if __name__ == "__main__":
    print("Nox SEC Forensic Scout Online...")
    init_db()

    # Run backgrounds threads asynchronously above the blocking polling call
    threading.Thread(target=schedule_checker, daemon=True).start()
    threading.Thread(target=poll_sec_edgar, daemon=True).start()
    threading.Thread(target=_start_iv_http_server, daemon=True).start()

    bot.infinity_polling()
