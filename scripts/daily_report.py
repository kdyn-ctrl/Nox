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
from datetime import datetime, date

# US market holidays (major ones that don't move)
US_HOLIDAYS = {
    (1, 1),    # New Year's Day
    (7, 4),    # Independence Day
    (12, 25),  # Christmas
    # Add moveable holidays as needed
}

def is_trading_day():
    """Check if today is a US trading day (weekday, not holiday)."""
    today = date.today()

    # Check if weekend
    if today.weekday() >= 5:  # Saturday=5, Sunday=6
        return False

    # Check if holiday
    if (today.month, today.day) in US_HOLIDAYS:
        return False

    # Check for Good Friday (Easter-based) — approximate for next few years
    # Format: (year, month, day)
    good_fridays = {
        (2024, 3, 29), (2025, 4, 18), (2026, 4, 10), (2027, 3, 26),
        (2028, 4, 14), (2029, 3, 30), (2030, 4, 19)
    }
    if (today.year, today.month, today.day) in good_fridays:
        return False

    return True

def fetch_options_accuracy(engine_url):
    """Fetch options signal accuracy report for the day."""
    try:
        response = requests.get(
            f"{engine_url}/daily-options-accuracy",
            timeout=30
        )

        if response.status_code == 200:
            return response.json()
        else:
            print(f"⚠️ Options accuracy report failed: HTTP {response.status_code}")
            return None
    except requests.exceptions.RequestException as e:
        print(f"⚠️ Could not fetch options accuracy: {e}")
        return None


def trigger_daily_report(engine_url):
    """Fetch and display daily report from the engine."""
    if not is_trading_day():
        print(f"[{datetime.now().isoformat()}] Today is not a US trading day. Skipping report.")
        return 0

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
        else:
            print(f"❌ Report generation failed: HTTP {response.status_code}")
            print(f"Response: {response.text}")
            return 1

        # Fetch personal options signal accuracy
        print(f"[{datetime.now().isoformat()}] Fetching options signal accuracy...")
        accuracy_data = fetch_options_accuracy(engine_url)

        if accuracy_data:
            summary = accuracy_data.get('summary', {})
            total = summary.get('total_trades', 0)

            if total > 0:
                print("\n📊 Personal Options Signal Accuracy\n")
                print(f"Date: {accuracy_data.get('date')}")
                print(f"Total Trades: {total}")
                print(f"  ✅ Wins: {summary.get('wins', 0)}")
                print(f"  ❌ Losses: {summary.get('losses', 0)}")
                print(f"  ➡️  Breakeven: {summary.get('breakeven', 0)}")
                print(f"\nWin Rate: {summary.get('win_rate_pct', 0):.1f}%")
                print(f"Avg Win: ${summary.get('avg_win_usd', 0):.2f}")
                print(f"Avg Loss: ${summary.get('avg_loss_usd', 0):.2f}")
                print(f"Profit Factor: {summary.get('profit_factor', 0):.2f}x")
                print(f"Cumulative P&L: ${summary.get('cumulative_pnl_usd', 0):.2f}")

                # Show individual trades
                trades = accuracy_data.get('trades', [])
                if trades:
                    print(f"\nTrades ({len(trades)}):")
                    for t in trades:
                        pnl = t.get('pnl', 0)
                        pnl_str = f"+${pnl:.2f}" if pnl >= 0 else f"-${abs(pnl):.2f}"
                        action = t.get('action', 'UNKNOWN')

                        if action == "OPEN":
                            strategy = t.get('option_type', 'unknown')
                            print(f"  {t.get('timestamp')} | {action:6s} | {t.get('ticker'):6s} | {strategy:20s} @ ${t.get('price'):.2f}")
                        else:  # CLOSE
                            option_type = t.get('option_type', 'unknown')
                            strike = t.get('strike')
                            exit_reason = t.get('exit_reason', 'manual close')
                            strike_str = f"${strike:.2f}" if strike else "?"
                            print(f"  {t.get('timestamp')} | {action:6s} | {t.get('ticker'):6s} {option_type:4s} {strike_str:>8s} | P&L: {pnl_str:>10s} | {exit_reason}")
            else:
                print("\n📊 Personal Options Signal Accuracy")
                print("No trades recorded today.")

        return 0

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
