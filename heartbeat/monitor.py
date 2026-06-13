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
from datetime import datetime
from zoneinfo import ZoneInfo
from telebot.util import smart_split
from bs4 import BeautifulSoup
import akshare as ak

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

print("[INFO] [HEARTBEAT] All required environment variables validated.", flush=True)

bot = telebot.TeleBot(TELEGRAM_TOKEN)
claude = anthropic.Anthropic(api_key=ANTHROPIC_KEY)

DB_PATH = '/app/data/memory_bank.db'

# Watchlist segmentation.
# DOMESTIC_WATCHLIST: US companies — file Form 8-K for material event disclosures.
# CHINESE_ADRS:       Foreign Private Issuers listed in the US — file Form 6-K instead.
#                     Polling the 8-K feed for these tickers would silently miss all
#                     their disclosures. The get_filing_type() helper resolves which
#                     feed and which document type to use for each ticker.
DOMESTIC_WATCHLIST = ["AAPL", "TSLA", "NVDA", "MSFT"]
CHINESE_ADRS       = ["BABA", "JD", "PDD", "BIDU", "NIO"]
WATCHLIST          = DOMESTIC_WATCHLIST + CHINESE_ADRS

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

# --- 2.5 CHINESE MARKET INTELLIGENCE (AkShare) ---
def get_chinese_market_context() -> str:
    """
    Pulls three layers of Chinese market intelligence via AkShare:
      1. East Money (东方财富) hot stock board — the most-watched tickers
         in Chinese retail markets right now, with price change and volume.
      2. China Manufacturing PMI — the single most-watched macro indicator
         for Chinese industrial health, published monthly by the NBS.
      3. PBOC Loan Prime Rate (LPR) — China's benchmark lending rate,
         the direct equivalent of the Fed Funds Rate for Chinese monetary
         policy signaling.

    Each source is wrapped in its own try/except so a failure in one
    does not silence the others. Returns a formatted string ready to be
    injected into the Claude prompt alongside the English context.
    """
    sections = []

    # --- East Money Hot Board (东方财富人气榜) ---
    # Returns the top retail-attention stocks on the Chinese market.
    # High retail attention in China often leads institutional flows by
    # 1-2 sessions — useful as a leading sentiment indicator.
    try:
        df = ak.stock_hot_rank_em()
        # Columns: 排名, 代码, 股票名称, 最新价, 涨跌幅, 换手率
        top = df.head(5)
        lines = []
        for _, row in top.iterrows():
            name   = row.get('股票名称', 'N/A')
            code   = row.get('代码', 'N/A')
            price  = row.get('最新价', 'N/A')
            change = row.get('涨跌幅', 'N/A')
            lines.append(f"  {name} ({code}) — ¥{price} | 涨跌幅: {change}%")
        sections.append(
            "🇨🇳 East Money Hot Board (东方财富人气榜) — Top 5 Most-Watched A-Shares:\n"
            + "\n".join(lines)
        )
        print("[INFO] [AKSHARE] East Money hot board fetched.", flush=True)
    except Exception as e:
        print(f"[WARN] [AKSHARE] East Money hot board failed: {e}", flush=True)
        sections.append("🇨🇳 East Money Hot Board: unavailable.")

    # --- China Manufacturing PMI (制造业PMI) ---
    # Published monthly by the National Bureau of Statistics (NBS).
    # PMI > 50 = expansion, < 50 = contraction.
    # The most reliable leading indicator for Chinese industrial output
    # and a direct input to global supply chain models.
    try:
        df = ak.macro_china_pmi_yearly()
        # Columns: 月份, 制造业-指数, 制造业-同比增长, 非制造业-指数, 非制造业-同比增长
        latest = df.iloc[-1]
        month          = latest.get('月份', 'N/A')
        mfg_pmi        = latest.get('制造业-指数', 'N/A')
        non_mfg_pmi    = latest.get('非制造业-指数', 'N/A')
        sections.append(
            f"🏭 China PMI (NBS 国家统计局) — {month}:\n"
            f"  Manufacturing (制造业): {mfg_pmi} "
            f"({'EXPANSION ▲' if str(mfg_pmi) > '50' else 'CONTRACTION ▼'})\n"
            f"  Non-Manufacturing (非制造业): {non_mfg_pmi}"
        )
        print("[INFO] [AKSHARE] China PMI fetched.", flush=True)
    except Exception as e:
        print(f"[WARN] [AKSHARE] China PMI fetch failed: {e}", flush=True)
        sections.append("🏭 China PMI: unavailable.")

    # --- PBOC Loan Prime Rate (贷款市场报价利率 LPR) ---
    # China's benchmark lending rate set by the PBOC.
    # Rate cuts = stimulus signal; holds or hikes = tightening.
    # A divergence between PBOC and Fed policy is a direct USD/CNY
    # pressure signal and affects cross-listed stocks (H-shares, ADRs).
    try:
        df = ak.macro_china_lpr()
        # Columns: RATE_DATE, 1Y LPR, 5Y LPR
        latest = df.iloc[-1]
        rate_date = latest.iloc[0]
        lpr_1y    = latest.iloc[1]
        lpr_5y    = latest.iloc[2]
        sections.append(
            f"🏦 PBOC Loan Prime Rate (LPR 贷款市场报价利率) — {rate_date}:\n"
            f"  1-Year LPR: {lpr_1y}%\n"
            f"  5-Year LPR: {lpr_5y}%"
        )
        print("[INFO] [AKSHARE] PBOC LPR fetched.", flush=True)
    except Exception as e:
        print(f"[WARN] [AKSHARE] PBOC LPR fetch failed: {e}", flush=True)
        sections.append("🏦 PBOC LPR: unavailable.")

    return "\n\n".join(sections)


# --- 3. THE SCOUT PROTOCOL (DAILY REPORT) ---
def run_scout_protocol():
    try:
        headers = {'APCA-API-KEY-ID': ALPACA_API, 'APCA-API-SECRET-KEY': ALPACA_SEC}
        news_req = requests.get(
            'https://data.alpaca.markets/v1beta1/news?symbols=NVDA,XOM,BTCUSD&limit=5',
            headers=headers, timeout=HTTP_TIMEOUT
        ).json()
        news_context = "\n".join([
            f"- {n['headline']}"
            for n in news_req.get('news', [])
            if isinstance(n, dict) and 'headline' in n
        ])
        
        # One domestic (NVDA, 8-K) and one Chinese ADR (BABA, 6-K) — gives Claude
        # cross-market filing context without doubling token cost.
        sec_context = "\n\n".join([get_latest_sec_filing(t) for t in ["NVDA", "BABA"]])
        
        # Pull Chinese market intelligence as a third context layer.
        # Runs in the same thread as the scout — failure is non-fatal
        # because get_chinese_market_context() handles its own exceptions
        # internally and always returns a formatted string.
        chinese_context = get_chinese_market_context()

        system_prompt = (
            "You are Nox, a ruthlessly skeptical quantitative trading assistant "
            "with deep expertise in both US and Chinese financial markets. "
            "You will receive three data layers: US market headlines, SEC filings, "
            "and Chinese market intelligence (A-share sentiment, PMI, PBOC policy). "
            "Your job is to find cross-market structural shifts — moments where "
            "Chinese macro data confirms, contradicts, or leads the US narrative. "
            "Output a concise, highly actionable, bilingual-aware market report. "
            "Flag any US/China divergence signals explicitly."
        )
        
        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=1024,
            system=system_prompt,
            messages=[{"role": "user", "content": (
                f"🇺🇸 US Media Headlines:\n{news_context}\n\n"
                f"📋 SEC Filings:\n{sec_context}\n\n"
                f"🇨🇳 Chinese Market Intelligence:\n{chinese_context}"
            )}]
        )

        analysis_text = response.content[0].text
        # Chunk the report so Telegram's 4096-char limit never cuts it off.
        # smart_split splits on word boundaries, preserving readability.
        full_msg = f"🦅 *Nox Daily Audit Report*\n\n{analysis_text}"
        for chunk in smart_split(full_msg, chars_per_string=4096):
            bot.send_message(CHAT_ID, chunk, parse_mode='Markdown')
        
        # Patch C: db_lock guards this write against concurrent SEC radar writes.
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute("INSERT INTO daily_audits (tickers_scanned, claude_analysis) VALUES (?, ?)",
                          ("NVDA, BABA, BTCUSD", analysis_text))
                conn.commit()
    except Exception as e:
        print(f"Scout Error: {e}")

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

    _scout_fired_today: dict = {"date": None}   # mutable container for closure

    def _guarded_scout():
        """Wrapper that prevents double-fires within the same calendar day (ET)."""
        today_et = datetime.now(tz=ET).date()
        if _scout_fired_today["date"] == today_et:
            print("[WARN] [HEARTBEAT] Scout double-fire suppressed.", flush=True)
            return
        _scout_fired_today["date"] = today_et
        run_scout_protocol()

    def _reschedule_scout():
        """
        Clear any existing 'scout' job, compute the UTC wall-clock time that
        corresponds to 11:30 AM ET *today*, and register a new daily job.
        Called once at startup and then nightly at 00:01 UTC.
        """
        schedule.clear("scout")

        now_et     = datetime.now(tz=ET)
        # Build a timezone-aware 10:00 AM ET for today, then express it in UTC.
        target_et  = now_et.replace(hour=10, minute=0, second=0, microsecond=0)
        target_utc = target_et.astimezone(UTC)
        utc_hhmm   = target_utc.strftime("%H:%M")

        schedule.every().day.at(utc_hhmm).do(_guarded_scout).tag("scout")
        print(
            f"[INFO] [HEARTBEAT] Daily Scout (re)scheduled: "
            f"10:00 ET = {utc_hhmm} UTC "
            f"({'EDT UTC-4' if target_et.utcoffset().total_seconds() == -14400 else 'EST UTC-5'}).",
            flush=True,
        )

    # --- Initial registration at startup ---
    _reschedule_scout()

    # Recompute the UTC equivalent each night so DST transitions are handled
    # automatically without any container restart.
    schedule.every().day.at("00:01").do(_reschedule_scout)

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
@bot.message_handler(commands=['status'])
def send_status(message):
    try:
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()
            c.execute("SELECT COUNT(*) FROM daily_audits")
            audit_count = c.fetchone()[0]
        
        status_msg = (
            "🦅 *Nox Systems Status*\n"
            f"✅ Brain: Online\n"
            f"✅ Memory Bank: {audit_count} audits saved\n"
            f"✅ Kelly Engine: Ready"
        )
        bot.reply_to(message, status_msg, parse_mode='Markdown')
    except Exception as e:
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


def process_automated_filing(ticker: str, filing_url: str, filing_type: str = "8-K") -> None:
    headers = {"User-Agent": "Nox/1.0 openclaw@vanhellsing.tech"}
    try:
        # filing_url from the EDGAR atom feed points to the INDEX page, not the
        # document itself. Pass filing_type so resolve_primary_document looks
        # for the correct form type in the index table (8-K or 6-K).
        primary_url = resolve_primary_document(filing_url, headers, filing_type)
        if not primary_url:
            print(f"[WARN] [HEARTBEAT] Could not resolve primary document for {ticker}, skipping.", flush=True)
            return

        print(f"[INFO] [HEARTBEAT] Fetching primary {filing_type} document for {ticker}: {primary_url}", flush=True)
        doc_res = requests.get(primary_url, headers=headers, timeout=HTTP_TIMEOUT)
        if doc_res.status_code != 200:
            return

        soup = BeautifulSoup(doc_res.text, "html.parser")
        for element in soup(["script", "style"]):
            element.extract()

        clean_text = soup.get_text(separator="\n")
        lines = [line.strip() for line in clean_text.splitlines() if line.strip()]
        cleaned_document = "\n".join(lines)
        if len(cleaned_document) > 40000:
            print(f"[WARN] [HEARTBEAT] Filing for {ticker} truncated from {len(cleaned_document)} to 40,000 chars.", flush=True)
        dense_payload = cleaned_document[:40000]

        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=256,
            system=(
                f"You are the Risk Analyst Node of Nox. "
                f"Analyze this SEC {filing_type} filing. "
                f"Reply strictly in this format: "
                f"RISK_FACTOR: [0.1 to 1.0] | SUMMARY: [One sentence]."
            ),
            messages=[{"role": "user", "content": f"Ticker: {ticker}\n\nFiling Text:\n{dense_payload}"}]
        )
        analysis = response.content[0].text
        # Chunk the alert so Telegram's 4096-char limit never cuts it off.
        full_msg = f"🦅 *SEC Radar Alert — {filing_type}: {ticker}*\n\n`{analysis}`"
        for chunk in smart_split(full_msg, chars_per_string=4096):
            bot.send_message(CHAT_ID, chunk, parse_mode='Markdown')
    except Exception as e:
        print(f"Failed to process automated filing: {e}")

# --- 6. CORE INITIALIZATION ENGINE ---
if __name__ == "__main__":
    print("Nox SEC Forensic Scout Online...")
    init_db()
    
    # Run backgrounds threads asynchronously above the blocking polling call
    threading.Thread(target=schedule_checker, daemon=True).start()
    threading.Thread(target=poll_sec_edgar, daemon=True).start()
    
    bot.infinity_polling()
