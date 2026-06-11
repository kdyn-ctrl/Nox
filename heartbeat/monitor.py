import os
import telebot
import requests
import anthropic
import schedule
import time
import threading
import xml.etree.ElementTree as ET
import sqlite3
from datetime import datetime
from telebot.util import smart_split
from bs4 import BeautifulSoup

# --- 1. CONFIGURATION ---
TELEGRAM_TOKEN = os.getenv('TELEGRAM_BOT_TOKEN')
CHAT_ID = os.getenv('TELEGRAM_CHAT_ID')
ANTHROPIC_KEY = os.getenv('ANTHROPIC_API_KEY')
ALPACA_API = os.getenv('ALPACA_API_KEY')
ALPACA_SEC = os.getenv('ALPACA_SECRET_KEY')

bot = telebot.TeleBot(TELEGRAM_TOKEN)
claude = anthropic.Anthropic(api_key=ANTHROPIC_KEY)

DB_PATH = '/app/data/memory_bank.db'
WATCHLIST = ["AAPL", "TSLA", "NVDA", "MSFT"]

# --- 1.5 THE MEMORY BANK ---
def init_db():
    """Creates the database and tables if they don't exist"""
    try:
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
def get_alpaca_portfolio():
    headers = {'APCA-API-KEY-ID': ALPACA_API, 'APCA-API-SECRET-KEY': ALPACA_SEC}
    try:
        acc_resp = requests.get('https://paper-api.alpaca.markets/v2/account', headers=headers, timeout=10)
        pos_resp = requests.get('https://paper-api.alpaca.markets/v2/positions', headers=headers, timeout=10)
        
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

def get_sec_8k(ticker):
    url = f"https://www.sec.gov/cgi-bin/browse-edgar?action=getcompany&CIK={ticker}&type=8-K&output=atom"
    headers = {'User-Agent': 'OpenClaw Swarm openclaw@vanhellsing.tech'}
    try:
        resp = requests.get(url, headers=headers, timeout=10)
        root = ET.fromstring(resp.content)
        ns = {'atom': 'http://www.w3.org/2005/Atom'}
        entries = root.findall('atom:entry', ns)
        if not entries: return f"No recent 8-Ks for {ticker}."
        
        latest = entries[0]
        title = latest.find('atom:title', ns).text
        summary = latest.find('atom:summary', ns).text
        return f"RAW SEC 8-K ({ticker}): {title} - {summary}"
    except Exception as e:
        return f"SEC Pull Failed for {ticker}"

# --- 3. THE SCOUT PROTOCOL (DAILY REPORT) ---
def run_scout_protocol():
    try:
        headers = {'APCA-API-KEY-ID': ALPACA_API, 'APCA-API-SECRET-KEY': ALPACA_SEC}
        news_req = requests.get('https://data.alpaca.markets/v1beta1/news?symbols=NVDA,XOM,BTCUSD&limit=5', headers=headers, timeout=10).json()
        news_context = "\n".join([f"- {n['headline']}" for n in news_req.get('news', [])])
        
        sec_context = "\n".join([get_sec_8k(ticker) for ticker in ["NVDA", "XOM"]])
        
        system_prompt = (
            "You are Nox, a ruthlessly skeptical quantitative trading assistant. "
            "Filter this market news and raw SEC 8-K data to find medium-term, structural shifts. "
            "Output a concise, highly actionable, and highly cynical market report."
        )
        
        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=350,
            system=system_prompt,
            messages=[{"role": "user", "content": f"Media Headlines:\n{news_context}\n\nSEC Filings:\n{sec_context}"}]
        )

        analysis_text = response.content[0].text
        bot.send_message(CHAT_ID, f"🦅 *Nox Daily Audit Report*\n\n{analysis_text}", parse_mode='Markdown')
        
        # Log to DB safely inside the successful try block
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()
            c.execute("INSERT INTO daily_audits (tickers_scanned, claude_analysis) VALUES (?, ?)", 
                      ("NVDA, XOM, BTCUSD", analysis_text))
            conn.commit()
    except Exception as e:
        print(f"Scout Error: {e}")

def schedule_checker():
    schedule.every().day.at("11:30").do(run_scout_protocol) 
    while True:
        schedule.run_pending()
        time.sleep(60)

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

@bot.message_handler(func=lambda message: True)
def chat_with_nox(message):
    try:
        bot.send_chat_action(message.chat.id, 'typing')
        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=250,
            system="You are Nox. Be witty, concise, and focused on algorithmic trading.",
            messages=[{"role": "user", "content": message.text + "\n\nData: " + get_alpaca_portfolio()}]
        )

        response_text = response.content[0].text
        chunks = smart_split(response_text, chars_per_string=4096)
        for chunk in chunks:
            bot.send_message(message.chat.id, chunk)
    except Exception as e:
        bot.reply_to(message, f"⚠️ Brain Error: {str(e)}")

# --- 5. REAL-TIME SEC POLING PIPELINE ---
def is_filing_processed(filing_id):
    with sqlite3.connect(DB_PATH) as conn:
        c = conn.cursor()
        c.execute("SELECT 1 FROM processed_filings WHERE filing_id = ?", (filing_id,))
        return c.fetchone() is not None

def mark_filing_processed(filing_id):
    with sqlite3.connect(DB_PATH) as conn:
        c = conn.cursor()
        c.execute("INSERT OR IGNORE INTO processed_filings (filing_id) VALUES (?)", (filing_id,))
        conn.commit()

def poll_sec_edgar():
    print("Nox Automated SEC Radar engaged...")
    sec_url = "https://www.sec.gov/cgi-bin/browse-edgar?action=getcurrent&type=8-K&output=atom"
    headers = {"User-Agent": "OpenClawSwarm/1.0 openclaw@vanhellsing.tech"}

    while True:
        try:
            response = requests.get(sec_url, headers=headers, timeout=10)
            if response.status_code == 200:
                root = ET.fromstring(response.content)
                ns = {'atom': 'http://www.w3.org/2005/Atom'}
                for entry in root.findall('atom:entry', ns):
                    title = entry.find('atom:title', ns).text
                    link = entry.find('atom:link', ns).attrib['href']
                    filing_id = entry.find('atom:id', ns).text

                    if is_filing_processed(filing_id):
                        continue

                    for ticker in WATCHLIST:
                        if f"({ticker})" in title or ticker in title.upper():
                            print(f"🚨 [SEC RADAR] New 8-K found for {ticker}!")
                            process_automated_filing(ticker, link)
                            mark_filing_processed(filing_id)
        except Exception as e:
            print(f"⚠️ SEC Radar Error: {e}")
        time.sleep(30)

def process_automated_filing(ticker, filing_url):
    headers = {"User-Agent": "OpenClawSwarm/1.0 openclaw@vanhellsing.tech"}
    try:
        doc_res = requests.get(filing_url, headers=headers, timeout=10)
        if doc_res.status_code != 200:
            return

        soup = BeautifulSoup(doc_res.text, "html.parser")
        for element in soup(["script", "style"]):
            element.extract()

        clean_text = soup.get_text(separator="\n")
        lines = [line.strip() for line in clean_text.splitlines() if line.strip()]
        cleaned_document = "\n".join(lines)
        dense_payload = cleaned_document[:40000] 

        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=150,
            system="You are the Risk Analyst Node of OpenClaw. Analyze this SEC 8-K. Reply strictly format: RISK_FACTOR: [0.1 to 1.0] | SUMMARY: [One sentence].",
            messages=[{"role": "user", "content": f"Ticker: {ticker}\n\nFiling Text:\n{dense_payload}"}]
        )
        analysis = response.content[0].text
        bot.send_message(CHAT_ID, f"🦅 *SEC Radar Alert: {ticker}*\n\n`{analysis}`", parse_mode='Markdown')
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
