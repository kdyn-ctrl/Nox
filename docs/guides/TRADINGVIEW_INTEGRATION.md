# TradingView ↔ Nox Integration Guide

> How to use TradingView alongside Nox — either fully independently, or wired
> into Nox's execution engine so TradingView alerts inject live trade signals.
>
> Verified against `execution/main.cpp` (`/webhook` handler at line 1603,
> `TradeSignal` struct at line 43) and `docker-compose.yml` (Traefik labels,
> lines 136–140) as of 2026-06-29.

---

## TL;DR

Nox exposes exactly one externally-reachable ingestion endpoint: **`POST /webhook`**
on the execution engine (port 8080). It authenticates with a `secret_key` field
**inside the JSON body** — which is exactly what TradingView webhooks can send.

You have two modes:

1. **Independent** — TradingView is your charting/alerting tool. Nox runs its own
   scanner. No integration needed. (Start here.)
2. **Integrated** — a TradingView alert fires an HTTP POST to Nox `/webhook`,
   which sizes and routes an order through Alpaca.

---

## Mode 1 — Run TradingView independently (recommended to start)

Nothing to configure in Nox. Use TradingView for charts, indicators, and alerts
delivered to your phone/email. Nox's own `OptionsSignalGenerator` threads keep
scanning and trading on their own schedule (`execution/main.cpp:1794`).

**Watch out for this:** once you integrate, both TradingView *and* Nox's scanner
can place trades on the same account. During paper testing, decide which is the
source of truth, or use separate paper accounts, so you can tell whose fill is
whose. There is no de-duplication between the two paths.

---

## Mode 2 — Wire TradingView alerts into Nox

### How a signal flows (verified path)

```
TradingView alert  ──HTTPS POST──▶  Traefik  ──▶  execution-engine:8080 /webhook
                                                        │ (main.cpp:1603)
                                                        ▼
                                          secret_key auth gate (main.cpp:1616)
                                                        │  (mismatch = silent drop, still HTTP 200)
                                                        ▼
                                          build TradeSignal (main.cpp:1621)
                                                        ▼
                                          record_signal()  → /recent-signals
                                                        ▼
                                          process(signal)  (main.cpp:539)
                                              BUY  → Kelly/tier sizing → Alpaca /v2/orders
                                              SELL → close position (T+1 gated)
                                              HOLD / REPORT → no-op
```

---

## Step 1 — Get a reachable webhook URL

### Option A — You have a domain/VPS (production setup)

If your server has a static IP and a domain, Traefik handles TLS automatically
(`docker-compose.yml:137-140`). Your URL is:
```
https://<your-domain>/webhook
```

> **Note on temporary domains:** If you previously used a Hostinger/vanhellsing
> domain, that is no longer in use. Update `docker-compose.yml` Traefik labels
> with your current domain before pointing TradingView at it.

### Option B — No domain yet (ngrok tunnel for testing)

The fastest way to test TradingView integration without committing to a domain:

1. Install ngrok: `curl -sSL https://ngrok-agent.s3.amazonaws.com/ngrok.asc | sudo tee /etc/apt/trusted.gpg.d/ngrok.asc && sudo apt install ngrok`
2. Start a tunnel to the execution engine: `ngrok http 8080`
3. ngrok gives you a temporary HTTPS URL like `https://abc123.ngrok-free.app`
4. Use `https://abc123.ngrok-free.app/webhook` in TradingView

**Limitations:** ngrok URL changes every restart (free tier). Use it for testing
only — once you're happy, set up a real domain.

### Option C — Direct IP (not recommended)

TradingView webhooks require HTTPS. A raw `http://IP:8080/webhook` will not work.
If you have a VPS IP without a domain, use Option B (ngrok) to get a free TLS URL.

### Get your secret

Your `WEBHOOK_SECRET_TOKEN` from `.env` is the auth credential. It goes in the
JSON body as `secret_key`.

---

## Step 2 — Create the alert in TradingView

1. Open a chart and add your indicator/strategy.
2. Click the **alert clock icon → Create Alert**.
3. Set your condition (indicator cross, price level, strategy order, etc.).
4. Under **Notifications → Webhook URL**, paste your webhook URL from Step 1.
5. In the **Message** box, paste the JSON payload below.

TradingView substitutes `{{ticker}}`, `{{close}}`, etc. at alert fire time.

---

## Step 3 — The payload schema

The webhook accepts a **single JSON object** or a **JSON array** of objects.
Fields (`TradeSignal` at `main.cpp:43-58`, parsed at `main.cpp:1621-1684`):

| Field | Type | Required | Notes |
|---|---|---|---|
| `secret_key` | string | **yes** | Must equal `WEBHOOK_SECRET_TOKEN`. Wrong/missing = silent 200. |
| `ticker` | string | yes (effectively) | Defaults to `"UNKNOWN"` if omitted. |
| `action` | string | **yes** | `BUY`, `SELL`, `HOLD`, or `REPORT`. Only `BUY`/`SELL` place an order. |
| `price` | number or string | recommended | Entry/reference price. |
| `risk_tier` | int | recommended | `0`/omit = Kelly sizing; `1`, `2`, … = fixed-percent tier. |
| `rsi` | number | no | Optional indicator context. |
| `vol` | int | no | Volume. |
| `atr` | number | no | Used with `stop_loss_atr_multiplier`. |
| `stop_loss_atr_multiplier` | number | no | Default `2.0`. |
| `vix` | number | no | Default `20.0`. |
| `spy_price` | number | no | Regime context. |
| `spy_200_sma` | number | no | Regime context. |
| `trade_date` | string | no | `"YYYY-MM-DD"` — backtest / CN T+1 use only. |

**Minimum viable BUY alert message:**
```json
{"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"{{ticker}}","action":"BUY","price":{{close}},"risk_tier":1}
```

**SELL (close) alert message:**
```json
{"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"{{ticker}}","action":"SELL","price":{{close}}}
```

**Multiple signals in one alert (array form):**
```json
[
  {"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"AAPL","action":"BUY","price":150.0,"risk_tier":1},
  {"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"MSFT","action":"BUY","price":410.0,"risk_tier":1}
]
```

---

## Step 4 — Verify your alerts are actually working

> **Critical:** TradingView always reports HTTP 200 — even when the `secret_key`
> is wrong or the payload is malformed — because Nox silently drops bad requests
> to avoid fingerprinting the auth boundary (`main.cpp:1616`). **Never trust
> TradingView's "sent successfully" confirmation alone.**

Use this verification checklist every time you change your payload or URL:

### Method 1 — Telegram confirmation (fastest)
Every successfully parsed signal triggers a Telegram message:
```
🚀 Signal Parsed: BUY AAPL
```
If you don't see this message within ~5 seconds of a TradingView test fire, the
signal was dropped. Common causes: wrong `secret_key`, malformed JSON, action typo.

### Method 2 — Manual curl test (best for debugging payload)
Test your exact payload directly from the machine running Nox:
```bash
# Inside the docker network (from the host machine)
docker compose exec execution-engine curl -s -X POST \
  http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"AAPL","action":"BUY","price":150.0,"risk_tier":1}'
```
Expected response: `Processed 1 signal(s)` (or `Processed 0 signal(s)` if auth failed).

If you get `Processed 0 signal(s)` with a valid-looking payload, your secret is
wrong. Double-check `.env` WEBHOOK_SECRET_TOKEN and rebuild the container.

### Method 3 — `/recent-signals` endpoint
Nox stores the last 50 received signals (auth-verified only) in memory:
```bash
docker compose exec execution-engine curl -s localhost:8080/recent-signals | python3 -m json.tool
```
If your signal is in the list, it was authenticated and parsed. If it's not, the
auth gate dropped it silently.

### Method 4 — Telegram `/signals` command
Send `/signals` to your bot. It proxies `/recent-signals` from the execution
engine and shows the last N parsed signals with timestamps. Same data as Method 3
but accessible from your phone.

### Method 5 — Docker logs
```bash
docker compose logs -f execution-engine | grep -E "Signal Parsed|WARN.*auth|BUY|SELL"
```
A successful parse logs: `[INFO] Signal Parsed successfully: BUY AAPL`
An auth failure logs: `[WARN] [EXECUTION] Unauthorized signal silently dropped`

### Quick debug checklist
- [ ] Telegram fires `🚀 Signal Parsed`? → signal reached the engine
- [ ] `/recent-signals` shows the signal? → auth passed
- [ ] Alpaca paper account shows an order? → `process()` routed it
- [ ] `action` is exactly `BUY` or `SELL` (not `buy`/`sell` — case sensitive)
- [ ] `secret_key` matches `.env` `WEBHOOK_SECRET_TOKEN` exactly (no extra spaces)
- [ ] JSON is valid — paste your payload into `jsonlint.com` if unsure
- [ ] TradingView's `{{ticker}}` placeholder resolves correctly (e.g. `AAPL` not `AAPL1!`)

---

## Step 5 — Test on paper first

1. Confirm `.env` Alpaca keys point at **paper**: `ALPACA_BASE_URL=https://paper-api.alpaca.markets`
2. Use TradingView's **"Send test notification"** button on the alert (or set a
   condition that fires immediately on a known ticker).
3. Watch for `🚀 Signal Parsed` in Telegram (Method 1 above).
4. Check Alpaca paper account → Orders tab for the fill.
5. Run `/signals` in Telegram to see the full signal record.

---

## Important caveats (read before going live)

1. **Stocks only via webhook.** `main.cpp:895` builds a stock Contract. There is
   no webhook path for options — those are generated by Nox's internal scanner.
2. **No duplicate protection.** A retry or duplicate alert = a second order.
   TradingView can retry on timeout. Watch for double-fills during paper testing.
3. **`action` is case-sensitive.** Must be exactly `BUY` or `SELL`. `buy`/`sell`
   → `HOLD` (default) → no-op, no order placed.
4. **Two trade sources.** Nox's scanner runs regardless. Decide which is truth.
5. **Public exposure.** The `/webhook` endpoint is internet-reachable. Lock it
   down before live money (see Security below).

---

## Security hardening (do before live trading)

- **IP-allowlist TradingView at Traefik.** TradingView publishes its webhook
  source IPs (check TradingView docs — they change). Add a Traefik
  `ipAllowList` middleware to the `execution-webhook` router so only those IPs
  (plus your own VPS/home IP) can POST to `/webhook`.
- **Rotate `WEBHOOK_SECRET_TOKEN`** if it has ever been shared.
- **Keep on paper** until you've seen a full week of alerts behave correctly.

---

## Extending Nox for richer TradingView control (optional, needs code)

Clean insertion point is beside the existing handler at `execution/main.cpp:1603`:

- **Options via webhook:** add a branch that, when `action` is `BUY_CALL`/`BUY_PUT`,
  constructs an `OptionsSignal` (`execution/OptionsSignalTypes.hpp:14`) and routes
  it through `OptionsOrderRouter` instead of `process()`.

---

## Quick reference

| Item | Value |
|---|---|
| Endpoint | `POST https://<your-host>/webhook` |
| Content-Type | `application/json` |
| Auth | `secret_key` field in body = `WEBHOOK_SECRET_TOKEN` |
| Handler | `execution/main.cpp:1603` |
| Struct | `TradeSignal`, `execution/main.cpp:43` |
| Router | `process()`, `execution/main.cpp:539` |
| Confirm receipt | Telegram `🚀` / `curl /recent-signals` / Telegram `/signals` |
| Debug auth failures | `docker compose logs execution-engine` grep `WARN.*auth` |
| Paper safety | `ALPACA_BASE_URL=https://paper-api.alpaca.markets` in `.env` |
| Temp HTTPS tunnel | `ngrok http 8080` → use the ngrok URL in TradingView |
