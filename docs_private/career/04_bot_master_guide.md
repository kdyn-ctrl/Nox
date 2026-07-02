# Master Guide — Know Nox/Nocturnal Like It's Part of You

Goal: after reading this, you should be able to explain any piece of this system from memory, in your own words, without looking at code. Organized as a narrative you can retell, not a reference manual.

## The one-sentence pitch
"A trading system that trades US and China equities/options, where every signal has to survive being contradicted by an independent data source before it's trusted, risk is gated at the execution layer (not just the strategy layer), and the whole thing recovers from its own failures because I've already been burned once by it failing silently."

## The story, in order (this is also your "walk me through it" answer)

**1. Data comes in from multiple independent sources**, split into two engines because US and China market structure differ enough to warrant it (settlement cycles, disclosure norms, capital controls, trading hours/circuit breakers — be ready to name at least 2 concrete differences you actually rely on).

**2. Every signal has to be cross-checked before it's trusted — this is the core design philosophy (Skeptic architecture).**
- **Contradiction vector (WS1):** text sentiment (lexicon-scored, signed + magnitude) is checked against options IV skew. If they agree → CONFIRM. If they disagree in the "informative" direction → CONTRADICT_*. If bullish text lines up with bearish-but-positive skew in a way that's actually just noise → IGNORE (this distinction matters — not all disagreement is informative, some is just how skew normally behaves).
- **Alt macro (WS2):** physical-world stress (tanker traffic deviation, marine insurance premium changes) vs political/news signal, per geographic chokepoint (Hormuz, Red Sea, Black Sea, Venezuela). When they conflict, physical data wins, because physical flows are harder to fake/spin than headlines.
- **Insider cluster filter (WS3):** SEC Form 4 filings, but only open-market buys (transaction code "P"), explicitly excluding Rule 10b5-1 (those are pre-scheduled and carry no new information). Requires ≥2 distinct officers/directors buying within a rolling 48-hour window — a cluster, not a single filing, because one insider buying is weak signal and easy to explain away (diversification, compensation timing); a cluster within 48h is much harder to explain as coincidence.

**3. Signals decay — nothing is trusted forever (WS4, the foundational layer).**
Every signal belongs to a `SignalCategory` and decays exponentially with its own half-life (λ = ln2/half-life — standard radioactive-decay-style math, same shape as a first-order chemical reaction, which is a genuinely useful mental model for you specifically). On top of steady decay, a **regime reset** can happen — detected via a VIX jump (`detect_volatility_catalyst`) — which is a "consume-once latch": it fires once per catalyst, forces signals to be treated as stale, and won't re-fire until reset. This exists because a market regime change (e.g., a shock) makes pre-shock signals actively misleading, not just old.

**4. Macro/weekend layer runs on a separate cadence (WS6).**
A Saturday-morning cron job (`weekend_batch.sh`) runs the WS1-3 pipeline directly (no HTTP round-trip — calls the Python functions in-process) and produces a scored report (HIGH/MEDIUM/LOW conviction per workstream) as both JSON and markdown, because weekly/weekend research doesn't need live-service latency and running it as a batch is simpler and cheaper.

**5. Before anything reaches an order, it passes through the liquidity gate (WS5).**
`LiquidityGate` tracks a rolling per-symbol baseline of relative bid/ask spread and computes a z-score; if the current spread is too many standard deviations wide (thin/stressed liquidity), it aborts the trade. Two important design choices to be able to defend:
- **Fail-open on bad/missing data** — if the gate can't compute a spread, it lets the trade through rather than blocking it. The reasoning: a broken data feed shouldn't be able to silently halt all trading (that's its own risk), so the tradeoff is "occasionally trade without this specific protection" vs. "a data hiccup takes down the whole execution path."
- **Warm-up period** — the gate doesn't act until it has enough history to have a real baseline, avoiding false aborts on a cold start.
- Placed **after the notional ceiling check** in the execution order — position sizing is checked first (a cheap, static check), then market-quality is checked right before the trade actually fires (a more expensive, live check) — cheapest/most certain filters first.

**6. Positions, once opened, are tracked through a real ledger with rule-based exits.**
This came from fixing a real bug: `add_position` was dead code and options positions were never actually being sold — found and fixed this along with building out `trade_history` as a proper ledger, equity persistence with reconciliation (paper vs. live), and rule-based equity exits. Reports (EOD/EOW) and a `/trades` endpoint were built on top of the ledger so performance is inspectable, not just inferred from logs.

**7. Data reliability is defensive by design, learned the hard way.**
All SEC/news/API fetches have retry/backoff. Reports **refuse to generate on incomplete data** — a report built on partial data is worse than no report, because it looks authoritative while being wrong. This mirrors the earlier incident (see below) — the system now fails loud/refuses rather than failing silent.

**8. The incident that shaped the current logging architecture.**
On 2026-06-28, containers crashed silently and the analyst→execution communication broke without anyone noticing until it had already been dark for a while. Fixed by adding persistent logging across the docker-compose stack for both the analyst and execution containers, verified analyst-execution communication was restored (completed 2026-07-01). This is your best "tell me about production failure" story — know the specifics: which containers, what "silent" meant (no crash log, no alert, just stopped producing output), what the logging change actually captures now, how you'd know today if it happened again.

**9. Public vs. private split.**
`main` (Nox, public GitHub) contains the public-safe subset of the architecture; `nocturnal` (private) holds the actual edge — e.g., the multi-source news fallback pipeline (Alpaca→NewsAPI→Polygon→RSS) lives only on `nocturnal`. This split is itself a talking point: you understand IP/edge protection, not just "open source everything," which is a real consideration at any actual trading shop.

## Things to be able to derive on a whiteboard, not just recite
- The exponential decay formula and why λ=ln2/half-life gives you that half-life at t=half-life (basic derivation: N(t)=N0·e^(-λt), solve N(half-life)=N0/2).
- Why a z-score is the right tool for "is this spread abnormal" (assumes roughly stationary local distribution of spread; z-score = (current - mean)/stdev over the rolling window).
- The full order of operations from signal → contradiction check → decay/regime check → position sizing → liquidity gate → order → ledger update. If asked to draw this as a flowchart from memory, you should be able to.

## Gaps to be honest about (don't pretend these don't exist)
- No stated out-of-sample backtest with realistic costs yet — see [[02_quant_evaluation_criteria]].
- Fusion logic across workstreams is rule-based, not learned/weighted — you have a reasoned defense (interpretability, data scarcity) but no empirical comparison yet.
- Sentiment scoring is lexicon-based, not a language model — deliberate choice (no heavy NLP weights in containers, memory-constrained), but be honest about the accuracy ceiling this implies.

Read this file end to end again right before any interview, and re-derive section by section out loud without looking — that's the "feel like it's part of you" test.
