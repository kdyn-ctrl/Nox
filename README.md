# OpenClaw Trading System

OpenClaw is a containerized, multi-agent quantitative trading framework designed for automated market regime analysis, risk-managed portfolio allocation, and low-latency execution. The system features a decoupling of macro analysis from order execution, running via an internal Docker bridge network.

---

## System Architecture

The project is divided into two decoupled, core executable modules built in C++ and a background Python diagnostic microservice.



### 1. The Analyst Agent (`/analyst`)
* **Heartbeat:** Runs an autonomous 24-hour execution cycle.
* **Regime Engine:** Evaluates macro structural market states (`RISK_ON`, `TRANSITION`, `RISK_OFF`) by feeding live data points from the Yahoo Finance API (VIX index metrics) and Alpaca Data API (SPY price relative to its 200-day Simple Moving Average).
* **Data Routing:** Serializes system state metrics into a clean JSON transmission payload and fires a POST request over the Docker bridge network to the execution engine webhook.

### 2. The Execution Server (`/execution`)
* **Network Gateway:** Spins up a blocking, multi-threaded `httplib` HTTP server listening strictly on port `8080/webhook`.
* **Authentication:** Screens incoming network buffers via a dedicated security shield (`WEBHOOK_SECRET_TOKEN`). 
* **Risk & Sizing Gate:** Intercepts validated payloads, triggers an active portfolio equity check against Alpaca, runs mathematical allocation sizing via a custom Kelly Criterion calculator, passes an RSI boundary check, and dispatches linear orders directly to the Alpaca API.

### 3. The Heartbeat Scout (`/heartbeat`)
* **State Tracker:** A background Python microservice monitoring active processing health.
* **Data Processing:** Polls external news feeds and regulatory data (SEC EDGAR filings), utilizing a local SQLite database engine to index and guarantee transaction persistency.

---

##  Critical Design & Safety Constraints (System Laws)

Any modifications, refactors, or feature additions made to this codebase must adhere strictly to these operational guardrails:

* **Fail-Secure Enforcement:** The engine must practice zero-tolerance credential management. If any required environmental variable (`WEBHOOK_SECRET_TOKEN`, `ALPACA_API_KEY`, `ALPACA_SECRET_KEY`) is missing or null at startup, the system **must execute a hard abort (`std::exit(1)`)**. Fallback strings or silent degradation states are strictly prohibited.
* **Thread Protection & Timeouts:** To eliminate memory leaks, container freezes, or thread-pool exhaustion, all network clients (HTTP/HTTPS connections to Alpaca, Telegram, Yahoo Finance) must have explicit, hardcoded connection and read timeout thresholds.
* **Zero Manual Parsing:** Payload analysis must never rely on manual pointer arithmetic or character-counting loops. Use native stream validation libraries (`nlohmann::json`) to reject malformed payloads before heap allocation strings can trigger system vulnerabilities.
* **Multi-Asset Framework:** Order formatting must isolate spot equities from derivative parameters (Options/Futures contracts). Derivative execution profiles require independent contract objects tracking expiration dates, strikes, and Greeks.

---

##  Deployment & Workspace Isolation

To maintain an enterprise portfolio while protecting active capital deployment strategies, this framework utilizes a **Core-and-Wrapper Git Submodule Architecture**:

```text
openclaw-trading/               <-- Private Deployment Wrapper
│
├── .env                        <-- Live Account Production Credentials (Git-Ignored)
├── config.production.json      <-- Custom Portfolio Sizing Weights
│
└── core/                       <-- Public Engine Framework (Tracks OpenClaw Core Repo)
    ├── analyst/                <-- Public C++ Analyst System
    ├── execution/              <-- Public C++ Engine Server
    └── docker-compose.yml
