# Journal: July 26, 2026 — Shedding Dead Code, Fixing the Data Pipeline, and Re-focusing the Engine

## Filling the Gap: Data Integrity & Pipeline Hardening (July 19–25)

The days following the July 18 barbell and trade-math implementation revealed several operational and pipeline failure modes that required systematic hardening:

1. **The Broken Earnings Calendar:** Alpaca's corporate-actions API was discovered to not support an `earnings` type at all, causing earnings calendar lookups to fail silently or return empty lists. Sourcing was cleanly migrated to Finnhub's earnings calendar API, chunking request batches to prevent payload truncation.
2. **SEC Filing Staleness & iXBRL Parsing:** The SEC filing scraper was occasionally surfacing months-old 8-K/6-K disclosures as fresh market-moving news because filing timestamps were missing from inline-XBRL header blocks. Stripping the viewer prefix and enforcing strict date validation eliminated stale filing noise in morning scout reports.
3. **Dead-Man's Switch (`job_supervisor.py`):** Rather than letting scheduled background tasks fail silently inside swallowed `except` blocks (RULE-D3), a dedicated job supervisor was added to track per-job failure counters, alert loudly on consecutive errors, and emit explicit recovery notifications.
4. **Satellite Pilot Isolation:** To prevent experimental signals from cluttering the primary trading interface, sports predictions and event-forecasting pilots (`world_events/` for Kalshi paper forecasting) were isolated to dedicated bot channels, keeping the main Telegram channel strictly focused on options/equity signals and execution.

---

## Today's Work: Purging Unofficial Scrapers & System Debloating

Today's focus was a thorough audit and cleanup of obsolete artifacts and technical debt across the codebase:

### 1. Removing Unofficial & Unused Brokerage Scrapers
The unofficial Robinhood client (`robin_stocks`), Plaid link importer (`plaid_fills_importer.py`), pasted-text confirmation block parser (`robinhood_confirmation_parser.py`), and Plaid link setup script (`plaid_link_once.py`) were permanently removed from the repository.

* **Security & Reliability Rationale:** Storing plain-text brokerage passwords and TOTP secrets in environment variables for unofficial scrapers carried login-lockout risks and security smells.
* **Double-Counting Prevention:** Running multiple auto-importers alongside IBKR Flex Web Service introduced deduplication edge cases (`investment_transaction_id` vs. broker `order_id`). Standardizing fills importer logic on official IBKR Flex Web Service + explicit manual Telegram entries (`/trade` + `/close`) eliminated ghost fill and double-counting bugs by construction.

### 2. Container & Environment Optimization
- Removed `plaid-python`, `robin-stocks`, and `pyotp` from `heartbeat/Dockerfile`, reducing image size and build complexity.
- Cleared obsolete `ROBINHOOD_*` and `PLAID_*` entries from `docker-compose.yml`, `.env.example`, and `.env`.
- Cleaned up docstrings and import self-checks in `heartbeat/monitor.py` and `heartbeat/barbell.py`.

### 3. Public Repository & Showcase Polish
- Updated `README.md` with standard technology and status badges (`C++17`, `Python 3.11+`, `Docker Enabled`, `Status: Paper Trading`) and refreshed the system documentation overview.
- Verified 100% clean test execution across the C++ execution engine and Python test suites (133/133 tests passed).
- Verified zero secret leaks via `gitleaks` baseline scan.

---

## Reflection

A codebase grows in complexity by default unless actively pruned. Removing Robinhood and Plaid scrapers eliminated hundreds of lines of fragile parsing code and reduced the system's attack surface without losing any execution capability. The resulting architecture is leaner, more secure, and strictly focused on official brokerage channels.
