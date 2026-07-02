# Alpaca Trailing Stop Signal Detection

## Problem

Previously, when Alpaca's trailing stop orders executed (closing a position), **no SELL signal was recorded in the Nox system**. The position would close on Alpaca's end, but the execution engine had no way to detect this and report it as a completed trade.

This meant:
- Trailing stops would execute silently on Alpaca
- No SELL signal would appear in `/signals` or `/details` endpoints
- No Telegram notifications about the trailing stop fill
- Trading activity would appear incomplete in reports and journals

## Solution

Added a **Trailing Stop Monitor Thread** that:

1. **Every 5 minutes**, queries Alpaca for all open equity positions
2. **Compares** against the set of positions the engine is tracking (stored when BUY orders fill)
3. **Detects closures**: If a position that was tracked is no longer open, it was likely closed by a trailing stop
4. **Records SELL signals**: Automatically generates a synthetic SELL signal for each detected closure
5. **Notifies you**: Sends a Telegram alert and logs the event

## How It Works

### Position Tracking

When a BUY order fills with a trailing stop:
```cpp
// Track this position for trailing stop monitoring
equity_positions_[ticker] = OpenEquityPosition{
    ticker, qty, entry_price, entry_time
};
```

### Detection Loop

Every 5 minutes, the monitor:
1. Fetches current positions from `GET /v2/positions` (Alpaca)
2. Checks which previously-tracked positions are now missing
3. For each missing position, records it as a SELL signal

```cpp
[TRAILING_STOP_MONITOR] Position NVDA detected as closed — likely hit trailing stop.
```

### Cleanup

When a SELL signal is received via webhook, the position is removed from tracking:
```cpp
equity_positions_.erase(ticker);
```

## Verification

Check the execution engine logs:
```bash
docker logs nox_execution | grep "TRAILING_STOP_MONITOR"
```

You should see:
- **Startup**: `[INFO] [TRAILING_STOP_MONITOR] Thread started. Checking for trailing stop fills every 5 minutes.`
- **Detection**: `[TRAILING_STOP_MONITOR] Position {TICKER} detected as closed — likely hit trailing stop.`
- **Alerts**: `[TRAILING_STOP_MONITOR] Detected N closed position(s).`

## Testing

To manually verify the system works:

1. **Place a buy order** via the market scanner or manual signal
   - Wait for the trailing stop to be placed: `[TRAILING STOP PLACED] Order ID: ...`

2. **Manually close the position in Alpaca**
   - Use Alpaca's dashboard or API to liquidate a position

3. **Wait up to 5 minutes** for the monitoring cycle
   - The monitor will detect the closure and record a SELL signal

4. **Check the signal**:
   ```bash
   curl http://localhost:8080/recent-signals -H "X-Nox-Token: $WEBHOOK_SECRET_TOKEN"
   ```
   - You should see a SELL signal with `"action": "SELL"` and the ticker

## Code Changes

Modified files:
- `execution/main.cpp`:
  - Added `OpenEquityPosition` struct to track open positions
  - Added `equity_positions_` map and mutex for thread-safe tracking
  - Added `fetch_open_positions()` to query Alpaca
  - Added `monitor_trailing_stops()` thread function
  - Integrated position tracking in BUY signal handler
  - Integrated position removal in SELL signal handler
  - Launched monitoring thread on startup

The monitoring thread is independent of signal generation and runs continuously, so **no configuration changes are needed** — it activates automatically when the engine starts.

## Edge Cases Handled

1. **Position entered before engine restart**: If a position was created before the engine started, the monitor won't track it, but that's fine — we only care about positions we opened.

2. **Alpaca API unavailable**: If a monitoring cycle fails (network error, Alpaca timeout), the next cycle retries. Failures are logged but don't block trading.

3. **Position deleted manually vs. trailing stop**: The monitor doesn't distinguish between a position closed by a trailing stop vs. manually closed. Either way, it's a closed position that should be recorded.

4. **Fast market moves**: If a position closes between monitoring cycles, we'll still detect it in the next cycle (5-minute max latency).

## Logging

All monitoring activity is logged with the `[TRAILING_STOP_MONITOR]` tag for easy filtering:

```bash
docker logs nox_execution | grep "\[TRAILING_STOP_MONITOR\]"
```

## Future Enhancements

Possible improvements (not implemented):
- Fetch the actual close price from Alpaca orders API for more detailed P&L tracking
- Use Alpaca order status webhooks instead of polling (not currently supported by their API)
- Store trailing stop monitoring stats in the database for analytics
