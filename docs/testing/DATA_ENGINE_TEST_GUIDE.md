# Test Guide for Data Engines (America & China)

The Data Engines are FastAPI services that fetch, process, and serve market data, news, and intelligence to the heartbeat monitor. Two instances run: `america-data-engine` (US equities, macro, news) and `china-data-engine` (CN equities, macro, news).

## Components Tested

- **Authentication Gate**: API key header validation
- **Data Scrapers**: News/macro data collection from external sources
- **Caching**: Response caching to reduce external API calls
- **Rate Limiting**: Graceful degradation under load
- **Error Handling**: Stale data fallback behavior

## Building & Running

```bash
# Build Docker image
docker build -t nox-data-engine ./

# Or run directly with Python
cd /root/Nox/america_data_engine
python3 -m pip install -r requirements.txt
WEBHOOK_SECRET_TOKEN="test_secret" python3 main.py
```

## Unit Tests

### Authentication Test (Manual)
```bash
# Start the data engine
export WEBHOOK_SECRET_TOKEN="my_secret_key"
python3 america_data_engine/main.py &
DATA_PID=$!

sleep 2

# Test with valid token
curl -X GET http://localhost:8001/health \
  -H "X-Nox-Token: my_secret_key"

# Expected: HTTP 200, response body: {"status": "healthy"}

# Test with invalid token
curl -X GET http://localhost:8001/health \
  -H "X-Nox-Token: wrong_secret"

# Expected: HTTP 401 Unauthorized

# Test without token
curl -X GET http://localhost:8001/health

# Expected: HTTP 403 Forbidden (auto_error=True from FastAPI)

kill $DATA_PID
```

### Scraper Function Tests
```bash
# Test Alpaca news fetcher
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/america_data_engine')
from scrapers import fetch_alpaca_news

# Test with valid symbol
news = fetch_alpaca_news("SPY")
assert isinstance(news, list), "Should return list of articles"
assert len(news) > 0, "Should return non-empty list"
assert "title" in news[0], "Articles should have title field"

print("✓ Alpaca news scraper works")
EOF
```

### Rate Limiting Test
```bash
# Simulate rapid requests
for i in {1..10}; do
  curl -X GET http://localhost:8001/api/news/spy \
    -H "X-Nox-Token: my_secret_key" &
done
wait

# Check logs for:
# - First few requests: 200 OK
# - Later requests: 429 Too Many Requests (if rate limit is 5 req/min)
# - Graceful degradation (no 500 errors)
```

## Integration Tests

### America Data Engine Full Test
```bash
# Terminal 1: Start America data engine
cd /root/Nox
export WEBHOOK_SECRET_TOKEN="test_secret"
python3 america_data_engine/main.py

# Terminal 2: Test endpoints
# Health check
curl -X GET http://localhost:8001/health \
  -H "X-Nox-Token: test_secret"

# Fetch news for SPY
curl -X GET http://localhost:8001/api/news/spy \
  -H "X-Nox-Token: test_secret"

# Expected response:
# [
#   {
#     "title": "Market Rally Continues...",
#     "source": "Reuters",
#     "publish_time": "2026-06-22T14:30:00Z",
#     "url": "https://..."
#   },
#   ...
# ]

# Fetch macro data
curl -X GET http://localhost:8001/api/macro \
  -H "X-Nox-Token: test_secret"

# Expected response:
# {
#   "vix": 18.5,
#   "spy_price": 482.50,
#   "unemployment": 4.1,
#   "gdp_growth": 2.4,
#   "timestamp": "2026-06-22T14:30:00Z"
# }
```

### China Data Engine Full Test
```bash
# Terminal 1: Start China data engine
cd /root/Nox
export WEBHOOK_SECRET_TOKEN="test_secret"
python3 china_data_engine/main.py

# Terminal 2: Test endpoints
# Health check
curl -X GET http://localhost:8002/health \
  -H "X-Nox-Token: test_secret"

# Fetch news for Alibaba
curl -X GET http://localhost:8002/api/news/baba \
  -H "X-Nox-Token: test_secret"

# Expected response: Same format as America engine
```

### Docker Compose Integration
```bash
# Start all three data engines
docker-compose up -d

sleep 3

# Test America engine
curl -X GET http://localhost:8001/health \
  -H "X-Nox-Token: ${WEBHOOK_SECRET_TOKEN}"

# Test China engine
curl -X GET http://localhost:8002/health \
  -H "X-Nox-Token: ${WEBHOOK_SECRET_TOKEN}"

# Verify both return 200
docker-compose logs america-data-engine | tail -10
docker-compose logs china-data-engine | tail -10
```

## Performance & Load Tests

### Response Time Test
```bash
# Measure response time for 100 requests
time for i in {1..100}; do
  curl -s -X GET http://localhost:8001/health \
    -H "X-Nox-Token: test_secret" > /dev/null
done

# Expected: Should complete in < 5 seconds
# Average latency < 50ms per request
```

### Concurrent Requests Test
```bash
# Send 20 concurrent requests
ab -n 20 -c 10 \
  -H "X-Nox-Token: test_secret" \
  http://localhost:8001/health

# Expected: All requests complete successfully (0 failures)
# Latency should remain consistent
```

### Cache Hit Test
```bash
# First request (cache miss)
time curl -X GET http://localhost:8001/api/news/spy \
  -H "X-Nox-Token: test_secret" > /dev/null

# Second request (cache hit) — should be much faster
time curl -X GET http://localhost:8001/api/news/spy \
  -H "X-Nox-Token: test_secret" > /dev/null

# Check logs:
# First request: [DEBUG] Cache miss for /api/news/spy
# Second request: [DEBUG] Cache hit for /api/news/spy (TTL: 300s)
```

## Error Handling Tests

### External API Timeout
```bash
# Stop internet connection or block API host
# Try to fetch news
curl -X GET http://localhost:8001/api/news/spy \
  -H "X-Nox-Token: test_secret"

# Expected: 503 Service Unavailable with message:
# {"error": "External API timeout", "cached_data_age": "5 minutes old"}
# (if cached data is available)
```

### Malformed Request
```bash
# Test invalid symbol
curl -X GET http://localhost:8001/api/news/INVALID_SYMBOL_XYZABC \
  -H "X-Nox-Token: test_secret"

# Expected: 400 Bad Request with message:
# {"error": "Invalid symbol format"}
```

### Missing Required Headers
```bash
# Test request without X-Nox-Token
curl -X GET http://localhost:8001/health

# Expected: HTTP 403 Forbidden (FastAPI auto-rejects before handler runs)
```

## Environment Variables Required

```bash
WEBHOOK_SECRET_TOKEN          # Shared secret for auth gate (REQUIRED)
ALPACA_API_KEY                # For Alpaca news scraper (REQUIRED)
ALPACA_SECRET_KEY             # For Alpaca news scraper (REQUIRED)
# Optional: Proxy, rate limit overrides, data sources
```

## When to Update This Guide

Update this guide when:
1. **New endpoints are added** — document URL path, parameters, response format
2. **Scraper sources change** — if switching from Alpaca to another news provider
3. **Authentication changes** — new header format or token validation logic
4. **Cache TTL changes** — update expected cache hit times
5. **Error handling changes** — new error codes or fallback strategies
6. **Rate limits change** — update concurrency expectations
7. **Response format changes** — update expected JSON structure

## Common Failures & Diagnostics

| Symptom | Check | Fix |
|---------|-------|-----|
| `401 Unauthorized` on every request | WEBHOOK_SECRET_TOKEN env var | Verify token matches header value |
| `503 Service Unavailable` | External API connectivity | Check internet, check API status page |
| Empty news array `[]` | Data source returned no articles | Normal during low-volume periods |
| Response takes > 5 seconds | Rate limit or external API lag | Check concurrent request count |
| `[ERROR] Parse error in scraper` | Source format changed | Check source HTML/API format |
| Inconsistent responses across replicas | Cache not shared | Verify same WEBHOOK_SECRET_TOKEN |

## Testing Checklist Before Deployment

- [ ] Auth gate rejects requests without valid token
- [ ] All endpoints return 200 when properly authenticated
- [ ] News scraper returns valid article arrays
- [ ] Macro data includes all expected fields
- [ ] Responses complete within 2 seconds (p95)
- [ ] Concurrent requests (10+) don't cause failures
- [ ] Cache properly reduces external API calls
- [ ] Timeouts don't crash the service
- [ ] Invalid requests return appropriate 4xx errors
