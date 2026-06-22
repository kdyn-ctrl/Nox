# Nox Testing Philosophy

This document explains the overall testing strategy for the Nox trading system.

## Core Principles

### 1. **Behavior Over Implementation**
Tests verify *what the system does*, not *how it does it*. We care that the Kelly calculator respects its 10% cap, not whether it uses floating-point or fixed-point math.

### 2. **Live Data Validation**
Where possible, unit tests use real data (historical market data, live API responses) to catch integration failures early. We do not mock the Alpaca API in execution tests — we test against paper trading instead.

### 3. **Gate-Centric Testing**
The system is organized around validation gates:
- **Analyst**: Data fetch gate, regime classification gate, transmission gate
- **Execution**: Schema gate, auth gate, momentum filter gate, regime gate, portfolio gate, Kelly sizing gate, order routing gate

Each gate has explicit test coverage. Cascading gates prevent invalid states.

### 4. **Fast Feedback**
Unit tests (RegimeStateMachine, Kelly sizing, CSV parsing) run in < 1 second and verify isolated logic. Integration tests are manual or require explicit Docker setup and are documented separately.

### 5. **Docs Are Tests**
Test guides are executable documentation. Every `bash` block in a guide should work when copy-pasted. Every `curl` command shows real HTTP interaction. Guides decay if not run regularly.

### 6. **Fail Loudly**
A misconfigured system shuts down immediately. Missing env vars → `[FATAL]` exit. Invalid API response → `[CRITICAL]` alert + Telegram + shutdown. We don't degrade gracefully — we fail explicitly so problems are visible.

---

## Testing Strategy by Layer

### Layer 1: Unit Tests (Fastest)
**Purpose**: Verify isolated logic in isolation.

**What**: 
- RegimeStateMachine classification
- Kelly position sizing calculation
- MCPT statistical utilities
- CSV parsing

**How**: Compile and run standalone executable.

**Time**: < 1 second per test

**When**: Every code change to these modules. Run `./run_tests.sh` before committing.

**Examples**: 
```bash
./build/test_regime
./build/test_kelly_sizing
./build/test_mcpt_example
```

### Layer 2: Component Tests (Moderate)
**Purpose**: Verify behavior of one service in isolation (or against mocked peers).

**What**:
- Analyst data fetching (against real Yahoo Finance)
- Execution order validation (via HTTP curl)
- Data engine endpoints (via HTTP curl)
- Heartbeat Claude integration (via API)
- Backtest simulation (against historical CSV)

**How**: Start service, interact via HTTP/CLI, verify output. All documented in component guides.

**Time**: 5-30 seconds per test

**When**: After modifications to a component. Run manually or via integration tests in CI.

**Examples**:
```bash
./build/backtester ./data/spy_vix_daily.csv
curl -X POST http://localhost:8080/webhook -d '{...}'
python3 heartbeat/monitor.py  # Start and check logs
```

### Layer 3: Integration Tests (Slowest)
**Purpose**: Verify components work together end-to-end.

**What**:
- Analyst → Execution → Alpaca pipeline
- Analyst → Heartbeat → Telegram notification chain
- Backtest results match live regime classification

**How**: 
- Start Docker Compose with multiple containers
- Send signal through pipeline
- Verify final output (order in Alpaca, message in Telegram)

**Time**: 30 seconds to 5 minutes per test

**When**: Before releases or major refactors. Document in each component guide.

**Examples**:
```bash
docker-compose up -d
curl http://localhost:8080/health  # Check analyst is running
# ... send signal through pipeline
docker logs nox_execution-engine | grep "Order submitted"
```

---

## Test Coverage Map

| Component | Unit | Component | Integration | Manual |
|-----------|------|-----------|-------------|--------|
| **RegimeStateMachine** | ✅ test_regime.cpp | ✅ TEST_GUIDE.md | ✅ All pipelines | - |
| **Kelly Sizing** | ✅ test_kelly.cpp | ✅ EXECUTION_TEST_GUIDE.md | ✅ Order test | - |
| **MCPT** | ✅ mcpt_example.cpp | ✅ TEST_GUIDE.md | - | - |
| **Analyst** | - | ✅ ANALYST_TEST_GUIDE.md | ✅ Full pipeline | Market hours |
| **Execution** | - | ✅ EXECUTION_TEST_GUIDE.md | ✅ Full pipeline | Market hours |
| **Backtest** | ✅ csv_parser | ✅ BACKTEST_TEST_GUIDE.md | - | Optimization |
| **Data Engine** | - | ✅ DATA_ENGINE_TEST_GUIDE.md | Docker Compose | Manual |
| **Heartbeat** | - | ✅ HEARTBEAT_TEST_GUIDE.md | Docker Compose | Manual |

### Gaps (Intentional)
- **No mocking of external APIs**: We test against real (paper) APIs to catch integration failures
- **No fuzzing**: System doesn't accept untrusted input; validation is gate-based
- **No load tests**: System is single-signal, not high-throughput; latency requirements are soft

---

## Test Execution Workflow

### Before Committing Code
```bash
# 1. Run unit tests (< 1s)
./run_tests.sh

# 2. Identify which guides to update
# Use: TEST_UPDATE_CHECKLIST.md

# 3. Update guides and spot-check one example
vi ANALYST_TEST_GUIDE.md
# (copy one bash example and run it)

# 4. Commit with guide updates
git add src/mychange.cpp ANALYST_TEST_GUIDE.md
git commit -m "feat: Add feature X (with test guide update)"
```

### Before Deployment
```bash
# 1. Run all unit tests
./run_tests.sh

# 2. Run component tests (manual, guided by TEST_GUIDE.md, etc.)
./build/backtester ./data/spy_vix_daily.csv

# 3. Test full pipeline via Docker Compose
docker-compose up -d
curl http://execution:8080/health  # Verify all services up
# ... send test signal
docker logs nox_execution-engine | grep success

# 4. Run production checklist from each guide
# (See "Testing Checklist Before Deployment" sections)
```

### Monthly Maintenance
```bash
# 1. Pick a guide at random
# 2. Run one test from that guide
# 3. Verify output matches expected
# 4. If failed, update guide or file bug
# 5. Add timestamp to guide: "Last verified: YYYY-MM-DD"
```

---

## Test Guide Maintenance

**Core Rule**: Guides are code. Treat them with equal care.

### When to Update Guides
- ✅ Any code change that affects behavior
- ✅ Any new parameters, thresholds, or env vars
- ✅ Any new error conditions
- ✅ Any HTTP endpoint or API format change
- ❌ Internal refactors with no behavior change
- ❌ Extracting helper functions with same interface

### Quality Checks for Guide Updates
1. **Copy-pasteable examples**: `bash` blocks should compile/run when pasted
2. **Current expected output**: Output examples match what code actually produces
3. **Complete reference**: Every public API/flag documented
4. **Diagnostic coverage**: "Common Failures" table covers realistic failure modes

### Decaying Guides
Guides decay over time. Combat decay by:
1. Running one example from each guide every month
2. Timestamping examples: `Last verified: 2026-06-22`
3. Immediately updating guides when failures occur
4. Pinning external API URLs (e.g., Yahoo Finance endpoint) with version numbers

---

## Philosophy vs. Practice

| Philosophy | How We Achieve It |
|-----------|-------------------|
| Test behavior, not implementation | Unit tests verify outputs for known inputs, not code paths |
| Use live data when possible | Integration tests hit real APIs (paper trading for Alpaca) |
| Fail loudly | `[FATAL]`/`[CRITICAL]` logs trigger alerts and shutdown |
| Fast feedback | Unit tests < 1s, component tests < 30s |
| Tests are docs | Guides include real working examples |
| Guides stay current | "Last verified" timestamps, monthly spot-checks |

---

## Testing Trade-Offs

### We Test (Comprehensive)
- Core logic (regime, Kelly) with multiple scenarios
- All gates (schema, auth, momentum, regime, portfolio)
- External integrations (Alpaca, Yahoo Finance, Telegram)
- Error handling (timeouts, invalid inputs, missing data)

### We Don't Test (Acceptable Risk)
- **Mocked APIs**: Too much distance from reality
- **Load testing**: System processes one signal at a time
- **Fuzzing**: System has strict input schema gates
- **Backwards compatibility**: System is maintained by one person; breaking changes are acceptable with docs

---

## Extending the Testing Strategy

### If You Add a New Component
1. Create a new guide: `COMPONENT_TEST_GUIDE.md`
2. Include sections: Building, Unit Tests, Component Tests, Integration Tests, Environment Variables, When to Update, Common Failures, Checklist
3. Add entry to [TEST_GUIDE.md](TEST_GUIDE.md) top-level table
4. Link in [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)
5. Update [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) "When to Update" section

### If You Change Multiple Components Together
Document cross-component impact in each guide. Example:

```markdown
### Integration Point: Analyst → Execution
If you modify analyst/main.cpp to add a new field to the JSON payload:
1. Update ANALYST_TEST_GUIDE.md "Payload Validation Test"
2. Update EXECUTION_TEST_GUIDE.md "Schema Validation Test" to accept new field
3. Update EXECUTION_TEST_GUIDE.md "Common Failures" if parsing could fail
```

---

## Summary

- **Unit tests** verify isolated logic (< 1s)
- **Component tests** verify one service (manual, < 30s)
- **Integration tests** verify end-to-end (manual, Docker, 5 min)
- **Guides are tests**: Examples should work when copied
- **Update guides with code**: Same commit, same PR
- **Decay prevention**: Monthly spot-checks, timestamps

This approach trades off breadth (not everything is automatically tested) for depth (everything that IS tested is thoroughly documented and regularly verified).

---

## Quick Links

- [Test Guide (Unit Tests)](TEST_GUIDE.md)
- [Test Update Checklist](TEST_UPDATE_CHECKLIST.md)
- [Test Maintenance Guide](TEST_MAINTENANCE_GUIDE.md)
- [Component Guides](TEST_GUIDE.md#-all-test-guides)
