#!/usr/bin/env python3
"""
Nox Sanitation & System Audit Script
Runs an end-to-end sanity and health check across:
1. Database Integrity (memory_bank.db signal tables, order ledger, kill switch)
2. Data Provider Consensus (Finnhub vs yfinance cross-validation for watchlist earnings)
3. Scanner Dry-Runs (PEAD, Lead-Lag, Pre-Earnings IV, IV Crush, FTD Squeeze)
4. Docker Container Health & Log Traceback Inspection
"""
import os
import sys
import json
import sqlite3
import subprocess
import logging
from datetime import datetime, timezone

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("sanitation_check")

# Add heartbeat to sys.path
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEARTBEAT_DIR = os.path.join(BASE_DIR, "heartbeat")
sys.path.insert(0, HEARTBEAT_DIR)

DB_PATH = os.path.join(BASE_DIR, "data", "memory_bank.db")


def section(title: str):
    print("\n" + "═" * 70)
    print(f"  {title}")
    print("═" * 70)


def audit_database():
    section("1. DATABASE INTEGRITY AUDIT")
    if not os.path.exists(DB_PATH):
        print(f"❌ DB File missing at {DB_PATH}")
        return False

    conn = sqlite3.connect(DB_PATH)
    cursor = conn.cursor()

    # Check Kill Switch
    cursor.execute("SELECT paused, reason, triggered_at FROM kill_switch_state WHERE id = 1;")
    ks = cursor.fetchone()
    if ks:
        status_str = "🔴 PAUSED (Active Kill Switch!)" if ks[0] == 1 else "🟢 ACTIVE (Normal Operation)"
        print(f"Kill Switch Status: {status_str}")
        if ks[0] == 1:
            print(f"   Reason: {ks[1]} (Triggered at: {ks[2]})")
    else:
        print("🟢 Kill Switch Table: Default OK (No pause row)")

    # Check Signal Tables
    tables = [
        ("options_signals", "scan_at"),
        ("signal_events", "scan_at"),
        ("equity_signals", "scan_at"),
        ("personal_signals", "created_at"),
        ("order_ledger", "sent_at"),
    ]

    for table, time_col in tables:
        try:
            if time_col == "scan_at":
                cursor.execute(f"SELECT count(*), max(datetime({time_col}, 'unixepoch')) FROM {table};")
            else:
                cursor.execute(f"SELECT count(*), max({time_col}) FROM {table};")
            row = cursor.fetchone()
            count, latest = row if row else (0, "None")
            print(f"Table '{table}': {count} total rows | Latest: {latest or 'None'}")
        except Exception as e:
            print(f"⚠️ Could not query {table}: {e}")

    conn.close()
    return True


def audit_data_provider_consensus():
    section("2. DATA PROVIDER CONSENSUS & EARNINGS CROSS-CHECK")
    try:
        import yfinance as yf
        import requests
    except ImportError:
        print("⚠️ yfinance/requests missing; skipping data cross-check.")
        return False

    # Watchlist check
    test_tickers = ["AMZN", "AAPL", "NVDA", "AMD", "PLTR"]
    print(f"Cross-checking Finnhub vs yfinance consensus earnings data for: {test_tickers}...")

    # Load API key from env
    key = os.getenv("FINNHUB_API_KEY", "")
    if not key and os.path.exists(os.path.join(BASE_DIR, ".env")):
        with open(os.path.join(BASE_DIR, ".env")) as f:
            for line in f:
                if line.startswith("FINNHUB_API_KEY="):
                    key = line.strip().split("=", 1)[1].strip("\"' ")
                    break

    for ticker in test_tickers:
        try:
            # yfinance check
            t = yf.Ticker(ticker)
            ed = t.get_earnings_dates(limit=4)
            yf_latest = None
            if ed is not None and not ed.empty:
                for idx, row in ed.iterrows():
                    act = row.get("Reported EPS")
                    est = row.get("EPS Estimate")
                    surp = row.get("Surprise(%)")
                    if act is not None and est is not None and not (idx.date() > datetime.now(timezone.utc).date()):
                        yf_latest = {
                            "date": idx.strftime("%Y-%m-%d"),
                            "actual": float(act),
                            "estimate": float(est),
                            "surprise_pct": float(surp * 100.0 if surp and abs(surp) < 5 else (surp or 0.0))
                        }
                        break

            # Finnhub check
            fh_latest = None
            if key:
                res = requests.get(f"https://finnhub.io/api/v1/stock/earnings?symbol={ticker}&token={key}", timeout=10)
                if res.status_code == 200:
                    arr = res.json()
                    if arr:
                        item = arr[0]
                        fh_latest = {
                            "period": item.get("period"),
                            "actual": item.get("actual"),
                            "estimate": item.get("estimate"),
                            "surprise_pct": item.get("surprisePercent")
                        }

            print(f"\n• [{ticker}]")
            if yf_latest:
                print(f"  yfinance: Date={yf_latest['date']} Act=${yf_latest['actual']} Est=${yf_latest['estimate']} Surprise={yf_latest['surprise_pct']:+.1f}%")
            else:
                print("  yfinance: No reported earnings found")

            if fh_latest:
                print(f"  Finnhub : Period={fh_latest['period']} Act=${fh_latest['actual']} Est=${fh_latest['estimate']} Surprise={fh_latest['surprise_pct']:+.1f}%")
            else:
                print("  Finnhub : No reported earnings found")

            if yf_latest and fh_latest:
                diff = abs(yf_latest['surprise_pct'] - fh_latest['surprise_pct'])
                if diff > 10.0:
                    print(f"  🚨 DISCREPANCY DETECTED ({diff:.1f}% diff) — Nox cross-validation will override Finnhub with yfinance consensus!")
                else:
                    print("  ✅ Providers in agreement")

        except Exception as e:
            print(f"  ⚠️ Error auditing {ticker}: {e}")

    return True


def audit_scanner_dry_runs():
    section("3. LIVE SCANNER DRY-RUN AUDIT")
    scanners = [
        ("Timezone Lead-Lag", "timezone_lead_lag_scanner.py", ["--dry-run"]),
        ("PEAD Screener", "squeeze_pead_scanner.py", ["--dry-run"]),
        ("Pre-Earnings IV Scanner", "pre_earnings_iv_scanner.py", []),
        ("IV Crush Scanner", "post_earnings_iv_crush_scanner.py", []),
    ]

    for name, script, args in scanners:
        script_path = os.path.join(HEARTBEAT_DIR, script)
        if not os.path.exists(script_path):
            print(f"⚠️ {name}: Script missing at {script_path}")
            continue

        cmd = [sys.executable, script_path] + args
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=25, cwd=BASE_DIR)
            if res.returncode == 0:
                print(f"🟢 {name}: PASS (Exit code 0)")
                # Print last 2 lines of output
                lines = [l for l in res.stdout.strip().split("\n") if l]
                if lines:
                    print(f"   Summary: {lines[-1]}")
            else:
                print(f"❌ {name}: FAIL (Exit code {res.returncode})")
                print(f"   Stderr: {res.stderr.strip()[:200]}")
        except subprocess.TimeoutExpired:
            print(f"⚠️ {name}: TIMEOUT (>25s)")
        except Exception as e:
            print(f"❌ {name}: ERROR ({e})")

    return True


def audit_docker_containers():
    section("4. DOCKER CONTAINER HEALTH & ERROR AUDIT")
    try:
        res = subprocess.run(["docker", "ps", "--format", "{{.Names}}|{{.Status}}"], capture_output=True, text=True)
        if res.returncode != 0:
            print("⚠️ Docker command unavailable.")
            return False

        containers = [l.strip() for l in res.stdout.strip().split("\n") if l]
        print(f"Running Nox Containers ({len(containers)} total):")
        for c in containers:
            print(f"  • {c}")

        print("\nChecking recent logs for errors in 'nox_heartbeat' and 'nox_execution'...")
        for cname in ["nox_heartbeat", "nox_execution"]:
            lres = subprocess.run(["docker", "logs", "--tail", "50", cname], capture_output=True, text=True)
            err_count = lres.stdout.count("[ERROR]") + lres.stderr.count("[ERROR]")
            tb_count = lres.stdout.count("Traceback") + lres.stderr.count("Traceback")
            if err_count == 0 and tb_count == 0:
                print(f"  🟢 {cname}: Clean (0 errors / 0 tracebacks in last 50 log lines)")
            else:
                print(f"  ⚠️ {cname}: {err_count} [ERROR] lines, {tb_count} Tracebacks found.")
    except Exception as e:
        print(f"⚠️ Docker audit failed: {e}")

    return True


def audit_monthly_signal_performance():
    section("5. MONTHLY SIGNAL PERFORMANCE & ACCURACY AUDIT")
    try:
        from monthly_signal_performance_analyzer import analyze_monthly_signals
        stats = analyze_monthly_signals(window_days=30)
        print(f"Total Signals in 30-Day Window: {stats['total_signals']}")
        if stats['total_signals'] > 0:
            print(f"  • Strategy Breakdown: {stats.get('strategy_counts', {})}")
            print(f"  • T+1 Hit Rate: {stats.get('hit_rate_t1', 0.0):.1f}% | T+5 Hit Rate: {stats.get('hit_rate_t5', 0.0):.1f}%")
            print(f"  • Profit Factor: {stats.get('profit_factor', 0.0):.2f}")
        else:
            print("  ℹ️ 0 signals recorded in 30-day evaluation window (strict filter protection active).")
    except Exception as e:
        print(f"⚠️ Monthly Signal Performance audit failed: {e}")
    return True


def main():
    print("NOX SYSTEM SANITATION & AUDIT REPORT")
    print(f"Timestamp: {datetime.now(timezone.utc).isoformat()} UTC")

    audit_database()
    audit_data_provider_consensus()
    audit_scanner_dry_runs()
    audit_docker_containers()
    audit_monthly_signal_performance()

    section("SANITATION AUDIT COMPLETE")


if __name__ == "__main__":
    main()
