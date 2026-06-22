# Test Guide for Analyst Module

The Analyst module is responsible for fetching market data (VIX, SPY), computing technical indicators, classifying market regimes, and transmitting signals to the execution engine.

## Components Tested

- **Data Fetching**: VIX and SPY price retrieval from Yahoo Finance
- **Regime Classification**: RegimeStateMachine evaluation
- **Signal Serialization**: JSON payload generation
- **Transmission**: HTTP POST to execution webhook

## Manual Testing

### 1. Standalone Regime Classification Test
```bash
g++ -std=c++17 -o build/test_regime tests/test_regime.cpp
./build/test_regime
```
**Purpose**: Verifies regime classification logic independent of data fetching.

**Expected Output**:
```
✓ RISK_ON: VIX < 35, SPY > 200-SMA
✓ RISK_OFF: VIX >= 35 or SPY < 200-SMA*0.98
✓ TRANSITION: SPY between thresholds
```

### 2. Live Data Fetch Test (Manual)
```bash
cd /root/Nox
export TELEGRAM_BOT_TOKEN="your_token"
export TELEGRAM_CHAT_ID="your_chat_id"

# Compile the analyst
g++ -std=c++17 -o build/analyst analyst/main.cpp -L. -lcurl -lssl -lcrypto

# Run one iteration
./build/analyst
```

**What to verify**:
1. Check logs for `[INFO] Fetching VIX from Yahoo Finance`
2. Verify SPY price is current (today's market close)
3. Confirm 200-day SMA is computed from 252+ data points
4. Look for `[INFO] Regime classification: RISK_ON|RISK_OFF|TRANSITION`
5. Confirm JSON payload is logged before transmission

**Expected signals** (when run during market hours):
- If VIX < 35 and SPY > 200-SMA → `RISK_ON`
- If VIX >= 35 or SPY < 200-SMA*0.98 → `RISK_OFF`

### 3. Network Connectivity Test
```bash
# Check if execution engine is reachable
curl -X GET http://execution-engine:8080/health

# If using Docker Compose:
docker-compose up -d
curl -X GET http://localhost:8080/health
```

**Expected**: HTTP 200 response from execution engine.

### 4. Payload Validation Test
```bash
# Extract the JSON payload from analyst logs
# Verify it contains:
{
  "regime": "RISK_ON|RISK_OFF|TRANSITION",
  "vix": <float>,
  "spy_price": <float>,
  "sma_200": <float>,
  "timestamp": "ISO 8601 string"
}
```

## Integration Tests

### Analyst → Execution Pipeline
```bash
# Terminal 1: Start execution engine
cd /root/Nox && docker-compose up execution-engine

# Terminal 2: Run analyst once
docker-compose run analyst

# Terminal 3: Check execution logs
docker logs nox_execution-engine

# Verify:
# 1. Analyst sent POST to /webhook
# 2. Execution received and logged signal
# 3. No [CRITICAL] errors in execution logs
```

### Telegram Notification Test
When analyst runs, check Telegram for regime classification message:
```
📊 [ANALYST] Regime Classification
Regime: RISK_ON
VIX: 18.5
SPY: 482.50
200-SMA: 475.20
Timestamp: 2026-06-22 14:30:00 UTC
```

If no message appears, verify:
- `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` are set
- Network connection is available
- Token is valid (test with `curl` to Telegram API)

## Environment Variables Required

```bash
TELEGRAM_BOT_TOKEN       # Telegram bot token (optional, disables if missing)
TELEGRAM_CHAT_ID         # Telegram chat ID (optional, disables if missing)
```

## When to Update This Guide

Update this guide when:
1. **Data fetch sources change** — if moving from Yahoo Finance to another provider
2. **Regime thresholds change** — document new VIX/SMA boundaries
3. **Signal format changes** — update expected JSON structure
4. **New indicators are added** — document what's being computed
5. **Transmission protocol changes** — update curl examples and expected responses

## Common Failures & Diagnostics

| Symptom | Check | Fix |
|---------|-------|-----|
| `[CRITICAL] Failed to fetch VIX` | Network connection, Yahoo Finance rate limits | Retry after 5 minutes, check IP blocklist |
| JSON parse error in execution | Analyst payload format changed | Review recent analyst/main.cpp edits |
| Telegram notification missing | Bot token/chat ID | Verify env vars, test token with curl |
| Regime always RISK_OFF | SPY data is stale | Check Yahoo Finance endpoint returns current bar |
| 200-SMA calculation seems wrong | Less than 252 bars in response | Verify full year of SPY data is fetched |
