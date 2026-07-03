# PROJECT NOX: ARCHITECTURE & STATUS SYNC
**Last Updated:** July 2, 2026  
**Status:** 🟢 STABLE — Paper trading ready, live trading ($5k account) ready, IBKR migration in progress on private `main` branch

---

## 1. GROUND TRUTH INFRASTRUCTURE & STACK

### Deployment Location
- **Production:** Hostinger VPS (24/7 live with fixed IP)
- **Environment:** Docker Compose multi-container (7 services)
- **Timezone:** America/New_York (enforced across all containers)

### Active Languages & Frameworks
| Component | Language | Framework | Version | Purpose |
|-----------|----------|-----------|---------|---------|
| **Execution Engine** | C++17 | Custom (no external framework) | 1.0 | Equity + options trading, position manager, rule exits |
| **Analyst Brain** | Python 3.10 | FastAPI + APScheduler | 1.0 | 6-hourly analysis, WS1-6 signal pipeline, Telegram dispatch |
| **Heartbeat/Monitor** | Python 3.10 | Flask + Alpaca SDK | 1.0 | IV skew tracking, EOD/EOW reports, portfolio queries |
| **Data Engines (US+CN)** | Python 3.10 | FastAPI + APScheduler | 1.0 | Market data, news, SEC filings, macro (WS2/3), report batch (WS6) |
| **Redis** | — | In-memory cache | 7.x | Inter-service communication, trade signal queue |

### Active Data Feeds
| Source | Method | Latency | Coverage | Status |
|--------|--------|---------|----------|--------|
| **Alpaca** | REST polling + WebSocket (paper) | ~1-2s | US equity, options chains | ✅ Live |
| **NewsAPI** | HTTP API (backup; optional) | ~5-10s | General US news headlines | ✅ Optional |
| **Polygon.io** | HTTP API (backup; optional) | ~2-5s | US news tickers, market data | ✅ Optional |
| **SEC EDGAR** | HTTP scrape + caching | ~30s (15min refresh) | Form 4 insider activity | ✅ Live |
| **AkShare (China)** | HTTP API via data-engine | ~10-30s | CN A-shares, macro | ✅ Live on `nocturnal` only |
| **East Money** | HTTP scrape (China) | ~10-30s | CN sector rotation, fund flows | ✅ Live on `nocturnal` only |
| **Yahoo Finance** (failover) | HTTP API | ~5-10s | Historical OHLCV, dividend data | ✅ Live |

### Communication Layer
- **Execution ↔ Heartbeat:** HTTP REST (Flask on heartbeat:8002)
- **Analyst ↔ Execution:** Redis queue + HTTP webhook (rule triggers)
- **Analyst ↔ Heartbeat:** HTTP (IV skew, contradiction detection)
- **All → Telegram:** Direct API calls (async, non-blocking)
- **Data Engines:** Internal-only HTTP (no external exposure per RULE-012)

---

## 2. ACTIVE FEATURE MATRIX (WHAT IS WORKING 100%)

### ✅ EQUITY TRADING (COMPLETE)

**Position Sizing & Risk**
- **Kelly Criterion:** 2% of account per trade (hard cap 10%)
- **Historical volatility:** 20-day rolling ATR-based; updates daily
- **Leverage:** Never exceeds 2:1 net; hard-coded in engine
- **Entry logic:** RSI < 30 on 5-min pullback (SMA-based mean reversion)

**Exit Rules (All Unified)**
| Exit Type | Trigger | Status | Ledger | Telegram |
|-----------|---------|--------|--------|----------|
| Take-profit % | +15% to +30% (ticker-dependent) | ✅ Logged | ✅ Yes | ✅ Yes |
| Stop-loss % | -8% to -12% (ticker-dependent) | ✅ Logged | ✅ Yes | ✅ Yes |
| RSI exhaustion | RSI > 78 (overbought) + 20-SMA break | ✅ Logged | ✅ Yes | ✅ Yes |
| Trend-break | Price falls below 200-SMA | ✅ Logged | ✅ Yes | ✅ Yes |
| Time-stop | > 10 days in position | ✅ Logged | ✅ Yes | ✅ Yes |
| Trailing-stop (ATR) | Dynamic ATR stops at broker | ✅ Logged | ✅ Yes | ✅ Yes |

**Watchlist**
- **US:** `AAPL,TSLA,NVDA,MSFT` (configurable via `NOX_WATCHLIST_US` env)
- **China:** `BABA,JD,PDD,BIDU,NIO` (configurable via `NOX_WATCHLIST_CN` env)
- Scan interval: 5 minutes
- Settlement gate: T+1 for CN A-shares (ready; dormant until IBKR)

**State Persistence**
- ✅ Trade ledger: SQLite `memory_bank.db` (survives restarts)
- ✅ Open positions tracked in `open_positions` table
- ✅ Trade history queryable via `/trades` endpoint
- ✅ Equity reconciliation: nightly summary vs broker statement

---

### ✅ OPTIONS TRADING (COMPLETE)

**Strategies Supported**
1. **Long Call/Put** (directional, small size)
   - Entry: IV rank < 30 + price mean-reversion
   - Exit: +50% profit, -50% loss, 21 DTE rule
   
2. **Short Put (CSP)** (premium selling, cash-secured)
   - Entry: IV percentile > 60 + support at strike
   - Exit: +50% profit (premium collected), -100% loss (assignment risk), 21 DTE rule
   
3. **Short Call (CC)** (covered call, capital preservation)
   - Entry: Holdings + OTM at +10% overhead
   - Exit: +50% profit, 21 DTE rule, assignment
   
4. **Iron Condor** (range-bound, vol contraction)
   - Entry: 30-DTE, center at current price ±1 std dev
   - Exit: +50% max profit, 21 DTE, breach

**Pricing & Greeks**
- **Black-Scholes:** Implemented, updated daily at market open
- **Greeks:** Delta, Gamma, Vega, Theta computed per position
- **IV Rank:** Updated from Alpaca chains (volatility regime gating)
- **Skew Detection:** IV skew ratio computed hourly for contradiction vector

**Position Monitoring (PositionManager)**
- 30-minute polling cycle: fetch Alpaca prices, check exit rules
- Database-backed: persistent across engine restarts
- Telegram alerts on all exits (success + failure)
- Graceful API error handling (returns -1.0 on fail, skips that cycle)

---

### ✅ REAL-TIME ANALYSIS (WS1 — CONTRADICTION VECTOR)

**Contradiction Detection**
- Triggers when equity signal ≠ options signal
  - E.g., "RSI bullish entry" + "IV skew puts breakeven south" = contradiction
- Scored via NLP sentiment (news) + options Greeks alignment
- **Status:** Fully implemented; threshold-gated (override with `CONTRADICTION_BYPASS=true`)

**Signal Sources (WS1-3)**

| Workstream | Detection | Status | Override Env |
|-----------|-----------|--------|---------------|
| **WS1** | Contradiction (equity vs options) | ✅ Live | `CONTRADICTION_BYPASS` |
| **WS2** | Alternative macro (insurance, AIS, OFAC) | ✅ Live | `ALT_MACRO_BYPASS` |
| **WS3** | Insider cluster (Form 4, 2+ execs 48h window) | ✅ Live | `INSIDER_CLUSTER_BYPASS` |

**Signal Quality (Skeptic Pipeline)**
All three workstreams feed the 6-stream analyzer:
- **WS4:** Regime detection (RSI, BB squeeze, correlation matrix)
- **WS5:** Liquidity-adjusted position sizing
- **WS6:** Weekend batch report + Skeptic confidence scores

---

### ✅ END-OF-DAY & WEEKLY REPORTING (WS4 + WS6)

**Daily Telegram Reports** (via heartbeat/monitor.py)
- Market regime: Risk-on / Transition / Risk-off
- Open positions: Ticker, entry, current P&L, exit rules
- Realized P&L: Day's closed trades + aggregate
- Time: 4:00 PM ET (market close)

**Weekly Reports** (Sunday 6 PM ET, batch)
- Correlation matrix (10 tickers vs VIX)
- Insider activity: Form 4 filings, cluster scoring
- Liquidity regime: VWAP spreads, option skew
- Skeptic confidence score (WS6)
- Saved to `/app/data/` (persistent volume)

**Trade Ledger (`/trades` endpoint)**
- Full history: entry, exit, P&L, exit reason, date
- Query API: `/trades`, `/trades?ticker=AAPL`, `/trades?limit=10`
- Queryable by exit type: `[webhook]`, `[rule:XXX]`, `[trailing_stop_close]`

---

### ✅ PAPER/LIVE SEAMLESSNESS

**Broker API Switching**
- All Alpaca URLs now read from `ALPACA_BASE_URL` env (C++ + Python)
- heartbeat/monitor.py: Fixed 6 hardcoded URLs → use `ALPACA_BROKER_URL` env
- **Result:** Flip `.env` `ALPACA_BASE_URL` to live → entire system switches (execution + reports + portfolio)
- ✅ VERIFIED: Reports show correct live/paper account balance

---

### ✅ DATA FETCH RELIABILITY

**Retry Logic**
- SEC, NewsAPI, Polygon: 3 retries + exponential backoff (1s → 2s → 4s)
- Timeout: 5s connection, 10s read (RULE-008 compliant)
- Fallback: Reports refuse to generate if **any critical feed** is down (fail-safe)

**Persistent Logging**
- All containers log to `./logs/` (persistent volume mount)
- Docker JSON driver: 50MB max per file, 10-file rotation
- Crash logs survive container restarts
- ✅ VERIFIED: June 28 incident (silent crashes) resolved

---

## 3. DEFCON TRIAGE (REAL CURRENT BLOCKERS)

### 🟢 **DEFCON 0: SYSTEM STABLE**

**Current Status:** Paper + small live ($5k) trading fully operational.

- ✅ All equity orders executing (buy/sell/trailing stops)
- ✅ All option positions tracking (monitor thread polling every 30min)
- ✅ Trade ledger recording all exits correctly (fixed July 1)
- ✅ Reports showing correct live/paper data (fixed July 1)
- ✅ Persistent logging capturing all signal flow (fixed July 1)
- ✅ Telegram notifications firing reliably

**No production fires. No critical data loss. No silent failures.**

---

### **DEFCON 2: Planned Work (Next Priority)**

#### **IBKR Migration** (on `nocturnal` branch, NOT on `main`)
- **Why:** Alpaca lacks futures, non-US options, multi-leg order routing. IBKR adds all three.
- **Status:**
  - ✅ `IBKRClient.hpp/cpp`: TWS API async socket, SPSC ring buffer, `ExecutionLogger` sqlite backend
  - ✅ 500k-item concurrent stress test passed (no loss/duplication)
  - ✅ Compiles clean (validated against TWS stub headers)
  - ❌ **NOT wired up:** Missing 5 integration pieces:
    1. Order construction helpers (Contract/Order builders for 8 strategies)
    2. `OptionsSignal` → IBKR contract + order mapping
    3. Replace PositionManager's Alpaca quote fetch with IBKR streaming ticks
    4. Integrate into `main.cpp` behind venue flag (Alpaca | IBKR)
    5. Vendor TWS API source under `third_party/` + CMake/Make target

- **Next steps:** Pick one of the 5 above, wire it up, test on paper IBKR account.
- **Timeline:** Estimate 2-3 weeks for full integration (if full-time focus).

---

#### **Enhanced Insider Clustering** (future enhancement)
- Current: 48-hour window, ≥2 execs (static threshold)
- Idea: Score cluster probability (Bayesian), weight by exec rank (insider vs. officer vs. director)
- **Status:** Deferred; current implementation sufficient for live trading

---

#### **Chinese A-Share Expansion** (dormant, ready)
- CN settlement gate (`CN-RULE-002`) fully implemented
- Awaiting IBKR integration to enable live CN trading
- Currently testing on Alpaca paper (which doesn't enforce T+1 gate)
- **Status:** Code ready, execution broker gate needed

---

### **DEFCON 3: Known Limitations (not blockers)**

| Limitation | Impact | Workaround | Priority |
|-----------|--------|-----------|----------|
| Options: 30-min polling | Misses high-vol exits in <30min | Add 5-min polling for near-threshold positions | Low |
| Options: Always market orders | Slippage on wide spreads | Could add limit-order mode (broker-side) | Low |
| Position entry: Manual | No auto-integration | Signal generation must call `add_position()` after fill | Medium |
| IV skew: 1-hour refresh | Stale contradiction vector | Could increase to 15-min poll | Low |
| Insider cluster: Static threshold | May miss subtle clusters | Implement Bayesian scoring (future) | Low |

---

## 4. ACTIVE WALLET RULES (FINANCIAL FIREWALL)

### ✅ IDE Controls (Claude Code)

**Protected Files (Off-Limits to Editing)**
```
# .cursorignore and .claudesignore configured with:
memory_bank.db              # Trade ledger — never edit
data/                       # Market data cache, reports — read-only
.env                        # Secrets (API keys, tokens) — never edit
.env.example               # For reference only
logs/                       # Crash/signal logs — read-only
```

**Allowed Edits**
- Source code: `src/`, `execution/`, `heartbeat/`, `analyst/`, `america_data_engine/`, `china_data_engine/`
- Config: `docker-compose.yml`, `config/` (non-secrets)
- Docs: `docs/`, `README.md`, markdown files
- Tests: `tests/`, `backtest/`

**Why This Matters**
- ❌ **NEVER** let AI edit `memory_bank.db` — trade ledger is ground truth for P&L
- ❌ **NEVER** commit `.env` with live API keys — use `.env.example` as template
- ✅ **ALWAYS** read `.cursorignore` / `.claudesignore` before suggesting edits

---

### 🎯 Claude Code Model Routing

**Daily Task Execution**
- **Haiku 4.5:** Quick code reviews, bug fixes, simple refactors
  - Cost-effective for routine work
  - Suitable for isolated, well-scoped tasks
  - Example: "Fix the trailing-stop calculation" (known scope)

- **Sonnet 5:** Complex design, multi-file refactors, research
  - Better reasoning for architectural decisions
  - Suitable for "design the IBKR integration" tasks
  - Example: "Plan the IBKR wiring; what are the 5 integration steps?" → returns structured plan

- **Opus 4.8:** Architecture disputes, adversarial review, novel problem-solving
  - Full capability for high-stakes decisions
  - Use when you're genuinely uncertain and want a second opinion
  - Example: "I think the options grid search is wrong; review it and propose a fix"

**Rule:** Match model to task scope. Don't overkill; don't undershoot. When in doubt, Sonnet is the safe bet.

---

### 🔒 Secrets Management

**Live API Keys Location**
- Stored in `.env` (not in repo; in `.gitignore`)
- Mounted into containers at runtime via `docker-compose.yml`
- **Never** committed to git; **never** logged to stdout

**Alpaca Paper vs Live**
```bash
# .env (example; actual keys redacted)
ALPACA_BASE_URL=https://paper-api.alpaca.markets    # Paper (default)
ALPACA_API_KEY=<paper-key>
ALPACA_SECRET_KEY=<paper-secret>

# To flip to live (dangerous; do this only when ready):
ALPACA_BASE_URL=https://api.alpaca.markets
ALPACA_API_KEY=<live-key>
ALPACA_SECRET_KEY=<live-secret>
```

**Danger Zone:** Flipping to live without testing on paper first = catastrophic loss. **Always** test on paper first; verify with `/portfolio` Telegram command; then flip.

---

### 📊 Trade Ledger Integrity

**Source of Truth**
- SQLite `memory_bank.db` at `./data/memory_bank.db` (persistent volume)
- Schema: `open_positions`, `trade_history`, `position_monitor_log`
- **Backup:** Persists across container restarts; survives crashes

**Reconciliation**
- Nightly: Engine compares ledger vs Alpaca account statement
- Discrepancies logged to Telegram with severity (WARN / CRITICAL)
- **Auditing:** `/trades` endpoint exports full history (JSON)

**Write Access**
- **Only:** `execution-engine` (C++) and `analyst-brain` (Python) write
- **Never:** Edit manually, never allow Claude to modify, never drop tables
- **Verification:** Run `SELECT COUNT(*) FROM trade_history;` to sanity-check

---

## 5. COMMAND REFERENCE (TELEGRAM / HTTP ENDPOINTS)

### Real-Time Telegram Commands
```
/portfolio       → Live account balance + positions (reads from ALPACA_BASE_URL)
/trades          → Full trade history from ledger
/signals         → Recent signals (last 24h)
/regime          → Current market regime (Risk-on / Transition / Risk-off)
/cn_status       → CN A-share gate state + tracked positions (T+1 check)
/details TICKER  → Detailed analysis for a single ticker
```

### HTTP Endpoints (Flask on heartbeat:8002)
```
GET  /portfolio              → JSON account + positions
GET  /trades                 → JSON trade history
GET  /trades?ticker=AAPL     → Trades for AAPL
GET  /recent-signals         → Last 100 signals with source tag
GET  /iv-snapshot            → Current IV percentiles (options)
GET  /skeptic-report         → WS6 batch report (JSON)
GET  /health                 → Service health check
```

### Execution Engine Internal HTTP
```
POST /webhook               → External signal entry (analyst, custom signals)
GET  /status                → Engine state (running, positions, regime)
GET  /cn_status             → CN settlement gate state
```

---

## 6. DEPLOYMENT CHECKLIST (GO-LIVE READINESS)

### ✅ Before Paper Trading
- [x] `.env` file populated (API keys, tokens, watchlist)
- [x] Docker Compose running all 7 services
- [x] Persistent volume mounts at `./data` and `./logs`
- [x] Telegram bot token + chat ID verified (test message sent)
- [x] Alpaca paper account created
- [x] Trade ledger (`memory_bank.db`) initialized
- [x] SSL certificates (if HTTPS needed)

### ✅ Before Small Live ($5k Account)
- [x] Paper trading tested for **1 week minimum** (no losses)
- [x] All exit rules verified in paper (take-profit, stop-loss, etc.)
- [x] Telegram alerts working reliably
- [x] `/portfolio` command shows correct account (paper → live flip safe)
- [x] Nightly ledger reconciliation passing
- [x] .cursorignore / .claudesignore protecting secrets + ledger
- [x] Backup of `.env` and `memory_bank.db` (keep safe copy)
- [x] Risk limits validated (Kelly %, position size, leverage cap)

### ⏳ Before IBKR Integration (Live CN + Futures)
- [ ] IBKR integration fully wired (all 5 components above)
- [ ] Paper testing on IBKR account (1-2 weeks)
- [ ] T+1 gate tested with actual CN A-share holdings
- [ ] Streaming ticks validation (tick ring buffer tested under load)
- [ ] Multi-leg orders (Iron Condor) tested on IBKR paper
- [ ] Kill-switch tested (manual stop trading via Telegram `/pause`)

---

## 7. GIT BRANCH STRATEGY

**Both `upstream/Nox` (public) and `private/Nocturnal` (private) now use `main` as their primary branch.**

### `upstream/main` (PUBLIC, SAFE FOR SHOWCASE)
- Production paper trading code
- Alpaca-only (no IBKR, no multi-leg futures)
- All tests passing
- Mergeable to live trading accounts ($5k+)

### `private/main` (PRIVATE, EXPERIMENTAL)
- IBKR integration work (in-progress)
- Multi-source news pipeline (Polygon, NewsAPI backup)
- Enhanced analysis (WS1-6 tuning, volatility models)
- **Never push IBKR or proprietary edge to `upstream/main`** (stays confidential edge research)
- User's personal account strategies

### Feature Branches (Off `main`)
- Prefix: `feature/`, `fix/`, `wip/`
- Test locally before PR
- Merge only to `main` if production-ready
- **For public work:** Cherry-pick public-safe commits from `private/main` to `upstream/main` when ready

---

## 8. NEXT ACTIONS (YOUR CALL)

### Option A: Stabilize Paper Trading Further
1. Run paper trading for 2 weeks; monitor for any edge cases
2. Tune watchlist (add/remove tickers based on signal quality)
3. Audit Kelly % sizing; consider dynamic position scaling
4. **Time estimate:** 1-2 weeks, low risk

### Option B: Begin IBKR Integration
1. Pick one of the 5 wiring tasks; implement & test
2. Set up IBKR paper account; connect via TWS
3. Stream live ticks; validate ring buffer under load
4. **Time estimate:** 2-4 weeks (1 per wiring task)

### Option C: Enhance Signal Quality (WS1-6)
1. Implement Bayesian insider cluster scoring
2. Add 5-minute polling for near-threshold options
3. Test contradiction vector on live signals (optional CONTRADICTION_BYPASS)
4. **Time estimate:** 1-2 weeks, medium risk (affects entry signals)

---

## SIGN-OFF

✅ **System Status:** STABLE (no fires, all systems operational)
✅ **Financial Firewall:** Configured (.cursorignore + .claudesignore protecting data + secrets)
✅ **Deployment:** Live on Hostinger VPS (24/7, persistent)
✅ **Model Routing:** Haiku for routine, Sonnet for design, Opus for disputes
✅ **Ready for:** Live $5k account OR IBKR integration work

**Last Updated:** 2026-07-02 by System Audit  
**Next Audit Due:** 2026-07-16 (2 weeks)
