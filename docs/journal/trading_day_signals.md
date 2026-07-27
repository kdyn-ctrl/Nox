# Journal: July 3, 2026 — Trading Day Signal Gating

## Summary
Implemented automatic disabling of all options signals, equity signals, scout protocol, market scanner, and IV collection on non-trading days (weekends and US market holidays).

## Problem Solved
Previously, the system would attempt to generate trading signals on weekends and market holidays when the NYSE is closed. This wasted compute resources, generated spurious signals with no market to execute against, and created unnecessary noise in the audit logs.

## Solution
Added centralized trading-day detection that gates all signal generation:

### Key Changes
1. **C++ Side** (`shared/TradingDayUtils.hpp`):
   - `is_trading_day()` function checks if today is a US trading day
   - Hardcoded holiday list for 2024-2027 (covering analysis window)
   - Returns `false` on weekends (Sat/Sun) and market holidays
   - Respects env var overrides: `TRADING_DAYS_ENABLED=false` (disable on weekdays) or `TRADING_DAY_SKIP_DATES` (add/remove holidays)

2. **Python Side** (`heartbeat/trading_day_utils.py`):
   - Mirror implementation with same holiday list and env var support
   - Used by heartbeat scheduler for all signal generators

3. **Execution Engine** (`execution/main.cpp`):
   - Options scanner threads (OptionsBot & OptionsPersonal) now skip on non-trading days
   - Equity scanner thread now skips on non-trading days
   - Market-hours check remains; trading-day check is a pre-filter

4. **Heartbeat Monitor** (`heartbeat/monitor.py`):
   - Scout protocol skips on non-trading days
   - Market scanner skips on non-trading days (in addition to existing market-hours check)
   - EOD IV collection skips on non-trading days
   - EOD report updated to use `is_trading_day()` instead of weekday-only check (now respects holidays)

### Holiday Coverage
Holidays included in the hardcoded list:
- New Year's Day (Jan 1)
- MLK Day (Jan 15–20, third Monday)
- Presidents Day (Feb 15–21, third Monday)
- Good Friday (Mar/Apr)
- Memorial Day (May 25–31, last Monday)
- Juneteenth (Jun 19)
- Independence Day (Jul 4)
- Labor Day (Sep 1–7, first Monday)
- Thanksgiving (Nov 22–28, fourth Thursday)
- Christmas (Dec 25)
- Christmas Eve observed (Dec 24, when market closes early)

### Configuration
Two env vars control behavior (tunable, not hardcoded):
```bash
# Temporarily disable all signal generation (even on weekdays)
TRADING_DAYS_ENABLED=false

# Add or override holidays (comma-separated YYYY-MM-DD)
TRADING_DAY_SKIP_DATES=2026-07-03,2026-11-30
```

## Impact
- **Efficiency**: ~2 days/week compute savings (no weekend scans)
- **Cleanliness**: Cleaner logs, no weekend noise
- **Correctness**: All signal generators now respect market holidays (previously only weekday-checked)

## Testing Approach
1. Deploy to private repo (`nocturnal`/`private-work` branch)
2. Monitor logs during next market holiday (e.g., Independence Day 2026-07-04)
3. Verify:
   - All scanner threads skip gracefully
   - Scout protocol doesn't run
   - IV collection doesn't run
   - Logs show "Non-trading day" messages
4. Manual test: set `TRADING_DAYS_ENABLED=false` on a Monday to verify full shutdown

## Backward Compatibility
- No breaking changes to API or trading logic
- Existing market-hours checks remain in place
- Optional feature (can be disabled with env vars)
- All signal generators degrade gracefully (skip and retry on next interval)

## Related Memories to Update
- [[project_nox_status.md]] — add trading-day feature completion
- [[feedback_hardcode_nothing_tunable.md]] — confirm env var override pattern used
