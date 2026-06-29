# Daily Report Setup Guide

## Overview

The daily report provides an end-of-day summary of your trading bot's activity, including:
- Account equity and drawdown metrics
- Signal flow (BUY/SELL/HOLD counts)
- Open positions summary
- Portfolio Greeks (delta, gamma, theta, vega)
- Unrealized P&L
- Circuit breaker status

The report is automatically sent to your Telegram channel at market close.

## Quick Start

### Option 1: Manual Trigger (Testing)

Run the report manually to test:

```bash
cd /root/Nox
python3 scripts/daily_report.py --engine-url http://localhost:8080
```

Or if your engine is on a remote server:

```bash
python3 scripts/daily_report.py --engine-url http://your-engine-ip:8080
```

### Option 2: Automatic Daily (Recommended)

Setup automated daily reports at market close (4:00 PM ET):

```bash
cd /root/Nox
bash scripts/setup_daily_report_cron.sh
```

This will:
- Create a wrapper script
- Install a cron job to run Monday-Friday at 4:00 PM ET
- Log output to `/var/log/nox_daily_report.log` (or `/tmp/nox_daily_report.log`)

**Note:** You need `crontab` access. If running in Docker, you'll need to run this from the host system or adjust the cron setup.

### Option 3: Docker Container (Cloud Deployments)

If running in Docker Compose, add a sidecar service:

```yaml
services:
  nox-engine:
    # ... existing config ...

  nox-reporter:
    image: python:3.11-slim
    container_name: nox-reporter
    volumes:
      - ./scripts/daily_report.py:/app/daily_report.py
      - ./scripts/cron_wrapper.sh:/app/cron_wrapper.sh
    entrypoint: |
      sh -c 'apt-get update && apt-get install -y curl &&
             echo "0 16 * * * /app/cron_wrapper.sh" | crontab - &&
             cron -f'
    environment:
      - ENGINE_URL=http://nox-engine:8080
    depends_on:
      - nox-engine
```

## Report Details

### Sample Output (Telegram)

```
📊 END-OF-DAY TRADING REPORT
════════════════════════════════════════

ACCOUNT SUMMARY
• Equity: $25,000.00
• Peak Equity: $25,500.00
• Drawdown: 1.96%
• Circuit Breaker: 🟢 NORMAL

SIGNAL FLOW
• BUY signals: 3
• SELL signals: 2
• HOLD signals: 5
• Total processed: 10

POSITIONS SUMMARY
• Open options: 2
• Total notional: $3,500.00
• Unrealized P&L: $285.50

PORTFOLIO GREEKS
• Delta: 0.450
• Gamma: 0.0025
• Theta: 5.75 per day
• Vega: 12.50

OPEN POSITIONS
• TSLA call $185.00 exp:2026-07-17 qty:1
• SPY put $450.00 exp:2026-07-10 qty:2
```

## API Endpoint

### GET `/daily-report`

Manually trigger the report via HTTP:

```bash
curl http://localhost:8080/daily-report | jq .
```

**Response:**
```json
{
  "status": "report_generated",
  "equity": 25000.00,
  "drawdown_pct": 1.96,
  "open_positions": 2,
  "signals_today": {
    "buy": 3,
    "sell": 2,
    "hold": 5
  },
  "greeks": {
    "delta": 0.450,
    "gamma": 0.0025,
    "theta": 5.75,
    "vega": 12.50
  },
  "unrealized_pnl": 285.50
}
```

## Customization

### Change Report Time

Edit your crontab:

```bash
crontab -e
```

Find the line with `run_daily_report.sh` and change the time. Examples:

- `0 15 * * 1-5` = 3:00 PM ET
- `0 17 * * 1-5` = 5:00 PM ET
- `30 16 * * 1-5` = 4:30 PM ET

### Change Report Frequency

To run multiple times per day:

```bash
# Every 2 hours during market hours (9:30 AM - 4:00 PM ET)
30 9,11,13,15 * * 1-5 /path/to/run_daily_report.sh >> /var/log/nox_daily_report.log 2>&1
```

### Disable Telegram Notifications

If you only want the HTTP report without Telegram alerts, modify the endpoint to skip the Telegram send:

(Currently, reports always send to Telegram — to disable, comment out the `TelegramNotifier::sendMessage()` line in main.cpp and recompile)

## Troubleshooting

### Report not triggering

1. Check cron is running:
   ```bash
   sudo service cron status
   ```

2. Verify cron job is installed:
   ```bash
   crontab -l | grep run_daily_report
   ```

3. Check log file:
   ```bash
   tail -f /var/log/nox_daily_report.log
   ```

### Connection refused

If you get "Connection error: Connection refused", the engine API is not reachable:

1. Verify engine is running:
   ```bash
   curl http://localhost:8080/health
   ```

2. If using remote engine, verify URL:
   ```bash
   python3 scripts/daily_report.py --remote-url http://your-ip:8080
   ```

3. Check firewall rules allow port 8080

### Telegram not sending

1. Verify `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` are set
2. Test manually:
   ```bash
   curl -X POST http://localhost:8080/webhook -H "Content-Type: application/json" \
     -d '{"action":"REPORT","ticker":"GLOBAL_AUDIT","secret_key":"YOUR_SECRET"}'
   ```

## Next Steps

- Set report time to your preferred market close time
- Test manually before relying on cron
- Monitor the log file first few days to ensure reliability
- Adjust Greeks calculations if you have live price data available
