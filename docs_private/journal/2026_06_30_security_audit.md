# Private Journal: June 30, 2026 — Auditing My Own Blast Radius

*Private mirror of [docs/journal/2026_06_30_security_audit.md](../../docs/journal/2026_06_30_security_audit.md) with real specifics kept out of the public version.*

## Concrete detail

- Covered-call collateral check existed for Alpaca but was completely missing for the IBKR path — naked short calls were possible on IBKR specifically. Fixed by adding the same check there.
- The Telegram bot ran `infinity_polling()` without `none_stop=True` — no crash supervision. I flagged this myself, unprompted, as "exactly your June 28 silent-crash failure mode" — a good, specific self-audit moment, but also proof I'd shipped a second instance of the same failure class within 48 hours of fixing the first one.
- `/webhook` (real order execution) was reachable from the public internet via Traefik, protected only by a shared secret in the request body — no real auth. Fixed alongside the rest.
- `CN-RULE-001` (a Chinese A-share board-lot sizing rule, default lot size 100) was being applied globally, including to US equities — nearly all US Kelly-sized quantities rounded down to 0 shares and silently aborted with no error. Changed the default to 1. This bug reappeared later (July 1) because the running Docker binary predated the source fix — a recurring theme: this project's Dockerfiles copy a pre-built binary rather than compiling in-container, so "I fixed it in source" and "it's fixed in production" are genuinely different claims here until a rebuild happens.
- `.env.example` was missing ~40 of ~60 real config keys; regenerated including `KELLY_*` key *names* (not values).
- A placeholder domain used temporarily in SEC EDGAR User-Agent headers (since I didn't want to pay for a real domain yet) was scrubbed from code and docs once I noticed it didn't belong anywhere user-facing.
- **The in-chat credential paste:** at the end of a long session, I pasted live Alpaca API key/secret strings directly into the chat. Caught essentially immediately, told plainly to treat them as compromised regardless of intent, rotated right away. Not flattering, included on purpose — the response (immediate rotation, no "it's probably fine") is the part worth being able to talk about.
- Separately, a real SQLite permissions bug: `data/` was root-owned while the container ran as `appuser`, hard-crashing `PositionManager` init; fixed by making that failure non-fatal and removing a `chown -R root:root` step in the heartbeat container that had been clobbering permissions on every restart.
- Confirmed the Skeptic pipeline (WS1 contradiction vector) was healthy but simply too conservative to fire (0 contradictions across 27 tickers that day) — correctly distinguished as "working as designed, just conservative" rather than misdiagnosed as a bug.

## Interview angle

For [[02_quant_evaluation_criteria]] item #7 (risk controls as a first-class citizen) — the public webhook exposure and the board-lot silent-zero bug are both good, specific answers to "what's the scariest bug you've shipped," because neither one crashed anything or threw an error — they just quietly did the wrong (safe, in the board-lot case; unsafe, in the webhook case) thing. The credential-paste incident is the honest answer if asked about a personal-process mistake rather than a code mistake — worth having ready since "have you ever made a security mistake yourself" is a fair question and pretending otherwise reads as less credible than owning it.
