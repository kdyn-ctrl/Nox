# Test Guide for Execution Module

The Execution module receives signals from the Analyst, validates them through multiple gates (schema, auth, momentum filter, regime), sizes positions using Kelly Criterion, and routes orders to Alpaca.

## Components Tested

- **Schema Gate**: JSON validation
- **Auth Gate**: Secret token verification
- **Momentum Filter**: RSI overbought/oversold checks
- **Regime Gate**: Market regime re-evaluation
- **Portfolio Fetch**: Live equity verification
- **Kelly Sizing**: Position size calculation
- **Order Routing**: Alpaca API integration

## Unit Tests

### Kelly Criterion Sizing
```bash
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
./build/test_kelly_sizing
```

**Tests**:
- Prevents trades when portfolio can't afford minimum (1 share)
- Scales position size based on win rate and win/loss ratio
- Respects 10% max risk cap
- Respects 1% minimum allocation

**Expected Output**:
```
✓ Kelly sizing: $100k portfolio, 1% min, 10% max
  - Kelly calc: 15% of portfolio
  - Cap to max: 10% → 1,000 shares @ $100 = $100,000
✓ Kelly sizing prevents 0-share trades
✓ Kelly sizing handles edge cases (no edge = 0 allocation)
```

## Manual Integration Tests

### 1. Schema Validation Test
```bash
# Test with valid payload
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# Expected: HTTP 200, order processed
# Check logs: [INFO] Schema validation passed
```

### 2. Auth Gate Test
```bash
# Test with invalid secret
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "secret_key": "WRONG_SECRET"
  }'

# Expected: HTTP 401 (silently dropped, no fingerprinting)
# Check logs: [WARN] Authentication failed
```

### 3. Schema Rejection Test
```bash
# Test with malformed JSON
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{invalid json}'

# Expected: HTTP 400, rejected before auth gate
# Check logs: [ERROR] JSON parse failed
```

### 4. Momentum Filter Test (Manual)
Create a test signal with RSI > 70 (overbought):

```bash
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "rsi": 75,
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# Expected: [WARN] RSI > 70, blocking BUY signal
# No order should be placed
```

### 5. Regime Gate Test (Manual)
Create a signal that switches to RISK_OFF:

```bash
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 40,              # VIX now high
    "spy_price": 450.0,     # SPY now below SMA
    "sma_200": 475.20,
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# Expected: [CRITICAL] Regime re-evaluated to RISK_OFF, blocking order
# Check Telegram for regime alert
```

### 6. Portfolio Equity Fetch Test
```bash
# Set ALPACA_BASE_URL to paper trading endpoint
export ALPACA_BASE_URL="https://paper-api.alpaca.markets"
export ALPACA_API_KEY="your_alpaca_key"
export ALPACA_SECRET_KEY="your_alpaca_secret"

# Send a valid signal
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{...signal...}'

# Check logs for:
# [INFO] Fetching live portfolio equity from Alpaca
# [INFO] Live equity: $100,000.00
# If equity fetch fails 3 times: [CRITICAL] Aborting order
```

### 7. Kelly Sizing Calculation Test (Manual)
```bash
# Send signal with expected sizing scenario:
# Portfolio: $100k, historical win rate: 0.55, avg win: 2%, avg loss: 1%
# Kelly = 0.55 - ((1-0.55)/2) = 0.55 - 0.225 = 0.325 = 32.5%

curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "kelly_fraction": 0.25,    # Safety multiplier: 32.5% * 0.25 = 8.125%
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# Check logs for:
# [INFO] Kelly calculated: 8.125% of $100k = $8,125
# [INFO] Share quantity: 16 shares @ $482.50 = $7,720.00
```

### 8. Order Routing Test (Paper Trading)
```bash
# Ensure ALPACA_BASE_URL points to paper API
# Send a valid signal during market hours

curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# Expected outcomes:
# 1. [INFO] Order submitted to Alpaca
# 2. [INFO] Order ID: xxxx-xxxx-xxxx-xxxx
# 3. Check Alpaca dashboard - new paper order should appear
# 4. Telegram notification with order confirmation
```

## Integration Tests (Full Pipeline)

### Analyst → Execution → Alpaca
```bash
# Terminal 1: Start execution engine
docker-compose up execution-engine

# Terminal 2: Run analyst
docker-compose run analyst

# Terminal 3: Monitor execution logs
docker logs -f nox_execution-engine

# Verify full chain:
# 1. Analyst fetches data
# 2. Analyst transmits signal
# 3. Execution receives and validates
# 4. Execution calls Alpaca
# 5. Order appears in Alpaca dashboard
# 6. Telegram notification received
```

## Environment Variables Required

```bash
WEBHOOK_SECRET_TOKEN          # Shared secret for auth gate (REQUIRED)
ALPACA_BASE_URL               # Paper or live API endpoint (REQUIRED)
ALPACA_API_KEY                # Alpaca API key (REQUIRED)
ALPACA_SECRET_KEY             # Alpaca secret key (REQUIRED)
KELLY_FRACTION                # Safety multiplier, default 0.25 (OPTIONAL)
TELEGRAM_BOT_TOKEN            # Telegram notifications (OPTIONAL)
TELEGRAM_CHAT_ID              # Telegram chat ID (OPTIONAL)
```

## When to Update This Guide

Update this guide when:
1. **Gate logic changes** — new validation rules added or thresholds adjusted
2. **Kelly formula changes** — if inputs or calculation method changes
3. **Alpaca API changes** — endpoint updates, new required fields
4. **Order format changes** — if order payload structure is modified
5. **New gates are added** — document validation sequence and failure modes

## Common Failures & Diagnostics

| Symptom | Check | Fix |
|---------|-------|-----|
| `[WARN] Authentication failed` | Check WEBHOOK_SECRET_TOKEN matches | Verify env var is set correctly |
| `[ERROR] JSON parse failed` | Payload malformed | Validate JSON syntax, required fields |
| `[CRITICAL] RSI filter blocked` | Signal RSI value | Adjust RSI thresholds or signal source |
| `[CRITICAL] Portfolio equity failed 3x` | Alpaca connectivity | Check API key, endpoint URL, network |
| `[INFO] Kelly calculated 0%` | No statistical edge | Increase win rate or win/loss ratio |
| Order not in Alpaca dashboard | Check ALPACA_BASE_URL | Paper vs live endpoint mismatch |
| Telegram notification missing | Check bot token/chat ID | Test Telegram connection separately |

## Testing Checklist Before Deployment

- [ ] All 3 gates (schema, auth, momentum) reject invalid signals
- [ ] Regime re-evaluation correctly switches RISK_OFF at 40 VIX
- [ ] Kelly sizing respects 1% min and 10% max caps
- [ ] Portfolio equity fetch works and retries 3 times
- [ ] Order routing creates order in Alpaca paper trading
- [ ] Telegram alerts fire on all critical events
- [ ] HTTP timeouts don't hang the server
- [ ] Large payloads (batches) are handled correctly
