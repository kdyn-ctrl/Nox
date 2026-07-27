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
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo
from telebot.util import smart_split
from bs4 import BeautifulSoup

import telemetry_watchdog
import squeeze_pead_scanner
from retry_utils import fetch_with_retry
from trading_day_utils import is_trading_day
import barbell
import job_supervisor

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

# ── Scheduled-job dead-man's switch (RULE-D3) ──────────────────────────────
# Every scheduled job below previously only logged to stdout on failure, so a
# job that failed on every run for weeks looked identical to "nothing to
# report". job_supervisor.JobSupervisor mirrors the C++ Skeptic "neutral N/M
# this scan" alarm at job granularity: it counts every run and Telegram-alerts
# (deduped, re-armed on recovery) after JOB_FAIL_ALERT_THRESHOLD consecutive
# failures. Jobs are wired at registration via supervised_job("name")(fn).
_job_supervisor = job_supervisor.JobSupervisor(
    alert_fn=lambda msg: bot.send_message(CHAT_ID, msg),
    logger=logger)


def record_job_result(name, ok, detail=""):
    _job_supervisor.record(name, ok, detail)


def supervised_job(name):
    return _job_supervisor.supervise(name)

# /sports, /sports-status, and the daily sports recap read the same shared
# memory_bank.db tables the sports_predictions container writes — these are
# just enough to know which sport/bot that container is configured for.
SPORTS_SPORT_KEY = os.getenv("SPORTS_PREDICTIONS_SPORT_KEY", "baseball_mlb")
SPORTS_SCAN_INTERVAL_MINUTES = int(os.getenv("SPORTS_SCAN_INTERVAL_MINUTES", "60"))
SPORTS_TELEGRAM_BOT_TOKEN = os.getenv("SPORTS_TELEGRAM_BOT_TOKEN", "")
SPORTS_TELEGRAM_CHAT_ID = os.getenv("SPORTS_TELEGRAM_CHAT_ID", "")
# Starting bankroll for the sports pool (2026-07-18 user decision: track a
# baseline pool + running P&L, same shape as the options barbell's
# core/satellite capital tracking). Fake-safe default 0 — an unset bankroll
# just omits bankroll display/warnings instead of sizing against a fake $0.
SPORTS_STARTING_BANKROLL = float(os.getenv("SPORTS_STARTING_BANKROLL", "0"))
# Soft-warn only (never blocks — same pattern as barbell.py's over-cap
# warning) if a single stake exceeds this fraction of the CURRENT bankroll.
# Real motivating case: a $10 stake against a $30 bankroll (33%) on a single
# 7.9%-edge MLB pick — no guardrail existed to flag that before this.
SPORTS_MAX_STAKE_PCT = float(os.getenv("SPORTS_MAX_STAKE_PCT", "0.10"))
# Parlays are all-or-nothing across every leg, so per-leg quality matters more
# than a single straight bet — this is a STRICTER bar than
# SPORTS_HIGH_CONVICTION_THRESHOLD (0.12) per explicit user request ("only
# the best of the best setups"). Soft-warn only if a leg misses it, same
# never-block-a-record rule as every other manual-entry command here.
SPORTS_PARLAY_MIN_CONFIDENCE = float(os.getenv("SPORTS_PARLAY_MIN_CONFIDENCE", "0.15"))
# Flat % of CURRENT bankroll suggested for the satellite/parlay sleeve — much
# smaller than a single-leg stake because a parlay's payout is realized only
# on a full sweep; sized as a moonshot allocation, not a repeatable edge bet.
SPORTS_PARLAY_STAKE_PCT = float(os.getenv("SPORTS_PARLAY_STAKE_PCT", "0.02"))


def _send_sports_telegram(msg: str) -> None:
    """Send to the separate sports bot, not the main `bot` (bound to
    TELEGRAM_BOT_TOKEN) — used for the scheduled daily recap, which is a
    summary, not a high-conviction alert, so it doesn't go to both bots."""
    if not SPORTS_TELEGRAM_BOT_TOKEN or not SPORTS_TELEGRAM_CHAT_ID:
        logger.info("[SPORTS] Sports Telegram not configured, skipping recap send.")
        return
    try:
        requests.post(
            f"https://api.telegram.org/bot{SPORTS_TELEGRAM_BOT_TOKEN}/sendMessage",
            json={"chat_id": SPORTS_TELEGRAM_CHAT_ID, "text": msg, "parse_mode": "Markdown"},
            timeout=HTTP_TIMEOUT,
        )
    except Exception as e:
        logger.warning(f"[SPORTS] Sports Telegram send failed: {e}")

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

# NYSE full-day closures, used by is_trading_day() to tell EOD/EOW reporting
# apart from a live trading day. Defaults to the real 2026 calendar (including
# the Jul-3 observed date for the Jul-4 Saturday holiday) so it works without
# any .env changes; override via NOX_MARKET_HOLIDAYS for other years.
_holidays_raw = os.getenv(
    "NOX_MARKET_HOLIDAYS",
    "2026-01-01,2026-01-19,2026-02-16,2026-04-03,2026-05-25,2026-06-19,"
    "2026-07-03,2026-09-07,2026-11-26,2026-12-25",
)
MARKET_HOLIDAYS = {d.strip() for d in _holidays_raw.split(",") if d.strip()}

# Daily report SEC context is pulled from this configurable ticker list.
# If NOX_DAILY_REPORT_TICKERS is unset, default to the public watchlist.
DAILY_REPORT_TICKERS_RAW = os.getenv("NOX_DAILY_REPORT_TICKERS", ",".join(WATCHLIST))
DAILY_REPORT_TICKERS = [t.strip() for t in DAILY_REPORT_TICKERS_RAW.split(",") if t.strip()] or WATCHLIST
MAX_DAILY_REPORT_SEC_TICKERS = int(os.getenv("MAX_DAILY_REPORT_SEC_TICKERS", str(len(DAILY_REPORT_TICKERS))))
# Scout used to fetch these sequentially (~30s for 9 tickers, purely from
# stacked baseline latency, not retries). Fetches per ticker are independent
# (own HTTP calls, no shared state) so a small thread pool cuts total time
# to roughly the slowest ticker. Capped well under SEC's ~10 req/sec fair-use
# ceiling (each ticker can issue up to 2 sequential SEC requests already).
SCOUT_SEC_FETCH_CONCURRENCY = int(os.getenv("SCOUT_SEC_FETCH_CONCURRENCY", "4"))
# 2026-07-22: Scout previously relied ENTIRELY on Python-fetched context
# (america-data-engine/china-data-engine/SEC/Alpaca) — any one of those
# failing left a visible gap (see the `gaps` list in run_scout_protocol)
# with no way to fill it. Claude's server-side web_search tool now runs
# alongside those fetches as a second, independent path: it can both
# cross-check the pre-fetched data and fill in for a source that failed.
# Fake-safe default: enabled with a small per-report search cap (cost is
# ~$10/1000 searches, so this bounds worst-case spend per report).
SCOUT_WEB_SEARCH_ENABLED = os.getenv("SCOUT_WEB_SEARCH_ENABLED", "true").strip().lower() == "true"
SCOUT_WEB_SEARCH_MAX_USES = int(os.getenv("SCOUT_WEB_SEARCH_MAX_USES", "8"))

# Broad market scanner watchlist — covers all major S&P 500 sectors.
# Scans 35+ tickers across all sectors by fetching bars from Alpaca and computing
# signals internally every 30 minutes during market hours.
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
]
# Note: XOM appears twice above (energy + growth); dedup is fine — set() used in scanner

# Market scanner tunable thresholds — all env-sourced with sensible defaults.
# RULE-011: No hardcoded thresholds; every gate is independently tunable.
SCANNER_MIN_PRICE       = float(os.getenv("SCANNER_MIN_PRICE", "5.0"))
SCANNER_MIN_VOLUME      = int(os.getenv("SCANNER_MIN_VOLUME", "500000"))
SCANNER_CANDIDATE_LIMIT = int(os.getenv("SCANNER_CANDIDATE_LIMIT", "80"))
SCANNER_BAR_LIMIT       = int(os.getenv("SCANNER_BAR_LIMIT", "60"))
SCANNER_RSI_MIN         = float(os.getenv("SCANNER_RSI_MIN", "45.0"))
SCANNER_RSI_MAX         = float(os.getenv("SCANNER_RSI_MAX", "68.0"))
SCANNER_VOLUME_MULT     = float(os.getenv("SCANNER_VOLUME_MULTIPLIER", "1.2"))
SCANNER_ATR_MULT        = float(os.getenv("SCANNER_ATR_MULTIPLIER", "2.0"))

# Movers report — reuses the scanner's universe/snapshot stages (same
# liquidity filter: SCANNER_MIN_PRICE/SCANNER_MIN_VOLUME) but surfaces the
# activity ranking itself instead of discarding it after signal selection.
MOVERS_REPORT_LIMIT              = int(os.getenv("MOVERS_REPORT_LIMIT", "20"))
MOVERS_REPORT_HOUR_ET             = int(os.getenv("MOVERS_REPORT_HOUR_ET", "9"))
MOVERS_REPORT_MINUTE_ET           = int(os.getenv("MOVERS_REPORT_MINUTE_ET", "45"))
# PEAD scanner — runs once daily after close (default 17:00 ET) so the
# reaction-day bar is finalized. Squeeze/Engine A is not scheduled — see
# squeeze_pead_scanner.py module docstring (2026-07-27: PEAD only, by
# explicit instruction, until it's validated).
PEAD_SCAN_HOUR_ET                = int(os.getenv("PEAD_SCAN_HOUR_ET", "17"))
PEAD_SCAN_MINUTE_ET              = int(os.getenv("PEAD_SCAN_MINUTE_ET", "0"))
# Below this SPY move (abs %), there's no measurable market-wide direction to
# align a mover against — tag everything idiosyncratic rather than invent a
# macro read on a flat day (RULE-D5: don't claim a premise that isn't measured).
MOVERS_MACRO_ALIGN_MIN_SPY_PCT    = float(os.getenv("MOVERS_MACRO_ALIGN_MIN_SPY_PCT", "0.3"))
# Chat-clutter guard (2026-07-22 user request): the top-N ranking always has
# SOMETHING in it (there's always a #1 mover by volume×%move), so ranking
# alone can't gate posting. Only send to Telegram if at least one candidate's
# move actually clears this bar — otherwise stay silent (full ranking still
# logged to market_movers_log regardless, so history isn't lost on quiet days).
MOVERS_NOTABLE_MIN_PCT            = float(os.getenv("MOVERS_NOTABLE_MIN_PCT", "5.0"))

print(
    f"[INFO] [HEARTBEAT] Scanner config: "
    f"MIN_PRICE=${SCANNER_MIN_PRICE} | MIN_VOL={SCANNER_MIN_VOLUME} | "
    f"CANDIDATES={SCANNER_CANDIDATE_LIMIT} | RSI={SCANNER_RSI_MIN}-{SCANNER_RSI_MAX} | "
    f"STOP_ATR_MULT={SCANNER_ATR_MULT}x | VOL_MULT={SCANNER_VOLUME_MULT}x",
    flush=True
)

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
                c.execute('''
                    CREATE TABLE IF NOT EXISTS market_movers_log (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        checked_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                        ticker TEXT,
                        pct_chg REAL,
                        volume INTEGER,
                        activity REAL,
                        spy_pct_chg REAL,
                        macro_tag TEXT
                    )
                ''')

                # Additive migration: the execution engine now writes trade_history
                # (the canonical trade ledger) with these extra columns. Older DBs
                # created the table without them, so add any that are missing. Must
                # stay in sync with execution/PositionManager.hpp::initialize_database.
                c.execute("PRAGMA table_info(trade_history)")
                existing_cols = {row[1] for row in c.fetchall()}
                for col, decl in (
                    ("asset_class", "TEXT DEFAULT 'equity'"),
                    ("quantity",    "REAL DEFAULT 0"),
                    ("detail",      "TEXT DEFAULT ''"),
                ):
                    if col not in existing_cols:
                        c.execute(f"ALTER TABLE trade_history ADD COLUMN {col} {decl}")

                # WS7 — Information lag windows: tracks the period between a
                # material 6-K SEC filing and Chinese retail media pickup.
                c.execute('''
                    CREATE TABLE IF NOT EXISTS lag_windows (
                        id                INTEGER PRIMARY KEY AUTOINCREMENT,
                        ticker            TEXT NOT NULL,
                        filing_url        TEXT NOT NULL,
                        materiality_score REAL NOT NULL DEFAULT 0.0,
                        opened_at         TEXT NOT NULL,
                        closed_at         TEXT,
                        closed_by_source  TEXT,
                        lag_hours         REAL,
                        abnormal_return   REAL,
                        grade             TEXT,
                        grade_reasoning   TEXT,
                        graded_at         TEXT
                    )
                ''')
                # Migrate existing deployments — ADD COLUMN is idempotent via try/except
                for _col, _typ in [
                    ("abnormal_return", "REAL"),
                    ("grade",           "TEXT"),
                    ("grade_reasoning", "TEXT"),
                    ("graded_at",       "TEXT"),
                ]:
                    try:
                        c.execute(f"ALTER TABLE lag_windows ADD COLUMN {_col} {_typ}")
                    except Exception:
                        pass  # column already exists
                c.execute('''
                    CREATE INDEX IF NOT EXISTS idx_lag_ticker
                    ON lag_windows(ticker)
                ''')
                # WS6 / Weekly Report — predicted vs actual outcomes for MAE tracking.
                # Rows are written by any workstream that records a forecast
                # (e.g. Claude risk-score vs realised PnL direction).
                c.execute('''
                    CREATE TABLE IF NOT EXISTS trade_predictions (
                        id                INTEGER PRIMARY KEY AUTOINCREMENT,
                        timestamp         DATETIME DEFAULT CURRENT_TIMESTAMP,
                        ticker            TEXT,
                        predicted_outcome REAL,
                        actual_outcome    REAL
                    )
                ''')
                # Tracks every filing that failed to parse / analyse so the
                # weekly report can surface systematic parsing regressions.
                c.execute('''
                    CREATE TABLE IF NOT EXISTS parsing_failures (
                        id          INTEGER PRIMARY KEY AUTOINCREMENT,
                        timestamp   DATETIME DEFAULT CURRENT_TIMESTAMP,
                        ticker      TEXT,
                        filing_type TEXT DEFAULT '8-K',
                        error_msg   TEXT
                    )
                ''')
                # Personal signal ideas — your own trade setups outside the system.
                # You log "I see a NVDA bull call setup" here, then track execution via /trade
                c.execute('''
                    CREATE TABLE IF NOT EXISTS personal_signals (
                        id              INTEGER PRIMARY KEY AUTOINCREMENT,
                        created_at      DATETIME DEFAULT CURRENT_TIMESTAMP,
                        ticker          TEXT NOT NULL,
                        strategy        TEXT,
                        direction       TEXT,
                        entry_level     REAL,
                        target          REAL,
                        stop_loss       REAL,
                        thesis          TEXT,
                        status          TEXT DEFAULT 'open',
                        market_price_at_log REAL
                    )
                ''')
                # Additive migration: market_price_at_log records the live quote at
                # the moment a personal signal was logged, distinct from entry_level
                # (which is the user's typed target/technical level, not a live price).
                # Older rows predate this column and stay NULL — /mysignals treats
                # that as "no live-price context available."
                try:
                    c.execute("ALTER TABLE personal_signals ADD COLUMN market_price_at_log REAL")
                except Exception:
                    pass  # column already exists
                # Additive migration: asset_class distinguishes EQUITY/OPTION
                # (live-price-able via Alpaca) from FUTURES/CRYPTO/FOREX (no
                # quote feed wired up yet — see PERSONAL_SIGNAL_ASSET_CLASSES).
                try:
                    c.execute("ALTER TABLE personal_signals ADD COLUMN asset_class TEXT DEFAULT 'EQUITY'")
                except Exception:
                    pass  # column already exists
                # Fills imported read-only from a linked brokerage (Plaid),
                # kept separate from personal_trades so "what the broker says
                # happened" never gets silently merged with "what you logged
                # as a signal" — matched_signal_id is a best-effort suggestion.
                c.execute('''
                    CREATE TABLE IF NOT EXISTS imported_fills (
                        id                       INTEGER PRIMARY KEY AUTOINCREMENT,
                        imported_at              DATETIME DEFAULT CURRENT_TIMESTAMP,
                        source_transaction_id    TEXT NOT NULL UNIQUE,
                        source                   TEXT NOT NULL DEFAULT 'plaid',
                        ticker                   TEXT NOT NULL,
                        action                   TEXT NOT NULL,
                        asset_class              TEXT NOT NULL,
                        quantity                 REAL,
                        price                    REAL,
                        amount                   REAL,
                        fees                     REAL,
                        trade_date               DATE,
                        trade_time               TEXT,
                        matched_signal_id        INTEGER,
                        direction                TEXT,
                        is_entry                 INTEGER DEFAULT 1
                    )
                ''')
                # Additive migrations, in order:
                # - direction (LONG/SHORT) and is_entry: collapsing "buy"/"sell short"
                #   into one BUY/SELL `action` would make a short ENTRY indistinguishable
                #   from a long EXIT — exactly the distinction signal correlation needs.
                # - source_transaction_id: originally `plaid_transaction_id` before the
                #   Robinhood (robin_stocks) importer was added alongside Plaid — renamed
                #   so the UNIQUE-constrained id column isn't vendor-named. SQLite's
                #   RENAME COLUMN (3.25+, well within this image's Python's bundled
                #   sqlite3) carries the UNIQUE constraint over automatically.
                # - source: which importer wrote this row ('plaid' or 'robinhood').
                # - trade_time: full timestamp when the source actually provides one
                #   (Robinhood order/execution timestamps do; Plaid's investment
                #   transactions are date-only) — NULL here means correlation falls
                #   back to a day-level match instead of pretending false precision.
                c.execute("PRAGMA table_info(imported_fills)")
                existing_cols = {row[1] for row in c.fetchall()}
                if "plaid_transaction_id" in existing_cols and "source_transaction_id" not in existing_cols:
                    c.execute("ALTER TABLE imported_fills RENAME COLUMN plaid_transaction_id TO source_transaction_id")
                    existing_cols.discard("plaid_transaction_id")
                    existing_cols.add("source_transaction_id")
                for col, decl in (
                    ("direction", "TEXT"),
                    ("is_entry", "INTEGER DEFAULT 1"),
                    ("source", "TEXT NOT NULL DEFAULT 'plaid'"),
                    ("trade_time", "TEXT"),
                ):
                    if col not in existing_cols:
                        c.execute(f"ALTER TABLE imported_fills ADD COLUMN {col} {decl}")
                # C2 — resolved outcomes for system signals: does the signal's
                # BULLISH/BEARISH call end up matching the realized price move?
                # Written by heartbeat/signal_outcome_resolver.py (own handle to
                # this same file, not imported here) at fixed T+N checkpoints
                # for every resolvable signal, plus a 'hold_duration' checkpoint
                # for signals matched to one of your actual entries once that
                # position is closed — checked over the real holding period
                # instead of an arbitrary fixed window. Purely informational:
                # nothing here feeds back into sizing/gating (see that module's
                # own docstring for the "not overfit" boundary this respects).
                c.execute('''
                    CREATE TABLE IF NOT EXISTS signal_outcomes (
                        id                INTEGER PRIMARY KEY AUTOINCREMENT,
                        resolved_at       DATETIME DEFAULT CURRENT_TIMESTAMP,
                        options_signal_id INTEGER NOT NULL,
                        ticker            TEXT NOT NULL,
                        direction         TEXT NOT NULL,
                        checkpoint_label  TEXT NOT NULL,
                        entry_price       REAL,
                        checkpoint_price  REAL,
                        move_pct          REAL,
                        direction_correct INTEGER,
                        UNIQUE(options_signal_id, checkpoint_label)
                    )
                ''')
                # Executions against any signal (system or personal).
                # signal_id can reference either options_signals.id (system) or personal_signals.id (yours)
                # signal_source distinguishes which: 'system' or 'personal'
                # NOTE (audit §1 C2): quantity/price are NULLABLE by design.
                # The documented primary form `/trade s:47` logs "I executed
                # this signal" with no qty/price typed — a NOT NULL constraint
                # there IntegrityError'd every such call, silently dropping
                # real-money executions. A migration below rebuilds any older
                # NOT NULL table (prod has 0 rows, so the copy is a no-op).
                # direction/asset_class are captured at OPEN time so /close can
                # compute pnl without re-deriving them; closes_trade_id links a
                # CLOSE row back to the OPEN it settles (audit §1 C1).
                c.execute('''
                    CREATE TABLE IF NOT EXISTS personal_trades (
                        id              INTEGER PRIMARY KEY AUTOINCREMENT,
                        timestamp       DATETIME DEFAULT CURRENT_TIMESTAMP,
                        signal_source   TEXT DEFAULT 'system',
                        signal_id       INTEGER,
                        ticker          TEXT NOT NULL,
                        strategy        TEXT,
                        action          TEXT NOT NULL,
                        quantity        REAL,
                        price           REAL,
                        pnl             REAL,
                        deviation_notes TEXT,
                        executed_at     DATETIME,
                        direction       TEXT,
                        asset_class     TEXT,
                        closes_trade_id INTEGER,
                        bucket          TEXT DEFAULT 'core'
                    )
                ''')
                # Additive migrations for older DBs (columns added 2026-07-16;
                # bucket added 2026-07-18 for the core/satellite barbell split).
                for _pt_col, _pt_decl in (
                    ("direction", "TEXT"),
                    ("asset_class", "TEXT"),
                    ("closes_trade_id", "INTEGER"),
                    ("bucket", "TEXT DEFAULT 'core'"),
                ):
                    try:
                        c.execute(f"ALTER TABLE personal_trades ADD COLUMN {_pt_col} {_pt_decl}")
                    except Exception:
                        pass  # column already exists
                # Rebuild-if-needed: drop the legacy NOT NULL on quantity/price.
                # SQLite can't ALTER away a NOT NULL constraint, so recreate the
                # table when PRAGMA still reports one. Guarded on a real prod
                # invariant — personal_trades has 0 rows in production — but the
                # copy handles rows too, so it's safe regardless.
                _pt_cols = {r[1]: r for r in c.execute("PRAGMA table_info(personal_trades)").fetchall()}
                _qty_notnull = _pt_cols.get("quantity", (None,)*4 + (0,))[3]
                _price_notnull = _pt_cols.get("price", (None,)*4 + (0,))[3]
                if _qty_notnull or _price_notnull:
                    logger.warning("Migrating personal_trades to nullable quantity/price (audit §1 C2)")
                    c.execute("ALTER TABLE personal_trades RENAME TO personal_trades_legacy_notnull")
                    c.execute('''
                        CREATE TABLE personal_trades (
                            id              INTEGER PRIMARY KEY AUTOINCREMENT,
                            timestamp       DATETIME DEFAULT CURRENT_TIMESTAMP,
                            signal_source   TEXT DEFAULT 'system',
                            signal_id       INTEGER,
                            ticker          TEXT NOT NULL,
                            strategy        TEXT,
                            action          TEXT NOT NULL,
                            quantity        REAL,
                            price           REAL,
                            pnl             REAL,
                            deviation_notes TEXT,
                            executed_at     DATETIME,
                            direction       TEXT,
                            asset_class     TEXT,
                            closes_trade_id INTEGER,
                            bucket          TEXT DEFAULT 'core'
                        )
                    ''')
                    # bucket already exists on the legacy table by now (the
                    # additive ALTER loop above runs before this rebuild), so it
                    # copies across like every other column.
                    c.execute('''
                        INSERT INTO personal_trades
                            (id, timestamp, signal_source, signal_id, ticker, strategy,
                             action, quantity, price, pnl, deviation_notes, executed_at,
                             direction, asset_class, closes_trade_id, bucket)
                        SELECT id, timestamp, signal_source, signal_id, ticker, strategy,
                               action, quantity, price, pnl, deviation_notes, executed_at,
                               direction, asset_class, closes_trade_id, bucket
                        FROM personal_trades_legacy_notnull
                    ''')
                    c.execute("DROP TABLE personal_trades_legacy_notnull")
                # Manual sports bets, mirroring personal_trades' role for
                # options: logs what the user actually placed against a
                # sports_predictions row (written by the separate
                # sports_predictions container into this same shared DB file).
                # No execution ever happens automatically — this is a record
                # of a bet the user placed themselves, same as manual options
                # entries via /trade.
                c.execute('''
                    CREATE TABLE IF NOT EXISTS sports_manual_bets (
                        id              INTEGER PRIMARY KEY AUTOINCREMENT,
                        prediction_id   INTEGER NOT NULL,
                        stake           REAL NOT NULL,
                        odds_taken      REAL,
                        notes           TEXT,
                        result          TEXT DEFAULT 'pending',
                        pnl             REAL,
                        placed_at       DATETIME DEFAULT CURRENT_TIMESTAMP,
                        bucket          TEXT DEFAULT 'core'
                    )
                ''')
                # bucket added 2026-07-18 — single-leg bets default 'core'
                # (the disciplined, bankroll-tracked flow); parlays are always
                # 'satellite' (all-or-nothing, moonshot sleeve), same split
                # personal_trades already uses for options.
                try:
                    c.execute("ALTER TABLE sports_manual_bets ADD COLUMN bucket TEXT DEFAULT 'core'")
                except Exception:
                    pass  # column already exists

                # Multi-leg parlays — a SEPARATE table from sports_manual_bets
                # rather than reusing it, because a parlay's stake/odds/result
                # apply to the WHOLE combination, not any single leg (a leg's
                # own game can finish independently of whether the other legs
                # have). sports_parlay_legs is the join table linking each leg
                # back to the single-leg prediction it rides on.
                c.execute('''
                    CREATE TABLE IF NOT EXISTS sports_manual_parlays (
                        id              INTEGER PRIMARY KEY AUTOINCREMENT,
                        stake           REAL NOT NULL,
                        combined_odds   REAL,
                        notes           TEXT,
                        result          TEXT DEFAULT 'pending',
                        pnl             REAL,
                        placed_at       DATETIME DEFAULT CURRENT_TIMESTAMP,
                        bucket          TEXT DEFAULT 'satellite'
                    )
                ''')
                c.execute('''
                    CREATE TABLE IF NOT EXISTS sports_parlay_legs (
                        id              INTEGER PRIMARY KEY AUTOINCREMENT,
                        parlay_id       INTEGER NOT NULL,
                        prediction_id   INTEGER NOT NULL,
                        result          TEXT DEFAULT 'pending'
                    )
                ''')

                conn.commit()
    except Exception as e:
        print(f"Database initialization error: {e}")

def _log_parsing_failure(ticker: str, filing_type: str, error_msg: str) -> None:
    """Persist a SEC filing parse/analysis failure to the parsing_failures table."""
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                conn.execute(
                    "INSERT INTO parsing_failures (ticker, filing_type, error_msg) "
                    "VALUES (?, ?, ?)",
                    (ticker, filing_type, str(error_msg)[:500]),
                )
    except Exception as e:
        logger.warning(f"_log_parsing_failure DB write failed: {e}")

# --- 2. DATA EXTRACTION ---
# RULE-008: All HTTP calls use a (connect_timeout, read_timeout) tuple.
# A scalar timeout=10 only sets the read timeout — the connection can still
# block indefinitely. The tuple form enforces both independently.
HTTP_TIMEOUT = (5, 10)  # (connect seconds, read seconds)

# get_latest_sec_filing() unconditionally took the newest entry in a
# ticker's 8-K/6-K feed with no check of how old it actually was — 8-Ks are
# event-driven, not periodic, so a quiet ticker's "latest" filing can be
# months old. Confirmed live 2026-07-19: AAPL's newest 8-K on record was
# dated 2026-04-30, and the report presented it undated, as if it were
# current SEC news. If the newest entry's <updated> is older than this,
# treat it the same as "no recent filing" instead of surfacing stale text.
SEC_FILING_MAX_AGE_DAYS = int(os.getenv("SEC_FILING_MAX_AGE_DAYS", "10"))

def get_alpaca_portfolio():
    headers = {'APCA-API-KEY-ID': ALPACA_API, 'APCA-API-SECRET-KEY': ALPACA_SEC}
    try:
        acc_resp = requests.get(f'{ALPACA_BROKER_URL}/v2/account',
                                headers=headers, timeout=HTTP_TIMEOUT)
        pos_resp = requests.get(f'{ALPACA_BROKER_URL}/v2/positions',
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

def escape_markdown(text: str) -> str:
    """Escape Markdown special characters for Telegram to prevent parsing errors."""
    for char in ['_', '*', '[', ']', '(', ')', '~', '`', '#', '+', '-', '=', '|', '{', '}', '.', '!']:
        text = text.replace(char, '\\' + char)
    return text


def get_filing_type(ticker: str) -> str:
    """
    Returns the correct SEC form type for a given ticker.
    Chinese ADRs are Foreign Private Issuers and file Form 6-K.
    All US domestic companies file Form 8-K.
    """
    return "6-K" if ticker in CHINESE_ADRS else "8-K"


def get_latest_sec_filing(ticker: str) -> tuple[str, bool]:
    """
    Fetches the actual text of the latest 8-K or 6-K filing for a ticker.

    Two-step process:
      1. Pull the company's Atom feed for the correct form type (8-K or 6-K).
      2. Resolve the index page from the latest entry to find and fetch
         the primary HTML document — the actual filing text, not metadata.

    Token budget: truncated to 8,000 chars for the daily scout. The
    real-time pipeline (process_automated_filing) uses 40,000 chars
    because it sends a dedicated alert and can afford the larger context.

    Returns (text, ok). ok=False only on a genuine fetch/parse failure (feed
    unreachable, bad status, malformed entry, doc unreachable) — a ticker
    simply having no recent filing is the common case and reports ok=True,
    so the daily Scout is not blocked every single day a ticker stays quiet.
    """
    filing_type = get_filing_type(ticker)
    url = (
        f"https://www.sec.gov/cgi-bin/browse-edgar"
        f"?action=getcompany&CIK={ticker}&type={filing_type}&output=atom"
    )
    headers = {'User-Agent': 'Nox/1.0 openclaw@vanhellsing.tech'}
    try:
        resp = fetch_with_retry(url, source=f"SEC {filing_type} feed:{ticker}", headers=headers, timeout=HTTP_TIMEOUT)
        if resp is None:
            return f"SEC feed unreachable for {ticker} after retries.", False
        if resp.status_code != 200:
            return f"SEC feed returned {resp.status_code} for {ticker}.", False

        root = ET.fromstring(resp.content)
        ns = {'atom': 'http://www.w3.org/2005/Atom'}
        entries = root.findall('atom:entry', ns)
        if not entries:
            return f"No recent {filing_type} filings found for {ticker}.", True

        filing_date_str = "unknown date"
        updated_el = entries[0].find('atom:updated', ns)
        if updated_el is not None and updated_el.text:
            try:
                updated_dt = datetime.fromisoformat(updated_el.text.strip())
                filing_date_str = updated_dt.date().isoformat()
                age_days = (datetime.now(updated_dt.tzinfo) - updated_dt).days
                if age_days > SEC_FILING_MAX_AGE_DAYS:
                    return (f"No {filing_type} filing within {SEC_FILING_MAX_AGE_DAYS}d for {ticker} "
                            f"(newest on record: {filing_date_str}, {age_days}d old — stale, not surfaced)."), True
            except ValueError:
                pass  # Unparseable date — fall through and surface the filing undated rather than block it.

        link_el = entries[0].find('atom:link', ns)
        if link_el is None:
            return f"No filing index link found in feed for {ticker}.", False

        index_url = link_el.attrib.get('href', '')
        if not index_url:
            return f"Empty filing index link for {ticker}.", False

        primary_url = resolve_primary_document(index_url, headers, filing_type)
        if not primary_url:
            return f"Could not resolve primary {filing_type} document for {ticker}.", False

        doc_res = fetch_with_retry(primary_url, source=f"SEC {filing_type} doc:{ticker}", headers=headers, timeout=HTTP_TIMEOUT)
        if doc_res is None:
            return f"Primary document unreachable for {ticker} after retries.", False
        if doc_res.status_code != 200:
            return f"Primary document fetch returned {doc_res.status_code} for {ticker}.", False

        soup = BeautifulSoup(doc_res.text, "html.parser")
        for element in soup(["script", "style"]):
            element.extract()

        lines = [line.strip() for line in soup.get_text(separator="\n").splitlines() if line.strip()]
        # 8,000 char budget for scout context — enough for Claude to identify
        # the item numbers and key disclosures without burning excess tokens.
        cleaned = "\n".join(lines)[:8000]
        print(f"[INFO] [HEARTBEAT] Resolved {filing_type} text for {ticker} ({len(cleaned)} chars).", flush=True)
        return f"SEC {filing_type} ({ticker}, filed {filing_date_str}):\n{cleaned}", True
    except Exception as e:
        print(f"[WARN] [HEARTBEAT] get_latest_sec_filing failed for {ticker}: {e}", flush=True)
        return f"SEC pull failed for {ticker}: {str(e)}", False

# --- 2.5 CHINESE MARKET INTELLIGENCE (china-data-engine) ---

def query_data_engine(endpoint: str, base_url: str = "http://china-data-engine:8000") -> tuple[dict, bool]:
    """
    Sends an authenticated GET request to an internal data-engine microservice
    (china-data-engine or america-data-engine, selected via base_url).

    The data engines run scrapers on their own APScheduler cycle and cache
    results in memory, so this call always returns instantly — no live scrape
    is triggered. Retries with backoff before giving up.

    Authentication follows the same shared-secret pattern used by the analyst →
    execution webhook (RULE-004): the X-Nox-Token header carries WEBHOOK_SECRET.
    RULE-008 timeouts are enforced via HTTP_TIMEOUT.

    Returns (payload, ok). On failure, payload is {} and ok is False, so the
    caller can distinguish "reached the engine, cache is just empty" from
    "could not reach the engine at all" instead of treating both as silence.
    """
    url = f"{base_url}{endpoint}"
    headers = {"X-Nox-Token": WEBHOOK_SECRET}
    res = fetch_with_retry(url, source=f"data-engine:{endpoint}", headers=headers, timeout=HTTP_TIMEOUT)
    if res is None:
        print(f"[WARN] [HEARTBEAT] Could not reach data engine at {url} after retries.", flush=True)
        return {}, False
    if res.status_code == 200:
        return res.json(), True
    print(
        f"[WARN] [HEARTBEAT] data engine at {base_url} returned HTTP {res.status_code} for {endpoint}.",
        flush=True,
    )
    return {}, False


def get_chinese_market_context() -> tuple[str, bool]:
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

    Returns (text, ok). ok is True only if every underlying data-engine call
    succeeded — a cache that is reachable but genuinely empty still counts as
    ok, since that's real data, not a fetch failure.
    """
    sections = []
    all_ok = True

    # --- East Money Hot Board (东方财富人气榜) ---
    sentiment_payload, ok = query_data_engine("/sentiment/china")
    all_ok = all_ok and ok
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
    macro_payload, ok = query_data_engine("/macro/china")
    all_ok = all_ok and ok
    pmi = macro_payload.get("pmi", {})
    if pmi:
        # A present key with a None value means that specific indicator's
        # parse failed upstream — render "N/A", never a bare None/0.0.
        mfg     = pmi.get('manufacturing') if pmi.get('manufacturing') is not None else 'N/A'
        non_mfg = pmi.get('non_manufacturing') if pmi.get('non_manufacturing') is not None else 'N/A'
        month   = pmi.get('month', 'N/A')
        source  = pmi.get('source', 'NBS')
        source_label = "NBS 国家统计局" if source == "NBS" else "Caixin 财新 (NBS feed stale, fallback active)"
        non_mfg_label = "Non-Manufacturing (非制造业)" if source == "NBS" else "Composite (综合PMI, proxy)"
        try:
            expansion = float(mfg) > 50.0
        except (TypeError, ValueError):
            expansion = False
        sections.append(
            f"🏭 China PMI ({source_label}) — {month}:\n"
            f"  Manufacturing (制造业): {mfg} "
            f"({'EXPANSION ▲' if expansion else 'CONTRACTION ▼'})\n"
            f"  {non_mfg_label}: {non_mfg}"
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
    news_payload, ok = query_data_engine("/news/cn")
    all_ok = all_ok and ok
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

    return "\n\n".join(sections), all_ok


def get_us_news_context() -> tuple[str, bool]:
    """
    Assembles US news context by querying the america-data-engine.

    Returns (text, ok). ok is False only when the america-data-engine call
    itself failed after retries — a reachable engine with a genuinely empty
    news cache still returns ok=True.
    """
    news_payload, ok = query_data_engine("/news/us", "http://america-data-engine:8001")
    news_us = news_payload.get("news", [])
    if news_us:
        lines = [
            f"- {n.get('headline', '')}"
            for n in news_us[:5] #top 5
        ]
        print(f"[INFO] [HEARTBEAT] US news received from america-data-engine ({len(lines)} headlines).", flush=True)
        return "\n".join(lines), ok
    else:
        if ok:
            print("[INFO] [HEARTBEAT] US news from america-data-engine was empty (not a failure).", flush=True)
        return "US news headlines unavailable.", ok


def get_earnings_insider_macro_context(days_ahead: int = 3) -> tuple[str, list[str]]:
    """
    Assembles near-term earnings, insider Form-4 buy clusters, and alt-macro
    chokepoint flags from america-data-engine's existing caches. All three
    are already scraped/cached there (WS2/WS3 feed the C++ Skeptic gating)
    but were never surfaced in the morning report — a structural "missing
    info" gap by omission, not a fetch failure.

    `days_ahead` controls the earnings look-ahead window: 3 for the daily
    morning report, 7 for the Sunday weekly-outlook report.

    Informational only: no gating/sizing impact here, so a failed sub-fetch
    degrades to "unavailable" in the returned notes without blocking the
    whole report the way news/SEC/china failures do (those are load-bearing
    for the report's core sections; these are supplemental).

    Returns (context_text, unavailable_notes).
    """
    et_tz = ZoneInfo('America/New_York')
    today = datetime.now(et_tz).date()
    notes = []

    earnings_data, earnings_ok = query_data_engine("/earnings/calendar", "http://america-data-engine:8001")
    earnings_lines = []
    if earnings_ok:
        failed_earnings_tickers = []
        for ticker, events in earnings_data.get("earnings_calendar", {}).items():
            # None means this ticker's fetch failed upstream — distinct from a
            # real [] ("confirmed no earnings scheduled"). Treating them the
            # same silently hides a data gap as "nothing coming up."
            if events is None:
                failed_earnings_tickers.append(ticker)
                continue
            for event in events:
                try:
                    event_date = datetime.strptime(event.get("date", ""), "%Y-%m-%d").date()
                except (ValueError, TypeError):
                    continue
                days_until = (event_date - today).days
                if days_until == 0:
                    earnings_lines.append(f"- {ticker}: reports TODAY ({event.get('description', 'earnings')})")
                elif 0 < days_until <= days_ahead:
                    earnings_lines.append(f"- {ticker}: reports {event.get('date')} (in {days_until}d)")
        if failed_earnings_tickers:
            notes.append(f"earnings fetch failed for {', '.join(failed_earnings_tickers)} (data stale/missing)")
    else:
        notes.append("earnings calendar (america-data-engine unreachable)")

    insider_data, insider_ok = query_data_engine("/insider/clusters", "http://america-data-engine:8001")
    insider_lines = []
    if insider_ok:
        for sig in insider_data.get("signals", [])[:5]:
            if not isinstance(sig, dict):
                continue
            insider_lines.append(
                f"- {sig.get('ticker', '?')}: {sig.get('officer_count', '?')} officer buy(s), "
                f"{sig.get('window_hours', '?')}h window"
            )
    else:
        notes.append("insider Form-4 clusters (america-data-engine unreachable)")

    macro_data, macro_ok = query_data_engine("/macro/alt", "http://america-data-engine:8001")
    macro_lines = []
    if macro_ok:
        for region in macro_data.get("regions", [])[:5]:
            if not isinstance(region, dict):
                continue
            verdict = region.get("verdict", "NEUTRAL")
            # NO_DATA is alt_macro.py's legitimate "nothing notable" verdict —
            # same as NEUTRAL, not an actual chokepoint flag worth surfacing.
            if verdict and verdict not in ("NEUTRAL", "NO_DATA"):
                macro_lines.append(f"- {region.get('region', '?')}: {verdict}")
    else:
        notes.append("alt-macro chokepoint check (america-data-engine unreachable)")

    context = (
        f"Earnings (next {days_ahead} days):\n" + ("\n".join(earnings_lines) if earnings_lines else "None") + "\n\n"
        "Insider buy clusters:\n" + ("\n".join(insider_lines) if insider_lines else "None flagged") + "\n\n"
        "Alt-macro chokepoint flags:\n" + ("\n".join(macro_lines) if macro_lines else "None flagged")
    )
    return context, notes


# --- 2.8 BROAD MARKET SCANNER ---
# Scans SCANNER_WATCHLIST every 30 minutes during market hours and posts
# market signals directly to the execution engine's internal /webhook endpoint
# over Docker's internal nox_net. Fully self-contained with no external dependencies.

# Personal signals log a typed entry level, not a live quote — if it's this far
# (%) from the market price at log/list time, /signal and /mysignals flag it so
# a support/technical level doesn't get mistaken for a currently-fillable price.
PERSONAL_SIGNAL_ENTRY_WARN_PCT = float(os.getenv("PERSONAL_SIGNAL_ENTRY_WARN_PCT", "3.0"))

# Asset classes /signal accepts. EQUITY/OPTION share Alpaca's stock snapshot
# feed for live-price sanity checks (an option's underlying is an equity
# ticker); FUTURES/CRYPTO/FOREX have no live quote source wired up yet, so
# /signal and /mysignals skip that fetch and say so explicitly instead of
# surfacing a generic "couldn't fetch a live price" failure.
PERSONAL_SIGNAL_ASSET_CLASSES = {"EQUITY", "OPTION", "FUTURES", "CRYPTO", "FOREX"}
PERSONAL_SIGNAL_LIVE_PRICE_ASSET_CLASSES = {"EQUITY", "OPTION"}
PERSONAL_SIGNAL_DIRECTION_ALIASES = {"BUY": "LONG", "LONG": "LONG", "SELL": "SHORT", "SHORT": "SHORT"}
# personal_trades.direction is normalised to LONG/SHORT regardless of whether
# the source signal spoke LONG/SHORT (personal) or BULLISH/BEARISH (system) —
# see cmd_trade. /close reads it back to sign the realized pnl.
_PERSONAL_TRADE_DIRECTION_NORMALIZE = {
    "LONG": "LONG", "SHORT": "SHORT", "BULLISH": "LONG", "BEARISH": "SHORT",
}
# Options strategies where you are LONG premium (paid a debit) — pnl is
# (exit - entry) per contract regardless of the market view a put expresses.
OPTIONS_LONG_PREMIUM_STRATEGIES = {
    "LONG_CALL", "LONG_PUT", "BULL_CALL_SPREAD", "BEAR_PUT_SPREAD",
    "STRADDLE", "STRANGLE", "REVERSE_IRON_CONDOR",
}
# Options strategies where you are SHORT premium (collected a credit) — pnl is
# (entry - exit): you profit when the option you sold gets cheaper.
OPTIONS_SHORT_PREMIUM_STRATEGIES = {"CSP", "CC"}
# Standard US options contract multiplier (shares per contract).
OPTIONS_CONTRACT_MULTIPLIER = 100

# ── Barbell (core / satellite) personal-trade allocation ──────────────────
# A reference figure for the *personal* trading sleeve only — used solely to
# size the satellite cap below. Not a risk limit on any automated order; the
# execution engine has its own MAX_* knobs. Env-tunable (RULE-D11); default
# tracks the $5k personal capital the strategies were bumped to (2026-07-14).
PERSONAL_ACCOUNT_CAPITAL = float(os.getenv("PERSONAL_ACCOUNT_CAPITAL", "5000"))
# Satellite ("moonshot") sleeve cap as a % of PERSONAL_ACCOUNT_CAPITAL. The
# classic barbell keeps the speculative sleeve to 10-20% so a total loss of it
# can't dent the disciplined core. Soft-warn only: exceeding it warns, never
# blocks a /trade (2026-07-18 user decision).
PERSONAL_SATELLITE_CAP_PCT = float(os.getenv("PERSONAL_SATELLITE_CAP_PCT", "15"))


def _personal_contract_multiplier(strategy, asset_class):
    """100 for an options position (contract multiplier), else 1. Single source
    of the options-vs-not decision shared by pnl and barbell cost basis."""
    is_option = asset_class == "OPTION" or strategy in (
        OPTIONS_LONG_PREMIUM_STRATEGIES | OPTIONS_SHORT_PREMIUM_STRATEGIES)
    return OPTIONS_CONTRACT_MULTIPLIER if is_option else 1


def _compute_personal_pnl(strategy, asset_class, direction, entry_price, exit_price, quantity):
    """Realized pnl for a closed personal trade. Returns None if it can't be
    computed (missing entry/exit/qty). Sign conventions (audit §1 C1):
      - long-premium options / long equity: (exit - entry) * qty * mult
      - short-premium options (CSP/CC): (entry - exit) * qty * mult
      - short equity: (entry - exit) * qty
    `mult` is 100 for options (contract multiplier), else 1."""
    if entry_price is None or exit_price is None or quantity is None:
        return None
    is_option = asset_class == "OPTION" or strategy in (
        OPTIONS_LONG_PREMIUM_STRATEGIES | OPTIONS_SHORT_PREMIUM_STRATEGIES)
    mult = _personal_contract_multiplier(strategy, asset_class)
    if strategy in OPTIONS_SHORT_PREMIUM_STRATEGIES:
        per_unit = entry_price - exit_price       # short premium
    elif is_option:
        per_unit = exit_price - entry_price       # long premium (calls AND puts)
    elif (direction or "").upper() in ("SHORT", "BEARISH"):
        per_unit = entry_price - exit_price       # short equity
    else:
        per_unit = exit_price - entry_price       # long equity
    return per_unit * quantity * mult


def _barbell_snapshot():
    """Standing core/satellite figures across all personal_trades. Thin DB
    wrapper around barbell.snapshot (which holds the pure math). Returns None
    only if the query itself fails, so callers can degrade gracefully."""
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                rows = conn.execute(
                    "SELECT id, bucket, action, quantity, price, pnl, "
                    "       asset_class, strategy, closes_trade_id "
                    "FROM personal_trades"
                ).fetchall()
    except Exception as e:
        logger.warning(f"barbell snapshot query failed: {e}")
        return None
    dict_rows = [
        dict(id=r[0], bucket=r[1], action=r[2], quantity=r[3], price=r[4],
             pnl=r[5], asset_class=r[6], strategy=r[7], closes_trade_id=r[8])
        for r in rows
    ]
    return barbell.snapshot(
        dict_rows,
        contract_multiplier=_personal_contract_multiplier,
        capital=PERSONAL_ACCOUNT_CAPITAL,
        cap_pct=PERSONAL_SATELLITE_CAP_PCT,
    )

# How far (hours) around one of your trades to look for a system options
# signal on the same ticker, for the EOD report's Signal Correlation
# section. Deliberately a plain ticker+time window, not a weighted score —
# see the section's own comment for why a dumb, transparent rule beats a
# fancier one tuned on a handful of trades.
SIGNAL_CORRELATION_WINDOW_HOURS = float(os.getenv("SIGNAL_CORRELATION_WINDOW_HOURS", "4"))
# options_signals.direction values are BULLISH/BEARISH (system vocabulary);
# personal_signals/imported_fills use LONG/SHORT — this is the translation
# between the two so a match can be judged AGREE/DISAGREE.
_SYSTEM_DIRECTION_TO_TRADE_DIRECTION = {"BULLISH": "LONG", "BEARISH": "SHORT"}

# Must match signal_outcome_resolver.py's own SIGNAL_OUTCOME_CHECKPOINT_DAYS
# (same env var) — used here only to order the EOD report's Signal Outcomes
# section (T+1 before T+5 before T+10 before hold_duration), not to compute
# anything: the resolver module is what actually writes signal_outcomes.
_outcome_checkpoint_raw = os.getenv("SIGNAL_OUTCOME_CHECKPOINT_DAYS", "1,5,10")
SIGNAL_OUTCOME_CHECKPOINT_DAYS = sorted({int(d.strip()) for d in _outcome_checkpoint_raw.split(",") if d.strip()})

ALPACA_DATA_URL  = "https://data.alpaca.markets"
# Broker/account host differs paper vs live — must track execution-engine's
# ALPACA_BASE_URL (same env var name) so reports never silently show the wrong
# account. Falls back to paper for local runs where the var isn't set.
ALPACA_BROKER_URL = os.getenv("ALPACA_BASE_URL", "https://paper-api.alpaca.markets")

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


def _bars_start_date(limit: int) -> str:
    """
    Alpaca's bars endpoint defaults to a narrow recent window when no `start`
    is given — `limit` alone does NOT reach back in time. Buffer generously
    (weekends + holidays) so `limit` trading days actually come back.
    """
    calendar_days = int(limit * 1.6) + 10
    return (datetime.now(timezone.utc) - timedelta(days=calendar_days)).strftime("%Y-%m-%d")


def fetch_bars(ticker: str, limit: int = 220) -> dict | None:
    """
    Fetch daily OHLCV bars from Alpaca market data API.
    Returns dict with keys: opens, highs, lows, closes, volumes (all lists, oldest first).
    """
    headers = {
        "APCA-API-KEY-ID":     ALPACA_API,
        "APCA-API-SECRET-KEY": ALPACA_SEC,
    }
    url = f"{ALPACA_DATA_URL}/v2/stocks/{ticker}/bars"
    logger.info(f"[FETCH_BARS] {ticker}: fetching from {url}")
    resp = fetch_with_retry(
        url,
        source=f"Alpaca bars:{ticker}",
        headers=headers,
        params={
            "timeframe": "1Day",
            "start": _bars_start_date(limit),
            "limit": limit,
            "adjustment": "raw",
            "feed": "iex",
        },
        timeout=(10, 20)  # Increased from (5, 10) to handle slower connections
    )
    logger.info(f"[FETCH_BARS] {ticker}: response={resp}")
    print(f"[FETCH_BARS] {ticker}: got response: {resp}", flush=True)
    if resp is None:
        print(f"[FETCH_BARS] {ticker}: fetch_with_retry returned None", flush=True)
        return None
    print(f"[FETCH_BARS] {ticker}: status={resp.status_code}", flush=True)
    if resp.status_code != 200:
        print(f"[FETCH_BARS] {ticker}: HTTP {resp.status_code}", flush=True)
        return None
    try:
        data = resp.json()
        bars = data.get("bars", [])
        if not bars:
            print(f"[FETCH_BARS] {ticker}: no bars in response", flush=True)
            return None
        print(f"[FETCH_BARS] {ticker}: fetched {len(bars)} bars", flush=True)
        return {
            "opens":   [b["o"] for b in bars],
            "highs":   [b["h"] for b in bars],
            "lows":    [b["l"] for b in bars],
            "closes":  [b["c"] for b in bars],
            "volumes": [b["v"] for b in bars],
        }
    except Exception as e:
        print(f"[FETCH_BARS] {ticker}: failed to parse: {e}", flush=True)
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
        resp = fetch_with_retry(
            f"{ALPACA_DATA_URL}/v2/stocks/bars",
            source=f"Alpaca batch bars chunk {i//CHUNK}",
            headers=headers,
            params={
                "symbols":   ",".join(chunk),
                "timeframe": "1Day",
                "start":     _bars_start_date(limit),
                "limit":     limit,
                "adjustment": "raw",
                "feed":      "iex",
            },
            timeout=(8, 30),
        )
        if resp is None or resp.status_code != 200:
            continue
        try:
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
            logger.warning(f"fetch_batch_bars chunk {i//CHUNK} failed to parse: {e}")
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
        resp = fetch_with_retry(
            f"{ALPACA_DATA_URL}/v2/stocks/snapshots",
            source=f"Alpaca snapshots chunk {i//CHUNK}",
            headers=headers,
            params={"symbols": ",".join(chunk), "feed": "iex"},
            timeout=(8, 30),
        )
        if resp is None or resp.status_code != 200:
            continue
        try:
            for ticker, snap in resp.json().items():
                try:
                    daily = snap.get("dailyBar", {})
                    prev  = snap.get("prevDailyBar", {})
                    lat   = snap.get("latestTrade", {})
                    price = lat.get("p", 0) or daily.get("c", 0)
                    vol   = daily.get("v", 0)
                    prev_c = prev.get("c", price) or price
                    signed_pct_chg = (price - prev_c) / prev_c * 100 if prev_c else 0
                    pct_chg = abs(signed_pct_chg)
                    result[ticker] = {
                        "price":   price,
                        "volume":  vol,
                        "pct_chg": pct_chg,
                        "signed_pct_chg": signed_pct_chg,  # direction preserved — pct_chg above stays abs for existing callers
                        "activity": vol * pct_chg,  # rank by dollar-volume × % move
                    }
                except Exception:
                    pass
        except Exception as e:
            logger.warning(f"fetch_batch_snapshots chunk {i//CHUNK} failed to parse: {e}")
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


def _fetch_vix_level_checked() -> tuple[float, bool]:
    """Current VIX via Yahoo Finance (free, no API key). Returns (level, ok) —
    ok=False means the neutral fallback was used because the fetch failed."""
    resp = fetch_with_retry(
        "https://query1.finance.yahoo.com/v8/finance/chart/%5EVIX",
        source="Yahoo Finance VIX",
        params={"interval": "1d", "range": "1d"},
        headers={"User-Agent": "Mozilla/5.0"},
        timeout=HTTP_TIMEOUT,
    )
    if resp is None or resp.status_code != 200:
        return 20.0, False  # neutral fallback — gates will still apply
    try:
        return float(resp.json()["chart"]["result"][0]["meta"]["regularMarketPrice"]), True
    except Exception:
        return 20.0, False  # neutral fallback — gates will still apply


def fetch_vix_level() -> float:
    return _fetch_vix_level_checked()[0]


def _fetch_spy_regime_checked() -> tuple[float, float, float, bool]:
    """
    Returns (spy_price, spy_200_sma, vix, ok). ok=False means either leg fell
    back to a neutral default because its fetch failed — callers that display
    this data to a human (rather than just gating on it) must not present it
    as real without checking `ok`.
    Fetched once per scan cycle and passed to each ticker scan to avoid
    220 × number-of-tickers redundant API calls.
    """
    vix, vix_ok = _fetch_vix_level_checked()

    # SPY is only used for regime gating (RISK_OFF check). If it fails,
    # use neutral defaults (0, 0) which disables the RISK_OFF gate,
    # allowing the scanner to proceed. Individual tickers will still be
    # assessed on their own technical merit.
    bars = fetch_bars("SPY", limit=210)
    if bars and len(bars["closes"]) >= 200:
        spy_price   = bars["closes"][-1]
        spy_200_sma = _calc_sma(bars["closes"], 200)
        return spy_price, spy_200_sma, vix, vix_ok

    # Fallback: neutral regime allows scanning to proceed
    logger.warning("[SCANNER] SPY regime fetch failed; using neutral regime (allows full scanning)")
    return 0.0, 0.0, vix, False


def fetch_spy_regime() -> tuple[float, float, float]:
    spy_price, spy_200_sma, vix, _ = _fetch_spy_regime_checked()
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
    if rsi < SCANNER_RSI_MIN or rsi > SCANNER_RSI_MAX:
        return None
    if price < sma20:
        return None

    # Volume confirmation: current volume vs 10-day average
    avg_vol_10 = _calc_sma(volumes, 10) if len(volumes) >= 10 else volumes[-1]
    if avg_vol_10 > 0 and volumes[-1] < avg_vol_10 * SCANNER_VOLUME_MULT:
        return None

    return {
        "secret_key":              WEBHOOK_SECRET,
        "ticker":                  ticker,
        "action":                  "BUY",
        "price":                   round(price, 4),
        "rsi":                     round(rsi, 2),
        "vol":                     int(volumes[-1]),
        "atr":                     round(atr, 4),
        "stop_loss_atr_multiplier": SCANNER_ATR_MULT,
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


def is_trading_day_at(dt: datetime) -> bool:
    """True if `dt` (any tz) falls on a weekday that isn't a NYSE holiday."""
    et_dt = dt.astimezone(ZoneInfo("America/New_York"))
    if et_dt.weekday() >= 5:  # Saturday / Sunday
        return False
    return et_dt.strftime("%Y-%m-%d") not in MARKET_HOLIDAYS


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
    if not is_trading_day():
        logger.info("Market scanner skipped — non-trading day.")
        return

    if not is_market_hours():
        logger.info("Market scanner skipped — outside market hours.")
        return

    logger.info("Market scanner cycle starting...")
    try:
        spy_price, spy_200_sma, vix = fetch_spy_regime()
        logger.info(f"Regime: SPY={spy_price:.2f}, SMA200={spy_200_sma:.2f}, VIX={vix:.1f}")
    except Exception as e:
        logger.error(f"[SCANNER] fetch_spy_regime() failed: {e}", exc_info=True)
        raise

    # If SPY data failed (0, 0), use neutral regime: continue scanning with permissive settings
    if spy_price == 0.0 and spy_200_sma == 0.0:
        logger.warning(f"[SCANNER] SPY data unavailable; using neutral regime (VIX={vix:.1f})")
        spy_price = 0.0  # Disable SPY price check
        spy_200_sma = 0.0  # Disable SMA check

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

    logger.info(
        f"Scanner cycle complete — {len(triggered)} signal(s) from "
        f"{len(top_tickers)} candidates (VIX={vix:.1f}). Use /details to review."
    )


def run_market_movers_report() -> None:
    """
    Broad-universe movers report — surfaces the top-N tickers by unusual
    activity (volume × % move) across the whole tradable market, tagged
    against the day's SPY move as a rough macro-vs-idiosyncratic read.

    Reuses the same universe/snapshot stages as run_market_scanner() rather
    than duplicating a second full-market fetch; the scanner discards its
    activity ranking after picking signal candidates — this just surfaces
    that ranking directly as its own report instead.

    The macro/idiosyncratic tag is a heuristic (sign of the mover's % change
    vs. sign of SPY's % change on a day SPY moved enough to have a
    direction), not a measured causal signal — logging-power only per
    RULE-D5, same as WS8's china-lag demotion. No sizing/gating impact.

    Only sends a Telegram message if at least one candidate clears
    MOVERS_NOTABLE_MIN_PCT — a quiet day stays quiet in chat, but the full
    top-N ranking is still logged to market_movers_log either way.
    """
    if not is_trading_day():
        logger.info("Market movers report skipped — non-trading day.")
        return

    try:
        universe = fetch_market_universe()
        if not universe:
            logger.warning("Market movers report: empty universe — skipping.")
            return

        snapshots = fetch_batch_snapshots(universe)
        spy_snap = snapshots.get("SPY")
        spy_pct_chg = spy_snap["signed_pct_chg"] if spy_snap else 0.0
        spy_ok = spy_snap is not None

        candidates = [
            (ticker, snap)
            for ticker, snap in snapshots.items()
            if snap["price"] >= SCANNER_MIN_PRICE
            and snap["volume"] >= SCANNER_MIN_VOLUME
        ]
        candidates.sort(key=lambda x: x[1]["activity"], reverse=True)
        top = candidates[:MOVERS_REPORT_LIMIT]

        if not top:
            logger.info("Market movers report: no candidates cleared the liquidity filter.")
            return

        have_macro_direction = spy_ok and abs(spy_pct_chg) >= MOVERS_MACRO_ALIGN_MIN_SPY_PCT

        rows = []
        notable_lines = []
        for ticker, snap in top:
            signed = snap["signed_pct_chg"]
            if have_macro_direction and (signed >= 0) == (spy_pct_chg >= 0):
                tag = "macro-aligned"
            else:
                tag = "idiosyncratic"
            rows.append((ticker, signed, int(snap["volume"]), snap["activity"], spy_pct_chg, tag))
            if abs(signed) >= MOVERS_NOTABLE_MIN_PCT:
                notable_lines.append(f"• {ticker}: {signed:+.2f}% vol={int(snap['volume']):,} — {tag}")

        # Full top-N ranking is always logged (history value on quiet days
        # too) — only the Telegram send is gated on something notable existing.
        if notable_lines:
            lines = [
                "*MARKET MOVERS*",
                "────────────────────────",
                f"SPY {spy_pct_chg:+.2f}%" + ("" if spy_ok else " (unavailable — tags below are idiosyncratic-only)"),
                "",
            ] + notable_lines
            _send_telegram_section("\n".join(lines))
        else:
            logger.info(
                f"Market movers: no candidate cleared the {MOVERS_NOTABLE_MIN_PCT}% notability "
                f"bar today (top move: {top[0][1]['signed_pct_chg']:+.2f}%) — staying quiet."
            )

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.executemany(
                    "INSERT INTO market_movers_log (ticker, pct_chg, volume, activity, spy_pct_chg, macro_tag) "
                    "VALUES (?, ?, ?, ?, ?, ?)",
                    rows,
                )
                conn.commit()

        logger.info(f"Market movers report sent — {len(top)} tickers, SPY {spy_pct_chg:+.2f}%.")
    except Exception as e:
        logger.error(f"Market movers report failed: {e}", exc_info=True)
        try:
            _send_telegram_section(f"⚠️ *MARKET MOVERS — FAILED*\n{e}")
        except Exception as alert_e:
            logger.error(f"Market movers failure alert also failed to send: {alert_e}")
        raise


# --- 3. THE SCOUT PROTOCOL (DAILY REPORT) ---

SCOUT_SYSTEM_PROMPT = """You are Nox, a quantitative trading assistant generating a pre-market briefing.

You have a web_search tool available. The message below is pre-fetched from
our own data feeds (news/SEC/China/macro APIs) — treat it as the primary
source and don't re-search anything it already answered cleanly. But the
pre-fetched context does NOT cover every field the sections below require —
notably it has no US index-level % moves, sector rotation, or volume-vs-
average. For any required section detail that is simply absent from the
pre-fetched context (not just explicitly flagged as unreachable), use
web_search to find it rather than writing "data gap" or skipping the bullet.
If the message flags specific data as unreachable ("Gaps: ..."), web_search
those too. Only fall back to stating a gap if web_search itself fails to
turn up the number.

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

*EARNINGS & FLOW*
────────────────────────
(earnings reporting today or in the next 3 days — name the date and why it matters; insider Form-4 buy clusters; alt-macro chokepoint flags. One bullet per item, or "Nothing material today")

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


def _extract_text(response) -> str:
    """
    Safely extracts text from a Claude response, skipping extended thinking
    blocks and server-tool blocks (e.g. server_tool_use/web_search_tool_result).

    With server-side tools like web_search, Claude can interleave multiple
    text blocks with search calls (search, write some text, search again,
    write more) — so this concatenates every text block in order instead of
    returning only the first one, which would silently drop everything
    written after the first search.
    """
    if not response or not response.content:
        return ""
    parts = [block.text for block in response.content if hasattr(block, 'text')]
    return "".join(parts)


def _narrative_text(response) -> str:
    """
    Extracts a Claude narrative response's text, appending a visible notice
    if the completion was cut off by max_tokens instead of finishing
    naturally — silently gluing a footer onto truncated text reads as a
    coherent sentence that just stops mid-word.

    Handles extended thinking responses by skipping ThinkingBlock objects.
    """
    text = _extract_text(response)
    if getattr(response, "stop_reason", None) == "max_tokens":
        text = text.rstrip() + "\n\n_[response truncated — hit max_tokens]_"
    return text


def _send_telegram_section(section: str) -> None:
    """Send a Telegram section safely so one failed message does not abort the report."""
    try:
        bot.send_message(CHAT_ID, section, parse_mode='Markdown')
    except Exception as e:
        logger.error(f"[ERROR] [HEARTBEAT] Failed to send Telegram section: {e}")
        print(f"[ERROR] [HEARTBEAT] Failed to send Telegram section: {e}", flush=True)


def run_scout_protocol():
    try:
        news_context, news_ok = get_us_news_context()
        report_tickers   = DAILY_REPORT_TICKERS[:MAX_DAILY_REPORT_SEC_TICKERS]
        skipped_tickers  = DAILY_REPORT_TICKERS[MAX_DAILY_REPORT_SEC_TICKERS:]
        with ThreadPoolExecutor(max_workers=min(SCOUT_SEC_FETCH_CONCURRENCY, len(report_tickers) or 1)) as pool:
            sec_results = list(pool.map(get_latest_sec_filing, report_tickers))
        sec_context     = "\n\n".join(text for text, _ in sec_results)
        sec_failed      = [t for t, (_, ok) in zip(report_tickers, sec_results) if not ok]
        chinese_context, china_ok = get_chinese_market_context()
        flow_context, flow_unavailable = get_earnings_insider_macro_context()

        # Data validation: on trading days, enforce strict requirements (all sources must succeed).
        # On non-trading days (weekends/holidays), be lenient and generate reports with whatever
        # data is available so the user remains informed during market closure.
        is_trading = is_trading_day()

        # Fetch market regime data (VIX, SPY price, SPY 200-SMA). Use the
        # checked variant here (unlike the scanner's gating call sites) —
        # this is displayed to a human and fed to Claude as fact, so a failed
        # fetch must be visible rather than silently rendered as real VIX=20.0
        # / SPY=$0.00 (RULE-D2: the consumer here is a human reading numbers,
        # not a gate that can safely no-op on neutral defaults).
        spy_price, spy_200_sma, vix, regime_ok = _fetch_spy_regime_checked()
        market_regime = (
            f"• *VIX:* {vix:.1f}\n"
            f"• *SPY:* ${spy_price:.2f}\n"
            f"• *SPY 200-SMA:* ${spy_200_sma:.2f}\n"
            f"• *Status:* {'RISK_OFF' if vix >= 25.0 and spy_price > 0 and spy_price < spy_200_sma else 'RISK_ON'}"
        )

        # Policy (revised 2026-07-22): a single data-source outage used to
        # cause the ENTIRE report to be refused on trading days — but that
        # made "no report" indistinguishable from "nothing material
        # happened" from the reader's side too, just via a different path,
        # and one flaky source (news/SEC/China/VIX) silenced the other
        # three that were fine. Always generate, with any gap surfaced
        # visibly inline (RULE-D3) — same treatment trading and non-trading
        # days now get, so a partial-data report reads as "here's what we
        # have and what's missing," not as a confident-sounding guess.
        gaps = []
        if not news_ok:
            gaps.append("US news (america-data-engine unreachable)")
        if sec_failed:
            gaps.append(f"SEC filings unreachable for: {', '.join(sec_failed)}")
        if not china_ok:
            gaps.append("Chinese market intelligence (china-data-engine unreachable)")
        if not regime_ok:
            gaps.append("Market regime (VIX/SPY unreachable — figures above are neutral fallbacks, not live data)")

        # Ticker-cap truncation is a config choice, not a fetch failure — note
        # it visibly (RULE-D3) but don't refuse the report over it the way a
        # real outage does above.
        cap_note = ""
        if skipped_tickers:
            cap_note += f"\n\n_SEC filings not checked (MAX_DAILY_REPORT_SEC_TICKERS cap): {', '.join(skipped_tickers)}_"
            print(f"[WARN] [HEARTBEAT] Scout protocol skipped SEC checks for {len(skipped_tickers)} ticker(s) due to MAX_DAILY_REPORT_SEC_TICKERS cap: {', '.join(skipped_tickers)}", flush=True)
        if flow_unavailable:
            cap_note += f"\n\n_Earnings/insider/macro flow unavailable: {', '.join(flow_unavailable)}_"
            print(f"[WARN] [HEARTBEAT] Scout protocol flow context partially unavailable: {', '.join(flow_unavailable)}", flush=True)

        # Surface any gap inline instead of refusing the whole report — see
        # policy note above. Trading vs non-trading days get the same
        # treatment now; the wording just notes market closure when relevant.
        non_trading_note = ""
        if gaps:
            gap_summary = "; ".join(gaps)
            day_note = "non-trading day" if not is_trading else "trading day"
            non_trading_note = f"\n\n⚠️ *Report generated with incomplete data ({day_note})*\n*Missing: {gap_summary}*"
            print(f"[WARN] [HEARTBEAT] Scout protocol running with incomplete data ({day_note}): {gap_summary}", flush=True)

        gap_instruction = ""
        if gaps and SCOUT_WEB_SEARCH_ENABLED:
            gap_instruction = (
                "\n\nData gaps below are marked as unreachable via our internal feeds. "
                "Use web_search to fill each one from public sources before writing the "
                "affected section(s); if a gap still can't be filled after searching, "
                "keep it in the report as a stated gap rather than guessing.\n"
                f"Gaps: {'; '.join(gaps)}"
            )

        create_kwargs = dict(
            model="claude-sonnet-5",
            max_tokens=9000,
            system=SCOUT_SYSTEM_PROMPT,
            messages=[{"role": "user", "content": (
                f"Market Regime:\n{market_regime}\n\n"
                f"US Headlines:\n{news_context}\n\n"
                f"SEC Filings:\n{sec_context}\n\n"
                f"Chinese Market Intelligence:\n{chinese_context}\n\n"
                f"Earnings/Insider/Alt-Macro Flow:\n{flow_context}"
                f"{gap_instruction}"
            )}]
        )
        if SCOUT_WEB_SEARCH_ENABLED:
            create_kwargs["tools"] = [{
                "type": "web_search_20250305",
                "name": "web_search",
                "max_uses": SCOUT_WEB_SEARCH_MAX_USES,
            }]
        response = claude.messages.create(**create_kwargs)

        analysis_text = _narrative_text(response)
        et_tz = ZoneInfo('America/New_York')
        timestamp = datetime.now(et_tz).strftime('%Y-%m-%d %H:%M ET')

        print(f"[INFO] [HEARTBEAT] Daily audit raw report length: {len(analysis_text or '')} chars", flush=True)

        header = (
            f"*NOX DAILY AUDIT*\n"
            f"────────────────────────\n"
            f"{timestamp}"
        )
        if non_trading_note:
            header += non_trading_note
        if cap_note:
            header += cap_note
        _send_telegram_section(header)

        sections = _split_scout_sections(analysis_text or "No report content was produced.")
        if not sections:
            sections = list(smart_split(analysis_text or "No report content was produced.", chars_per_string=3800))

        for section in sections:
            _send_telegram_section(section)

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute("INSERT INTO daily_audits (tickers_scanned, claude_analysis) VALUES (?, ?)",
                          (", ".join(report_tickers), analysis_text))
                conn.commit()
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] Scout protocol failed: {e}", flush=True)
        # RULE-D3: a crash here used to be a silent no-op — no Telegram
        # message at all, indistinguishable from a healthy day with nothing
        # to report. Alert on absence instead of swallowing.
        try:
            _send_telegram_section(
                "*NOX DAILY AUDIT — FAILED*\n"
                "────────────────────────\n"
                f"Scout protocol crashed before a report could be generated: {e}"
            )
        except Exception as alert_e:
            print(f"[ERROR] [HEARTBEAT] Scout protocol failure alert also failed to send: {alert_e}", flush=True)
        raise

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
        # Alpaca options chains endpoint — served from the broker/account host,
        # which differs paper vs live (unlike ALPACA_DATA_URL).
        url = f"{ALPACA_BROKER_URL}/v1beta3/options/chains/{ticker}"
        resp = fetch_with_retry(url, source=f"Alpaca options chain:{ticker}", headers=headers, timeout=HTTP_TIMEOUT)

        if resp is None or resp.status_code != 200:
            logger.warning(f"Alpaca options chain request failed for {ticker}: HTTP {resp.status_code if resp is not None else 'no response'}")
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
    url = f"{ALPACA_BROKER_URL}/v1beta3/options/chains/{ticker}"
    resp = fetch_with_retry(url, source=f"IV skew chain:{ticker}", headers=headers, timeout=HTTP_TIMEOUT)
    if resp is None:
        return {"ticker": ticker, "method": "error", "error": "options chain unreachable after retries"}
    if resp.status_code != 200:
        return {"ticker": ticker, "method": "error",
                "error": f"chain HTTP {resp.status_code}"}

    try:
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
    if not is_trading_day():
        logger.info("EOD IV collection skipped — non-trading day.")
        return

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
    # --- Telemetry Watchdog ---
    # Disabled by default: get_current_price()/get_option_current_price() in
    # telemetry_watchdog.py are still mock stubs (hardcoded price dict,
    # random.choice() for option P&L) — RULE-D5 forbids an invented number
    # from having sizing/alerting power. Enable only once real price lookups
    # are wired in.
    telemetry_watchdog_enabled = os.getenv("TELEMETRY_WATCHDOG_ENABLED", "false").strip().lower() in ("true", "1", "yes")
    if telemetry_watchdog_enabled:
        schedule.every(5).minutes.do(supervised_job("telemetry_watchdog")(telemetry_watchdog.run_watchdog)).tag("telemetry_watchdog")
        logger.info("Telemetry Watchdog scheduled to run every 5 minutes.")
    else:
        logger.info("Telemetry Watchdog: disabled (TELEMETRY_WATCHDOG_ENABLED=false) — price lookups are still mock stubs.")
    # RULE-006: Daily Scout MUST fire at 9:00 AM Eastern Time (ET), not UTC.
    #
    # Why this approach:
    #   • `schedule` library has no native timezone awareness — `.at("HH:MM")`
    #     always interprets the time as the system clock (UTC inside Docker).
    #   • The tz-argument form `.at("HH:MM", "America/New_York")` was introduced
    #     in schedule 1.2.0. Pinning that version in the Dockerfile is required
    #     (see Dockerfile); this code depends on it.
    #   • DST transitions mean the UTC equivalent of 9:00 ET changes twice a
    #     year (UTC-5 in winter → UTC-4 in summer). We recompute at midnight UTC
    #     each day so the offset is always correct.
    #
    # Design:
    #   _reschedule_scout() is called once at startup and then once per day at
    #   00:01 UTC. It clears the previous 'scout' job, computes today's correct
    #   UTC wall-clock equivalent of 9:00 ET (pre-market), and registers a fresh job.
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

        schedule.every().day.at(utc_hhmm).do(supervised_job("scout")(_scheduled_scout)).tag("scout")
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

        schedule.every().day.at(utc_hhmm).do(supervised_job("iv_collection")(collect_eod_iv_snapshots)).tag("iv_collection")
        logger.info(
            f"EOD IV collection (re)scheduled: "
            f"4:30 PM ET (post-market) = {utc_hhmm} UTC "
            f"({'EDT UTC-4' if target_et.utcoffset().total_seconds() == -14400 else 'EST UTC-5'})."
        )

    _reschedule_iv_collection()

    # --- End-of-Day / End-of-Week reports (post-close) ---
    # EOD 16:05 ET daily (narrative briefing on non-trading days, inside run_eod_report);
    # EOW 16:10 ET Friday (narrative briefing if that Friday is a holiday).
    # DST-aware like the scout: recomputed nightly at 00:01 UTC.
    def _reschedule_eod_eow():
        schedule.clear("eod_report")
        schedule.clear("eow_report")
        now_et = datetime.now(tz=ET)

        eod_et  = now_et.replace(hour=16, minute=5, second=0, microsecond=0)
        eod_utc = eod_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(eod_utc).do(supervised_job("eod_report")(run_eod_report)).tag("eod_report")

        eow_et  = now_et.replace(hour=16, minute=10, second=0, microsecond=0)
        eow_utc = eow_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().friday.at(eow_utc).do(supervised_job("eow_report")(run_eow_report)).tag("eow_report")

        logger.info(f"EOD report scheduled 16:05 ET = {eod_utc} UTC (daily); "
                    f"EOW report scheduled 16:10 ET = {eow_utc} UTC (Friday).")

    _reschedule_eod_eow()

    # --- Fundamentals-risk (Beneish/FCF) check, decoupled from the EOD bundle ---
    # Producer (america_data_engine) scans the broad universe at
    # FUNDAMENTALS_SCAN_HOUR_ET (default 20:00 ET); this reads its result at
    # 21:30 ET, after the scan has had time to finish. DST-aware like the
    # blocks above: recomputed nightly at 00:01 UTC.
    def _reschedule_fundamentals_risk_check():
        schedule.clear("fundamentals_risk_check")
        now_et = datetime.now(tz=ET)
        check_et  = now_et.replace(hour=21, minute=30, second=0, microsecond=0)
        check_utc = check_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(check_utc).do(supervised_job("fundamentals_risk_check")(run_fundamentals_risk_check)).tag("fundamentals_risk_check")
        logger.info(f"Fundamentals-risk check scheduled 21:30 ET = {check_utc} UTC (daily).")

    _reschedule_fundamentals_risk_check()

    # --- Bullish quality (Piotroski F-Score) check — bullish mirror of the
    # block above. Same producer scan, same "read after it's had time to
    # finish" reasoning, run 1 minute after so a transient failure in one
    # doesn't affect the other (separate schedule entries, not a shared call).
    def _reschedule_fundamentals_bullish_check():
        schedule.clear("fundamentals_bullish_check")
        now_et = datetime.now(tz=ET)
        check_et  = now_et.replace(hour=21, minute=31, second=0, microsecond=0)
        check_utc = check_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(check_utc).do(supervised_job("fundamentals_bullish_check")(run_fundamentals_bullish_check)).tag("fundamentals_bullish_check")
        logger.info(f"Fundamentals-bullish check scheduled 21:31 ET = {check_utc} UTC (daily).")

    _reschedule_fundamentals_bullish_check()

    # --- CELH search-signal checkpoint (90d/180d, monitoring-only) ---
    # Fires at most twice ever (see celh_signal_tracker.py); cheap to check
    # daily since the DB row dedup makes a no-op check nearly free. Placed
    # in the same quiet post-close slot as the other fundamentals checks.
    def _reschedule_celh_signal_check():
        schedule.clear("celh_signal_check")
        now_et = datetime.now(tz=ET)
        check_et  = now_et.replace(hour=21, minute=32, second=0, microsecond=0)
        check_utc = check_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(check_utc).do(supervised_job("celh_signal_check")(run_celh_signal_check)).tag("celh_signal_check")
        logger.info(f"CELH search-signal checkpoint check scheduled 21:32 ET = {check_utc} UTC (daily).")

    _reschedule_celh_signal_check()

    # --- Earnings-revision-momentum (consensus EPS drift) check ---
    # No producer-cache dependency (fetches Finnhub directly), so no timing
    # coupling to the 20:00/21:00/21:30/21:31 ET cluster above — 17:00 ET is
    # just a quiet slot shortly after close. DST-aware like the blocks above.
    def _reschedule_earnings_revision_check():
        schedule.clear("earnings_revision_check")
        now_et = datetime.now(tz=ET)
        check_et  = now_et.replace(hour=17, minute=0, second=0, microsecond=0)
        check_utc = check_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(check_utc).do(supervised_job("earnings_revision_check")(run_earnings_revision_check)).tag("earnings_revision_check")
        logger.info(f"Earnings-revision check scheduled 17:00 ET = {check_utc} UTC (daily).")

    _reschedule_earnings_revision_check()

    # --- Evening session-summary report ---
    # Confirms whether the morning Scout call held up and recaps the day's
    # signal/order activity — distinct from the 16:05 ET EOD account bundle.
    # Scheduled ~21:00 ET: after the day's news cycle settles and after
    # america_data_engine's 20:00 ET fundamentals-universe scan, before the
    # 21:30 ET fundamentals-risk consumer. DST-aware, same pattern as above.
    def _reschedule_session_summary():
        schedule.clear("session_summary")
        now_et = datetime.now(tz=ET)
        summary_et  = now_et.replace(hour=21, minute=0, second=0, microsecond=0)
        summary_utc = summary_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(summary_utc).do(supervised_job("session_summary")(run_session_summary_report)).tag("session_summary")
        logger.info(f"Session-summary report scheduled 21:00 ET = {summary_utc} UTC (daily; skips non-trading days internally).")

    _reschedule_session_summary()

    # --- Weekly outlook (Sunday) and weekly performance recap (Friday) ---
    # Sunday previews the week ahead against which Friday's recap is checked.
    # Friday's recap reuses the existing win/loss + MAE weekly-performance
    # report (previously manual-only via /weekly_report, never scheduled).
    # Both are DST-aware/recomputed nightly like the blocks above.
    def _reschedule_weekly_reports():
        schedule.clear("weekly_outlook")
        schedule.clear("weekly_recap")
        now_et = datetime.now(tz=ET)

        outlook_et  = now_et.replace(hour=18, minute=0, second=0, microsecond=0)
        outlook_utc = outlook_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().sunday.at(outlook_utc).do(supervised_job("weekly_outlook")(run_weekly_outlook_report)).tag("weekly_outlook")

        recap_et  = now_et.replace(hour=21, minute=15, second=0, microsecond=0)
        recap_utc = recap_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().friday.at(recap_utc).do(supervised_job("weekly_recap")(run_weekly_performance_report)).tag("weekly_recap")

        logger.info(f"Weekly outlook scheduled 18:00 ET Sunday = {outlook_utc} UTC; "
                    f"weekly performance recap scheduled 21:15 ET Friday = {recap_utc} UTC.")

    _reschedule_weekly_reports()

    # --- Sports predictions daily recap (before most games start) ---
    # DST-aware like the scout/EOD-EOW blocks above. Sent to the SPORTS bot
    # only — this is a summary, not a high-conviction alert, so it doesn't
    # duplicate to the main bot the way run_cycle's alerts do.
    def _reschedule_sports_recap():
        schedule.clear("sports_recap")
        now_et = datetime.now(tz=ET)
        recap_et = now_et.replace(hour=9, minute=0, second=0, microsecond=0)
        recap_utc = recap_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(recap_utc).do(supervised_job("sports_recap")(run_sports_daily_recap)).tag("sports_recap")
        logger.info(f"Sports daily recap scheduled 9:00 AM ET = {recap_utc} UTC.")

    _reschedule_sports_recap()

    # --- Market Movers report (once, shortly after open) ---
    # DST-aware like the scout/EOD-EOW blocks above. Scheduled a few minutes
    # after 9:30 ET open (default 9:45) so opening-range volume has had a
    # chance to build before ranking activity.
    def _reschedule_market_movers():
        schedule.clear("market_movers")
        now_et = datetime.now(tz=ET)
        movers_et = now_et.replace(hour=MOVERS_REPORT_HOUR_ET, minute=MOVERS_REPORT_MINUTE_ET, second=0, microsecond=0)
        movers_utc = movers_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(movers_utc).do(supervised_job("market_movers")(run_market_movers_report)).tag("market_movers")
        logger.info(f"Market movers report scheduled {MOVERS_REPORT_HOUR_ET:02d}:{MOVERS_REPORT_MINUTE_ET:02d} ET = {movers_utc} UTC (daily).")

    # --- PEAD scanner (once daily, after close) ---
    # Off by default — this is the observation window, not production trust
    # yet (RULE-D8): PEAD_SCANNER_ENABLED turns the schedule on at all,
    # PEAD_SCANNER_DRY_RUN (default true) additionally keeps it from writing
    # to options_signals or sending a Telegram alert while you watch its
    # logged output. Squeeze/Engine A is not scheduled — see
    # squeeze_pead_scanner.py module docstring.
    pead_scanner_enabled = os.getenv("PEAD_SCANNER_ENABLED", "false").strip().lower() in ("true", "1", "yes")
    pead_scanner_dry_run = os.getenv("PEAD_SCANNER_DRY_RUN", "true").strip().lower() in ("true", "1", "yes")

    def _run_pead_scan():
        results = squeeze_pead_scanner.run_scanner(dry_run=pead_scanner_dry_run)
        mode = "DRY-RUN" if pead_scanner_dry_run else "LIVE"
        logger.info(f"PEAD scan ({mode}) complete: {len(results)} approved candidate(s).")

    def _reschedule_pead_scan():
        schedule.clear("pead_scan")
        if not pead_scanner_enabled:
            logger.info("PEAD scanner: disabled (PEAD_SCANNER_ENABLED=false).")
            return
        now_et = datetime.now(tz=ET)
        pead_et = now_et.replace(hour=PEAD_SCAN_HOUR_ET, minute=PEAD_SCAN_MINUTE_ET, second=0, microsecond=0)
        pead_utc = pead_et.astimezone(UTC).strftime("%H:%M")
        schedule.every().day.at(pead_utc).do(supervised_job("pead_scan")(_run_pead_scan)).tag("pead_scan")
        mode = "DRY-RUN (logging only)" if pead_scanner_dry_run else "LIVE (writes signals + Telegram alerts)"
        logger.info(f"PEAD scanner scheduled {PEAD_SCAN_HOUR_ET:02d}:{PEAD_SCAN_MINUTE_ET:02d} ET = {pead_utc} UTC (daily, {mode}).")

    _reschedule_pead_scan()

    _reschedule_market_movers()

    # --- Market Scanner (every 30 minutes, market hours only) ---
    # Runs independently of DST — the is_market_hours() check inside handles
    # the ET window, so we schedule on a fixed wall-clock interval, not a time.
    # Disabled by default (MARKET_SCANNER_ENABLED=false) to avoid sending legacy/flawed technical signals.
    market_scanner_enabled = os.getenv("MARKET_SCANNER_ENABLED", "false").strip().lower() in ("true", "1", "yes")
    schedule.clear("market_scanner")
    if market_scanner_enabled:
        schedule.every(30).minutes.do(supervised_job("market_scanner")(run_market_scanner)).tag("market_scanner")
        logger.info("Market scanner scheduled: every 30 minutes (market hours gated internally).")
        # Fire once immediately at startup so first signals don't wait 30 minutes.
        threading.Thread(target=run_market_scanner, daemon=True).start()
    else:
        logger.info("Market scanner: disabled (MARKET_SCANNER_ENABLED=false) — skipping raw technical whole-market scans.")

    # --- Auto-detected brokerage fills importer ---
    # Exactly ONE source polls, chosen by FILLS_IMPORTER_SOURCE (ibkr | none).
    # Default is "none"; fills are logged manually via /trade or via IBKR poll.
    fills_source = os.getenv("FILLS_IMPORTER_SOURCE", "none").strip().lower()
    ibkr_poll_minutes = int(os.getenv("IBKR_POLL_INTERVAL_MINUTES", "30"))

    def _run_ibkr_fills_poll():
        import ibkr_fills_importer
        ibkr_fills_importer.poll_ibkr_fills()

    if fills_source == "ibkr":
        schedule.every(ibkr_poll_minutes).minutes.do(supervised_job("ibkr_fills")(_run_ibkr_fills_poll)).tag("ibkr_fills")
        logger.info("Fills importer: IBKR (FILLS_IMPORTER_SOURCE=ibkr)")
    elif fills_source in ("none", "off", ""):
        logger.info("Fills importer: disabled (FILLS_IMPORTER_SOURCE=none) — fills logged manually via /trade")
    else:
        logger.warning(
            f"FILLS_IMPORTER_SOURCE='{fills_source}' unrecognized — expected "
            f"ibkr|none. No fills importer scheduled to avoid double-counting."
        )

    # Reschedule IV collection each night along with the scout
    def _combined_reschedule():
        _reschedule_scout()
        _reschedule_iv_collection()
        _reschedule_eod_eow()
        _reschedule_fundamentals_risk_check()
        _reschedule_fundamentals_bullish_check()
        _reschedule_celh_signal_check()
        _reschedule_earnings_revision_check()
        _reschedule_session_summary()
        _reschedule_weekly_reports()
        _reschedule_sports_recap()
        _reschedule_market_movers()
        _reschedule_pead_scan()

    schedule.clear("reschedule")
    # Supervised: if the nightly DST-recompute silently dies, every daily job
    # freezes at the next DST boundary — exactly the kind of invisible failure
    # the dead-man's switch exists to catch.
    schedule.every().day.at("00:01").do(supervised_job("reschedule")(_combined_reschedule)).tag("reschedule")

    # --- Monthly journal (1st of each month at 00:05 UTC) ---
    schedule.every().day.at("00:05").do(
        supervised_job("monthly_journal")(
            lambda: run_monthly_journal() if datetime.now().day == 1 else None)
    ).tag("monthly_journal")

    while True:
        mark_alive("schedule_checker")  # dead-man's-switch heartbeat (audit §5 C3)
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
    then runs the full pipeline (news + SEC + Chinese market context + market regime + Claude)
    in a background thread so the bot remains responsive during generation.
    """
    try:
        bot.reply_to(message, "⚙️ *Nox Scout firing now...* Assembling data layers (market regime + news + SEC + China intel). Report coming in 40-60 seconds.", parse_mode='Markdown')
        threading.Thread(target=run_scout_protocol, daemon=True).start()
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /report command failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to trigger report: {str(e)}")

@bot.message_handler(commands=['pause'])
def pause_trading(message):
    """
    /pause [reason] - Global kill switch: halts ALL new order entries
    (equity + options) at the execution engine. Existing positions are left
    untouched — only new-entry submission is blocked. Persisted at the
    engine (execution/KillSwitchStore.hpp) so a restart doesn't silently
    resume trading. See also /resume.
    """
    reason = message.text.split(maxsplit=1)[1].strip() if len(message.text.split(maxsplit=1)) > 1 else None
    try:
        payload = {"reason": reason} if reason else {}
        resp = requests.post("http://execution-engine:8080/pause", json=payload, timeout=HTTP_TIMEOUT)
        if resp.status_code == 200:
            data = resp.json()
            bot.reply_to(message, f"🛑 Trading paused.\nReason: {data.get('reason', 'operator halt')}")
        else:
            bot.reply_to(message, f"⚠️ Pause request failed (HTTP {resp.status_code}) — engine may be unreachable.")
    except requests.RequestException as e:
        bot.reply_to(message, f"⚠️ Could not reach execution engine to pause trading: {e}")


@bot.message_handler(commands=['resume'])
def resume_trading(message):
    """/resume - Clears the global kill switch, whether triggered by /pause or the automatic daily-loss-limit halt."""
    try:
        resp = requests.post("http://execution-engine:8080/resume", timeout=HTTP_TIMEOUT)
        if resp.status_code == 200:
            bot.reply_to(message, "✅ Trading resumed. New entries are no longer blocked.")
        else:
            bot.reply_to(message, f"⚠️ Resume request failed (HTTP {resp.status_code}) — engine may be unreachable.")
    except requests.RequestException as e:
        bot.reply_to(message, f"⚠️ Could not reach execution engine to resume trading: {e}")


@bot.message_handler(commands=['killswitch', 'pausestatus'])
def kill_switch_status(message):
    """/killswitch - Reports whether the global kill switch is currently active and why."""
    try:
        resp = requests.get("http://execution-engine:8080/kill-switch-status", timeout=HTTP_TIMEOUT)
        if resp.status_code != 200:
            bot.reply_to(message, f"⚠️ Status request failed (HTTP {resp.status_code}).")
            return
        data = resp.json()
        if not data.get("available", False):
            bot.reply_to(message, "⚠️ Kill switch store unavailable at the engine — cannot report status.")
        elif data.get("paused"):
            bot.reply_to(message,
                f"🛑 Trading is PAUSED\nTriggered by: {data.get('triggered_by', 'unknown')}\nReason: {data.get('reason', '')}")
        else:
            bot.reply_to(message, "✅ Trading is active — no halt in effect.")
    except requests.RequestException as e:
        bot.reply_to(message, f"⚠️ Could not reach execution engine: {e}")


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


@bot.message_handler(commands=['pulse'])
def cmd_pulse(message):
    """
    /pulse — Fast intraday market pulse (VIX + headlines + gap analysis).
    Gathers current VIX, recent headlines, contradiction verdicts, and upcoming
    earnings; sends to Claude for a short market read + up to 3 gaps the
    trader may not be thinking about given their current positions.
    Response is fast (~10 seconds) because all data is cached in the data engines.
    """
    try:
        bot.reply_to(message, "📡 *Market Pulse analyzing...* Gathering VIX, headlines, and position gaps.", parse_mode='Markdown')

        # 1. Fetch VIX
        vix = fetch_vix_level()

        # 2. Fetch headlines from america-data-engine.
        # query_data_engine() returns (payload, ok) — ok=False means the
        # engine was unreachable, distinct from a legitimately empty cache.
        news_data, news_ok = query_data_engine("/news/us", "http://america-data-engine:8001")
        headlines = [a.get("headline", "") for a in news_data.get("news", [])[:8]] if news_ok else []

        # 3. Fetch contradiction verdicts
        contradiction_data, contradiction_ok = query_data_engine("/contradiction/us", "http://america-data-engine:8001")
        contradictions = {}
        if contradiction_ok:
            for result in contradiction_data.get("results", []):
                if isinstance(result, dict):
                    ticker = result.get("ticker")
                    verdict = result.get("verdict", "NEUTRAL")
                    if ticker and verdict != "NEUTRAL":
                        contradictions[ticker] = verdict

        # 4. Fetch upcoming earnings (next 5 days)
        earnings_data, earnings_ok = query_data_engine("/earnings/calendar", "http://america-data-engine:8001")
        upcoming_earnings_tickers = set()
        if earnings_ok:
            earnings_cal = earnings_data.get("earnings_calendar", {})
            today = datetime.now()
            for ticker, events in earnings_cal.items():
                for event in (events or []):
                    try:
                        event_date = datetime.strptime(event.get("date", ""), "%Y-%m-%d").date()
                        days_until = (event_date - today.date()).days
                        if 0 <= days_until <= 5:
                            upcoming_earnings_tickers.add(ticker)
                    except (ValueError, AttributeError):
                        pass

        # 5. Fetch current positions from Alpaca
        positions_text = "No open positions"
        try:
            pos_resp = requests.get(
                f"{ALPACA_BROKER_URL}/v2/positions",
                headers={
                    "APCA-API-KEY-ID": ALPACA_API,
                    "APCA-API-SECRET-KEY": ALPACA_SEC,
                },
                timeout=HTTP_TIMEOUT
            )
            if pos_resp.status_code == 200:
                positions = pos_resp.json()
                if positions:
                    position_strs = []
                    for p in positions:
                        try:
                            plpc = float(p.get("unrealized_plpc", 0))
                            position_strs.append(
                                f"{p['symbol']} ({p['side']}, {plpc*100:+.1f}%)"
                            )
                        except (ValueError, KeyError):
                            position_strs.append(f"{p.get('symbol', '?')} ({p.get('side', '?')})")
                    if position_strs:
                        positions_text = ", ".join(position_strs)
        except Exception as e:
            logger.warning(f"Failed to fetch positions: {e}")
            positions_text = "Positions unavailable"

        # 6. Build Claude prompt
        prompt = (
            f"VIX: {vix:.1f}\n\n"
            f"Recent US headlines:\n" +
            "\n".join(f"- {h}" for h in headlines if h) +
            f"\n\nCurrent positions: {positions_text}\n\n"
            f"Contradiction signals (text vs IV): {contradictions if contradictions else 'None flagged'}\n\n"
            f"Upcoming earnings (next 5 days): {', '.join(sorted(upcoming_earnings_tickers)) if upcoming_earnings_tickers else 'None'}\n\n"
            "Answer in TWO sections (short and direct):\n\n"
            "MARKET READ (2-3 sentences): What is driving the tape right now? Is sentiment constructive or cautious? One actionable insight.\n\n"
            "GAPS (max 3 bullets): What risk or opportunity is this trader likely NOT thinking about right now? "
            "Reference actual tickers, contradictions, or upcoming catalysts from the data above. "
            "Skip this section if there are no material gaps."
        )

        # 7. Call Claude
        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=500,
            system="You are a quantitative market analyst. Be direct and specific. Assume the trader knows technicals and fundamentals. No preamble.",
            messages=[{"role": "user", "content": prompt}]
        )

        analysis = _extract_text(response)

        # 8. Send response
        bot.reply_to(
            message,
            f"📡 *Market Pulse* — VIX {vix:.1f}\n\n{analysis}",
            parse_mode='Markdown'
        )

        logger.info("Pulse command completed successfully")
    except Exception as e:
        logger.error(f"Pulse command failed: {e}")
        bot.reply_to(message, f"⚠️ Pulse analysis failed: {str(e)}")


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
                         "• Market scanner hasn't triggered (check market hours)\n"
                         "• Auth secret mismatch (signal silently dropped)\n"
                         "• Execution engine not running or network issue")
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
            source = s.get('source', '') or 'webhook'

            lines.append(
                f"• `{ts}` *{escape_markdown(action)}* {escape_markdown(ticker)} "
                f"@ ${price:.2f} RSI={rsi:.1f} VIX={vix:.1f} [{escape_markdown(source)}]"
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


@bot.message_handler(commands=['cn_status'])
def send_cn_status(message):
    """
    /cn_status — Single-command diagnostic for CN-RULE-001/002 (Chinese A-share
    board-lot truncation + T+1 settlement gate). Answers "is CN protection
    currently active, and what does it think it's tracking" without grepping
    logs or reading code. Queries /cn-status on the execution engine.
    """
    try:
        resp = requests.get("http://execution-engine:8080/cn-status", timeout=HTTP_TIMEOUT)
        if resp.status_code != 200:
            bot.reply_to(message, f"⚠️ Execution engine returned HTTP {resp.status_code}.")
            return

        data = resp.json()
        lot_size    = data.get("board_lot_size", "?")
        gate_active = data.get("gate_active", False)
        today       = data.get("today", "?")
        positions   = data.get("positions", [])

        status_line = (
            "🟢 *ACTIVE* — board-lot truncation and T+1 gate are enforced on every ticker"
            if gate_active else
            "⚪ *DORMANT* — board_lot_size=1, no CN-specific restriction applied to any ticker"
        )

        lines = [
            f"🇨🇳 *CN-RULE-001/002 Status*",
            f"────────────────────────",
            f"• *Board Lot Size:* {lot_size}",
            f"• *Gate:* {status_line}",
            f"• *Today (ET):* {today}",
        ]

        if positions:
            lines.append(f"\n*T+1 Tracked Positions ({len(positions)}):*")
            for p in positions:
                cleared = "✅ cleared" if p.get("cleared") else "⏳ pending"
                lines.append(f"• {p.get('ticker','?')} — entry {p.get('entry_date','?')} ({cleared})")
        elif gate_active:
            lines.append("\n_No positions currently tracked._")

        bot.reply_to(message, "\n".join(lines), parse_mode="Markdown")
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /cn_status command failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to fetch CN status: {str(e)}")


@bot.message_handler(commands=['trades'])
def send_trades(message):
    """
    /trades [n] — Last N executed trades from the persistent trade ledger
    (trade_history). Unlike /signals (in-memory, wiped on engine restart), this
    reads the DB so filled entries/exits and realized P&L survive restarts.
    Defaults to 15, capped at 50.
    """
    try:
        parts = message.text.strip().split()
        try:
            requested = int(parts[1]) if len(parts) > 1 else 15
            count = max(1, min(requested, 50))
        except (ValueError, IndexError):
            count = 15

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT timestamp, ticker, action, asset_class, quantity, price, pnl, detail "
                    "FROM trade_history ORDER BY id DESC LIMIT ?",
                    (count,),
                )
                rows = c.fetchall()

        if not rows:
            bot.reply_to(
                message,
                "📭 No trades recorded yet.\n\n"
                "The engine records every equity/option entry and exit here once it "
                "places or closes an order. If this stays empty during market hours, "
                "check that signals are arriving (/signals) and orders aren't being "
                "blocked by a gate.",
            )
            return

        lines = []
        for ts, ticker, action, asset_class, qty, price, pnl, detail in rows:
            icon = "🟢" if action in ("BUY", "OPEN") else "🔴" if action in ("SELL", "CLOSE") else "⚪"
            kind = "opt" if (asset_class or "") == "option" else "eq"
            pnl_str = ""
            if pnl is not None and action in ("SELL", "CLOSE"):
                pnl_str = f" | P&L ${pnl:+.2f}"
            qty_str = f"x{qty:g}" if qty else ""
            lines.append(
                f"{icon} `{str(ts)[:19]}` *{action}* {ticker} [{kind}] "
                f"{qty_str} @ ${price:.2f}{pnl_str}"
            )

        bot.reply_to(
            message,
            f"📒 *Last {len(rows)} executed trade(s):*\n\n" + "\n".join(lines),
            parse_mode="Markdown",
        )
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /trades command failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to fetch trades: {str(e)}")


@bot.message_handler(commands=['details'])
def send_details(message):
    """
    /details [n] — Full breakdown of the last N signals received by the execution engine.
    Shows ticker, action, price, RSI, VIX, and timestamp. Defaults to 5, capped at 20.
    """
    try:
        parts = message.text.strip().split()
        try:
            requested = int(parts[1]) if len(parts) > 1 else 5
            count = max(1, min(requested, 20))
        except (ValueError, IndexError):
            count = 5

        resp = requests.get("http://execution-engine:8080/recent-signals", timeout=HTTP_TIMEOUT)
        if resp.status_code != 200:
            bot.reply_to(message, f"⚠️ Engine returned HTTP {resp.status_code}.")
            return

        signals = resp.json()
        if not signals:
            bot.reply_to(message, "📭 No signals on record yet.")
            return

        signals = signals[-count:]
        lines = []
        for s in reversed(signals):
            ts     = s.get('received_at', '?')[:16].replace('T', ' ')
            ticker = s.get('ticker', '?')
            action = s.get('action', '?')
            price  = s.get('price', 0.0)
            rsi    = s.get('rsi', 0.0)
            vix    = s.get('vix', 0.0)
            source = s.get('source', '') or 'webhook'
            icon   = "🟢" if action == "BUY" else "🔴" if action == "SELL" else "⚪"
            lines.append(
                f"{icon} *{ticker}* {action}  `{ts}`  [{source}]\n"
                f"   Price ${price:.2f} · RSI {rsi:.1f} · VIX {vix:.1f}"
            )

        bot.reply_to(
            message,
            f"📋 *Last {len(signals)} signal(s):*\n\n" + "\n\n".join(lines),
            parse_mode="Markdown",
        )
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /details command failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to fetch details: {str(e)}")


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

# ---------------------------------------------------------------------------
# WS7 — Information Lag Window: tracks the period between a material 6-K SEC
# filing and Chinese retail media pickup (china-data-engine /lag/check).
# process_automated_filing() opens a window via _lag_open_window() when it
# detects a heavyweight 6-K for a CN watchlist ticker; _lag_monitor_loop()
# polls china-data-engine every 15 minutes and closes/grades windows.
# ---------------------------------------------------------------------------
_CN_DATA_ENGINE_URL = "http://china-data-engine:8000"
_LAG_WINDOW_MAX_HOURS = 48  # auto-expire windows older than this


def _lag_open_window(ticker: str, filing_url: str, materiality_score: float) -> int:
    """Open a new lag window. Returns row id. Skips if already open for ticker."""
    now = datetime.utcnow().isoformat()
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            existing = conn.execute(
                "SELECT id FROM lag_windows WHERE ticker=? AND closed_at IS NULL",
                (ticker,),
            ).fetchone()
            if existing:
                return existing[0]
            cur = conn.execute(
                "INSERT INTO lag_windows (ticker, filing_url, materiality_score, opened_at) VALUES (?,?,?,?)",
                (ticker, filing_url, materiality_score, now),
            )
            return cur.lastrowid


def _lag_close_window(window_id: int, closed_by_source: str) -> float:
    """Close a lag window, compute lag_hours, return it."""
    now_dt = datetime.utcnow()
    now_iso = now_dt.isoformat()
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            row = conn.execute(
                "SELECT opened_at FROM lag_windows WHERE id=? AND closed_at IS NULL",
                (window_id,),
            ).fetchone()
            if not row:
                return 0.0
            opened = datetime.fromisoformat(row[0])
            lag_hours = round((now_dt - opened).total_seconds() / 3600, 2)
            conn.execute(
                "UPDATE lag_windows SET closed_at=?, closed_by_source=?, lag_hours=? WHERE id=?",
                (now_iso, closed_by_source, lag_hours, window_id),
            )
            return lag_hours


def _lag_get_open_windows() -> list:
    """Return all open, non-expired lag windows (includes materiality_score)."""
    now_dt = datetime.utcnow()
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            rows = conn.execute(
                """SELECT id, ticker, filing_url, materiality_score, opened_at
                   FROM lag_windows WHERE closed_at IS NULL"""
            ).fetchall()

    active = []
    for row in rows:
        wid, ticker, filing_url, mat_score, opened_str = row
        opened = datetime.fromisoformat(opened_str)
        age_hours = (now_dt - opened).total_seconds() / 3600
        if age_hours >= _LAG_WINDOW_MAX_HOURS:
            _lag_close_window(wid, "TIMEOUT")
        else:
            active.append({
                "id":                wid,
                "ticker":            ticker,
                "filing_url":        filing_url,
                "materiality_score": mat_score or 0.0,
                "opened_at":         opened_str,
            })
    return active


def _check_lag_window_for_ticker(ticker: str) -> dict:
    """Query china-data-engine /lag/check for a ticker. Returns {} on failure."""
    try:
        r = requests.get(
            f"{_CN_DATA_ENGINE_URL}/lag/check",
            params={"ticker": ticker},
            headers={"X-Nox-Token": WEBHOOK_SECRET},
            timeout=HTTP_TIMEOUT,
        )
        if r.status_code == 200:
            return r.json()
    except Exception as e:
        print(f"[WARN] [HEARTBEAT] /lag/check failed for {ticker}: {e}", flush=True)
    return {}


def _fetch_bars_range(ticker: str, start_iso: str, days: int = 10) -> list:
    """
    Fetch up to `days` daily bars from Alpaca starting at start_iso (UTC ISO string).
    Returns list of {t, o, h, l, c, v} dicts, oldest first.
    """
    headers = {
        "APCA-API-KEY-ID":     ALPACA_API,
        "APCA-API-SECRET-KEY": ALPACA_SEC,
    }
    try:
        resp = requests.get(
            f"{ALPACA_DATA_URL}/v2/stocks/{ticker}/bars",
            headers=headers,
            params={
                "timeframe":  "1Day",
                "start":      start_iso[:10] + "T00:00:00Z",
                "limit":      days,
                "adjustment": "raw",
                "feed":       "iex",
            },
            timeout=HTTP_TIMEOUT,
        )
        if resp.status_code == 200:
            return resp.json().get("bars", [])
    except Exception as e:
        print(f"[WARN] [HEARTBEAT] WS7 bars fetch failed for {ticker}: {e}", flush=True)
    return []


def _grade_lag_window(window_id: int, ticker: str, filing_url: str,
                      materiality_score: float, opened_at: str,
                      lag_hours: float, closed_by_source: str) -> None:
    """
    Background task: compute AR for a just-closed lag window, call Claude Haiku
    to grade it (A/B/C/F), and persist the result to SQLite.

    AR = ticker_return − MCHI_return over the lag window period.
    Grade rubric:
      A  material + |AR| > 1%  + closed by CN media (confirmed edge)
      B  material + |AR| > 0.3% or moderate materiality
      C  low materiality or near-zero AR
      F  reverse return or TIMEOUT (CN media never picked up)
    """
    try:
        ticker_bars = _fetch_bars_range(ticker, opened_at, days=10)
        mchi_bars   = _fetch_bars_range("MCHI",  opened_at, days=10)

        ar = None
        if ticker_bars and mchi_bars:
            t_entry = ticker_bars[0]["c"]
            m_entry = mchi_bars[0]["c"]
            # Find the bar nearest to lag_hours after open
            lag_days = max(1, int(lag_hours / 24) + 1)
            t_idx = min(lag_days, len(ticker_bars) - 1)
            m_idx = min(lag_days, len(mchi_bars)  - 1)
            t_exit = ticker_bars[t_idx]["c"]
            m_exit = mchi_bars[m_idx]["c"]
            if t_entry and m_entry:
                ar = ((t_exit - t_entry) / t_entry) - ((m_exit - m_entry) / m_entry)
                ar = round(ar, 6)

        ar_pct = f"{ar * 100:+.2f}%" if ar is not None else "N/A"

        prompt = (
            f"Ticker: {ticker}\n"
            f"Lag duration: {lag_hours:.1f}h\n"
            f"Materiality score: {materiality_score:.2f} (0=routine, 1=highly material)\n"
            f"Closed by: {closed_by_source}\n"
            f"Abnormal return during window: {ar_pct} vs MCHI\n\n"
            f"Grade this WS7 lag window event.\n"
            f"A=clear edge (material, |AR|>1%, closed by CN media)\n"
            f"B=moderate edge (material or |AR|>0.3%)\n"
            f"C=weak (low materiality or near-zero AR)\n"
            f"F=no edge (reverse return or TIMEOUT)\n\n"
            f"Reply strictly: GRADE: [A/B/C/F] | REASONING: [max 100 chars]"
        )
        response = claude.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=80,
            system=(
                "You are the WS7 Signal Grader for Nox, a quant trading system. "
                "Grade lag window events objectively. Be terse."
            ),
            messages=[{"role": "user", "content": prompt}],
        )
        raw = _extract_text(response).strip()
        # Parse "GRADE: A | REASONING: ..."
        grade, reasoning = "?", raw
        if "GRADE:" in raw and "REASONING:" in raw:
            try:
                parts = raw.split("|")
                grade    = parts[0].split("GRADE:")[-1].strip()
                reasoning = parts[1].split("REASONING:")[-1].strip()[:120]
            except Exception:
                pass

        now_iso = datetime.utcnow().isoformat()
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                conn.execute(
                    """UPDATE lag_windows
                       SET abnormal_return=?, grade=?, grade_reasoning=?, graded_at=?
                       WHERE id=?""",
                    (ar, grade, reasoning, now_iso, window_id),
                )
        print(
            f"[INFO] [HEARTBEAT] WS7 graded window {window_id} ({ticker}): "
            f"{grade} | AR={ar_pct} | {reasoning}",
            flush=True,
        )
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] WS7 grading failed for window {window_id}: {e}", flush=True)


def _lag_monitor_loop():
    """
    Background thread — polls china-data-engine every 15 minutes for all open
    lag windows. When a window closes (ticker appears in CN media), fires a
    Telegram alert with the measured lag duration, then spawns a grading thread.
    """
    print("[INFO] [HEARTBEAT] WS7 lag monitor loop started.", flush=True)
    while True:
        mark_alive("lag_monitor")  # dead-man's-switch heartbeat (audit §5 C3)
        time.sleep(900)  # 15 minutes — same cadence as china-data-engine refresh
        try:
            open_windows = _lag_get_open_windows()
            for window in open_windows:
                presence = _check_lag_window_for_ticker(window["ticker"])
                if not presence:
                    continue
                if not presence.get("lag_open", True):
                    source = (
                        "East Money" if presence.get("is_on_hot_board") else "Cailian"
                    )
                    lag_hours = _lag_close_window(window["id"], source.lower().replace(" ", "_"))
                    msg = (
                        f"🔔 *[WS7] LAG WINDOW CLOSED — {window['ticker']}*\n"
                        f"Source: {source}\n"
                        f"Lag: *{lag_hours:.1f}h* since 6-K filing\n"
                        f"[Filing]({window['filing_url']})"
                    )
                    try:
                        bot.send_message(CHAT_ID, msg, parse_mode="Markdown")
                    except Exception as e:
                        print(f"[WARN] [HEARTBEAT] WS7 Telegram send failed: {e}", flush=True)
                    # Spawn grading asynchronously — doesn't block the poll cycle
                    threading.Thread(
                        target=_grade_lag_window,
                        args=(
                            window["id"], window["ticker"], window["filing_url"],
                            window["materiality_score"], window["opened_at"],
                            lag_hours, source,
                        ),
                        daemon=True,
                    ).start()
        except Exception as e:
            print(f"[ERROR] [HEARTBEAT] WS7 lag monitor loop error: {e}", flush=True)


@bot.message_handler(commands=['lagstats'])
def send_lag_stats(message):
    """
    /lagstats — WS7 meta-analysis report.
    Shows grade distribution, mean lag, mean AR, and last 10 graded events
    from the lag_windows SQLite table.
    """
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                rows = conn.execute(
                    """SELECT ticker, lag_hours, abnormal_return, grade,
                              grade_reasoning, closed_by_source, closed_at
                       FROM lag_windows
                       WHERE closed_at IS NOT NULL
                       ORDER BY closed_at DESC
                       LIMIT 50"""
                ).fetchall()

        if not rows:
            bot.reply_to(message, "📭 No closed lag windows yet. Waiting for 6-K detections.")
            return

        graded = [(r[0], r[1], r[2], r[3], r[4], r[5]) for r in rows if r[3]]
        grades = {"A": 0, "B": 0, "C": 0, "F": 0}
        ar_by_grade = {"A": [], "B": [], "C": [], "F": []}
        for _, lag_h, ar, grade, _, _ in graded:
            g = grade.strip().upper() if grade else "?"
            if g in grades:
                grades[g] += 1
                if ar is not None:
                    ar_by_grade[g].append(ar)

        total_graded = sum(grades.values())
        mean_lag = sum(r[1] for r in rows if r[1]) / max(len(rows), 1)

        lines = [
            f"📊 *WS7 Lag Window — Meta Analysis*",
            f"Total closed windows: {len(rows)} | Graded: {total_graded}",
            f"Mean lag: *{mean_lag:.1f}h*",
            "",
            "*Grade Distribution:*",
        ]
        for g in ["A", "B", "C", "F"]:
            count = grades[g]
            ars = ar_by_grade[g]
            mean_ar = f"{sum(ars)/len(ars)*100:+.2f}%" if ars else "N/A"
            lines.append(f"  `{g}` ×{count}  mean AR={mean_ar}")

        lines += ["", "*Last 10 Events:*"]
        for row in rows[:10]:
            ticker, lag_h, ar, grade, reasoning, source, closed_at = row
            lag_str = f"{lag_h:.1f}h" if lag_h else "—"
            ar_str  = f"{ar*100:+.2f}%" if ar is not None else "—"
            g_str   = grade if grade else "?"
            lines.append(
                f"`{ticker}` {g_str} | lag={lag_str} | AR={ar_str} | via {source or '?'}"
            )
            if reasoning:
                lines.append(f"  _{reasoning}_")

        bot.reply_to(message, "\n".join(lines), parse_mode="Markdown")
    except Exception as e:
        bot.reply_to(message, f"⚠️ /lagstats failed: {e}")


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
        mark_alive("poll_sec_edgar")  # dead-man's-switch heartbeat (audit §5 C3)
        for feed_type, sec_url in SEC_FEEDS:
            response = fetch_with_retry(sec_url, source=f"SEC radar:{feed_type}", headers=headers, timeout=HTTP_TIMEOUT)
            if response is None:
                print(f"[WARN] [HEARTBEAT] SEC {feed_type} feed unreachable after retries.", flush=True)
                continue
            if response.status_code != 200:
                print(f"[WARN] [HEARTBEAT] SEC {feed_type} feed returned {response.status_code}.", flush=True)
                continue

            try:
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

def _strip_ixbrl_viewer(href: str, base_url: str = "https://www.sec.gov") -> str:
    """Normalize an EDGAR index-table document href to a fetchable raw-document URL.

    EDGAR now lists the primary document as its inline-XBRL VIEWER URL
    (…/ix?doc=/Archives/…htm). That viewer returns a ~6KB JavaScript shell whose
    only text is "Please enable JavaScript to use the EDGAR Inline XBRL Viewer" —
    so the filing came back as 73 chars of "enable JavaScript" and the analyst
    reported it could not read the filing. Stripping the `ix?doc=` wrapper yields
    the raw server-rendered document the BeautifulSoup get_text() pipeline parses
    correctly. Also makes a relative href absolute.
    """
    if "ix?doc=" in href:
        href = href.split("ix?doc=", 1)[1]
    return href if href.startswith("http") else base_url + href


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
    idx_res = fetch_with_retry(index_url, source=f"SEC index page:{index_url}", headers=headers, timeout=HTTP_TIMEOUT)
    if idx_res is None:
        print(f"[WARN] [HEARTBEAT] Index page unreachable after retries: {index_url}", flush=True)
        return None
    if idx_res.status_code != 200:
        print(f"[WARN] [HEARTBEAT] Index page returned {idx_res.status_code}: {index_url}", flush=True)
        return None

    try:
        soup = BeautifulSoup(idx_res.text, "html.parser")

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
                        return _strip_ixbrl_viewer(link_tag["href"])

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
                    return _strip_ixbrl_viewer(link_tag["href"])

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
        analysis = _extract_text(response)
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
            summaries.append(_extract_text(response))
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
        doc_res = fetch_with_retry(primary_url, source=f"SEC {filing_type} doc:{ticker}", headers=headers, timeout=HTTP_TIMEOUT)
        if doc_res is None:
            print(f"[WARN] [HEARTBEAT] Primary doc unreachable for {ticker} after retries, skipping.", flush=True)
            return
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


# ── End-of-Day / End-of-Week account summaries ───────────────────────────────
# These read the canonical trade_history ledger (written by the execution engine)
# plus a live Alpaca account snapshot, and are pushed to Telegram at the close.
# Before this existed there was no EOD/EOW trade report at all — only a 9 AM news
# briefing and a monthly journal.

def _fetch_account_and_positions():
    """Return (account_dict, positions_list) from Alpaca, or (None, None)."""
    headers = {'APCA-API-KEY-ID': ALPACA_API, 'APCA-API-SECRET-KEY': ALPACA_SEC}
    try:
        acc = requests.get(f'{ALPACA_BROKER_URL}/v2/account',
                           headers=headers, timeout=HTTP_TIMEOUT)
        pos = requests.get(f'{ALPACA_BROKER_URL}/v2/positions',
                           headers=headers, timeout=HTTP_TIMEOUT)
        if acc.status_code != 200 or pos.status_code != 200:
            return None, None
        a, p = acc.json(), pos.json()
        if not isinstance(a, dict) or not isinstance(p, list):
            return None, None
        return a, p
    except Exception as e:
        logger.warning(f"account snapshot fetch failed: {e}")
        return None, None


def _period_start_utc(scope: str) -> str:
    """
    UTC 'YYYY-MM-DD HH:MM:SS' string for the start of the reporting window.
    'day'  → midnight ET today; 'week' → most recent Monday 00:00 ET.
    trade_history.timestamp is stored in UTC, so we compare against a UTC bound.
    """
    et  = ZoneInfo("America/New_York")
    utc = ZoneInfo("UTC")
    now_et = datetime.now(et)
    start_et = now_et.replace(hour=0, minute=0, second=0, microsecond=0)
    if scope == "week":
        start_et = start_et - timedelta(days=now_et.weekday())  # back to Monday
    return start_et.astimezone(utc).strftime("%Y-%m-%d %H:%M:%S")


def generate_activity_report(scope: str) -> str:
    """Build a full account-summary report ('day' or 'week') as a Markdown string."""
    et = ZoneInfo("America/New_York")
    now = datetime.now(et)
    title = "End-of-Day" if scope == "day" else "End-of-Week"
    period_desc = now.strftime("%A, %Y-%m-%d") if scope == "day" \
        else f"week ending {now.strftime('%A, %Y-%m-%d')}"
    start_utc = _period_start_utc(scope)

    # ── Live account snapshot ──────────────────────────────────────────────
    acc, positions = _fetch_account_and_positions()
    if acc is not None:
        equity     = float(acc.get('portfolio_value', 0) or 0)
        last_equity = float(acc.get('last_equity', 0) or 0)
        cash       = float(acc.get('cash', 0) or 0)
        buying_pw  = float(acc.get('buying_power', 0) or 0)
        day_change = equity - last_equity
        day_pct    = (day_change / last_equity * 100) if last_equity else 0.0
        acct_block = (
            f"• *Equity:* ${equity:,.2f}\n"
            f"• *Since prior close:* ${day_change:+,.2f} ({day_pct:+.2f}%)\n"
            f"• *Cash:* ${cash:,.2f}  |  *Buying power:* ${buying_pw:,.2f}"
        )
        open_positions = positions or []
    else:
        acct_block = "_⚠️ Could not fetch live account snapshot from Alpaca._"
        open_positions = []

    # ── Open positions w/ unrealized P&L ───────────────────────────────────
    if open_positions:
        pos_lines = []
        total_unreal = 0.0
        for p in open_positions:
            if not isinstance(p, dict):
                continue
            upl = float(p.get('unrealized_pl', 0) or 0)
            total_unreal += upl
            uplpc = float(p.get('unrealized_plpc', 0) or 0) * 100
            pos_lines.append(
                f"• {p.get('symbol','?')} x{p.get('qty','?')} @ "
                f"${float(p.get('avg_entry_price',0) or 0):.2f} → "
                f"${float(p.get('current_price',0) or 0):.2f}  "
                f"(${upl:+,.2f} / {uplpc:+.1f}%)"
            )
        pos_block = "\n".join(pos_lines) + f"\n*Total unrealized:* ${total_unreal:+,.2f}"
    else:
        pos_block = "_No open positions._"

    # ── Realized trades in the window (from the ledger) ────────────────────
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()
            c.execute(
                "SELECT ticker, action, asset_class, quantity, price, pnl, detail, timestamp "
                "FROM trade_history WHERE timestamp >= ? ORDER BY id ASC",
                (start_utc,),
            )
            trades = c.fetchall()

    entries = [t for t in trades if t[1] in ("BUY", "OPEN")]
    exits   = [t for t in trades if t[1] in ("SELL", "CLOSE")]
    realized = sum((t[5] or 0.0) for t in exits)
    wins   = [t for t in exits if (t[5] or 0.0) > 0]
    losses = [t for t in exits if (t[5] or 0.0) < 0]
    win_rate = (len(wins) / len(exits) * 100) if exits else 0.0

    def _class_pnl(cls):
        return sum((t[5] or 0.0) for t in exits if (t[2] or "equity") == cls)
    eq_pnl, opt_pnl = _class_pnl("equity"), _class_pnl("option")

    def _fmt_trades(rows, n=8):
        if not rows:
            return "_None_"
        out = []
        for ticker, action, cls, qty, price, pnl, detail, ts in rows[:n]:
            kind = "opt" if (cls or "") == "option" else "eq"
            pnl_str = f" | ${pnl:+.2f}" if (action in ("SELL", "CLOSE") and pnl is not None) else ""
            out.append(f"• `{str(ts)[11:16]}` {action} {ticker} [{kind}] "
                       f"x{qty:g} @ ${price:.2f}{pnl_str}")
        extra = f"\n_…and {len(rows) - n} more_" if len(rows) > n else ""
        return "\n".join(out) + extra

    best  = max(exits, key=lambda t: (t[5] or 0.0), default=None)
    worst = min(exits, key=lambda t: (t[5] or 0.0), default=None)
    def _one(t):
        return f"{t[0]} {t[1]} ${t[5]:+.2f}" if t else "—"

    report = (
        f"📊 *{title} Report — {period_desc}*\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
        f"*Account*\n{acct_block}\n\n"
        f"*Trades this {'day' if scope == 'day' else 'week'}*\n"
        f"• Entries: {len(entries)}  |  Exits: {len(exits)}\n"
        f"• Realized P&L: ${realized:+,.2f}\n"
        f"• Win rate: {win_rate:.0f}%  ({len(wins)}W / {len(losses)}L)\n"
        f"• By class: equity ${eq_pnl:+,.2f}  |  options ${opt_pnl:+,.2f}\n"
        f"• Best: {_one(best)}  |  Worst: {_one(worst)}\n\n"
        f"*Open Positions*\n{pos_block}\n\n"
        f"*Entries*\n{_fmt_trades(entries)}\n\n"
        f"*Exits*\n{_fmt_trades(exits)}\n\n"
        f"_Generated {now.strftime('%Y-%m-%d %H:%M ET')} from the trade ledger._"
    )
    return report


def _send_report(scope: str):
    """Generate and Telegram-push an EOD/EOW report, chunking to Telegram limits."""
    try:
        text = generate_activity_report(scope)
        for chunk in smart_split(text, chars_per_string=4096):
            bot.send_message(CHAT_ID, chunk, parse_mode="Markdown")
        logger.info(f"{scope}-report sent.")
    except Exception as e:
        logger.error(f"{scope}-report failed: {e}")
        try:
            bot.send_message(CHAT_ID, f"⚠️ {scope.upper()} report failed to generate: {e}")
        except Exception:
            pass


def generate_narrative_report(scope: str) -> str:
    """
    Build a non-trading-day report ('day' or 'week'): no trade ledger table
    (it would just be empty), instead a Claude-written explanation of why the
    Skeptic pipeline made the moves it made, a market read, and upcoming
    catalysts framed for a reader who isn't a market/tech expert.
    """
    et = ZoneInfo("America/New_York")
    now = datetime.now(et)
    title = "Weekend/Holiday Briefing" if scope == "day" else "Weekly Briefing"
    period_desc = now.strftime("%A, %Y-%m-%d") if scope == "day" \
        else f"week ending {now.strftime('%A, %Y-%m-%d')}"
    start_utc = _period_start_utc(scope)

    # ── Grounding: the Skeptic's own daily narratives + the raw trade list ──
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()
            c.execute(
                "SELECT timestamp, claude_analysis FROM daily_audits "
                "WHERE timestamp >= ? ORDER BY id ASC",
                (start_utc,),
            )
            audits = c.fetchall()
            c.execute(
                "SELECT ticker, action, asset_class, quantity, price, pnl, timestamp "
                "FROM trade_history WHERE timestamp >= ? ORDER BY id ASC",
                (start_utc,),
            )
            trades = c.fetchall()

    audit_text = "\n\n".join(f"[{ts}]\n{analysis}" for ts, analysis in audits) or "No Skeptic analysis recorded for this period."
    trade_text = "\n".join(
        f"- {ts} {action} {ticker} ({cls}) x{qty:g} @ ${price:.2f}"
        + (f", P&L ${pnl:+.2f}" if action in ("SELL", "CLOSE") and pnl is not None else "")
        for ticker, action, cls, qty, price, pnl, ts in trades
    ) or "No trades were recorded for this period."

    # ── Fresh market context — same calls cmd_pulse already relies on ──────
    vix = fetch_vix_level()
    news_data, news_ok = query_data_engine("/news/us", "http://america-data-engine:8001")
    headlines = [a.get("headline", "") for a in news_data.get("news", [])[:8]] if news_ok else []
    contradiction_data, contradiction_ok = query_data_engine("/contradiction/us", "http://america-data-engine:8001")
    contradictions = {}
    if contradiction_ok:
        for result in contradiction_data.get("results", []):
            if isinstance(result, dict):
                ticker = result.get("ticker")
                verdict = result.get("verdict", "NEUTRAL")
                if ticker and verdict != "NEUTRAL":
                    contradictions[ticker] = verdict
    earnings_data, earnings_ok = query_data_engine("/earnings/calendar", "http://america-data-engine:8001")
    upcoming_earnings = []
    if earnings_ok:
        earnings_cal = earnings_data.get("earnings_calendar", {})
        today = datetime.now()
        for ticker, events in earnings_cal.items():
            for event in (events or []):
                try:
                    event_date = datetime.strptime(event.get("date", ""), "%Y-%m-%d").date()
                    days_until = (event_date - today.date()).days
                    if 0 <= days_until <= 7:
                        upcoming_earnings.append(f"{ticker} ({event.get('date')})")
                except (ValueError, AttributeError):
                    pass

    prompt = (
        f"Period: {period_desc} (no live trading today — market closed for the weekend/holiday).\n\n"
        f"Skeptic pipeline's own daily analyses this period:\n{audit_text}\n\n"
        f"Trades recorded this period:\n{trade_text}\n\n"
        f"Current VIX: {vix:.1f}\n"
        f"Recent US headlines:\n" + "\n".join(f"- {h}" for h in headlines if h) + "\n\n"
        f"Contradiction signals (text vs IV): {contradictions if contradictions else 'None flagged'}\n\n"
        f"Upcoming earnings (next 7 days): {', '.join(upcoming_earnings) if upcoming_earnings else 'None'}\n\n"
        "Write a briefing in THREE sections:\n\n"
        "WHY THE SKEPTIC MOVED — Explain, in plain language, the reasoning behind this period's trades "
        "(or, if none occurred, why the Skeptic found no qualifying setups), grounded in the analyses "
        "and trade list above. Be direct that this is a numbers-driven test system without a "
        "hand-crafted strategy — the Skeptic pipeline (contradiction/insider/macro checks) is the "
        "closest thing to a strategy it currently has.\n\n"
        "MARKET READ — 2-4 sentences on what's driving the tape and whether sentiment is constructive "
        "or cautious.\n\n"
        "UPCOMING EVENTS & EXPERT PERSPECTIVE — Explain the catalysts a technologist or market expert "
        "would flag going into next week, in language a non-expert reader can learn from and act on."
    )

    response = claude.messages.create(
        model="claude-sonnet-5",
        max_tokens=1200,
        system="You are a quantitative market analyst writing an educational weekend briefing for "
               "someone new to trading. Be honest, specific, and reference actual tickers/events from "
               "the data given. No preamble.",
        messages=[{"role": "user", "content": prompt}],
    )
    narrative = _narrative_text(response)

    return (
        f"🌙 *{title} — {period_desc}*\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
        f"{narrative}\n\n"
        f"_Generated {now.strftime('%Y-%m-%d %H:%M ET')}. Markets closed — no trade ledger to report._"
    )


def _send_narrative_report(scope: str):
    """Generate and Telegram-push a non-trading-day (weekend/holiday) briefing."""
    try:
        text = generate_narrative_report(scope)
        for chunk in smart_split(text, chars_per_string=4096):
            bot.send_message(CHAT_ID, chunk, parse_mode="Markdown")
        logger.info(f"{scope}-narrative-report sent.")
    except Exception as e:
        logger.error(f"{scope}-narrative-report failed: {e}")
        try:
            bot.send_message(CHAT_ID, f"⚠️ {scope.upper()} briefing failed to generate: {e}")
        except Exception:
            pass


def _get_personal_trades_for_day() -> list:
    """Fetch today's personal trades with linked signal details.

    signal_id can point at either options_signals (signal_source='system')
    or personal_signals (signal_source='personal') — the join is source-aware
    so a personal-sourced trade never accidentally pulls an unrelated system
    signal row that happens to share the same numeric id.
    """
    et = ZoneInfo("America/New_York")
    now = datetime.now(et)
    today_start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    start_utc = today_start.astimezone(timezone.utc).isoformat()

    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                # pt.bucket is appended LAST so existing positional row indices
                # (used throughout the EOD report) stay valid; bucket is row[17].
                c.execute("""
                    SELECT pt.id, pt.signal_id, pt.ticker, pt.strategy, pt.action,
                           pt.quantity, pt.price, pt.pnl, pt.deviation_notes, pt.executed_at,
                           os.direction, os.strike, os.dte, os.quality_score, os.scan_at,
                           pt.signal_source, ps.direction, pt.bucket
                    FROM personal_trades pt
                    LEFT JOIN options_signals os ON pt.signal_id = os.id AND pt.signal_source = 'system'
                    LEFT JOIN personal_signals ps ON pt.signal_id = ps.id AND pt.signal_source = 'personal'
                    WHERE pt.executed_at >= ?
                    ORDER BY pt.executed_at ASC
                """, (start_utc,))
                return c.fetchall()
    except Exception as e:
        logger.warning(f"Failed to fetch personal trades: {e}")
        return []


def _get_imported_fills_for_day() -> list:
    """Fetch today's auto-detected fills — mirrors _get_personal_trades_for_day()."""
    et = ZoneInfo("America/New_York")
    now = datetime.now(et)
    today_start_date = now.replace(hour=0, minute=0, second=0, microsecond=0).date().isoformat()

    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute("""
                    SELECT id, ticker, action, asset_class, quantity, price, trade_date,
                           matched_signal_id, direction, is_entry, trade_time, source
                    FROM imported_fills
                    WHERE trade_date >= ?
                    ORDER BY trade_date ASC, id ASC
                """, (today_start_date,))
                return c.fetchall()
    except Exception as e:
        logger.warning(f"Failed to fetch imported fills: {e}")
        return []


def _correlate_trade_with_signals(conn: sqlite3.Connection, ticker: str, trade_ts_utc: datetime, trade_direction: str, window_hours: float = None) -> dict:
    """Look for a system options signal on `ticker` within window_hours of
    trade_ts_utc and classify AGREE/DISAGREE/NO_SIGNAL. Deliberately a plain
    ticker+time+direction rule, not a weighted score — see
    SIGNAL_CORRELATION_WINDOW_HOURS' own comment for why. Returns the
    matched signal's own fields too (quality_score, reason, regime) so the
    report can show the user the underlying data instead of just a verdict
    label.

    window_hours defaults to SIGNAL_CORRELATION_WINDOW_HOURS. Callers with
    only a trade DATE (not a real timestamp — Plaid's investment
    transactions rarely carry one) should pass a wider window anchored at
    midday instead of pretending to hour-level precision they don't have.
    """
    window_seconds = int((window_hours if window_hours is not None else SIGNAL_CORRELATION_WINDOW_HOURS) * 3600)
    trade_ts_unix = int(trade_ts_utc.timestamp())
    row = conn.execute(
        "SELECT direction, quality_score, reason, regime, scan_at FROM options_signals "
        "WHERE ticker = ? AND scan_at BETWEEN ? AND ? "
        "ORDER BY ABS(scan_at - ?) ASC LIMIT 1",
        (ticker, trade_ts_unix - window_seconds, trade_ts_unix + window_seconds, trade_ts_unix),
    ).fetchone()

    if not row:
        return {"verdict": "NO_SIGNAL", "signal_direction": None, "quality_score": None, "reason": None}

    sys_direction, quality_score, reason, regime, scan_at = row
    mapped_direction = _SYSTEM_DIRECTION_TO_TRADE_DIRECTION.get(sys_direction, sys_direction)
    verdict = "AGREE" if mapped_direction == trade_direction else "DISAGREE"
    return {
        "verdict": verdict,
        "signal_direction": sys_direction,
        "quality_score": quality_score,
        "reason": reason,
        "regime": regime,
    }


def generate_consolidated_eod_report(scope: str = "day") -> str:
    """
    Build a comprehensive end-of-day/week report with all available data:
    - Broker account snapshot (Alpaca/IBKR separated)
    - Broker trades (options vs equity separated)
    - Personal trades (user input via /manual-trade)
    - Options signal accuracy + personal accuracy comparison
    - System metrics (portfolio Greeks, risk manager status, signal events)
    - Trade quality metrics (MAE, calibration)
    - Market context at close

    Returns Markdown string.
    """
    et = ZoneInfo("America/New_York")
    now = datetime.now(et)
    title = "📊 *Consolidated End-of-Day Report*" if scope == "day" else "📊 *Consolidated End-of-Week Report*"
    period_desc = now.strftime("%A, %Y-%m-%d") if scope == "day" \
        else f"week ending {now.strftime('%A, %Y-%m-%d')}"
    start_utc = _period_start_utc(scope)

    # ── Account snapshot ───────────────────────────────────────────────────
    acc, positions = _fetch_account_and_positions()
    if acc is not None:
        equity     = float(acc.get('portfolio_value', 0) or 0)
        last_equity = float(acc.get('last_equity', 0) or 0)
        cash       = float(acc.get('cash', 0) or 0)
        buying_pw  = float(acc.get('buying_power', 0) or 0)
        day_change = equity - last_equity
        day_pct    = (day_change / last_equity * 100) if last_equity else 0.0
        acct_block = (
            f"*Alpaca Account (live)*\n"
            f"  Equity: ${equity:,.2f}\n"
            f"  Day change: ${day_change:+,.2f} ({day_pct:+.2f}%)\n"
            f"  Cash: ${cash:,.2f} | Buying power: ${buying_pw:,.2f}"
        )
        open_positions = positions or []
    else:
        acct_block = "_⚠️ Could not fetch live account from Alpaca._"
        open_positions = []

    # ── Broker trades (separated by asset class) ──────────────────────────
    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()
            c.execute(
                "SELECT ticker, action, asset_class, quantity, price, pnl, detail, timestamp "
                "FROM trade_history WHERE timestamp >= ? ORDER BY timestamp ASC",
                (start_utc,),
            )
            broker_trades = c.fetchall()

    eq_trades = [t for t in broker_trades if (t[2] or "equity") == "equity"]
    opt_trades = [t for t in broker_trades if (t[2] or "equity") == "option"]

    eq_realized = sum((t[5] or 0.0) for t in eq_trades if t[1] in ("SELL", "CLOSE"))
    opt_realized = sum((t[5] or 0.0) for t in opt_trades if t[1] in ("SELL", "CLOSE"))
    total_realized = eq_realized + opt_realized

    def _fmt_trade_list(trades, limit=10):
        if not trades:
            return "_None_"
        out = []
        for ticker, action, cls, qty, price, pnl, detail, ts in trades[:limit]:
            pnl_str = f" | ${pnl:+.2f}" if (action in ("SELL", "CLOSE") and pnl) else ""
            out.append(f"  {str(ts)[11:16]} {action:6s} {ticker:6s} x{qty:g} @ ${price:.2f}{pnl_str}")
        if len(trades) > limit:
            out.append(f"  _…and {len(trades) - limit} more_")
        return "\n".join(out)

    # ── Personal trades (separate section) ─────────────────────────────────
    personal_trades = _get_personal_trades_for_day()
    imported_fills = _get_imported_fills_for_day()
    personal_realized = 0.0
    personal_wins = 0
    personal_losses = 0
    # Barbell split: core is compared apples-to-apples against the system win
    # rate below; satellite (moonshots) is reported on its own line because a
    # low win rate there is expected-by-design, not underperformance.
    core_wins = core_losses = satellite_wins = satellite_losses = 0
    core_realized = satellite_realized = 0.0

    if personal_trades:
        for row in personal_trades:
            pnl = row[7]  # pnl is in index 7
            action = row[4]  # action in index 4
            bucket = (row[17] or barbell.CORE)  # bucket appended last (see query)
            if action in ("CLOSE", "SELL") and pnl:
                personal_realized += pnl
                if pnl > 0:
                    personal_wins += 1
                elif pnl < 0:
                    personal_losses += 1
                if bucket == barbell.SATELLITE:
                    satellite_realized += pnl
                    if pnl > 0:
                        satellite_wins += 1
                    elif pnl < 0:
                        satellite_losses += 1
                else:
                    core_realized += pnl
                    if pnl > 0:
                        core_wins += 1
                    elif pnl < 0:
                        core_losses += 1

    personal_block = ""
    if personal_trades:
        # Only surface the satellite line once a moonshot trade actually closed,
        # so a pure-core day reads clean.
        if satellite_wins or satellite_losses:
            personal_summary = (
                f"Core   W:{core_wins} L:{core_losses} | ${core_realized:+,.2f}\n"
                f"🎯 Sat  W:{satellite_wins} L:{satellite_losses} | ${satellite_realized:+,.2f}"
            )
        else:
            personal_summary = f"W:{personal_wins} L:{personal_losses} | P&L: ${personal_realized:+,.2f}"
        personal_list = []
        for row in personal_trades:
            # row: (id, signal_id, ticker, strategy, action, quantity, price, pnl, deviation_notes, executed_at,
            #       signal_direction, signal_strike, signal_dte, signal_quality, signal_scan_at)
            pt_id, sig_id, ticker, strategy, action, qty, price, pnl, dev_notes, exec_at = row[:10]
            sig_src = row[1]  # signal_source (from earlier in the row, but let me recount)

            # Let me be more explicit
            exec_time = exec_at.split('T')[1][:5] if exec_at else "??:??"
            pnl_str = f" | ${pnl:+.2f}" if pnl is not None and action in ("CLOSE", "SELL") else ""
            deviation_str = f" ⚠️ {dev_notes}" if dev_notes else ""
            signal_ref = f"s#{sig_id}" if sig_id else "manual"
            # qty/price are nullable (audit §1 C2): `/trade s:47` logs an
            # execution against a signal without typing them.
            qty_str = f"x{qty:g}" if qty is not None else "x?"
            price_str = f"@ ${price:.2f}" if price is not None else "@ $?"
            sat_str = " 🎯" if (row[17] or barbell.CORE) == barbell.SATELLITE else ""

            personal_list.append(
                f"  {exec_time} {action:6s} {ticker:6s} {qty_str} {price_str}{pnl_str} [{signal_ref}]{sat_str}{deviation_str}"
            )

        personal_block = f"\n*Personal Trades ({len(personal_trades)})*\n{personal_summary}\n```\n" + \
                         "\n".join(personal_list) + "\n```"

    # ── Signal correlation (both personal_trades entries and Plaid-detected
    # fills, kept as two clearly-labeled groups rather than fuzzy-deduped
    # against each other) ───────────────────────────────────────────────────
    correlation_block = ""
    correlation_lines = []
    traded_tickers = set()
    tally = {"AGREE": 0, "DISAGREE": 0, "NO_SIGNAL": 0}

    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                for row in personal_trades:
                    (pt_id, sig_id, ticker, strategy, action, qty, price, pnl, dev_notes,
                     exec_at, sys_dir, strike, dte, quality, scan_at, sig_src, personal_dir,
                     _bucket) = row
                    if action not in ("OPEN", "BUY") or not exec_at:
                        continue  # exits aren't a fresh directional bet — correlating them isn't meaningful here
                    traded_tickers.add(ticker)
                    trade_direction = personal_dir if sig_src == "personal" and personal_dir else _SYSTEM_DIRECTION_TO_TRADE_DIRECTION.get(sys_dir)
                    if not trade_direction:
                        continue  # no direction info available (e.g. a /trade with no s:/p: reference) — nothing to correlate
                    trade_ts_utc = datetime.fromisoformat(exec_at).astimezone(timezone.utc)
                    match = _correlate_trade_with_signals(conn, ticker, trade_ts_utc, trade_direction)
                    tally[match["verdict"]] += 1
                    detail = f" q:{match['quality_score']:.1f}" if match["quality_score"] is not None else ""
                    reason_str = f"  reason: {match['reason']}" if match["reason"] else ""
                    correlation_lines.append(
                        f"  {ticker:6s} {trade_direction:5s} vs {match['signal_direction'] or 'no signal'}{detail} — {match['verdict']}{reason_str}"
                    )

                for fill_id, ticker, action, asset_class, qty, price, trade_date, matched_sig_id, direction, is_entry, trade_time, source in imported_fills:
                    if not is_entry or not direction or not trade_date:
                        continue
                    traded_tickers.add(ticker)
                    if trade_time:
                        # Robinhood order/execution timestamps give real
                        # precision — use the normal hour window like personal_trades.
                        trade_ts_utc = datetime.fromisoformat(trade_time).astimezone(timezone.utc)
                        match = _correlate_trade_with_signals(conn, ticker, trade_ts_utc, direction)
                        granularity_note = ""
                    else:
                        # Plaid transactions are date-only, not timestamped — anchor
                        # at midday ET and widen the window to the whole calendar
                        # day rather than pretending hour-level precision we don't have.
                        anchor = datetime.fromisoformat(f"{trade_date}T12:00:00").replace(tzinfo=ZoneInfo("America/New_York")).astimezone(timezone.utc)
                        match = _correlate_trade_with_signals(conn, ticker, anchor, direction, window_hours=12.0)
                        granularity_note = "  _(day-level match)_"
                    tally[match["verdict"]] += 1
                    detail = f" q:{match['quality_score']:.1f}" if match["quality_score"] is not None else ""
                    reason_str = f"  reason: {match['reason']}" if match["reason"] else ""
                    correlation_lines.append(
                        f"  {ticker:6s} {direction:5s} vs {match['signal_direction'] or 'no signal'}{detail} — {match['verdict']}{reason_str}{granularity_note}"
                    )
    except Exception as e:
        logger.warning(f"Signal correlation failed: {e}")

    if correlation_lines:
        tally_str = f"Agree: {tally['AGREE']} | Disagree: {tally['DISAGREE']} | No signal: {tally['NO_SIGNAL']}"
        correlation_block = f"\n*Signal Correlation*\n{tally_str}\n```\n" + "\n".join(correlation_lines) + "\n```"

    # ── Signal outcomes (C2) — is the system's directional call actually
    # right, checked at fixed T+N horizons and at real trade hold durations.
    # All-time cumulative per checkpoint, not just today's newly-resolved
    # rows — a single day's sample is usually too small to be worth reading
    # on its own. Purely informational, same "not overfit" boundary as
    # Signal Correlation above: nothing here changes sizing/gating.
    outcomes_block = ""
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                outcome_rows = conn.execute(
                    "SELECT checkpoint_label, "
                    "SUM(CASE WHEN direction_correct = 1 THEN 1 ELSE 0 END), "
                    "COUNT(*) "
                    "FROM signal_outcomes WHERE direction_correct IS NOT NULL "
                    "GROUP BY checkpoint_label"
                ).fetchall()
        if outcome_rows:
            order = {f"T+{d}": i for i, d in enumerate(SIGNAL_OUTCOME_CHECKPOINT_DAYS)}
            order["hold_duration"] = len(order)
            outcome_rows.sort(key=lambda r: order.get(r[0], 99))
            lines = []
            for label, correct, total in outcome_rows:
                pct = (correct / total * 100) if total else 0
                display_label = "Hold-duration" if label == "hold_duration" else label
                lines.append(f"  {display_label:14s} {pct:.0f}% correct (n={total})")
            outcomes_block = "\n*Signal Outcomes (System, all-time)*\n```\n" + "\n".join(lines) + "\n```"
    except Exception as e:
        logger.warning(f"Signal outcomes summary failed: {e}")

    # Engine-wide prediction-quality — one compact line per source with
    # sufficient data, most recent computed rollup (run_eod_report() already
    # refreshed prediction_quality_rollup before this report is built). Full
    # breakdown is available on /quality; this is a glance-level summary.
    quality_block = ""
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                rollup_rows = conn.execute(
                    "SELECT source_type, window_days, hit_rate, n FROM prediction_quality_rollup "
                    "WHERE id IN (SELECT MAX(id) FROM prediction_quality_rollup GROUP BY source_type, window_days) "
                    "ORDER BY source_type, window_days"
                ).fetchall()
        if rollup_rows:
            lines = []
            for source_type, window_days, hit_rate, n in rollup_rows:
                label = "7d" if window_days == 7 else f"{window_days}d"
                pct = (hit_rate * 100) if hit_rate is not None else 0.0
                lines.append(f"  {source_type} ({label}): {pct:.0f}% hit (n={n})")
            quality_block = ("\n*Prediction Quality (see /quality for full breakdown)*\n```\n"
                              + "\n".join(lines) + "\n```")
    except Exception as e:
        logger.warning(f"Prediction quality summary failed: {e}")

    # ── News digest (C4) — one best-effort headline per ticker you actually
    # traded today, via america-data-engine's per-ticker endpoint. Purely
    # context for you to read alongside the correlation/outcome verdicts
    # above — not scored, not fed into anything. Capped at 5 tickers so a
    # heavy trading day doesn't blow up the report with API calls/length.
    news_block = ""
    try:
        news_lines = []
        for ticker in sorted(traded_tickers)[:5]:
            payload, ok = query_data_engine(f"/news/us/{ticker}", "http://america-data-engine:8001")
            if not ok or not payload.get("news"):
                continue
            top = payload["news"][0]
            headline = (top.get("headline") or "").strip()
            if headline:
                news_lines.append(f"  {ticker:6s} — {headline[:100]}")
        if news_lines:
            news_block = "\n*News Digest (tickers you traded today)*\n```\n" + "\n".join(news_lines) + "\n```"
    except Exception as e:
        logger.warning(f"News digest failed: {e}")

    # ── Options signal accuracy ────────────────────────────────────────────
    options_accuracy_block = ""
    try:
        response = requests.get(
            "http://localhost:8080/daily-options-accuracy",
            timeout=30
        )
        if response.status_code == 200:
            accuracy_data = response.json()
            summary = accuracy_data.get('summary', {})
            total = summary.get('total_trades', 0)

            if total > 0:
                system_wins = summary.get('wins', 0)
                system_losses = summary.get('losses', 0)
                win_rate = summary.get('win_rate_pct', 0)
                profit_factor = summary.get('profit_factor', 0)
                cumulative_pnl = summary.get('cumulative_pnl_usd', 0)

                # Only compare against personal win rate when there is ACTUAL
                # closed personal data. Previously this printed "vs personal: 0%
                # (Δ −55%)" any day a trade was merely opened — personal trades
                # could never close (audit §1 C1), so wins+losses was always 0
                # and the report told the user daily that their manual trading
                # was infinitely worse than the system. Now: report only real
                # closed-trade win rates; otherwise say so plainly.
                # Barbell: compare CORE only against the system win rate —
                # blending in the satellite (moonshot) sleeve, whose low WR is
                # by design, would understate the disciplined side's real edge.
                system_vs_personal = ""
                core_closed = core_wins + core_losses
                if core_closed > 0:
                    core_wr = core_wins / core_closed * 100
                    wr_diff = core_wr - win_rate
                    system_vs_personal = (
                        f"\n_vs personal core: {core_wr:.0f}% "
                        f"(n={core_closed}, Δ {wr_diff:+.0f}%)_"
                    )
                elif personal_trades:
                    system_vs_personal = "\n_vs personal: no closed core trades yet (use /close)_"

                options_accuracy_block = (
                    f"\n*Options Signal Accuracy (System)*\n"
                    f"  Trades: {total} (W:{system_wins} L:{system_losses})\n"
                    f"  Win rate: {win_rate:.0f}%{system_vs_personal}\n"
                    f"  Profit factor: {profit_factor:.2f}x\n"
                    f"  Cumulative P&L: ${cumulative_pnl:+,.2f}"
                )
    except Exception as e:
        logger.warning(f"Failed to fetch options accuracy: {e}")

    # ── Consolidated report ────────────────────────────────────────────────
    report = (
        f"{title}\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"_{period_desc}_\n\n"
        f"*Account*\n{acct_block}\n\n"
        f"*Broker Trades Summary*\n"
        f"  Equity: {len(eq_trades)} trades | ${eq_realized:+,.2f} P&L\n"
        f"  Options: {len(opt_trades)} trades | ${opt_realized:+,.2f} P&L\n"
        f"  **Total realized: ${total_realized:+,.2f}**\n\n"
        f"*Open Positions*\n"
    )

    if open_positions:
        pos_lines = []
        for p in open_positions:
            if isinstance(p, dict):
                upl = float(p.get('unrealized_pl', 0) or 0)
                uplpc = float(p.get('unrealized_plpc', 0) or 0) * 100
                pos_lines.append(
                    f"  {p.get('symbol','?')} x{p.get('qty','?')} | "
                    f"${upl:+,.2f} ({uplpc:+.1f}%)"
                )
        if pos_lines:
            report += "\n".join(pos_lines)
        else:
            report += "  _None_"
    else:
        report += "  _None_"

    report += f"\n\n*Equity Trades*\n```\n{_fmt_trade_list(eq_trades)}\n```"
    report += f"\n*Options Trades*\n```\n{_fmt_trade_list(opt_trades)}\n```"
    report += options_accuracy_block
    report += personal_block
    report += correlation_block
    report += outcomes_block
    report += quality_block
    report += news_block
    report += f"\n\n_Generated {now.strftime('%Y-%m-%d %H:%M ET')}_"

    return report


def generate_consolidated_eod_narrative(scope: str = "day") -> str:
    """
    Claude Haiku-powered narrative analysis of the full EOD report:
    - Day's performance vs expectations
    - Options signal accuracy vs personal trades
    - System health (Greeks, risk manager status, event audit trail)
    - Key learnings and next-day focus areas
    """
    et = ZoneInfo("America/New_York")
    now = datetime.now(et)
    title = "🌙 *EOD Narrative — " if scope == "day" else "🌙 *EOW Narrative — "
    period_desc = now.strftime("%A, %Y-%m-%d") if scope == "day" \
        else f"week ending {now.strftime('%A, %Y-%m-%d')}"

    # Gather all data for Claude context
    structured_report = generate_consolidated_eod_report(scope)

    # Fetch market close context
    vix = fetch_vix_level()
    spy_price, spy_200_sma, _ = fetch_spy_regime()

    # Get today's system events (signal_events audit trail)
    # For now, we'll note this is available but may not be in the heartbeat API
    events_summary = "Signal events: [queried from execution engine's signal_events table]"

    prompt = (
        f"Period: {period_desc}\n\n"
        f"Structured EOD Report:\n{structured_report}\n\n"
        f"Market Close Context:\n"
        f"  VIX: {vix:.1f}\n"
        f"  SPY: ${spy_price:.2f} (200-SMA: ${spy_200_sma:.2f})\n\n"
        f"Write a brief EOD narrative in THREE sections:\n\n"
        f"EXECUTION & SIGNAL ACCURACY — Analyze today's trades: "
        f"what the system's options signals predicted vs what actually happened, "
        f"and how personal trades compared. Reference specific win/loss outcomes.\n\n"
        f"SYSTEM HEALTH — Comment on position sizing, risk exposure, and any alerts "
        f"(Greeks breaches, alpha decay, circuit breaker triggers). If quiet, note that.\n\n"
        f"TOMORROW'S FOCUS — 2-3 key things to watch or tune based on today's results. "
        f"Be actionable: calibration drift? Sizing too aggressive? Good signal quality?\n"
    )

    response = claude.messages.create(
        model="claude-sonnet-5",
        max_tokens=1800,
        system="You are a quantitative trading analyst writing a crisp end-of-day review "
               "for a trader who runs automated systems + manual trades. Be honest, specific, "
               "reference actual numbers from the data, and focus on what changed or what to fix.",
        messages=[{"role": "user", "content": prompt}],
    )
    narrative = _narrative_text(response)

    return (
        f"{title}{period_desc}*\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
        f"{narrative}\n\n"
        f"_Generated {now.strftime('%Y-%m-%d %H:%M ET')}_"
    )


def _send_consolidated_report(scope: str, use_narrative: bool = False):
    """Send consolidated EOD/EOW report via Telegram, chunked for message limits."""
    try:
        if use_narrative:
            text = generate_consolidated_eod_narrative(scope)
        else:
            text = generate_consolidated_eod_report(scope)

        for chunk in smart_split(text, chars_per_string=4096):
            bot.send_message(CHAT_ID, chunk, parse_mode="Markdown")
        logger.info(f"Consolidated {scope}-report sent.")
    except Exception as e:
        logger.error(f"Consolidated {scope}-report failed: {e}")
        try:
            bot.send_message(CHAT_ID, f"⚠️ Consolidated report failed: {e}")
        except Exception:
            pass


def run_sports_daily_recap():
    """
    Daily sports predictions recap. Scheduled ~9:00 AM ET, before most games
    of the day start. Sent to the SPORTS bot only (a summary, not a
    high-conviction alert — those already go to both bots from
    sports_predictions/main.py directly).
    """
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT COUNT(*) FROM sports_predictions "
                    "WHERE created_at >= datetime('now', '-1 day')"
                )
                predictions_24h = c.fetchone()[0]

                c.execute(
                    "SELECT COUNT(*) FROM sports_manual_bets "
                    "WHERE placed_at >= datetime('now', '-1 day')"
                )
                bets_placed_24h = c.fetchone()[0]

                c.execute(
                    "SELECT result, COUNT(*), SUM(pnl) FROM sports_manual_bets "
                    "WHERE placed_at >= datetime('now', '-1 day') AND result != 'pending' "
                    "GROUP BY result"
                )
                resolved_rows = c.fetchall()

                c.execute("SELECT COUNT(*) FROM sports_manual_bets WHERE result = 'pending'")
                pending_bets = c.fetchone()[0]

                bankroll = _sports_bankroll_summary(conn) if SPORTS_STARTING_BANKROLL > 0 else None

        wins = next((r[1] for r in resolved_rows if r[0] == "win"), 0)
        losses = next((r[1] for r in resolved_rows if r[0] == "loss"), 0)
        total_pnl = sum(r[2] for r in resolved_rows if r[2] is not None)

        bankroll_line = ""
        if bankroll is not None:
            bankroll_line = (
                f"\n💵 *Bankroll:* ${bankroll['current_bankroll']:.2f} "
                f"(started ${SPORTS_STARTING_BANKROLL:.2f}, total P&L ${bankroll['total_pnl']:+.2f}, "
                f"lifetime {bankroll['wins']}-{bankroll['losses']})"
            )

        recap_msg = (
            f"🏆 *Sports Predictions — Daily Recap*\n"
            f"{'─' * 24}\n"
            f"📊 *Predictions made (24h):* {predictions_24h}\n"
            f"💰 *Bets placed (24h):* {bets_placed_24h}\n"
            f"✅ *Resolved (24h):* {wins}W-{losses}L (pnl: {total_pnl:+.2f})\n"
            f"⏳ *Still pending:* {pending_bets}"
            f"{bankroll_line}"
        )
        _send_sports_telegram(recap_msg)
        logger.info(f"[SPORTS] Daily recap sent — {predictions_24h} predictions, "
                    f"{bets_placed_24h} bets placed, {wins}W-{losses}L resolved.")
    except Exception as e:
        logger.error(f"[SPORTS] Daily recap failed: {e}")


def run_eod_report():
    """End-of-day summary. Scheduled ~16:05 ET daily; narrative briefing on non-trading days."""
    now = datetime.now(ZoneInfo("America/New_York"))

    # C2 — resolve signal outcomes BEFORE building the report, so today's
    # freshly-resolved checkpoints are reflected in it, not next run's.
    # Isolated in its own try/except (same convention as the alpha-decay
    # check below) so a resolver bug (e.g. Alpaca creds missing) never
    # blocks the report that would otherwise succeed.
    try:
        import signal_outcome_resolver
        signal_outcome_resolver.resolve_signal_outcomes()
    except Exception as e:
        logger.warning(f"Signal outcome resolution failed: {e}")

    # Engine-wide prediction-quality: resolve pending predictions_log rows
    # (WS1/Skeptic/personal signals) into prediction_outcomes, then roll
    # everything up (including options_signal via signal_outcomes above)
    # into per-source weekly/monthly quality scores. Same isolation
    # convention as every other post-close check — surface-only, never
    # blocks the report.
    try:
        import prediction_outcome_resolver
        prediction_outcome_resolver.resolve_prediction_outcomes()
        import prediction_quality_scorer
        prediction_quality_scorer.run_rollup()
    except Exception as e:
        logger.warning(f"Prediction quality rollup failed: {e}")

    if is_trading_day_at(now):
        _send_consolidated_report("day", use_narrative=False)
    else:
        _send_narrative_report("day")

    # Phase 3: alpha-decay check — same once-daily post-close cadence as the
    # EOD report. Isolated in its own try/except so a decay-monitor bug (e.g.
    # too little daily_ledger history yet) can never take down the report
    # that already succeeded above.
    try:
        import alpha_decay_monitor
        alpha_decay_monitor.run_daily_check()
    except Exception as e:
        logger.warning(f"Alpha decay check failed: {e}")


def run_fundamentals_risk_check():
    """
    Fundamental-risk (Beneish/FCF) check. Used to run inside run_eod_report's
    16:05 ET bundle, but the producer (america_data_engine's broad-universe
    scan) now runs at FUNDAMENTALS_SCAN_HOUR_ET (default 20:00 ET, see
    CLAUDE.md) so it finishes after market close and catches the day's news —
    16:05 ET would have read yesterday's cache every single day (RULE-D2:
    verify the consumer, not just the producer). Scheduled separately at
    21:30 ET, a comfortable buffer past the ~40-minute scan.
    """
    # No local try/except: this is registered via supervised_job("fundamentals_risk_check"),
    # which records the outcome for the dead-man's switch and swallows so the
    # scheduler thread survives. Swallowing here too would hide the failure from
    # that alarm (RULE-D3).
    import fundamentals_risk_monitor
    fundamentals_risk_monitor.run_daily_check()


def run_fundamentals_bullish_check():
    """
    Bullish-quality (Piotroski F-Score) check — mirrors run_fundamentals_risk_check
    exactly, reading the same producer scan's bullish half. Scheduled
    separately at 21:31 ET (1 minute after the bearish check) so the two are
    independent fail-domains rather than one calling the other.
    """
    # Registered via supervised_job("fundamentals_bullish_check") — see the
    # bearish sibling above for why there's no local swallow.
    import fundamentals_bullish_monitor
    fundamentals_bullish_monitor.run_daily_check()


def run_celh_signal_check():
    """
    CELH search-signal checkpoint (90d/180d, monitoring-only). See
    celh_signal_tracker.py's docstring for the full rationale — this checks
    a DB-row dedup and is a no-op on every day except the two target dates.
    """
    # Registered via supervised_job("celh_signal_check") — no local swallow so a
    # real failure reaches the dead-man's switch.
    import celh_signal_tracker
    celh_signal_tracker.run_daily_check()


def run_earnings_revision_check():
    """
    Earnings-revision-momentum (consensus EPS drift) check. Unlike the
    fundamentals bearish/bullish checks, this doesn't read a producer's
    cache — it fetches Finnhub directly and self-accumulates history, so it
    has no "wait for the scan to finish" timing dependency. Scheduled at a
    time that doesn't collide with the 20:00/21:00/21:30/21:31 ET cluster.
    """
    # Registered via supervised_job("earnings_revision_check") — no local swallow
    # so a real failure reaches the dead-man's switch. (This module fails open
    # cleanly by returning a dict rather than raising, so a data-source outage
    # still records a success; a genuine crash is what this catches.)
    import earnings_revision_monitor
    earnings_revision_monitor.run_daily_check()


def run_eow_report():
    """End-of-week summary. Scheduled ~16:10 ET Friday; narrative briefing if Friday is a holiday."""
    now = datetime.now(ZoneInfo("America/New_York"))
    if is_trading_day_at(now):
        _send_consolidated_report("week", use_narrative=False)
    else:
        _send_narrative_report("week")


def run_session_summary_report():
    """
    Evening session-summary report. Scheduled ~21:00 ET on trading days —
    after the day's news cycle settles and after america_data_engine's
    20:00 ET fundamentals-universe scan, before the 21:30 ET fundamentals-
    risk consumer (run_fundamentals_risk_check). Deliberately NOT the same
    report as the 16:05 ET EOD account bundle (run_eod_report /
    _send_consolidated_report, which covers P&L/win-rate): this one checks
    whether this morning's Scout call (run_scout_protocol) held up against
    what the market actually did today, and recaps the day's signal/order
    activity that the EOD bundle doesn't itemize.
    """
    try:
        now = datetime.now(ZoneInfo("America/New_York"))
        if not is_trading_day_at(now):
            logger.info("[SESSION SUMMARY] Non-trading day — nothing to recap, skipping.")
            return

        session_start = now.replace(hour=9, minute=0, second=0, microsecond=0)
        session_start_epoch = int(session_start.astimezone(timezone.utc).timestamp())
        session_start_sql   = session_start.astimezone(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT claude_analysis FROM daily_audits "
                    "WHERE date(timestamp) = date('now') ORDER BY timestamp DESC LIMIT 1"
                )
                row = c.fetchone()
                morning_report = row[0] if row else None

                try:
                    c.execute(
                        "SELECT outcome, COUNT(*) FROM options_signals WHERE scan_at >= ? GROUP BY outcome",
                        (session_start_epoch,)
                    )
                    options_outcomes = c.fetchall()
                except sqlite3.OperationalError:
                    options_outcomes = []  # execution engine hasn't created the table yet

                try:
                    c.execute(
                        "SELECT ticker, strategy, status, qty FROM order_ledger WHERE sent_at >= ? ORDER BY sent_at",
                        (session_start_epoch,)
                    )
                    orders_today = c.fetchall()
                except sqlite3.OperationalError:
                    orders_today = []

                c.execute(
                    "SELECT ticker, strategy, direction FROM personal_signals WHERE created_at >= ?",
                    (session_start_sql,)
                )
                personal_signals_today = c.fetchall()

                c.execute(
                    "SELECT ticker, action, price FROM personal_trades WHERE timestamp >= ?",
                    (session_start_sql,)
                )
                personal_trades_today = c.fetchall()

        morning_summary = morning_report or "No morning Scout report found for today (may have been skipped on incomplete data — see gap policy in run_scout_protocol)."
        options_summary = "; ".join(f"{outcome}: {count}" for outcome, count in options_outcomes) or "No options signals scanned today"
        orders_summary = "\n".join(f"- {t}: {strat} status={s} qty={q}" for t, strat, s, q in orders_today) or "No orders sent today"
        personal_signals_summary = "\n".join(f"- {t} {strat} {d}" for t, strat, d in personal_signals_today) or "None logged"
        personal_trades_summary = "\n".join(f"- {t} {a} @ {p}" for t, a, p in personal_trades_today) or "None logged"

        # Fresh evening context to check this morning's call against.
        vix = fetch_vix_level()
        spy_price, spy_200_sma, _ = fetch_spy_regime()
        news_context, news_ok = get_us_news_context()

        prompt = (
            f"This morning's Scout briefing:\n{morning_summary}\n\n"
            f"Evening market snapshot — VIX: {vix:.1f}, SPY: ${spy_price:.2f} vs 200-SMA ${spy_200_sma:.2f}\n\n"
            f"Fresh evening headlines:\n{news_context if news_ok else 'unavailable'}\n\n"
            f"Options signal outcomes today: {options_summary}\n\n"
            f"Orders sent today:\n{orders_summary}\n\n"
            f"Personal signals logged today:\n{personal_signals_summary}\n\n"
            f"Personal trades logged today:\n{personal_trades_summary}\n\n"
            "Write a SESSION SUMMARY in Telegram Markdown (bold with *single asterisks*, • bullets, no # headers), "
            "in two sections:\n\n"
            "*MORNING CALL CHECK*\n"
            "For each setup/regime call from this morning's briefing, state whether today's actual price action "
            "or headlines CONFIRMED, CONTRADICTED, or left it STILL PENDING — be specific and cite a number. "
            "If there was no morning report, say so plainly and skip to the recap.\n\n"
            "*SESSION RECAP*\n"
            "3-5 bullets on what actually happened this session (signal/order activity, personal trades), "
            "closing with one sentence on tomorrow's carry-over risk."
        )

        response = claude.messages.create(
            model="claude-sonnet-5",
            max_tokens=2000,
            system="You are a quantitative trading analyst writing a session summary. Only reference "
                   "tickers, catalysts, and figures present in the data given above — do not invent "
                   "or state market-cap/company-size classifications, financial facts, or catalysts "
                   "not grounded in that data.",
            messages=[{"role": "user", "content": prompt}]
        )
        analysis_text = _narrative_text(response) or "No summary content was produced."

        timestamp = now.strftime('%Y-%m-%d %H:%M ET')
        _send_telegram_section(f"*NOX SESSION SUMMARY*\n────────────────────────\n{timestamp}")
        for section in smart_split(analysis_text, chars_per_string=3800):
            _send_telegram_section(section)

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                conn.execute(
                    "CREATE TABLE IF NOT EXISTS session_summaries ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                    "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
                    "claude_analysis TEXT)"
                )
                conn.execute("INSERT INTO session_summaries (claude_analysis) VALUES (?)", (analysis_text,))
                conn.commit()
    except Exception as e:
        logger.error(f"[SESSION SUMMARY] Failed: {e}")
        print(f"[ERROR] [HEARTBEAT] Session summary report failed: {e}", flush=True)


def run_weekly_outlook_report():
    """
    Sunday weekly-outlook report. Previews the week ahead — earnings,
    insider/alt-macro flags on file, and current regime — so Friday
    evening's weekly performance recap (run_weekly_performance_report) has
    a concrete expectation to check itself against. Markets are closed on
    Sunday, so none of the trading-day gap-refusal policy in
    run_scout_protocol applies here; this degrades gracefully instead.
    """
    try:
        now = datetime.now(ZoneInfo("America/New_York"))
        flow_context, flow_unavailable = get_earnings_insider_macro_context(days_ahead=7)
        vix, vix_ok = _fetch_vix_level_checked()
        spy_price, spy_200_sma, _, regime_ok = _fetch_spy_regime_checked()
        news_context, news_ok = get_us_news_context()

        prompt = (
            f"Current regime — VIX: {vix:.1f}{'' if vix_ok else ' (fallback, fetch failed)'}, "
            f"SPY: ${spy_price:.2f} vs 200-SMA ${spy_200_sma:.2f}"
            f"{'' if regime_ok else ' (fallback, fetch failed)'}\n\n"
            f"Latest US headlines:\n{news_context if news_ok else 'unavailable'}\n\n"
            f"Week-ahead earnings/insider/alt-macro flow:\n{flow_context}\n\n"
            "Write a WEEKLY OUTLOOK in Telegram Markdown (bold *single asterisks*, • bullets, no # headers), "
            "in two sections:\n\n"
            "*WEEK AHEAD*\n"
            "3-5 bullets naming the specific catalysts this week (earnings dates, insider clusters, "
            "macro/chokepoint flags, current regime) and what each implies for positioning.\n\n"
            "*WHAT WOULD CONFIRM VS CONTRADICT*\n"
            "2-3 falsifiable expectations for the week (e.g. 'if VIX stays under X and AAPL earnings beat, "
            "expect Y') so Friday's weekly recap can be checked against something concrete rather than vibes."
        )
        response = claude.messages.create(
            model="claude-sonnet-5",
            max_tokens=1800,
            system="You are a quantitative market analyst writing a weekly outlook. Only reference "
                   "tickers, catalysts, and figures present in the data given above — do not invent "
                   "or state market-cap/company-size classifications, financial facts, or catalysts "
                   "not grounded in that data.",
            messages=[{"role": "user", "content": prompt}],
        )
        analysis_text = _narrative_text(response) or "No outlook content was produced."

        timestamp = now.strftime('%Y-%m-%d %H:%M ET')
        header = f"*NOX WEEKLY OUTLOOK*\n────────────────────────\n{timestamp}"
        if flow_unavailable:
            header += f"\n\n_Some flow data unavailable: {', '.join(flow_unavailable)}_"
        _send_telegram_section(header)
        for section in smart_split(analysis_text, chars_per_string=3800):
            _send_telegram_section(section)
    except Exception as e:
        logger.error(f"[WEEKLY OUTLOOK] Failed: {e}")
        print(f"[ERROR] [HEARTBEAT] Weekly outlook report failed: {e}", flush=True)


@bot.message_handler(commands=['eod'])
def cmd_eod(message):
    """/eod — consolidated end-of-day account summary (structured data)."""
    _send_consolidated_report("day", use_narrative=False)


@bot.message_handler(commands=['eod-narrative'])
def cmd_eod_narrative(message):
    """/eod-narrative — end-of-day narrative analysis (Claude-powered)."""
    _send_consolidated_report("day", use_narrative=True)


@bot.message_handler(commands=['quality'])
def cmd_quality(message):
    """/quality — full per-source weekly/monthly prediction-quality breakdown
    (hit-rate, confidence calibration, move magnitude). Surface-only: never
    affects sizing/gating. Recomputes on demand rather than waiting for the
    next EOD cadence, so this always reflects the latest resolved data."""
    try:
        import prediction_quality_scorer
        summary = prediction_quality_scorer.run_rollup()
        text = prediction_quality_scorer.format_quality_report(summary)
    except Exception as e:
        text = f"❌ Prediction quality rollup failed: {e}"
    bot.reply_to(message, text, parse_mode="Markdown")


_DATE_OVERRIDE_RE = re.compile(r'^date:(\d{4}-\d{2}-\d{2})(?:[T ](\d{2}:\d{2}))?$', re.IGNORECASE)


def _extract_date_override(text):
    """
    Strip an optional `date:YYYY-MM-DD[THH:MM]` token out of a raw /signal,
    /trade, or /close command string, for logging a trade after the fact
    instead of at the moment it happened (Robinhood fills are rarely typed
    in real time). Returns (cleaned_text, override_datetime_in_ET_or_None);
    time defaults to noon ET when only a date is given. Only the first
    matching token is honored so a thesis/notes field can't accidentally
    smuggle a second one in.
    """
    tokens = text.split()
    cleaned = []
    override = None
    for tok in tokens:
        m = _DATE_OVERRIDE_RE.match(tok)
        if m and override is None:
            date_str, time_str = m.groups()
            override = datetime.strptime(
                f"{date_str} {time_str or '12:00'}", "%Y-%m-%d %H:%M"
            ).replace(tzinfo=ZoneInfo("America/New_York"))
        else:
            cleaned.append(tok)
    return " ".join(cleaned), override


@bot.message_handler(commands=['signal'])
def cmd_log_personal_signal(message):
    """
    /signal [date:YYYY-MM-DD?] [ticker] [asset_class?] [strategy] [direction] [entry] [target] [stop] [thesis]

    Log a personal trade idea (outside the system) for tracking. Add an
    optional `date:YYYY-MM-DD` (or `date:YYYY-MM-DDTHH:MM`) token anywhere in
    the command to backdate it to when you actually made the trade, instead
    of stamping it with the moment you happened to type this.
    asset_class is optional — omit it and it defaults to EQUITY (old format
    still works unchanged). When given, it must be one of:
      EQUITY | OPTION | FUTURES | CRYPTO | FOREX
    direction accepts LONG/BUY or SHORT/SELL (stored canonically as LONG/SHORT).

    Examples:
      /signal NVDA BULL_CALL LONG 120 135 115 earnings_breakout
      /signal CL FUTURES SHORT_OIL SHORT 68 62 72 hormuz_supply_thesis
      /signal BTC CRYPTO MOMENTUM SHORT 60000 52000 65000 macro_risk_off
      /signal date:2026-07-10 NVDA BULL_CALL LONG 120 135 115 earnings_breakout

    FUTURES/CRYPTO/FOREX have no live-price feed wired up yet — /signal will
    log them without the entry-vs-live-price sanity check EQUITY/OPTION get.
    A backdated entry also skips the live-price sanity check (there's no way
    to know the quote at a past moment from a current snapshot) — logged
    without it either way, same as FUTURES/CRYPTO/FOREX.

    All fields required. Use underscores for multi-word thesis.
    """
    try:
        raw_text, date_override = _extract_date_override(message.text.strip())
        parts = raw_text.split()
        if len(parts) < 8:
            bot.reply_to(
                message,
                "❌ Format: /signal [date:YYYY-MM-DD?] [ticker] [asset_class?] [strategy] [direction] [entry] [target] [stop] [thesis]\n"
                "Example: /signal NVDA BULL_CALL LONG 120 135 115 earnings_breakout"
            )
            return

        ticker = parts[1].upper()

        # asset_class is an optional positional arg right after ticker — if the
        # next token isn't one of the recognized asset classes, assume it was
        # omitted (old format) and default to EQUITY so existing usage keeps
        # working unchanged.
        if parts[2].upper() in PERSONAL_SIGNAL_ASSET_CLASSES:
            asset_class = parts[2].upper()
            field_start = 3
        else:
            asset_class = "EQUITY"
            field_start = 2

        min_len = field_start + 5  # strategy, direction, entry, target, stop, +thesis
        if len(parts) < min_len:
            bot.reply_to(
                message,
                "❌ Format: /signal [ticker] [asset_class?] [strategy] [direction] [entry] [target] [stop] [thesis]"
            )
            return

        strategy = parts[field_start].upper()
        direction_raw = parts[field_start + 1].upper()
        direction = PERSONAL_SIGNAL_DIRECTION_ALIASES.get(direction_raw)
        if direction is None:
            bot.reply_to(message, "❌ Direction must be LONG/BUY or SHORT/SELL")
            return

        try:
            entry = float(parts[field_start + 2])
            target = float(parts[field_start + 3])
            stop = float(parts[field_start + 4])
        except ValueError:
            bot.reply_to(message, "❌ Entry, target, and stop must be numbers")
            return

        thesis_tokens = parts[field_start + 5:]
        if not thesis_tokens:
            bot.reply_to(message, "❌ Thesis is required")
            return
        thesis = " ".join(thesis_tokens).replace("_", " ")

        # entry_level is a typed target/technical level, not a live quote — fetch
        # the actual current price so /mysignals can show how far apart they are
        # instead of the user only discovering the gap when they try to fill it.
        # Only EQUITY/OPTION are servable today — Alpaca's snapshot feed is
        # equity-only, and an option's underlying is an equity ticker. Skipped
        # for a backdated entry too: a snapshot taken now says nothing about
        # the quote at the past moment being logged.
        market_price = None
        if date_override is None and asset_class in PERSONAL_SIGNAL_LIVE_PRICE_ASSET_CLASSES:
            try:
                snap = fetch_batch_snapshots([ticker])
                market_price = snap.get(ticker, {}).get("price") or None
            except Exception as e:
                logger.warning(f"/signal: couldn't fetch live price for {ticker}: {e}")

        created_at = (date_override or datetime.now(ZoneInfo("America/New_York"))).isoformat()

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "INSERT INTO personal_signals (ticker, strategy, direction, entry_level, target, stop_loss, thesis, market_price_at_log, asset_class, created_at) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (ticker, strategy, direction, entry, target, stop, thesis, market_price, asset_class, created_at),
                )
                sig_id = c.lastrowid

                # Engine-wide prediction-quality logging (additive only — see
                # CLAUDE.md's "Engine-Wide Prediction Quality Scoring"). Table
                # is also declared by execution/OrderLedger.hpp; both sides
                # use CREATE TABLE IF NOT EXISTS, same multi-handle-to-one-
                # file pattern as every other shared table in this project.
                conn.execute(
                    "CREATE TABLE IF NOT EXISTS predictions_log ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT, source_type TEXT NOT NULL, "
                    "source_ref_id INTEGER, ticker TEXT NOT NULL, direction TEXT NOT NULL, "
                    "confidence REAL, logged_at INTEGER NOT NULL, detail TEXT)"
                )
                # asset_class lets the resolver refuse to price a FUTURES/CRYPTO/
                # FOREX prediction against Alpaca *equity* bars (audit §1 C3: the
                # doc example `/signal CL FUTURES SHORT_OIL SHORT` was scored
                # against Colgate-Palmolive). Additive; NULL means EQUITY.
                try:
                    conn.execute("ALTER TABLE predictions_log ADD COLUMN asset_class TEXT")
                except Exception:
                    pass  # column already exists
                conn.execute(
                    "INSERT INTO predictions_log (source_type, source_ref_id, ticker, direction, confidence, logged_at, detail, asset_class) "
                    "VALUES ('personal_signal', ?, ?, ?, NULL, ?, ?, ?)",
                    (sig_id, ticker, "BULLISH" if direction == "LONG" else "BEARISH",
                     int((date_override or datetime.now(ZoneInfo("America/New_York"))).astimezone(timezone.utc).timestamp()),
                     strategy, asset_class),
                )
                conn.commit()

        reply = (
            f"✅ Signal #{sig_id} logged\n"
            f"  {ticker} [{asset_class}] {strategy} {direction}\n"
            f"  Entry: ${entry:.2f} | Target: ${target:.2f} | Stop: ${stop:.2f}\n"
            f"  Thesis: {thesis}"
        )
        if date_override is not None:
            reply += f"\n  🕓 Backdated to {date_override.strftime('%Y-%m-%d %H:%M ET')}"
        elif asset_class not in PERSONAL_SIGNAL_LIVE_PRICE_ASSET_CLASSES:
            reply += f"\n  ℹ️ No live-price feed for {asset_class} yet — logged without a fillability check."
        elif market_price:
            dist_pct = (entry - market_price) / market_price * 100
            reply += f"\n  Current price: ${market_price:.2f} (entry is {dist_pct:+.1f}% away)"
            if abs(dist_pct) >= PERSONAL_SIGNAL_ENTRY_WARN_PCT:
                reply += (
                    f"\n  ⚠️ Entry is {abs(dist_pct):.1f}% away from the live price — "
                    f"this reads as a target level, not a fillable price right now."
                )
        else:
            reply += "\n  ⚠️ Couldn't fetch a live price to sanity-check this entry."

        bot.reply_to(message, reply)
        return sig_id

    except Exception as e:
        logger.error(f"/signal failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")
        return None


@bot.message_handler(commands=['mysignals'])
def cmd_list_personal_signals(message):
    """
    /mysignals [status?] — List your signal ideas.
    Status: open (default), closed, all
    """
    try:
        parts = message.text.strip().split()
        status = "open"
        if len(parts) > 1:
            status = parts[1].lower()

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                if status == "all":
                    c.execute(
                        "SELECT id, ticker, strategy, direction, entry_level, target, stop_loss, thesis, status, created_at, asset_class "
                        "FROM personal_signals ORDER BY created_at DESC LIMIT 20"
                    )
                else:
                    c.execute(
                        "SELECT id, ticker, strategy, direction, entry_level, target, stop_loss, thesis, status, created_at, asset_class "
                        "FROM personal_signals WHERE status = ? ORDER BY created_at DESC LIMIT 20",
                        (status,)
                    )
                signals = c.fetchall()

        if not signals:
            bot.reply_to(message, f"No {status} personal signals found.")
            return

        # Live prices drift after logging — re-fetch at view time (not just once
        # at /signal creation) so a stale entry level is obvious every time this
        # is checked, not only the moment it was typed. Only EQUITY/OPTION are
        # servable via Alpaca's equity snapshot feed — FUTURES/CRYPTO/FOREX are
        # skipped rather than attempted-and-failed every time.
        tickers = sorted({row[1] for row in signals if (row[10] or "EQUITY") in PERSONAL_SIGNAL_LIVE_PRICE_ASSET_CLASSES})
        try:
            live_prices = {t: v.get("price") for t, v in fetch_batch_snapshots(tickers).items() if v.get("price")} if tickers else {}
        except Exception as e:
            logger.warning(f"/mysignals: live price fetch failed: {e}")
            live_prices = {}

        lines = [f"*Your {status.upper()} Signals*\n"]
        for sig_id, ticker, strat, direction, entry, target, stop, thesis, sig_status, created_ts, asset_class in signals:
            asset_class = asset_class or "EQUITY"
            created = datetime.fromisoformat(created_ts).astimezone(ZoneInfo("America/New_York"))
            status_emoji = "🟢" if sig_status == "open" else "🟡"
            line = (
                f"{status_emoji} `[{sig_id:3d}]` {created.strftime('%m-%d %H:%M')} | "
                f"{ticker:6s} [{asset_class}] {strat:15s} {direction:4s}\n"
                f"         Entry: ${entry:7.2f} | Target: ${target:7.2f} | Stop: ${stop:7.2f} | {thesis}"
            )
            if asset_class not in PERSONAL_SIGNAL_LIVE_PRICE_ASSET_CLASSES:
                line += f"\n         ℹ️ No live-price feed for {asset_class} yet"
            else:
                live = live_prices.get(ticker)
                if live and entry:
                    dist_pct = (entry - live) / live * 100
                    flag = " ⚠️ not currently fillable" if abs(dist_pct) >= PERSONAL_SIGNAL_ENTRY_WARN_PCT else ""
                    line += f"\n         Live: ${live:7.2f} ({dist_pct:+.1f}% from entry){flag}"
            lines.append(line)

        report = "\n".join(lines)
        for chunk in smart_split(report, chars_per_string=4096):
            bot.send_message(message.chat.id, chunk, parse_mode="Markdown")

    except Exception as e:
        logger.error(f"/mysignals failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")


@bot.message_handler(commands=['fills'])
def cmd_list_imported_fills(message):
    """
    /fills [days?] — List auto-detected fills (e.g. via IBKR Flex Web Service).
    Default: last 3 days.

    Coverage: EQUITY/OPTION only.

    If nothing shows up, check that FILLS_IMPORTER_SOURCE and the required
    credentials (e.g., IBKR_FLEX_TOKEN) are set. This command reads what the
    scheduled importer wrote to imported_fills.
    """
    try:
        parts = message.text.strip().split()
        days = int(parts[1]) if len(parts) > 1 else 3

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT ticker, action, asset_class, quantity, price, trade_date, matched_signal_id, source "
                    "FROM imported_fills WHERE trade_date >= date('now', ?) "
                    "ORDER BY trade_date DESC, id DESC LIMIT 30",
                    (f"-{days} days",),
                )
                fills = c.fetchall()

        if not fills:
            bot.reply_to(message, f"No fills detected in the last {days} day(s).")
            return

        lines = [f"*Detected Fills (last {days}d)*\n"]
        for ticker, action, asset_class, qty, price, trade_date, matched_signal_id, source in fills:
            line = f"{trade_date} | {ticker:6s} [{asset_class}] {action} {qty:g} @ ${price:.2f} ({source})"
            if matched_signal_id:
                line += f" — matches personal signal #{matched_signal_id}"
            lines.append(line)

        report = "\n".join(lines)
        for chunk in smart_split(report, chars_per_string=4096):
            bot.send_message(message.chat.id, chunk, parse_mode="Markdown")

    except Exception as e:
        logger.error(f"/fills failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")


@bot.message_handler(commands=['signals'])
def cmd_list_signals(message):
    """
    /signals [hours?] — List recent SYSTEM signal recommendations.
    Default: last 4 hours of signals.
    Example: /signals 2
    Use /mysignals for your personal signal ideas.
    """
    try:
        parts = message.text.strip().split()
        hours = 4
        if len(parts) > 1:
            try:
                hours = int(parts[1])
            except ValueError:
                hours = 4

        # Query execution engine's options_signals table
        cutoff_time = int((datetime.now(timezone.utc) - timedelta(hours=hours)).timestamp())

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT id, ticker, strategy, direction, strike, dte, outcome, scan_at "
                    "FROM options_signals WHERE scan_at >= ? ORDER BY scan_at DESC LIMIT 20",
                    (cutoff_time,)
                )
                signals = c.fetchall()

        if not signals:
            bot.reply_to(message, f"No system signals found in the last {hours} hour(s).")
            return

        lines = ["*Recent SYSTEM Signals*\n"]
        for sig_id, ticker, strategy, direction, strike, dte, outcome, scan_ts in signals:
            ts = datetime.fromtimestamp(scan_ts, tz=timezone.utc).astimezone(ZoneInfo("America/New_York"))
            strike_str = f" ${strike:.2f}" if strike else ""
            outcome_emoji = "✅" if outcome == "generated" else "❌" if outcome == "blocked" else "❓"
            lines.append(
                f"{outcome_emoji} `[{sig_id:3d}]` {ts.strftime('%H:%M')} {ticker:6s} {strategy:20s} "
                f"{direction:4s}{strike_str} {dte}DTE"
            )

        report = "\n".join(lines[:25])
        if len(signals) > 25:
            report += f"\n_(showing 25 of {len(signals)})_"

        for chunk in smart_split(report, chars_per_string=4096):
            bot.send_message(message.chat.id, chunk, parse_mode="Markdown")

    except Exception as e:
        logger.error(f"/signals failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")


@bot.message_handler(commands=['trade'])
def cmd_trade(message):
    """
    /trade [date:YYYY-MM-DD?] [s|p]:[signal_id] [qty] [price] [deviation_notes?] [bucket:core|satellite?]

    Log your execution against a system or personal signal.
    Prefix: s=system signal, p=personal signal

    Examples:
      /trade s:47                           — executed system signal #47
      /trade p:12                           — executed personal signal #12
      /trade s:47 10 450.50                 — with custom qty/price
      /trade p:12 5 125.00 used tighter stop
      /trade date:2026-07-10 p:12 5 125.00  — backdated to when it actually filled
      /trade p:12 3 0.45 bucket:satellite   — logged to the moonshot sleeve

    Signal details auto-fill from the log. Optional: add deviation notes.

    bucket: core (default) = your disciplined base sleeve; satellite = the
    speculative "moonshot" sleeve (0DTE/OTM/high-IV). Satellite entries get a
    soft-warn if they push you past your PERSONAL_SATELLITE_CAP_PCT cap or
    re-fund a losing satellite sleeve from fresh capital — it warns, never
    blocks. See /barbell for the current split.
    """
    try:
        raw_text, date_override = _extract_date_override(message.text.strip())
        # Optional `bucket:core|satellite` selector for the barbell split.
        # Prefixed-only so a bare word in the notes field can't be mistaken
        # for a bucket. Absent → core (the disciplined default sleeve).
        raw_text, bucket = barbell.extract_bucket_override(raw_text)
        parts = raw_text.split(maxsplit=4)
        if len(parts) < 2:
            bot.reply_to(
                message,
                "❌ Format: /trade [s|p]:[signal_id] [qty?] [price?] [notes?] [bucket:core|satellite]\n"
                "Examples:\n"
                "  /trade s:47\n"
                "  /trade p:12 10 450.50 tighter stop\n"
                "  /trade p:12 3 0.45 bucket:satellite   (moonshot sleeve)"
            )
            return

        signal_ref = parts[1]
        if ':' not in signal_ref:
            bot.reply_to(message, "❌ Format: /trade [s|p]:[signal_id]  (s=system, p=personal)")
            return

        signal_source, signal_id_str = signal_ref.split(':', 1)
        signal_source = signal_source.lower()
        if signal_source not in ('s', 'p'):
            bot.reply_to(message, "❌ Prefix must be 's' (system) or 'p' (personal)")
            return

        try:
            signal_id = int(signal_id_str)
        except ValueError:
            bot.reply_to(message, "❌ Signal ID must be a number")
            return

        # Look up the signal
        ticker = strategy = direction = signal_time = quality_score = None
        strike = dte = None
        asset_class = None

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                if signal_source == 's':
                    c.execute(
                        "SELECT ticker, strategy, direction, strike, dte, quality_score, scan_at "
                        "FROM options_signals WHERE id = ?",
                        (signal_id,)
                    )
                    row = c.fetchone()
                    if row:
                        ticker, strategy, direction, strike, dte, quality_score, scan_ts = row
                        signal_time = datetime.fromtimestamp(scan_ts, tz=timezone.utc).astimezone(ZoneInfo("America/New_York"))
                        asset_class = "OPTION"  # every options_signals row is an option
                else:
                    c.execute(
                        "SELECT ticker, strategy, direction, entry_level, target, stop_loss, created_at, asset_class "
                        "FROM personal_signals WHERE id = ?",
                        (signal_id,)
                    )
                    row = c.fetchone()
                    if row:
                        ticker, strategy, direction, entry_level, target, stop_loss, created_ts, asset_class = row
                        signal_time = datetime.fromisoformat(created_ts).astimezone(ZoneInfo("America/New_York"))
                        quality_score = None  # Personal signals don't have a quality score

        if not ticker:
            source_name = "System signal" if signal_source == 's' else "Personal signal"
            bot.reply_to(
                message,
                f"❌ {source_name} #{signal_id} not found.\n"
                f"Use /signals for system signals or /mysignals for your personal signals."
            )
            return

        # Parse optional execution details
        quantity = None
        price = None
        deviation_notes = ""

        if len(parts) > 2:
            try:
                quantity = float(parts[2])
                if len(parts) > 3:
                    try:
                        price = float(parts[3])
                        if len(parts) > 4:
                            deviation_notes = parts[4]
                    except ValueError:
                        deviation_notes = parts[3]
            except ValueError:
                deviation_notes = parts[2]

        # /trade only ever logs an ENTRY — a close is logged with /close, which
        # writes the CLOSE row + pnl and flips personal_signals.status (audit
        # §1 C1: the old `"close" in direction.lower()` branch was dead code —
        # direction is only ever LONG/SHORT/BULLISH/BEARISH — so nothing ever
        # produced a CLOSE row or a realized pnl).
        action = "OPEN" if (asset_class == "OPTION" or strategy in (
            "LONG_CALL", "LONG_PUT", "BULL_CALL_SPREAD", "BEAR_PUT_SPREAD",
            "STRADDLE", "STRANGLE", "REVERSE_IRON_CONDOR", "CSP", "CC")) else "BUY"

        # Normalise signal_source and direction to the vocabulary the joins
        # (_get_personal_trades_for_day) and resolvers (signal_outcome_resolver)
        # actually query on: 'system'/'personal' and LONG/SHORT. Storing 's'/'p'
        # (the old bug) made every source-aware join miss silently.
        source_full = "system" if signal_source == "s" else "personal"
        trade_direction = _PERSONAL_TRADE_DIRECTION_NORMALIZE.get(
            (direction or "").upper(), direction)

        # Store execution
        exec_time = date_override or datetime.now(ZoneInfo("America/New_York"))
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                cur = conn.execute(
                    "INSERT INTO personal_trades "
                    "(signal_source, signal_id, ticker, strategy, action, quantity, price, "
                    " deviation_notes, executed_at, direction, asset_class, bucket) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (source_full, signal_id, ticker, strategy, action, quantity, price,
                     deviation_notes, exec_time.isoformat(), trade_direction, asset_class, bucket),
                )
                trade_row_id = cur.lastrowid
                conn.commit()

        # Confirmation
        qty_str = f" x{quantity}" if quantity else ""
        price_str = f" @ ${price:.2f}" if price else ""
        notes_str = f"\n💬 {deviation_notes}" if deviation_notes else ""
        # quality_score is on a 0.00–1.00 scale (weighted-component sum, capped
        # at 1.0 since commit 1d7a1eb) — the old "/10" label misread e.g. 0.40
        # as "0.40/10".
        quality_str = f"Quality: {quality_score:.2f}/1.00 | " if quality_score else ""
        backdate_str = f"\n  🕓 Backdated to {date_override.strftime('%Y-%m-%d %H:%M ET')}" if date_override else ""

        source_emoji = "🤖" if signal_source == 's' else "👤"
        bucket_emoji = "🎯" if bucket == barbell.SATELLITE else "🧱"
        bucket_str = f"\n  {bucket_emoji} Bucket: {bucket}"

        # Barbell soft-warns — computed AFTER the insert so the just-logged
        # trade is included in the exposure figure. Never blocks.
        barbell_str = ""
        if bucket == barbell.SATELLITE:
            snap = _barbell_snapshot()
            warns = barbell.satellite_soft_warnings(snap) if snap else []
            reminder = (
                "🎯 Satellite rules: treat this premium as already lost (no "
                "tight stop), and don't paper-hand it into a base hit — you're "
                "paying for the tail, so demand the tail."
            )
            barbell_str = "\n" + "\n".join(warns + [reminder])
        else:
            # Core is supposed to be the evidence-backed sleeve, not just
            # "not satellite" by default — flag core entries outside the one
            # measured-edge ticker/strategy combination (2026-07-18 plan).
            scope_warn = barbell.core_scope_warning(ticker, strategy)
            if scope_warn:
                barbell_str = "\n" + scope_warn

        bot.reply_to(
            message,
            f"{source_emoji} ✅ Logged execution of {'system' if signal_source == 's' else 'personal'} signal #{signal_id}\n"
            f"  {ticker} {strategy} {direction}\n"
            f"  {action}{qty_str}{price_str}{bucket_str}\n"
            f"  {quality_str}Sent: {signal_time.strftime('%H:%M ET')}{notes_str}{backdate_str}\n"
            f"  Close it later with: /close {trade_row_id} EXIT_PRICE"
            f"{barbell_str}"
        )
        return trade_row_id

    except Exception as e:
        logger.error(f"/trade failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")
        return None


@bot.message_handler(commands=['close'])
def cmd_close(message):
    """
    /close [date:YYYY-MM-DD?] TRADE_ID EXIT_PRICE [notes]

    Close a personal trade you previously opened with /trade, at a known exit
    price. Writes a CLOSE row with realized pnl and flips the linked personal
    signal's status to 'closed' (audit §1 C1: closing was previously
    impossible, so personal win rate was hard-wired to 0%). TRADE_ID is the
    personal_trades id echoed by /trade.

    Examples:
      /close 12 4.85
      /close 12 4.85 rolled early, IV crushed
      /close date:2026-07-12 12 4.85         — backdated to when it actually closed
    """
    try:
        raw_text, date_override = _extract_date_override(message.text.strip())
        parts = raw_text.split(maxsplit=3)
        if len(parts) < 3:
            bot.reply_to(
                message,
                "❌ Format: /close [date:YYYY-MM-DD?] TRADE_ID EXIT_PRICE [notes]\n"
                "Example: /close 12 4.85 rolled early"
            )
            return
        try:
            trade_id = int(parts[1])
        except ValueError:
            bot.reply_to(message, "❌ TRADE_ID must be a number (the id /trade echoed).")
            return
        try:
            exit_price = float(parts[2])
        except ValueError:
            bot.reply_to(message, "❌ EXIT_PRICE must be a number.")
            return
        close_notes = parts[3] if len(parts) > 3 else ""

        exec_time = date_override or datetime.now(ZoneInfo("America/New_York"))
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT signal_source, signal_id, ticker, strategy, action, quantity, "
                    "       price, direction, asset_class "
                    "FROM personal_trades WHERE id = ?",
                    (trade_id,),
                )
                row = c.fetchone()
                if not row:
                    bot.reply_to(message, f"❌ Trade #{trade_id} not found. See /eod for your logged trades.")
                    return
                (sig_source, sig_id, ticker, strategy, action, quantity,
                 entry_price, direction, asset_class) = row

                if action not in ("OPEN", "BUY"):
                    bot.reply_to(
                        message,
                        f"❌ Trade #{trade_id} is a {action} row, not an open entry — "
                        f"you can only /close an OPEN/BUY trade."
                    )
                    return

                already = c.execute(
                    "SELECT id FROM personal_trades WHERE closes_trade_id = ?",
                    (trade_id,),
                ).fetchone()
                if already:
                    bot.reply_to(message, f"❌ Trade #{trade_id} is already closed (by CLOSE row #{already[0]}).")
                    return

                pnl = _compute_personal_pnl(strategy, asset_class, direction,
                                            entry_price, exit_price, quantity)
                close_action = "SELL" if strategy in OPTIONS_SHORT_PREMIUM_STRATEGIES or action == "BUY" else "CLOSE"

                c.execute(
                    "INSERT INTO personal_trades "
                    "(signal_source, signal_id, ticker, strategy, action, quantity, price, pnl, "
                    " deviation_notes, executed_at, direction, asset_class, closes_trade_id) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (sig_source, sig_id, ticker, strategy, close_action, quantity, exit_price, pnl,
                     close_notes, exec_time.isoformat(), direction, asset_class, trade_id),
                )
                # Flip the linked personal signal to closed so /mysignals closed
                # works and importer fill→signal matching stops re-matching it.
                if sig_source == "personal" and sig_id:
                    c.execute(
                        "UPDATE personal_signals SET status = 'closed' WHERE id = ?",
                        (sig_id,),
                    )
                conn.commit()

        pnl_str = f"${pnl:+,.2f}" if pnl is not None else "n/a (missing qty/entry)"
        entry_str = f"${entry_price:.2f}" if entry_price is not None else "?"
        notes_str = f"\n💬 {close_notes}" if close_notes else ""
        backdate_str = f"\n  🕓 Backdated to {date_override.strftime('%Y-%m-%d %H:%M ET')}" if date_override else ""
        result_emoji = "🟢" if (pnl is not None and pnl > 0) else ("🔴" if (pnl is not None and pnl < 0) else "⚪")
        bot.reply_to(
            message,
            f"{result_emoji} ✅ Closed trade #{trade_id}\n"
            f"  {ticker} {strategy} {direction}\n"
            f"  Entry {entry_str} → Exit ${exit_price:.2f}\n"
            f"  Realized P&L: {pnl_str}{notes_str}{backdate_str}"
        )

    except Exception as e:
        logger.error(f"/close failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")


@bot.message_handler(commands=['barbell'])
def cmd_barbell(message):
    """
    /barbell

    Show the current core vs satellite (moonshot) allocation of your personal
    trades: open exposure per sleeve, the satellite cap and headroom, and
    lifetime realized P&L / win rate per sleeve. The satellite sleeve is
    *supposed* to have a low win rate — keeping it measured apart is the whole
    point of the split (a low-WR moonshot is a success by design; blending it
    into one number flatters it and slanders the core).
    """
    try:
        snap = _barbell_snapshot()
        if snap is None:
            bot.reply_to(message, "❌ Couldn't read personal trades right now — try again.")
            return
        bot.reply_to(message, barbell.render_status(snap), parse_mode="Markdown")
    except Exception as e:
        logger.error(f"/barbell failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")



def _sports_bankroll_summary(conn: sqlite3.Connection) -> dict:
    """
    Mirrors sports_predictions/predictions_store.py's PredictionsStore.bankroll_summary
    — duplicated rather than imported (same shared-DB-no-cross-import pattern
    as every other sports_manual_bets reader/writer in this file) since this
    container and sports_predictions are separate processes/images. Folds in
    resolved parlay P&L (sports_manual_parlays) on top of single-leg bets, and
    reports a satellite-only line (parlays + any bet explicitly bucketed
    satellite) separate from core — same rationale as barbell.py's
    core-vs-satellite split on the options side: a moonshot sleeve succeeding
    by having a low hit rate shouldn't slander the disciplined core numbers.
    """
    resolved = conn.execute(
        "SELECT COUNT(*) AS n, "
        "SUM(CASE WHEN result = 'win' THEN 1 ELSE 0 END) AS wins, "
        "SUM(CASE WHEN result = 'loss' THEN 1 ELSE 0 END) AS losses, "
        "SUM(pnl) AS total_pnl "
        "FROM sports_manual_bets WHERE result != 'pending'"
    ).fetchone()
    pending = conn.execute(
        "SELECT COUNT(*) AS n, SUM(stake) AS total_stake "
        "FROM sports_manual_bets WHERE result = 'pending'"
    ).fetchone()
    core_resolved = conn.execute(
        "SELECT COUNT(*) AS n, "
        "SUM(CASE WHEN result = 'win' THEN 1 ELSE 0 END) AS wins, "
        "SUM(CASE WHEN result = 'loss' THEN 1 ELSE 0 END) AS losses, "
        "SUM(pnl) AS total_pnl "
        "FROM sports_manual_bets WHERE result != 'pending' AND bucket = 'core'"
    ).fetchone()

    try:
        parlay_resolved = conn.execute(
            "SELECT COUNT(*) AS n, "
            "SUM(CASE WHEN result = 'win' THEN 1 ELSE 0 END) AS wins, "
            "SUM(CASE WHEN result = 'loss' THEN 1 ELSE 0 END) AS losses, "
            "SUM(pnl) AS total_pnl "
            "FROM sports_manual_parlays WHERE result != 'pending'"
        ).fetchone()
        parlay_pending = conn.execute(
            "SELECT COUNT(*) AS n, SUM(stake) AS total_stake "
            "FROM sports_manual_parlays WHERE result = 'pending'"
        ).fetchone()
    except sqlite3.OperationalError:
        parlay_resolved, parlay_pending = None, None

    total_pnl = (resolved[3] if resolved else None) or 0.0
    total_pnl += (parlay_resolved[3] if parlay_resolved else None) or 0.0
    satellite_pnl = ((resolved[3] if resolved else None) or 0.0) - ((core_resolved[3] if core_resolved else None) or 0.0)
    satellite_pnl += (parlay_resolved[3] if parlay_resolved else None) or 0.0
    return {
        "starting_bankroll": SPORTS_STARTING_BANKROLL,
        "current_bankroll": SPORTS_STARTING_BANKROLL + total_pnl,
        "total_pnl": total_pnl,
        "wins": (resolved[1] if resolved else None) or 0,
        "losses": (resolved[2] if resolved else None) or 0,
        "resolved_bets": (resolved[0] if resolved else None) or 0,
        "pending_bets": (pending[0] if pending else None) or 0,
        "pending_stake": (pending[1] if pending else None) or 0.0,
        "core_pnl": (core_resolved[3] if core_resolved else None) or 0.0,
        "core_wins": (core_resolved[1] if core_resolved else None) or 0,
        "core_losses": (core_resolved[2] if core_resolved else None) or 0,
        "satellite_pnl": satellite_pnl,
        "parlays_resolved": (parlay_resolved[0] if parlay_resolved else None) or 0,
        "parlays_won": (parlay_resolved[1] if parlay_resolved else None) or 0,
        "parlays_lost": (parlay_resolved[2] if parlay_resolved else None) or 0,
        "parlays_pending": (parlay_pending[0] if parlay_pending else None) or 0,
        "parlay_pending_stake": (parlay_pending[1] if parlay_pending else None) or 0.0,
    }


# NOX-PUBLIC-SYNC-NOTE: sports betting is kept private by default as of
# 2026-07-12 (see .public-denylist and docs_private/journal for the
# rationale). This command and the sports_manual_bets table above are the
# one place that decision isn't enforceable by a path-based denylist, since
# monitor.py itself is otherwise a public-safe file — strip this handler and
# its table before ever syncing this file upstream.
def _cmd_bet_parlay(message, bot=bot):
    """
    /bet parlay log legs:<pred_id_1>,<pred_id_2>[,...] stake:<amount> odds:<american_odds> [notes?]

    key:value tokens (same style as /trade's bucket:core|satellite) rather
    than positional args — a positional `<id1> <id2> <stake> <odds>` list is
    ambiguous about where leg ids end and stake/odds begin once there are 2+
    legs. Always logged to sports_manual_parlays with bucket='satellite'
    (all-or-nothing by construction, never the disciplined core sleeve).

    Example:
      /bet parlay log legs:14,27 stake:5 odds:+450 Lakers ML + Celtics -3.5
    """
    try:
        raw = message.text.strip()
        legs_match = re.search(r'legs:([\d,]+)', raw)
        stake_match = re.search(r'stake:([\d.]+)', raw)
        odds_match = re.search(r'odds:([+-]?[\d.]+)', raw)
        if not legs_match or not stake_match:
            bot.reply_to(
                message,
                "❌ Format: /bet parlay log legs:<id1>,<id2>[,...] stake:<amount> odds:<american_odds> [notes?]\n"
                "Example: /bet parlay log legs:14,27 stake:5 odds:+450 Lakers ML + Celtics -3.5"
            )
            return

        try:
            leg_ids = [int(x) for x in legs_match.group(1).split(',') if x]
            stake = float(stake_match.group(1))
        except ValueError:
            bot.reply_to(message, "❌ leg ids and stake must be numbers.")
            return
        if len(leg_ids) < 2:
            bot.reply_to(message, "❌ A parlay needs at least 2 legs — use /bet log for a single pick.")
            return

        combined_odds = float(odds_match.group(1)) if odds_match else None
        notes = raw[odds_match.end():].strip() if odds_match else raw[stake_match.end():].strip()
        # legs:/stake:/odds: can appear in any order — strip any that ended
        # up inside the free-text tail instead of assuming a fixed order.
        notes = re.sub(r'\b(legs|stake|odds):\S+\b', '', notes).strip()

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    f"SELECT id, home_team, away_team, pick, confidence FROM sports_predictions "
                    f"WHERE id IN ({','.join('?' * len(leg_ids))})",
                    leg_ids,
                )
                leg_rows = {r[0]: r for r in c.fetchall()}
                missing = [lid for lid in leg_ids if lid not in leg_rows]
                if missing:
                    bot.reply_to(message, f"❌ Prediction id(s) not found: {missing}")
                    return

                weak_legs = [leg_rows[lid] for lid in leg_ids if leg_rows[lid][4] < SPORTS_PARLAY_MIN_CONFIDENCE]

                c.execute(
                    "INSERT INTO sports_manual_parlays (stake, combined_odds, notes, bucket) "
                    "VALUES (?, ?, ?, 'satellite')",
                    (stake, combined_odds, notes),
                )
                parlay_id = c.lastrowid
                c.executemany(
                    "INSERT INTO sports_parlay_legs (parlay_id, prediction_id) VALUES (?, ?)",
                    [(parlay_id, lid) for lid in leg_ids],
                )
                conn.commit()
                bankroll = _sports_bankroll_summary(conn) if SPORTS_STARTING_BANKROLL > 0 else None

        leg_lines = "\n".join(
            f"  • #{lid} {leg_rows[lid][2]} @ {leg_rows[lid][1]} — pick {leg_rows[lid][3]} "
            f"(edge {leg_rows[lid][4]:.1%})"
            for lid in leg_ids
        )
        odds_str = f" @ {combined_odds:+.0f}" if combined_odds is not None else ""
        warn_str = ""
        if weak_legs:
            weak_str = ", ".join(f"#{leg_rows[lid][0]} ({leg_rows[lid][4]:.1%})" for lid in leg_ids
                                  if leg_rows[lid][4] < SPORTS_PARLAY_MIN_CONFIDENCE)
            warn_str = (
                f"\n⚠️ {len(weak_legs)} leg(s) below your {SPORTS_PARLAY_MIN_CONFIDENCE:.0%} "
                f"best-of-best bar for parlays: {weak_str} — logged anyway, just flagging it."
            )
        bankroll_str = ""
        if bankroll is not None:
            stake_pct = stake / bankroll["current_bankroll"] if bankroll["current_bankroll"] > 0 else float("inf")
            bankroll_str = (
                f"\n💰 Bankroll: ${bankroll['current_bankroll']:.2f} — this stake is {stake_pct:.1%} of it "
                f"(satellite sleeve; suggested parlay sizing is ~{SPORTS_PARLAY_STAKE_PCT:.0%})"
            )
        bot.reply_to(
            message,
            f"🎲 ✅ Logged {len(leg_ids)}-leg parlay #{parlay_id} (satellite)\n"
            f"{leg_lines}\n"
            f"Stake: {stake}{odds_str}{f' — {notes}' if notes else ''}"
            f"{warn_str}{bankroll_str}"
        )

    except Exception as e:
        logger.error(f"/bet parlay failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")


@bot.message_handler(commands=['bet'])
def cmd_bet(message, bot=bot):
    """
    /bet log <prediction_id> <stake> <odds> [notes?]
    /bet parlay log <pred_id_1> <pred_id_2> [...] <stake> <odds> [notes?]

    Log a sports bet you placed yourself against a prediction the
    sports_predictions container sent you. Predictions-only by design —
    nothing in this repo places a sports bet automatically; this command
    just records what you did, same relationship /trade has to options
    signals.

    Example:
      /bet log 14 25 -110 took Lakers -3.5 at DraftKings
      /bet parlay log 14 22 5 +450 Lakers ML + Celtics -3.5
    """
    try:
        parts = message.text.strip().split(maxsplit=4)
        if len(parts) >= 2 and parts[1].lower() == 'parlay':
            _cmd_bet_parlay(message, bot)
            return
        if len(parts) < 4 or parts[1].lower() != 'log':
            bot.reply_to(
                message,
                "❌ Format: /bet log <prediction_id> <stake> <odds> [notes?]\n"
                "Example: /bet log 14 25 -110 took Lakers -3.5 at DraftKings\n"
                "Or: /bet parlay log <pred_id_1> <pred_id_2> [...] <stake> <odds> [notes?]"
            )
            return

        try:
            prediction_id = int(parts[2])
            stake = float(parts[3])
        except ValueError:
            bot.reply_to(message, "❌ prediction_id and stake must be numbers.")
            return

        odds_taken = None
        notes = ""
        if len(parts) > 4:
            rest = parts[4].split(maxsplit=1)
            try:
                odds_taken = float(rest[0])
                notes = rest[1] if len(rest) > 1 else ""
            except ValueError:
                notes = parts[4]

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT home_team, away_team, pick, edge FROM sports_predictions WHERE id = ?",
                    (prediction_id,)
                )
                pred_row = c.fetchone()
                if not pred_row:
                    bot.reply_to(
                        message,
                        f"❌ Prediction #{prediction_id} not found. It should have arrived via a "
                        "Telegram alert from the sports predictions bot."
                    )
                    return

                c.execute(
                    "INSERT INTO sports_manual_bets (prediction_id, stake, odds_taken, notes) "
                    "VALUES (?, ?, ?, ?)",
                    (prediction_id, stake, odds_taken, notes),
                )
                conn.commit()

                # Bankroll BEFORE this stake — deliberately excludes the bet
                # just inserted (still 'pending', so bankroll_summary already
                # excludes it) so the % check is "how much of what I have am
                # I risking," not diluted by the stake itself.
                bankroll = _sports_bankroll_summary(conn) if SPORTS_STARTING_BANKROLL > 0 else None

        home_team, away_team, pick, edge = pred_row
        odds_str = f" @ {odds_taken:+.0f}" if odds_taken is not None else ""
        notes_str = f"\n💬 {notes}" if notes else ""
        bankroll_str = ""
        warn_str = ""
        if bankroll is not None:
            stake_pct = stake / bankroll["current_bankroll"] if bankroll["current_bankroll"] > 0 else float("inf")
            bankroll_str = (
                f"\n💰 Bankroll: ${bankroll['current_bankroll']:.2f} "
                f"(started ${SPORTS_STARTING_BANKROLL:.2f}, P&L ${bankroll['total_pnl']:+.2f}) "
                f"— this stake is {stake_pct:.1%} of it"
            )
            if stake_pct > SPORTS_MAX_STAKE_PCT:
                warn_str = (
                    f"\n⚠️ That's above your {SPORTS_MAX_STAKE_PCT:.0%} soft cap — logged anyway, "
                    "just flagging it."
                )
        bot.reply_to(
            message,
            f"🏀 ✅ Logged bet against prediction #{prediction_id}\n"
            f"  {away_team} @ {home_team} — pick: {pick} (edge {edge:.1%})\n"
            f"  Stake: {stake}{odds_str}{notes_str}{bankroll_str}{warn_str}"
        )

    except Exception as e:
        logger.error(f"/bet failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")


@bot.message_handler(commands=['sports'])
def cmd_sports(message, bot=bot):
    """
    /sports [n] — recent predictions (edge, model vs. market prob, whether a
    bet was logged) plus recently-resolved bets. Reads the same shared
    memory_bank.db tables the sports_predictions container writes/monitor.py's
    /bet command writes — no cross-process import, same pattern as every
    other shared-table command in this file.

    `bot=bot` default: this command (and /sports-status, /bet) is also
    registered on sports_bot near the bottom of this file with
    pass_bot=True, so telebot calls this as cmd_sports(message, bot=sports_bot)
    when triggered from @Noxrivalbot — the `bot` name inside this function
    then refers to whichever bot actually received the command, so replies
    go back through the right chat instead of always the main bot's token.
    """
    try:
        parts = message.text.strip().split()
        try:
            requested = int(parts[1]) if len(parts) > 1 else 8
            count = max(1, min(requested, 25))
        except (ValueError, IndexError):
            count = 8

        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()
                c.execute(
                    "SELECT id, home_team, away_team, pick, model_home_prob, "
                    "market_home_prob, confidence, created_at "
                    "FROM sports_predictions ORDER BY created_at DESC LIMIT ?",
                    (count,)
                )
                predictions = c.fetchall()

                c.execute(
                    "SELECT b.prediction_id, b.stake, b.odds_taken, b.result, b.pnl "
                    "FROM sports_manual_bets b ORDER BY b.placed_at DESC LIMIT ?",
                    (count,)
                )
                bets = c.fetchall()

        if not predictions:
            bot.reply_to(
                message,
                f"📭 No {SPORTS_SPORT_KEY} predictions yet. "
                f"Check /sports-status for whether the service is running.",
            )
            return

        bets_by_prediction = {b[0]: b for b in bets}
        lines = [f"🏆 *Last {len(predictions)} prediction(s) — {SPORTS_SPORT_KEY}*\n"]
        for pred_id, home, away, pick, model_p, market_p, confidence, created_at in predictions:
            bet_str = ""
            if pred_id in bets_by_prediction:
                _, stake, odds, result, pnl = bets_by_prediction[pred_id]
                pnl_str = f" pnl={pnl:.2f}" if pnl is not None else ""
                bet_str = f" — bet logged: {result}{pnl_str}"
            lines.append(
                f"• `#{pred_id}` {escape_markdown(away)} @ {escape_markdown(home)} — "
                f"pick *{escape_markdown(pick)}* (edge {confidence:.1%})\n"
                f"   model {model_p:.1%} vs market {market_p:.1%}{bet_str}"
            )

        bot.reply_to(message, "\n".join(lines), parse_mode="Markdown")

    except Exception as e:
        logger.error(f"/sports failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")


@bot.message_handler(commands=['sports-status'])
def cmd_sports_status(message, bot=bot):
    """
    /sports-status — health check mirroring /status's shape: last prediction
    cycle, predictions today, Elo model coverage for the active sport, and
    odds API quota (read from a value the sports_predictions container
    already persisted as a byproduct of its own calls — this command never
    spends a request of its own to find out).
    """
    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()

                c.execute("SELECT MAX(created_at) FROM sports_predictions")
                last_cycle_row = c.fetchone()
                last_cycle_at = last_cycle_row[0] if last_cycle_row else None

                c.execute(
                    "SELECT COUNT(*) FROM sports_predictions "
                    "WHERE created_at >= datetime('now', '-1 day')"
                )
                predictions_24h = c.fetchone()[0]

                c.execute(
                    "SELECT COUNT(*), AVG(rating) FROM sport_elo_ratings WHERE sport = ?",
                    (SPORTS_SPORT_KEY,)
                )
                elo_row = c.fetchone()
                teams_tracked, avg_rating = (elo_row[0] or 0, elo_row[1])

                c.execute("SELECT remaining, used, updated_at FROM sports_api_quota WHERE id = 1")
                quota_row = c.fetchone()

                c.execute(
                    "SELECT COUNT(*) FROM sports_manual_bets WHERE result = 'pending'"
                )
                pending_bets = c.fetchone()[0]

                bankroll = _sports_bankroll_summary(conn) if SPORTS_STARTING_BANKROLL > 0 else None

        if last_cycle_at:
            last_cycle_dt = datetime.fromisoformat(last_cycle_at).replace(tzinfo=timezone.utc)
            age_minutes = (datetime.now(timezone.utc) - last_cycle_dt).total_seconds() / 60
            staleness = "🟢 fresh" if age_minutes < SPORTS_SCAN_INTERVAL_MINUTES * 2 else "🟡 stale — check container logs"
            last_cycle_str = f"{int(age_minutes)}m ago ({staleness})"
        else:
            last_cycle_str = "Never — no predictions in the DB yet"

        avg_rating_str = f"{avg_rating:.0f}" if avg_rating is not None else "N/A"
        quota_str = (f"{quota_row[0]}/{quota_row[0] + quota_row[1]} remaining (as of {quota_row[2][:19]})"
                     if quota_row and quota_row[0] is not None else "Unknown — no cycle has completed yet")

        bankroll_line = ""
        if bankroll is not None:
            record = f"{bankroll['wins']}-{bankroll['losses']}"
            bankroll_line = (
                f"\n💰 *Bankroll:* ${bankroll['current_bankroll']:.2f} "
                f"(started ${SPORTS_STARTING_BANKROLL:.2f}, P&L ${bankroll['total_pnl']:+.2f}, "
                f"record {record}) — ${bankroll['pending_stake']:.2f} at risk in "
                f"{bankroll['pending_bets']} pending bet(s)\n"
                f"🎲 *Core vs satellite:* core ${bankroll['core_pnl']:+.2f} "
                f"({bankroll['core_wins']}-{bankroll['core_losses']}) / "
                f"satellite ${bankroll['satellite_pnl']:+.2f} "
                f"({bankroll['parlays_won']}-{bankroll['parlays_lost']} parlays, "
                f"{bankroll['parlays_pending']} pending)"
            )

        status_msg = (
            f"🏆 *Sports Predictions Status*\n"
            f"{'─' * 24}\n"
            f"⚙️ *Sport:* `{SPORTS_SPORT_KEY}`\n"
            f"🔄 *Last cycle:* {last_cycle_str}\n"
            f"📊 *Predictions (24h):* {predictions_24h}\n"
            f"📈 *Elo teams tracked:* {teams_tracked} (avg rating {avg_rating_str})\n"
            f"🎯 *Odds API quota:* {quota_str}\n"
            f"⏳ *Pending bets:* {pending_bets}"
            f"{bankroll_line}"
        )
        bot.reply_to(message, status_msg, parse_mode="Markdown")

    except Exception as e:
        logger.error(f"/sports-status failed: {e}")
        bot.reply_to(message, f"❌ Error: {str(e)}")


@bot.message_handler(commands=['eow'])
def cmd_eow(message):
    """/eow — consolidated end-of-week account summary (structured data)."""
    _send_consolidated_report("week", use_narrative=False)


@bot.message_handler(commands=['eow-narrative'])
def cmd_eow_narrative(message):
    """/eow-narrative — end-of-week narrative analysis (Claude-powered)."""
    _send_consolidated_report("week", use_narrative=True)


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


# --- 5.5 WEEKLY PERFORMANCE REPORT (win/loss, MAE, calibration, parsing failures) ---
# Complementary to /eow (trade-ledger equity report): this one tracks the
# trade_predictions (predicted vs actual outcome MAE/calibration) and
# parsing_failures tables, which /eow does not surface.

def get_weekly_stats() -> dict:
    """
    Query memory_bank.db for the current calendar week's performance metrics.

    Returns a dict with keys:
        week_label, trade_count, total_pnl, wins, losses,
        win_loss_ratio, mae, calibration_score, parsing_failure_count
    On DB error returns {week_label, error}.
    """
    et      = ZoneInfo("America/New_York")
    now_et  = datetime.now(et)
    monday  = (now_et - timedelta(days=now_et.weekday())).replace(
        hour=0, minute=0, second=0, microsecond=0
    )
    week_label    = monday.strftime("%b %d") + " - " + now_et.strftime("%b %d, %Y")
    week_start_str = monday.strftime("%Y-%m-%d %H:%M:%S")

    try:
        with db_lock:
            with sqlite3.connect(DB_PATH) as conn:
                c = conn.cursor()

                # -- Total P&L and Win/Loss breakdown --
                c.execute(
                    "SELECT pnl FROM trade_history "
                    "WHERE timestamp >= ? AND pnl IS NOT NULL",
                    (week_start_str,),
                )
                pnl_rows    = [r[0] for r in c.fetchall()]
                trade_count = len(pnl_rows)
                total_pnl   = sum(pnl_rows)
                wins        = sum(1 for p in pnl_rows if p > 0)
                losses      = trade_count - wins
                win_loss_ratio = wins / losses if losses > 0 else float(wins)

                # -- MAE: predicted vs actual outcome --
                # Rows are written by workstreams that log forecasts; the table
                # starts empty and MAE is reported as N/A until data accumulates.
                c.execute(
                    "SELECT predicted_outcome, actual_outcome "
                    "FROM trade_predictions "
                    "WHERE timestamp >= ? "
                    "  AND predicted_outcome IS NOT NULL "
                    "  AND actual_outcome    IS NOT NULL",
                    (week_start_str,),
                )
                pred_rows = c.fetchall()
                if pred_rows:
                    mae = sum(abs(p - a) for p, a in pred_rows) / len(pred_rows)
                    # Calibration: 1 = perfect (MAE=0), 0 = total miscalibration
                    calibration_score = max(0.0, min(1.0, 1.0 - mae))
                else:
                    mae               = None
                    calibration_score = None

                # -- SEC parsing failures (all form types) --
                c.execute(
                    "SELECT COUNT(*) FROM parsing_failures WHERE timestamp >= ?",
                    (week_start_str,),
                )
                parsing_failure_count = c.fetchone()[0] or 0

        return {
            "week_label":            week_label,
            "trade_count":           trade_count,
            "total_pnl":             total_pnl,
            "wins":                  wins,
            "losses":                losses,
            "win_loss_ratio":        win_loss_ratio,
            "mae":                   mae,
            "calibration_score":     calibration_score,
            "parsing_failure_count": parsing_failure_count,
        }
    except Exception as e:
        logger.error(f"get_weekly_stats failed: {e}")
        return {"week_label": week_label, "error": str(e)}


def format_weekly_report(stats: dict) -> str:
    """
    Render weekly performance stats as a Telegram-ready Markdown message.

    Uses a monospace code block for the table — pipe-based Markdown tables
    are not supported in Telegram's Markdown mode; a code block gives clean
    fixed-width rendering without requiring MarkdownV2 escaping.
    """
    if "error" in stats:
        return f"⚠️ *Weekly Report Error*\n`{stats['error']}`"

    pnl_str = (
        f"+${stats['total_pnl']:.2f}"
        if stats["total_pnl"] >= 0
        else f"-${abs(stats['total_pnl']):.2f}"
    )
    if stats["losses"] > 0:
        wl_ratio_str = f"{stats['win_loss_ratio']:.2f}"
    elif stats["wins"] > 0:
        wl_ratio_str = "inf (all wins)"
    else:
        wl_ratio_str = "N/A"

    mae_str = f"{stats['mae']:.4f}" if stats["mae"] is not None else "N/A"
    cal_str = (
        f"{stats['calibration_score']:.1%}"
        if stats["calibration_score"] is not None
        else "N/A — no predictions logged"
    )

    W    = 22   # metric label column width
    rows = [
        ("Total P&L",            pnl_str),
        ("Trades",               f"{stats['trade_count']}  "
                                 f"({stats['wins']}W / {stats['losses']}L)"),
        ("Win/Loss Ratio",       wl_ratio_str),
        ("MAE (Pred vs Actual)", mae_str),
        ("Calibration Score",    cal_str),
        ("8-K Parse Failures",   str(stats["parsing_failure_count"])),
    ]
    header = f"{'Metric':<{W}}| Value"
    sep    = "-" * W + "+" + "-" * 16
    body   = "\n".join(f"{label:<{W}}| {value}" for label, value in rows)
    table  = f"```\n{header}\n{sep}\n{body}\n```"

    return (
        f"📊 *NOX WEEKLY PERFORMANCE REPORT*\n"
        f"------------------------\n"
        f"*Week:* {stats['week_label']}\n\n"
        + table
    )


def run_weekly_performance_report() -> None:
    """
    Build and deliver the weekly performance report via Telegram.
    smart_split ensures the message never exceeds Telegram's 4096-char cap.
    """
    try:
        logger.info("Weekly performance report building...")
        stats  = get_weekly_stats()
        report = format_weekly_report(stats)
        for chunk in smart_split(report, chars_per_string=4096):
            bot.send_message(CHAT_ID, chunk, parse_mode="Markdown")
        logger.info("Weekly performance report delivered.")
    except Exception as e:
        logger.error(f"run_weekly_performance_report failed: {e}")
        try:
            bot.send_message(CHAT_ID, f"⚠️ Weekly report failed: {e}")
        except Exception:
            pass


@bot.message_handler(commands=['weekly_report'])
def trigger_weekly_report(message):
    """
    /weekly_report — Manually triggers the win/loss + MAE/calibration weekly
    performance report on demand (distinct from /eow, which reports the
    trade-ledger equity summary). Runs in a background thread so the bot
    stays responsive during DB queries.
    """
    try:
        bot.reply_to(
            message,
            "⚙️ *Building weekly performance report...*",
            parse_mode='Markdown',
        )
        threading.Thread(target=run_weekly_performance_report, daemon=True).start()
    except Exception as e:
        print(f"[ERROR] [HEARTBEAT] /weekly_report command failed: {e}", flush=True)
        bot.reply_to(message, f"⚠️ Failed to trigger weekly report: {str(e)}")


# NOX-BUG-2026-07-14: this catch-all MUST be the LAST @bot.message_handler
# registered in the file. telebot dispatches in registration order and stops
# at the first filter match (see _run_middlewares_and_handler) — a
# func=lambda message: True handler registered before a specific commands=[...]
# handler silently swallows it, forever, with no error. This exact bug hid
# every command defined after it (originally registered right after /details,
# near the top of the file) for an unknown length of time before being found
# via /sports going to Claude's general chat instead of the sports dashboard.
# If you add a new command handler below this comment, move this block again.
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

        response_text = _extract_text(response)
        chunks = smart_split(response_text, chars_per_string=4096)
        for chunk in chunks:
            bot.send_message(message.chat.id, chunk)
    except Exception as e:
        bot.reply_to(message, f"⚠️ Brain Error: {str(e)}")


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
                # Dead-man's-switch (audit §5 C3): 503 if any supervised daemon
                # thread has gone stale, so the compose healthcheck flips this
                # container to unhealthy instead of it looking "up" forever.
                stale = stale_threads()
                if stale:
                    self._send(503, {"status": "unhealthy",
                                     "stale_threads": {n: a for n, a in stale}})
                else:
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


# --- 6.5 SPORTS BOT COMMAND LISTENER (separate token, own polling loop) ---
# Telegram's "one poller per token" limit is PER TOKEN, not global — a
# second bot instance on a different token polling in its own thread never
# conflicts with `bot.infinity_polling()` above. This lets /sports,
# /sports-status, and /bet also be typed directly in @Noxrivalbot, not just
# the main trading bot, per explicit request ("do both").
sports_bot = None
if SPORTS_TELEGRAM_BOT_TOKEN and SPORTS_TELEGRAM_CHAT_ID:
    sports_bot = telebot.TeleBot(SPORTS_TELEGRAM_BOT_TOKEN)
    sports_bot.message_handler(commands=['sports'], pass_bot=True)(cmd_sports)
    sports_bot.message_handler(commands=['sports-status'], pass_bot=True)(cmd_sports_status)
    sports_bot.message_handler(commands=['bet'], pass_bot=True)(cmd_bet)


def _run_sports_bot_polling():
    if sports_bot is None:
        logger.info("[SPORTS] SPORTS_TELEGRAM_BOT_TOKEN not set — sports bot command listener disabled "
                    "(commands still work on the main bot).")
        return
    try:
        sports_bot.infinity_polling()
    except Exception as e:
        # Isolated from the main bot's polling loop below — a sports-bot
        # failure (bad token, network hiccup) must never take down the
        # trading commands.
        logger.error(f"[SPORTS] sports_bot polling crashed: {e}")


# --- 5.9 DAEMON-THREAD DEAD-MAN'S-SWITCH (audit §5 C3 / RULE-D3) ---
# monitor.py runs several daemon threads in one process. If one dies, the
# process stays "up", `restart: unless-stopped` never fires, and the failure is
# invisible (the /pause brake dies with it). Each supervised thread stamps a
# heartbeat every iteration; /health reports 503 when any goes stale (so the
# compose healthcheck flips to unhealthy) and a supervisor Telegram-alerts —
# the MISSING heartbeat is the alarm, not silence.
_thread_heartbeats = {}
_thread_heartbeats_lock = threading.Lock()

# Max seconds a thread may go without a heartbeat before it's presumed dead.
# Each is comfortably above the thread's own loop period (2x+) so a single slow
# iteration never false-alarms.
THREAD_MAX_STALENESS = {
    "schedule_checker": 120,    # 30s tick
    "poll_sec_edgar": 600,      # ~30s tick + feed-fetch/retry time
    "lag_monitor": 2400,        # 900s tick
}


def mark_alive(name):
    """Called at the top of each supervised thread's loop iteration."""
    with _thread_heartbeats_lock:
        _thread_heartbeats[name] = time.time()


def seed_thread_heartbeats():
    """Seed at startup so threads get their full grace window before the
    supervisor / healthcheck can flag them (avoids a cold-start false alarm)."""
    now = time.time()
    with _thread_heartbeats_lock:
        for name in THREAD_MAX_STALENESS:
            _thread_heartbeats[name] = now


def stale_threads():
    """Return [(name, age_seconds_or_None)] for every thread past its max age."""
    now = time.time()
    stale = []
    with _thread_heartbeats_lock:
        for name, max_age in THREAD_MAX_STALENESS.items():
            last = _thread_heartbeats.get(name)
            age = None if last is None else round(now - last, 1)
            if last is None or (now - last) > max_age:
                stale.append((name, age))
    return stale


def _liveness_supervisor():
    """Alert (deduped, with recovery re-arm) when a supervised thread stalls."""
    seed_thread_heartbeats()
    alerted = set()
    while True:
        time.sleep(60)
        stale = stale_threads()
        stale_names = {n for n, _ in stale}
        for name, age in stale:
            if name in alerted:
                continue
            alerted.add(name)
            shown = f"{age}s" if age is not None else "never"
            try:
                bot.send_message(
                    CHAT_ID,
                    f"🚨 DEAD-MAN'S SWITCH: heartbeat thread '{name}' last reported "
                    f"{shown} ago (max {THREAD_MAX_STALENESS[name]}s). A daemon "
                    f"thread has likely died while the process stays up — /pause, "
                    f"scheduled reports, or the SEC radar may be silently down. "
                    f"Redeploy / investigate.",
                )
            except Exception as e:
                print(f"[ERROR] [HEARTBEAT] dead-man alert send failed: {e}", flush=True)
        # Re-arm alerts for any thread that has recovered.
        alerted &= stale_names


def startup_import_self_check():
    """RULE-D3 / audit §5 C1: run_eod_report() lazily imports its modules inside
    a swallowing `except`, so a Dockerfile COPY gap surfaced only as a silent
    daily ModuleNotFoundError. Verify at startup that every module the image is
    supposed to ship is importable, and alarm loudly (log + Telegram) if not.
    Uses find_spec so a present module is not executed for its side effects."""
    import importlib.util
    required = [
        "retry_utils", "trading_day_utils", "polygon_iv_backfill",
        "signal_outcome_resolver", "alpha_decay_monitor",
        "fundamentals_risk_monitor", "fundamentals_bullish_monitor",
        "earnings_revision_monitor", "celh_signal_tracker",
        "prediction_outcome_resolver",
        "prediction_quality_scorer", "validation_readiness_check",
        # barbell and job_supervisor are imported at module top (not lazily), so
        # a COPY gap would already hard-crash monitor on startup — listed here
        # for completeness so the alert names them explicitly rather than a bare
        # traceback.
        "barbell", "job_supervisor",
    ]
    missing = [m for m in required if importlib.util.find_spec(m) is None]
    if missing:
        msg = ("🚨 STARTUP SELF-CHECK: modules missing from the deployed image "
               "(Dockerfile COPY gap?): " + ", ".join(missing) + ". Features that "
               "import these (EOD report, alpha-decay, fundamentals, resolvers) "
               "are DEAD until the image is fixed.")
        print(f"[CRITICAL] [HEARTBEAT] {msg}", flush=True)
        try:
            bot.send_message(CHAT_ID, msg)
        except Exception as e:
            print(f"[ERROR] [HEARTBEAT] self-check alert send failed: {e}", flush=True)
    else:
        print("[INFO] [HEARTBEAT] startup import self-check OK — all "
              f"{len(required)} required modules present.", flush=True)


# --- 6. CORE INITIALIZATION ENGINE ---
if __name__ == "__main__":
    print("Nox SEC Forensic Scout Online...")
    init_db()

    # Audit §5 C1 (D3): fail loudly at startup on a packaging gap instead of
    # swallowing a daily ModuleNotFoundError at 16:05 ET.
    startup_import_self_check()

    # Seed dead-man's-switch heartbeats before the threads start so none is
    # flagged stale during its first (cold-start) interval.
    seed_thread_heartbeats()

    # Run backgrounds threads asynchronously above the blocking polling call
    threading.Thread(target=schedule_checker, daemon=True).start()
    threading.Thread(target=poll_sec_edgar, daemon=True).start()
    threading.Thread(target=_start_iv_http_server, daemon=True).start()
    threading.Thread(target=_lag_monitor_loop, daemon=True).start()  # WS7
    threading.Thread(target=_run_sports_bot_polling, daemon=True).start()
    # Dead-man's-switch supervisor (audit §5 C3): alerts when any of the above
    # stops heartbeating. Started last so the others have registered.
    threading.Thread(target=_liveness_supervisor, daemon=True).start()

    bot.infinity_polling()
