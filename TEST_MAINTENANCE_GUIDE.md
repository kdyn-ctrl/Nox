# Test Maintenance Guide

This guide explains when and how to keep test documentation synchronized with code changes across the Nox system.

## Overview

Test guides exist for these components:
- `TEST_GUIDE.md` — Core C++ unit tests (RegimeStateMachine, Kelly sizing, MCPT)
- `ANALYST_TEST_GUIDE.md` — Data ingestion and signal generation
- `EXECUTION_TEST_GUIDE.md` — Order validation, sizing, routing
- `BACKTEST_TEST_GUIDE.md` — Historical simulation
- `DATA_ENGINE_TEST_GUIDE.md` — Market data services
- `HEARTBEAT_TEST_GUIDE.md` — Monitoring and intelligence
- `TEST_MAINTENANCE_GUIDE.md` — This file

## When to Update Test Guides

### Always Update When:

1. **Adding new code paths**
   - New validation gates
   - New endpoints (REST API)
   - New CLI flags or arguments
   - New scheduled tasks
   
   **Action**: Add new test case section to relevant guide(s) showing how to exercise the new code.

2. **Changing thresholds or parameters**
   - Regime classification VIX/SMA boundaries
   - Kelly sizing caps or multipliers
   - Rate limits or timeouts
   - Data retention policies
   
   **Action**: Update expected values in test examples and "Common Failures" table.

3. **Changing data formats**
   - JSON payload structure
   - CSV file format
   - HTTP request/response bodies
   - Database schema
   
   **Action**: Update all code examples and expected outputs in the affected guide.

4. **Changing external API integrations**
   - Yahoo Finance → new data provider
   - Alpaca endpoint URLs
   - Telegram API changes
   - Claude API parameter changes
   
   **Action**: Update integration test section with new endpoint details and auth mechanisms.

5. **Adding new error conditions**
   - New exception types
   - New validation rejections
   - New timeout scenarios
   
   **Action**: Add row to "Common Failures & Diagnostics" table.

### Consider Updating When:

6. **Refactoring internal logic** (without changing behavior)
   - Extracting helper functions
   - Reorganizing code layout
   - Changing variable names
   
   **Action**: No guide update needed unless the public-facing behavior changes.

7. **Performance optimizations**
   - Faster algorithms
   - Better caching
   - Connection pooling
   
   **Action**: Update "Response Time Test" or performance expectations if latency targets change.

8. **Environment variable changes**
   - New required vars added
   - Vars made optional
   - Rename existing vars
   
   **Action**: Update "Environment Variables Required" section in relevant guide.

## How to Update Test Guides

### Step 1: Identify Affected Guides
When you make a code change, determine which test guides are affected:

```
If you modify...             Update these guides:
─────────────────────────── ──────────────────────────────
analyst/main.cpp            ANALYST_TEST_GUIDE.md
execution/main.cpp          EXECUTION_TEST_GUIDE.md
tests/*.cpp                 TEST_GUIDE.md
backtest-engine/main.cpp    BACKTEST_TEST_GUIDE.md
*_data_engine/main.py       DATA_ENGINE_TEST_GUIDE.md
heartbeat/monitor.py        HEARTBEAT_TEST_GUIDE.md
shared/RegimeStateMachine   TEST_GUIDE.md + any using it
```

### Step 2: Update Relevant Sections
For each affected guide, update these sections (in order):

1. **Code Examples** — if command-line, API calls, or compilation changes
2. **Expected Output** — if results, logs, or console output changes
3. **Environment Variables** — if new vars added or requirements change
4. **Common Failures** — if new error conditions are possible
5. **Testing Checklist** — if acceptance criteria change

### Step 3: Test Your Changes
Before committing:

```bash
# Run the test that corresponds to your change
./run_tests.sh                              # For C++ unit tests

# Or manually test the component
python3 heartbeat/monitor.py                # For Python services

# Verify the guide's examples still work
bash -x <(grep "^g++" ANALYST_TEST_GUIDE.md)  # Extract and run compile commands
```

### Step 4: Commit Both Code & Docs
**Rule**: Code changes that affect behavior MUST include guide updates in the same commit.

```bash
git add src/mychange.cpp TEST_GUIDE.md
git commit -m "feat: Add new validation gate

- Implements stricter portfolio check in execution
- Rejects orders when cash < 10% of portfolio

TEST_GUIDE.md: Updated 'Common Failures' table with new error case"
```

## Guide-by-Guide Checklist

Use this checklist when updating each guide:

### NOX_USER_GUIDE.md (Non-technical user guide)
- [ ] Alerts described match actual Telegram message formats
- [ ] Regime descriptions match RegimeStateMachine rules
- [ ] Risk limits (10% cap, 1% min) are current
- [ ] Trading hours are correct
- [ ] Performance metrics (Sharpe ratio, win rate) are explained accurately
- [ ] FAQ answers are still relevant

**When to update**:
- When alert types or formats change
- When regime thresholds change (VIX 35, SPY/SMA ratio, etc.)
- When risk limits change (10% cap, Kelly formula)
- When trading hours change
- When new features are added (new endpoints, new monitoring options)

### TEST_GUIDE.md (Core C++ unit tests)
- [ ] Test command examples compile without errors
- [ ] Expected output matches actual test run
- [ ] All passing tests listed in the status table
- [ ] Description of what each test verifies is accurate

### ANALYST_TEST_GUIDE.md
- [ ] Data fetch URLs still valid (Yahoo Finance endpoints)
- [ ] Regime thresholds match RegimeStateMachine.hpp
- [ ] Expected JSON payload structure is correct
- [ ] Telegram notification format is current

### EXECUTION_TEST_GUIDE.md
- [ ] HTTP endpoint paths match execution/main.cpp
- [ ] Auth header name and format are correct
- [ ] Kelly formula variables and caps are current
- [ ] Alpaca order payload structure is accurate
- [ ] Gate validation sequence is documented correctly

### BACKTEST_TEST_GUIDE.md
- [ ] CSV column names match csv_parser.hpp
- [ ] Command-line flags match main.cpp
- [ ] Parameter ranges are realistic
- [ ] Example output metrics align with code

### DATA_ENGINE_TEST_GUIDE.md
- [ ] REST endpoint paths match FastAPI routes
- [ ] Auth header validation matches code
- [ ] Response JSON structure matches scrapers.py
- [ ] Rate limits reflect current configuration

### HEARTBEAT_TEST_GUIDE.md
- [ ] SQLite schema matches database initialization
- [ ] Telegram message formatting matches code
- [ ] Scheduled task intervals are correct
- [ ] Claude integration prompts are current

### TEST_MAINTENANCE_GUIDE.md (This file)
- [ ] Update the "When to Update" section if new categories emerge
- [ ] Add new guides if new components are created
- [ ] Document new patterns if they become common

## Automated Test Guide Validation

### Pre-Commit Hook (Optional)
Add to `.git/hooks/pre-commit` to catch obvious guide errors:

```bash
#!/bin/bash
# Validate that code changes have corresponding guide updates

STAGED_CODE=$(git diff --cached --name-only | grep -E '\.(cpp|py|hpp)$')
STAGED_DOCS=$(git diff --cached --name-only | grep -E '_TEST_GUIDE\.md$')

if [ -n "$STAGED_CODE" ] && [ -z "$STAGED_DOCS" ]; then
  echo "⚠️  Code changed but test guides not updated"
  echo "Modified: $STAGED_CODE"
  echo "Please update the corresponding test guides"
  exit 1
fi
exit 0
```

### Manual Validation
After editing a guide, validate its examples:

```bash
# Extract code blocks and run them
python3 << 'EOF'
import re
guide_file = "ANALYST_TEST_GUIDE.md"
with open(guide_file) as f:
    content = f.read()
    
# Find all bash code blocks
blocks = re.findall(r'```bash\n(.*?)\n```', content, re.DOTALL)
print(f"Found {len(blocks)} bash code blocks in {guide_file}")

# Check for broken examples
for i, block in enumerate(blocks):
    # Extract the command (not variable assignments)
    for line in block.split('\n'):
        if line.startswith('curl ') or line.startswith('./'):
            print(f"Block {i}: {line[:60]}...")
EOF
```

## Common Guide Update Mistakes

### ❌ Don't:
1. Update guide with aspirational changes that haven't been coded yet
2. Write guides for "future" features not yet implemented
3. Remove test cases that are still valid
4. Change expected output without verifying against actual code
5. Mix code changes and guide updates in separate commits (lose context)

### ✅ Do:
1. Run tests after updating examples to verify they still pass
2. Include guide updates in the same commit as code changes
3. Keep deprecated test sections marked as "[DEPRECATED — use X instead]"
4. Cross-reference guides when one test depends on another
5. Commit guide updates even if tests are temporarily failing (mark as [TODO])

## Deprecating Test Cases

When a component changes so much that old tests no longer apply:

```markdown
### [DEPRECATED] Old Test Name
This test is no longer valid because [reason].
→ Use [NEW_TEST_NAME] instead (see section below).

### New Test Name
[Updated test procedure]
```

## Review Checklist for Code Reviewers

When reviewing a PR that touches testable code:

- [ ] Code change has corresponding guide update
- [ ] Guide examples are syntactically correct
- [ ] Expected output matches code behavior
- [ ] New error cases are documented
- [ ] Environment variable section is updated if needed
- [ ] "When to Update" section mentions this type of change

## Links Between Guides

Test guides reference each other. When you rename or consolidate guides:

```bash
# Find all cross-references
grep -r "ANALYST_TEST_GUIDE\|EXECUTION_TEST_GUIDE" *.md

# Update them all
sed -i 's/ANALYST_TEST_GUIDE/NEW_GUIDE_NAME/g' *.md
```

## Guide Rot Prevention

Test guides decay over time as code changes. To keep them fresh:

1. **Monthly Review**: Pick one guide at random, run one test, verify it still works
2. **Before Release**: Run all guide examples to catch bit-rot
3. **CI Integration**: Consider adding guide validation to CI pipeline
4. **Timestamp Updates**: Add "Last verified: YYYY-MM-DD" comments to guides

Example:
```markdown
### Kelly Sizing Calculation Test
Last verified: 2026-06-22

```bash
g++ -std=c++17 -pthread -o build/test_kelly tests/test_kelly_sizing.cpp
```

This test was last run and verified on 2026-06-22.
```

## Summary

**The Rule**: Every testable code change requires guide updates in the same commit. Keep examples current by running them regularly.

Guides are not ornamental — they're the system's knowledge base for how to verify correctness. Treat them with the same care as the code itself.
