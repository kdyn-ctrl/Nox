# Journal: July 12, 2026 — A Third Asset Class, Signals Only

## What I was building

An extension of the existing signal-generation pattern into a new asset
class: futures. The instinct going in was that this would mean standing up a
new market-data vendor integration from scratch. It turned out to mostly be
a reuse story instead.

## The broker doesn't have the data, but the macro pipeline already does

The current broker integration for equities and options has zero futures
support at the data layer, full stop — not a configuration gap, the vendor
simply doesn't offer it. The other broker client in this codebase (built for
a different asset class entirely) does have a path to futures market data in
principle, but it's gated behind a funded live account and paid exchange
subscriptions that aren't in place yet, so that route is a dead end for now.

The actual unlock was noticing that an existing macro-signal pipeline — built
originally to compare political announcements against physical supply data
(insurance premiums, shipping traffic, sanctions actions) for a completely
different purpose — already produces a directional bias for the exact
commodity a first futures signal would want to track. No new macro logic
needed to be written; the new generator just became a second consumer of
data that was already being computed and served internally.

## Signals only, deliberately

The new generator follows the same shape as the existing signal generators in
this codebase (own scan loop, own alert path, no shared execution
infrastructure) but with one hard constraint: there is no order-submission
code path in it at all. It fetches an optional price feed if one is
configured, fetches the existing macro signal, combines the two into a
directional bias with a quality score, and writes the result to its own
audit table. If the optional price feed isn't configured, it falls back
cleanly to macro-only — the price layer was never a hard dependency, which
turned out to matter once cost became a real consideration (see below).

Tested the combination logic directly (bias-from-macro-agreement,
bias-from-macro-contradiction, momentum-only fallback, no-data case, and a
clamp so a degenerate input can't produce a quality score outside a sane
range) without needing a mock server for any of it — the logic itself is
pure arithmetic once the two inputs are on the table, so there was nothing
that actually needed mocking.

## The cost question turned out to matter more than the code

Once the generator was built, the real decision wasn't architectural — it was
whether to pay for a market-data subscription at all before knowing if the
underlying signal is worth trading on. Landed on: don't. Ship the free,
macro-only version first, run it for real, and only spend money on the price
layer once there's evidence the free version is producing something worth
paying to sharpen. That's the same "prove it out before scaling spend"
logic this project has applied to every other paid-data decision so far.

## What's still open

Live-fired now, alt_macro-only, on one contract. No price-momentum layer
yet (deliberately deferred pending cost justification), no additional
contracts, and — same as every other phase of this project — no live/paper
validation beyond "the logic is unit-tested and it started up clean." That
validation is watching it run for real over the next stretch, not something
a test suite can shortcut.
