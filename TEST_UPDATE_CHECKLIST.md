# Test Guide Update Checklist

**Use this before committing code changes.**

## Quick Reference: Which Guide to Update?

```
Modified File(s)                    Update These Guides
────────────────────────────────────────────────────────────
analyst/main.cpp                    → ANALYST_TEST_GUIDE.md + NOX_USER_GUIDE.md
analyst/RegimeStateMachine.hpp      → ANALYST_TEST_GUIDE.md + TEST_GUIDE.md + NOX_USER_GUIDE.md

execution/main.cpp                  → EXECUTION_TEST_GUIDE.md + NOX_USER_GUIDE.md
execution/RegimeStateMachine.hpp    → EXECUTION_TEST_GUIDE.md + TEST_GUIDE.md + NOX_USER_GUIDE.md

backtest-engine/main.cpp            → BACKTEST_TEST_GUIDE.md
backtest-engine/csv_parser.hpp      → BACKTEST_TEST_GUIDE.md

*_data_engine/main.py               → DATA_ENGINE_TEST_GUIDE.md
*_data_engine/scrapers.py           → DATA_ENGINE_TEST_GUIDE.md

heartbeat/monitor.py                → HEARTBEAT_TEST_GUIDE.md + NOX_USER_GUIDE.md

tests/test_*.cpp                    → TEST_GUIDE.md

shared/RegimeStateMachine.hpp       → TEST_GUIDE.md + all guides using it + NOX_USER_GUIDE.md
```

**Note**: If your change affects **alerts, thresholds, risk limits, or trading behavior**, also update `NOX_USER_GUIDE.md` so non-technical users understand the new behavior.

## Pre-Commit Checklist

Before running `git commit`:

- [ ] **Code compiles** without warnings/errors
  ```bash
  ./run_tests.sh  # For C++ changes
  python3 -m py_compile heartbeat/monitor.py  # For Python changes
  ```

- [ ] **Identify affected guides** using the reference table above

- [ ] **For each affected guide**, verify:
  - [ ] Code examples are still syntactically correct
  - [ ] Expected outputs match actual code behavior
  - [ ] Any changed thresholds/parameters are updated
  - [ ] Any new error conditions are documented
  - [ ] Environment variable section is current

- [ ] **Environment variables**: If you added/changed/removed env vars:
  - [ ] Update "Environment Variables Required" section
  - [ ] Mark as `REQUIRED` or `OPTIONAL`
  - [ ] Document default values if any

- [ ] **API changes**: If you modified HTTP routes, parameters, or response format:
  - [ ] Update `curl` examples in guides
  - [ ] Update expected response JSON structure
  - [ ] Update HTTP status codes if changed

- [ ] **Test this specific change**:
  ```bash
  # Example: Modified analyst data fetching
  g++ -std=c++17 -o build/analyst analyst/main.cpp
  ./build/analyst  # Run and verify output
  
  # Then review ANALYST_TEST_GUIDE.md "Live Data Fetch Test" section
  # Ensure it matches what you just ran
  ```

## Change Type → Guide Updates

### 🔧 Added new feature / new code path
- [ ] Add new test case section
- [ ] Document expected behavior
- [ ] Add row to "Common Failures & Diagnostics" if new error paths exist
- [ ] Update "Testing Checklist Before Deployment" if acceptance criteria change

### 📊 Changed parameter / threshold
- [ ] Update "expected" values in examples
- [ ] Update "Common Failures" diagnostic thresholds
- [ ] Update any tables with current values

### 🚪 Added/changed/removed HTTP endpoints
- [ ] Update all `curl` examples (paths, headers, body)
- [ ] Update expected response structure
- [ ] Update status codes if changed
- [ ] Document new/changed env vars

### 💾 Changed database schema / file format
- [ ] Update schema description
- [ ] Update example CSV/JSON structure
- [ ] Update any parsing/validation logic descriptions

### 🔗 Changed external API integration
- [ ] Update API endpoint URLs
- [ ] Update auth mechanism if changed
- [ ] Update expected response format
- [ ] Update "Common Failures" with new error modes

### ♻️ Refactored code (no behavior change)
- [ ] Usually no guide update needed
- [ ] UNLESS: public-facing behavior changed

## Commit Message Format

Include a note about test guide updates:

```
feat: Add streaming support to analyst webhook

- Allows analyst to send multiple signals in batch
- Adds BATCH_SIZE env var (default 10)
- Retries unchanged

ANALYST_TEST_GUIDE.md:
- Updated "Payload Validation Test" to show array example
- Added env var: BATCH_SIZE
- Documented batch chunking behavior
```

## Example: Step-by-Step

You modified `execution/main.cpp` to change Kelly cap from 10% to 15%:

```bash
# 1. Make the code change
vi execution/main.cpp
# Change: kelly_cap = 0.10  →  kelly_cap = 0.15

# 2. Test locally
g++ -std=c++17 -o build/execution execution/main.cpp
curl -X POST http://localhost:8080/webhook -d '{...signal...}'
# Verify order is sized correctly with 15% cap

# 3. Update test guide(s)
vi EXECUTION_TEST_GUIDE.md
# Find: "Kelly Sizing Calculation Test"
# Update example: "10% → 15%"
# Find: "Common Failures" table
# Update cap reference to 15%

# 4. Run the test examples to verify
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
./build/test_kelly_sizing
# Should still pass

# 5. Commit with note
git add execution/main.cpp EXECUTION_TEST_GUIDE.md
git commit -m "feat: Increase Kelly position cap to 15%

Allows more aggressive sizing while maintaining risk bounds.

EXECUTION_TEST_GUIDE.md: Updated cap value in all examples"
```

## Guide Update Red Flags 🚩

These indicate you probably forgot to update guides:

- [ ] Your code has new parameters but "Environment Variables" section unchanged
- [ ] You changed API response format but test guide still shows old JSON
- [ ] You added validation logic but "Common Failures" table unchanged
- [ ] You changed error behavior but expected outputs in examples don't reflect it
- [ ] You renamed a command-line flag but guide still references old name

## Testing Guide Examples

After updating a guide, spot-check one example:

```bash
# Example: Testing ANALYST_TEST_GUIDE.md "Live Data Fetch Test"
cd /root/Nox
export TELEGRAM_BOT_TOKEN="test"
export TELEGRAM_CHAT_ID="test"

# Copy the test command from the guide
g++ -std=c++17 -o build/analyst analyst/main.cpp
./build/analyst

# Verify output matches guide's "Expected:" section
```

## Questions?

- **When to update?** → [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)
- **How to update?** → [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) "How to Update Test Guides"
- **Which guide covers component X?** → See table at top of this file
- **Need to add a new guide?** → See [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)

---

**TL;DR**: Whenever you commit code, also commit guide updates. Use the table above to find which guides are affected. Run one example from each guide to verify correctness. Done!
