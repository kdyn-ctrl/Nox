# Nox Documentation Overview

This document explains all the documentation in the repository and how to keep it current.

---

## 📚 What Documentation Exists?

### 1. For Non-Technical Users (No Coding Required)
**File**: [NOX_USER_GUIDE.md](NOX_USER_GUIDE.md)

- Explains what the bot does in plain English
- Shows what each Telegram alert means
- How to monitor the bot without coding
- FAQ for common questions
- Glossary of simple terms

**Who should read this**: Anyone using the bot who isn't a developer

**When to update**: When bot behavior changes, new alerts appear, thresholds change

---

### 2. For Developers: Testing Strategy
**Files**:
- [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md) — Why we test the way we do
- [TESTING_README.md](TESTING_README.md) — Quick navigation guide
- [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) — Quick reference before committing

**Purpose**: Understand testing philosophy, know which guides to update

**When to read**: 
- Before making your first code change
- When deciding what to test

---

### 3. For Developers: Component Testing
**Files** (one per component):
- [TEST_GUIDE.md](TEST_GUIDE.md) — Core logic (RegimeStateMachine, Kelly sizing, MCPT)
- [ANALYST_TEST_GUIDE.md](ANALYST_TEST_GUIDE.md) — Data ingestion and signals
- [EXECUTION_TEST_GUIDE.md](EXECUTION_TEST_GUIDE.md) — Order validation and routing
- [BACKTEST_TEST_GUIDE.md](BACKTEST_TEST_GUIDE.md) — Historical simulation
- [DATA_ENGINE_TEST_GUIDE.md](DATA_ENGINE_TEST_GUIDE.md) — Data services
- [HEARTBEAT_TEST_GUIDE.md](HEARTBEAT_TEST_GUIDE.md) — Monitoring and alerts

**Purpose**: Know how to test each component

**When to read**: When working on that component

**When to update**: When code for that component changes

---

### 4. For Developers: Maintenance
**File**: [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)

**Purpose**: 
- Know when guides should be updated
- Understand how to keep guides accurate
- Prevent guides from becoming outdated

**When to read**: When updating guides, quarterly review

---

## 🔄 Update Rules

### The Core Rule
**Every code change must have a corresponding documentation update in the same commit.**

### Code Changes → Which Docs to Update

| If you change... | Update... | Because... |
|------------------|-----------|-----------|
| VIX/SMA thresholds | NOX_USER_GUIDE.md + ANALYST_TEST_GUIDE.md + TEST_GUIDE.md | Affects bot behavior and alerts |
| Kelly cap (10%) or min (1%) | EXECUTION_TEST_GUIDE.md + NOX_USER_GUIDE.md | Affects position sizing and risk |
| Regime classification logic | ANALYST_TEST_GUIDE.md + TEST_GUIDE.md + NOX_USER_GUIDE.md | Changes how market regime is detected |
| HTTP endpoints | EXECUTION_TEST_GUIDE.md + DATA_ENGINE_TEST_GUIDE.md | Changes how to test API interaction |
| Telegram alert types/format | HEARTBEAT_TEST_GUIDE.md + NOX_USER_GUIDE.md | Affects how users read alerts |
| Kelly formula | EXECUTION_TEST_GUIDE.md + TEST_GUIDE.md | Changes position sizing calculation |
| Trading hours | NOX_USER_GUIDE.md | Changes when bot is active |
| RSI thresholds | EXECUTION_TEST_GUIDE.md + NOX_USER_GUIDE.md | Changes momentum filter behavior |
| Env variables | All relevant guides | Users/developers need to know what vars are required |
| Unit test logic | TEST_GUIDE.md | Keep unit test docs current |
| Backtest features | BACKTEST_TEST_GUIDE.md | Changes how to use backtest |

### Update Checklist
Before committing, use [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md):

```bash
# 1. Identify affected files
# 2. Read the quick reference table
# 3. Update each affected guide
# 4. Copy-paste one example and verify it works
# 5. Commit code + docs together
```

---

## 📋 Documentation by Audience

### Non-Technical Users
- **Start here**: [NOX_USER_GUIDE.md](NOX_USER_GUIDE.md)
- **Then check**: [TESTING_README.md](TESTING_README.md) for component status

### New Developers
1. Read: [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)
2. Read: [TEST_GUIDE.md](TEST_GUIDE.md)
3. Run: `./run_tests.sh`
4. Pick a component guide and run one test

### Experienced Developers
1. Check: [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) before committing
2. Update: Relevant component guides
3. Verify: Copy-paste examples work

### Code Reviewers
1. Use: [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) to verify docs were updated
2. Check: Examples are still valid
3. Approve: When code + docs are both correct

### System Maintainers
1. Read: [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) monthly
2. Spot-check: One random guide example
3. Update: Any docs that don't match current behavior
4. Add timestamp: "Last verified: YYYY-MM-DD"

---

## 🔍 How to Keep Docs Current

### Monthly Maintenance (30 minutes)
```bash
# Pick a random guide
GUIDE=$(ls *_TEST_GUIDE.md | shuf | head -1)

# Copy one test example and run it
bash -x $GUIDE

# Update timestamp if it works
sed -i "s/Last verified:.*/Last verified: $(date +%Y-%m-%d)/" $GUIDE
```

### Quarterly Review (1 hour)
```bash
# Check all guides for "Last verified" timestamps older than 90 days
find . -name "*_TEST_GUIDE.md" -o -name "*_USER_GUIDE.md" | \
  xargs grep -l "Last verified"

# Run at least one test from each old guide
# Update timestamp
```

### When a Bug Is Found (Immediate)
1. **Fix the bug** in code
2. **Add test case** to component guide showing the bug is fixed
3. **Verify** the test example works
4. **Commit** with message: "Fix: Description of bug + add test case"

### When Code Changes (Before Commit)
1. **Identify affected guides** using [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)
2. **Update each guide** with new behavior
3. **Verify examples** still work
4. **Commit** code + docs together

---

## ⚠️ Red Flags (Guide Is Out of Date)

- ❌ Example command doesn't compile
- ❌ Expected output doesn't match actual output
- ❌ Threshold value in guide ≠ threshold value in code
- ❌ Env var in guide doesn't exist in code
- ❌ HTTP endpoint in guide doesn't exist
- ❌ Alert message in guide doesn't match actual message
- ❌ Last verified timestamp > 90 days old

**If you see any red flag**: Update the guide immediately, don't skip it.

---

## 📊 Documentation Quality Checklist

Before considering a guide "done":

### For All Guides
- [ ] Examples compile/run without errors
- [ ] Expected output matches actual behavior
- [ ] All commands can be copy-pasted
- [ ] References to code (line numbers, file paths) are current
- [ ] Timestamps are recent (< 90 days)

### For Component Guides
- [ ] "Environment Variables" section is current
- [ ] "Common Failures" table covers realistic failures
- [ ] "Testing Checklist Before Deployment" is complete
- [ ] All HTTP endpoints/parameters are correct

### For User Guide
- [ ] Alert examples match actual Telegram formats
- [ ] Thresholds (VIX, SMA, RSI) match code
- [ ] Risk limits (10%, 1%) match code
- [ ] Trading hours match code
- [ ] FAQ answers are accurate

### For Philosophy/Maintenance Guides
- [ ] Examples are relevant
- [ ] File paths are correct
- [ ] Workflow descriptions are realistic

---

## 🚀 Typical Doc Maintenance Workflows

### Scenario 1: You Change VIX Threshold
```
Code change: VIX threshold 35 → 40

Docs to update:
1. ANALYST_TEST_GUIDE.md — Update regime classification test to use 40
2. EXECUTION_TEST_GUIDE.md — Update regime gate test to use 40
3. NOX_USER_GUIDE.md — Update "VIX" glossary entry and "What Does Nox Look At"
4. TEST_GUIDE.md — Update expected output if test_regime changed

For each guide:
- Find all references to "35"
- Replace with "40"
- Run one test example
- Verify output
- Commit all together
```

### Scenario 2: You Add New Alert Type
```
Code change: Add new alert "EQUILIBRIUM_DETECTED"

Docs to update:
1. HEARTBEAT_TEST_GUIDE.md — Add test for new alert
2. NOX_USER_GUIDE.md — Add new section to "Understanding Alerts"
3. TEST_UPDATE_CHECKLIST.md — Note that user guide affects heartbeat changes

For each guide:
- Add new alert description
- Show example Telegram message
- Explain what user should do
- Run through verification
- Commit all together
```

### Scenario 3: Code Refactor (No Behavior Change)
```
Code change: Extract helper function, rename internal variable

Docs to update:
- None required (internal refactor)
- UNLESS: Public behavior changed
- UNLESS: Example code paths changed

Rule: If developers can't see the change, docs don't need updating.
```

---

## 📞 Questions?

- **Which guide do I update?** → [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)
- **How do I update?** → [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)
- **Why do we have so much documentation?** → [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)
- **I found a docs error** → Fix it immediately, commit with explanation
- **A guide is out of date** → Update it using the monthly maintenance process above

---

## Summary

**Every change to code should have a matching change to documentation.** This keeps everything in sync and prevents confusion.

Use [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) before every commit to identify which docs need updating. Run one example from each updated guide to verify correctness. Commit code + docs together.

Monthly spot-checks keep guides fresh. If a guide example doesn't work, update it immediately.

---

**Last Updated: 2026-06-22**
