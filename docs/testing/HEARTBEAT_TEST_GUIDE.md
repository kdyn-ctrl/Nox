# Test Guide for Heartbeat Monitor

The Heartbeat Monitor is a Python service that continuously watches the trading system, generates intelligence reports, monitors SEC filings, and provides a conversational CLI interface via Claude AI.

## Components Tested

- **Health Monitoring**: Analyst and Execution container liveness
- **Intelligence Reports**: Market regime, portfolio status, trade summaries
- **SEC Radar**: Filing alerts for watchlist symbols
- **Claude Integration**: Conversational interface for system queries
- **Telegram Alerts**: Critical alerts and routine status updates
- **SQLite Database**: Trade history and cache management

## Building & Running

```bash
# Install dependencies
cd /root/Nox/heartbeat
python3 -m pip install -r requirements.txt

# Start heartbeat monitor
export TELEGRAM_BOT_TOKEN="your_token"
export TELEGRAM_CHAT_ID="your_chat_id"
export ANTHROPIC_API_KEY="your_api_key"
export ALPACA_API_KEY="your_alpaca_key"
export ALPACA_SECRET_KEY="your_alpaca_secret"
export WEBHOOK_SECRET_TOKEN="your_webhook_secret"

python3 monitor.py
```

## Unit Tests

### Configuration Validation Test
```bash
# Test with missing required env var
unset TELEGRAM_BOT_TOKEN
python3 monitor.py

# Expected output:
# [FATAL] [HEARTBEAT] Required env var 'TELEGRAM_BOT_TOKEN' is not set. Refusing to start.
# Exit code: 1

# Test with all required vars
export TELEGRAM_BOT_TOKEN="token"
export TELEGRAM_CHAT_ID="chat_id"
export ANTHROPIC_API_KEY="key"
export ALPACA_API_KEY="api_key"
export ALPACA_SECRET_KEY="secret"
export WEBHOOK_SECRET_TOKEN="webhook_secret"

python3 monitor.py

# Expected: Service starts successfully
# [INFO] [HEARTBEAT] All required environment variables validated.
```

## Integration Tests

### Health Monitoring Test
```bash
# Terminal 1: Start Heartbeat Monitor
cd /root/Nox
export WEBHOOK_SECRET_TOKEN="test_secret"
python3 heartbeat/monitor.py &
HEARTBEAT_PID=$!

# Terminal 2: Check logs
sleep 3
tail -50 logs/heartbeat.log

# Expected logs:
# [INFO] [HEARTBEAT] Health check: analyst-brain OK
# [INFO] [HEARTBEAT] Health check: execution-engine OK
# [INFO] [HEARTBEAT] System status: HEALTHY

# Terminal 3: Simulate analyst container crash
docker-compose stop analyst-brain

# Check heartbeat logs:
# [WARN] [HEARTBEAT] Health check: analyst-brain FAILED
# [ALERT] System health degraded to UNHEALTHY
# Telegram alert should be sent

kill $HEARTBEAT_PID
```

### Intelligence Report Generation Test
```bash
# Manually trigger report generation
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import generate_market_intelligence

# Test report generation
report = generate_market_intelligence()

assert "regime" in report.lower(), "Report should mention regime"
assert "portfolio" in report.lower(), "Report should mention portfolio"
assert "trades" in report.lower(), "Report should mention trades"

print("✓ Market intelligence report generated successfully")
print(f"Report length: {len(report)} characters")
EOF
```

### Trade History Tracking Test
```bash
# Start heartbeat monitor
python3 heartbeat/monitor.py &
HEARTBEAT_PID=$!

# Simulate a trade webhook (execute this in another terminal)
curl -X POST http://localhost:8080/trade_notification \
  -H "Content-Type: application/json" \
  -d '{
    "order_id": "test-123",
    "symbol": "SPY",
    "quantity": 50,
    "entry_price": 480.00,
    "timestamp": "2026-06-22T14:30:00Z"
  }'

# Check SQLite database
python3 << 'EOF'
import sqlite3
conn = sqlite3.connect('heartbeat/trades.db')
cursor = conn.cursor()
cursor.execute('SELECT * FROM trades ORDER BY timestamp DESC LIMIT 5')
for row in cursor.fetchall():
    print(row)
conn.close()
EOF

# Verify trade record was created

kill $HEARTBEAT_PID
```

### Claude Integration Test
```bash
# Test Claude conversation interface
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import ClaudeInterface

# Initialize Claude interface
interface = ClaudeInterface()

# Test queries
queries = [
    "What's the current market regime?",
    "How many trades were executed this week?",
    "Is SPY in a buy signal?",
    "What's the portfolio value?"
]

for query in queries:
    response = interface.query(query)
    assert len(response) > 0, f"Should return non-empty response for: {query}"
    print(f"Q: {query}")
    print(f"A: {response[:100]}...\n")

print("✓ Claude integration test passed")
EOF
```

### SEC Radar Test
```bash
# Test SEC filing alert for watchlist
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import SECRadar

radar = SECRadar(symbols=["AAPL", "MSFT", "GOOGL"])

# Check for recent filings
recent_filings = radar.check_filings()

for symbol, filings in recent_filings.items():
    print(f"{symbol}: {len(filings)} recent filings")
    for filing in filings[:2]:  # Show first 2
        print(f"  - {filing['type']}: {filing['date']}")

# Expected: Should return list of recent 8-K, 10-Q, 10-K filings
EOF
```

## Telegram Notification Tests

### Alert Delivery Test
```bash
# Monitor Telegram chat while sending test alerts

# Terminal 1: Start heartbeat
python3 heartbeat/monitor.py

# Terminal 2: Trigger different alert types
python3 << 'EOF'
import sys
import time
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import TelegramNotifier

notifier = TelegramNotifier()

# Test different alert types
alerts = [
    {"level": "INFO", "message": "🟢 System health check passed"},
    {"level": "WARN", "message": "🟡 High portfolio concentration detected"},
    {"level": "CRITICAL", "message": "🔴 RISK_OFF regime activated"},
]

for alert in alerts:
    notifier.send_alert(alert["level"], alert["message"])
    time.sleep(2)

EOF

# Terminal 3: Monitor Telegram chat
# Should receive 3 messages with different emoji/urgency levels
```

### High-Volume Alert Test
```bash
# Test alert chunking for long messages
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import TelegramNotifier

notifier = TelegramNotifier()

# Generate large message (> 4000 chars)
large_message = "Trade Report:\n" + ("SPY +0.5%\n" * 500)

# Should split into multiple Telegram messages
notifier.send_alert("INFO", large_message)

# Check Telegram: should receive 2-3 messages
EOF
```

## Database Tests

### Trade History Integrity
```bash
python3 << 'EOF'
import sqlite3
from datetime import datetime, timedelta

# Verify database schema
conn = sqlite3.connect('heartbeat/trades.db')
cursor = conn.cursor()

# Check trades table exists
cursor.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='trades'")
assert cursor.fetchone() is not None, "trades table should exist"

# Check required columns
cursor.execute("PRAGMA table_info(trades)")
columns = {row[1] for row in cursor.fetchall()}
required = {"order_id", "symbol", "quantity", "entry_price", "entry_time"}
assert required.issubset(columns), f"Missing columns: {required - columns}"

# Verify no duplicate trades
cursor.execute("SELECT order_id, COUNT(*) FROM trades GROUP BY order_id HAVING COUNT(*) > 1")
duplicates = cursor.fetchall()
assert len(duplicates) == 0, f"Found duplicate trades: {duplicates}"

# Verify time ordering
cursor.execute("SELECT entry_time FROM trades ORDER BY entry_time DESC")
times = [row[0] for row in cursor.fetchall()]
assert times == sorted(times, reverse=True), "Trade times should be chronologically ordered"

print("✓ Database integrity check passed")
conn.close()
EOF
```

### Cache Performance Test
```bash
# Verify caching reduces external API calls

python3 << 'EOF'
import sys
import time
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import MarketDataCache

cache = MarketDataCache()

# First call (cache miss)
start = time.time()
data1 = cache.get_market_data("SPY")
miss_time = time.time() - start

# Second call (cache hit)
start = time.time()
data2 = cache.get_market_data("SPY")
hit_time = time.time() - start

# Cache hit should be significantly faster
print(f"Cache miss time: {miss_time*1000:.2f}ms")
print(f"Cache hit time: {hit_time*1000:.2f}ms")
assert hit_time < miss_time / 2, "Cache hit should be at least 2x faster"

print("✓ Cache performance test passed")
EOF
```

## Scheduled Task Tests

### Report Generation Schedule
```bash
# Monitor scheduled tasks
# The heartbeat should generate reports at fixed intervals

tail -f logs/heartbeat.log | grep -E "REPORT|SCHEDULE"

# Expected output (every 5 minutes during market hours):
# [INFO] [HEARTBEAT] Generating market intelligence report...
# [INFO] [HEARTBEAT] Report generated: 1524 characters
# [INFO] [HEARTBEAT] Alert sent to Telegram
```

### SEC Radar Schedule
```bash
# Monitor SEC filing checks (runs daily)
tail -f logs/heartbeat.log | grep -E "SEC|FILING"

# Expected (once per day):
# [INFO] [HEARTBEAT] Checking SEC filings for watchlist...
# [INFO] [HEARTBEAT] Found 3 new filings
# [ALERT] New 8-K filing: AAPL (2026-06-22)
```

## Environment Variables Required

```bash
TELEGRAM_BOT_TOKEN              # Telegram bot token (REQUIRED)
TELEGRAM_CHAT_ID                # Telegram chat ID (REQUIRED)
ANTHROPIC_API_KEY               # Claude API key (REQUIRED)
ALPACA_API_KEY                  # Alpaca API key (REQUIRED)
ALPACA_SECRET_KEY               # Alpaca secret key (REQUIRED)
WEBHOOK_SECRET_TOKEN            # Shared webhook secret (REQUIRED)

# Optional heartbeat configuration
# NOX_DAILY_REPORT_TICKERS       # Comma-separated tickers for SEC context in the daily report
# MAX_DAILY_REPORT_SEC_TICKERS   # Maximum number of SEC tickers included in the daily report (default 8)
```

## When to Update This Guide

Update this guide when:
1. **New scheduled tasks are added** — document schedule and expected output
2. **Claude integration changes** — new system prompts or response formats
3. **Database schema changes** — new tables or columns
4. **Telegram formatting changes** — new alert types or emoji
5. **Health check endpoints change** — new services to monitor
6. **SEC filing sources change** — new data sources or check frequency
7. **CLI commands change** — new conversational commands available

## Common Failures & Diagnostics

| Symptom | Check | Fix |
|---------|-------|-----|
| `[FATAL] Required env var not set` | All 6 required vars present | Set all env vars before starting |
| Health check shows container DOWN | Container running? | `docker-compose ps` and check logs |
| No Telegram alerts received | Bot token/chat ID valid | Test token with curl to Telegram API |
| Claude queries timeout | API key valid, network | Check ANTHROPIC_API_KEY, rate limits |
| No SEC filing alerts | Watchlist symbols correct | Verify AAPL/MSFT format in config |
| Database locked error | Another process has it open | Check for multiple heartbeat instances |
| Old cached data returned | Cache TTL too long | Check cache expiry settings |

## Testing Checklist Before Deployment

- [ ] All required environment variables are set
- [ ] Health monitoring correctly detects container failures
- [ ] Market intelligence reports generate without errors
- [ ] Trade history records are created and queryable
- [ ] Claude integration responds to sample queries
- [ ] SEC filing alerts fire for watchlist symbols
- [ ] Telegram alerts deliver with correct formatting
- [ ] Database has no corrupted records
- [ ] Scheduled tasks run at expected intervals
- [ ] Large messages are correctly split for Telegram
