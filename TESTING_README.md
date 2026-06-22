# Nox Testing Documentation

Welcome! This directory contains comprehensive testing guides for the Nox trading system. Use this file to navigate to the right guide for what you're working on.

## 👤 Non-Technical Users

**If you're not a developer**, start here:
- **[NOX_USER_GUIDE.md](NOX_USER_GUIDE.md)** — Understand what the bot does, read alerts, monitor performance
  - Plain English explanation of how Nox works
  - What each Telegram alert means
  - How to check if something is wrong
  - FAQ for common questions
  - No coding knowledge required!

---

## 🚀 I Want To...

### ...write code and commit
1. **Read first**: [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) — 5 min
2. **Make code change**
3. **Update guides**: Use the checklist to identify which test guides to update
4. **Run one example** from each updated guide to verify
5. **Commit** code + guide updates together

**Example**: You modified `execution/main.cpp`:
- Run: `./run_tests.sh` (verifies compilation)
- Update: `EXECUTION_TEST_GUIDE.md` (if behavior changed)
- Verify: Copy one curl example and run it
- Commit: `git add execution/main.cpp EXECUTION_TEST_GUIDE.md`

### ...understand the testing strategy
**Read**: [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md) — 10 min

Covers:
- Why we test the way we do
- What's tested (and what's not, and why)
- How tests are organized by layer (unit, component, integration)
- When and how to maintain guides

### ...test a specific component

| Component | Guide | Typical Tests |
|-----------|-------|---------------|
| **Regime Classification** | [TEST_GUIDE.md](TEST_GUIDE.md) | Unit test: `./run_tests.sh` |
| **Kelly Sizing** | [TEST_GUIDE.md](TEST_GUIDE.md) | Unit test: `./build/test_kelly_sizing` |
| **MCPT** | [TEST_GUIDE.md](TEST_GUIDE.md) | Unit test: `./build/test_mcpt_example` |
| **Analyst (data fetch, signals)** | [ANALYST_TEST_GUIDE.md](ANALYST_TEST_GUIDE.md) | Manual: Compile, run, check Telegram |
| **Execution (order validation, routing)** | [EXECUTION_TEST_GUIDE.md](EXECUTION_TEST_GUIDE.md) | Manual: curl HTTP endpoints |
| **Backtest Engine** | [BACKTEST_TEST_GUIDE.md](BACKTEST_TEST_GUIDE.md) | Manual: Run with historical data |
| **Data Engines (news, macro)** | [DATA_ENGINE_TEST_GUIDE.md](DATA_ENGINE_TEST_GUIDE.md) | Manual: Docker Compose + curl |
| **Heartbeat Monitor** | [HEARTBEAT_TEST_GUIDE.md](HEARTBEAT_TEST_GUIDE.md) | Manual: Start service, check logs |

### ...update test documentation
**Read**: [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) — 15 min

Covers:
- When guides should be updated
- How to update them correctly
- Deprecating old tests
- Preventing guide rot

### ...prepare for a release
**Checklist**:
1. Run `./run_tests.sh` ✅
2. For each component changed, manually run tests from its guide's "Testing Checklist Before Deployment" section
3. Review all guides modified in this release for correctness
4. Run one full integration test via Docker Compose

### ...add a new component to Nox
1. Create `COMPONENT_TEST_GUIDE.md` (use existing guides as template)
2. Include: Building, Unit Tests, Component Tests, Integration Tests, Environment Variables, Common Failures, Checklist
3. Add to [TEST_GUIDE.md](TEST_GUIDE.md) navigation table
4. Update [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) 
5. Update [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)

---

## 📋 Test Guide Index

### Core Unit Tests
- **[TEST_GUIDE.md](TEST_GUIDE.md)** — RegimeStateMachine, Kelly sizing, MCPT
  - Quick start: `./run_tests.sh`
  - Time: < 1 second

### Component Tests (Manual)
- **[ANALYST_TEST_GUIDE.md](ANALYST_TEST_GUIDE.md)** — Data fetching, regime classification, signal transmission
  - Time: 5-30 seconds per test
- **[EXECUTION_TEST_GUIDE.md](EXECUTION_TEST_GUIDE.md)** — Order validation, position sizing, routing
  - Time: 5-30 seconds per test
- **[BACKTEST_TEST_GUIDE.md](BACKTEST_TEST_GUIDE.md)** — Historical simulation, parameter tuning
  - Time: 10-60 seconds per test
- **[DATA_ENGINE_TEST_GUIDE.md](DATA_ENGINE_TEST_GUIDE.md)** — News/macro data services
  - Time: 5-30 seconds per test
- **[HEARTBEAT_TEST_GUIDE.md](HEARTBEAT_TEST_GUIDE.md)** — Monitoring, intelligence, alerts
  - Time: 5-30 seconds per test

### Operational Guides
- **[TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)** — Strategy, principles, trade-offs
- **[TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)** — When/how to update guides
- **[TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)** — Quick reference before committing

---

## 🔄 Typical Workflows

### Add a Feature
```bash
# 1. Write code
vi analyst/main.cpp

# 2. Check tests pass
./run_tests.sh

# 3. Identify affected guide(s)
# Use TEST_UPDATE_CHECKLIST.md

# 4. Update guide
vi ANALYST_TEST_GUIDE.md

# 5. Test the example you added
bash -x example_command

# 6. Commit
git add analyst/main.cpp ANALYST_TEST_GUIDE.md
git commit -m "feat: Add feature"
```

### Fix a Bug
```bash
# 1. Identify root cause
# Read relevant guide to understand expected behavior

# 2. Fix code
vi execution/main.cpp

# 3. Verify fix with guide's test
bash -x test_example_from_guide

# 4. Update guide if behavior changed
vi EXECUTION_TEST_GUIDE.md

# 5. Commit
git add execution/main.cpp EXECUTION_TEST_GUIDE.md
git commit -m "fix: Description of bug"
```

### Run Tests Before Release
```bash
# Unit tests (required)
./run_tests.sh

# Component tests (manual, per guide)
./build/backtester ./data/spy_vix_daily.csv
curl http://localhost:8080/webhook ...
python3 heartbeat/monitor.py

# Full integration (if time)
docker-compose up -d
# ... verify pipeline end-to-end
docker-compose down
```

---

## ⚡ Quick Commands

```bash
# Run all unit tests
./run_tests.sh

# Run backtest
./build/backtester ./data/spy_vix_daily.csv

# Test execution webhook
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{"regime":"RISK_ON","vix":18.5,"secret_key":"YOUR_SECRET"}'

# Check Telegram alerts
# (monitor your Telegram chat while tests run)

# Docker Compose full stack
docker-compose up -d
docker logs -f nox_execution-engine

# Verify guide examples work
bash -x ANALYST_TEST_GUIDE.md  # Extract and run all bash blocks
```

---

## 📚 Learning Paths

### For New Contributors
1. Read: [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md) (understand the why)
2. Read: [TEST_GUIDE.md](TEST_GUIDE.md) (see simple examples)
3. Run: `./run_tests.sh` (verify your environment)
4. Pick one component guide and run one test manually

### For Code Reviewers
1. Use: [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) to verify guides were updated
2. Copy-paste one example from modified guides to verify correctness
3. Check: "Testing Checklist" at end of each guide

### For Maintenance
1. Monthly: Pick random guide, run one test
2. Quarterly: Review all "Last verified" dates
3. When bug found: Ensure it's covered by guide examples
4. When adding guides: Use existing guides as template

---

## 🎯 Principles

1. **Tests = Executable Docs** — Every example should work when copy-pasted
2. **Update Guides with Code** — Same commit, same PR
3. **No Silent Failures** — System fails loudly; tests verify this
4. **Fast Feedback** — Unit tests < 1s; manual tests < 30s
5. **Keep It Current** — Guides decay; monthly spot-checks prevent rot

---

## 📞 Questions?

- **Which guide covers component X?** → See table above
- **When should I update guides?** → [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) "When to Update"
- **How do I update a guide?** → [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) "How to Update"
- **What should I test before committing?** → [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)
- **Why do we test this way?** → [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)

---

**TL;DR**: Run `./run_tests.sh` before committing. Update test guides when code changes. Copy-paste one example from each updated guide to verify it works. Done!
