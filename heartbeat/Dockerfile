FROM python:3.11-slim
WORKDIR /app

# Pin every dependency so builds are fully reproducible (RULE-009 / scan #17).
# schedule 1.2.2  — minimum 1.2.0 required: that release introduced the
#                   tz-argument form .at("HH:MM", tz) used in schedule_checker.
#                   Without this pin an older version could be resolved and the
#                   whole scheduler thread would crash silently at runtime.
#
# akshare is intentionally NOT listed here. All Chinese market data (East Money,
# Cailian Press, NBS PMI, PBOC LPR) is now fetched by the dedicated china-data-engine
# microservice and read by this container via authenticated HTTP calls to
# http://china-data-engine:8000. Removing akshare shrinks this image by ~400 MB and
# eliminates any risk of a slow AkShare network call blocking the Telegram bot.
RUN pip install --no-cache-dir \
    requests==2.32.3 \
    pyTelegramBotAPI==4.34.0 \
    anthropic==0.109.2 \
    schedule==1.2.2 \
    beautifulsoup4==4.12.3 \
    lxml==5.3.0 \
    pandas==2.2.3 \
    pytrends==4.9.2 \
    ib_async==2.1.0

# Create the data directory that DB_PATH ('/app/data/memory_bank.db') depends on.
# Without this sqlite3.connect() raises OperationalError at runtime.
# Declared as a VOLUME so the DB survives container restarts when mounted.
RUN mkdir -p /app/data
VOLUME ["/app/data"]

# NOTE (audit §5 C1): run_eod_report() imports the five EOD modules below inside
# a swallowing `except Exception`. They were absent from this COPY list, so every
# 16:05 ET report raised ModuleNotFoundError silently since 2026-07-09 (alpha-decay
# defense dead in prod while its unit tests passed). All five verified to depend
# only on stdlib + signal_outcome_resolver (already copied). Keep this list in sync
# with monitor.py's run_eod_report imports.
COPY monitor.py retry_utils.py trading_day_utils.py polygon_iv_backfill.py \
     signal_outcome_resolver.py \
     alpha_decay_monitor.py fundamentals_risk_monitor.py fundamentals_bullish_monitor.py \
     earnings_revision_monitor.py \
     prediction_outcome_resolver.py \
     prediction_quality_scorer.py validation_readiness_check.py \
     barbell.py celh_signal_tracker.py \
     telemetry_watchdog.py ibkr_fills_importer.py ibkr_quotes.py \
     squeeze_pead_scanner.py \
     job_supervisor.py .

CMD ["python", "-u", "monitor.py"]
