#!/usr/bin/env python3
"""
Trigger end-of-day report from Nox trading engine.
Run this at market close (4:00 PM ET) via cron or task scheduler.

Usage:
    python3 daily_report.py --engine-url http://localhost:8080

Or configure in crontab:
    0 16 * * 1-5 cd /root/Nox && python3 scripts/daily_report.py >> /tmp/nox_report.log 2>&1
"""

import requests
import sys
import argparse
from datetime import datetime

def trigger_daily_report(engine_url):
    """Fetch and display daily report from the engine."""
    try:
        print(f"[{datetime.now().isoformat()}] Fetching daily report from {engine_url}...")

        response = requests.get(
            f"{engine_url}/daily-report",
            timeout=30
        )

        if response.status_code == 200:
            data = response.json()
            print("\n✅ Daily Report Generated Successfully\n")
            print(f"Equity: ${data['equity']:.2f}")
            print(f"Drawdown: {data['drawdown_pct']:.2f}%")
            print(f"Open Positions: {data['open_positions']}")
            print(f"Today's Signals - Buy: {data['signals_today']['buy']}, "
                  f"Sell: {data['signals_today']['sell']}, "
                  f"Hold: {data['signals_today']['hold']}")
            if data['open_positions'] > 0:
                print(f"\nPortfolio Greeks:")
                print(f"  Delta: {data['greeks']['delta']:.3f}")
                print(f"  Gamma: {data['greeks']['gamma']:.4f}")
                print(f"  Theta: {data['greeks']['theta']:.2f}")
                print(f"  Vega: {data['greeks']['vega']:.2f}")
            print(f"\nUnrealized P&L: ${data['unrealized_pnl']:.2f}")
            print("\n📱 Full report sent to Telegram\n")
            return 0
        else:
            print(f"❌ Report generation failed: HTTP {response.status_code}")
            print(f"Response: {response.text}")
            return 1

    except requests.exceptions.RequestException as e:
        print(f"❌ Connection error: {e}")
        return 1
    except Exception as e:
        print(f"❌ Unexpected error: {e}")
        return 1

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Trigger Nox daily trading report at market close"
    )
    parser.add_argument(
        "--engine-url",
        default="http://localhost:8080",
        help="Engine API base URL (default: http://localhost:8080)"
    )
    parser.add_argument(
        "--remote-url",
        help="Remote engine URL (overrides local, useful for cloud deployments)"
    )

    args = parser.parse_args()
    engine_url = args.remote_url or args.engine_url

    sys.exit(trigger_daily_report(engine_url))
