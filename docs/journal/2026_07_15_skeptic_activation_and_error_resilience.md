# Journal: July 15, 2026 — Making the Skeptic Actually Do Something

## The question that started this

Talking through where the trading edge actually comes from, the answer
wasn't "better analysis of the same free data everyone has" — it's partly
*which* data gets fetched and how many independent read on it exist before a
trade sizes itself. That led to checking on the multi-workstream "Skeptic"
research pipeline that had been built earlier: contradiction detection,
insider-cluster scanning, physical-vs-political macro verification. All of
it ran, all of it produced a weekly report. None of it — except the very
first workstream — actually touched a trade.

## Only one of six was wired in

Going through the execution path confirmed it: the contradiction-vs-skew
check was the only workstream that ever reached the position-sizing
calculation. The insider-cluster scanner and the alt-macro physical-supply
verifier were both fully built, both exposed over an internal API, and both
completely unconsumed by anything that places an order. That's a real gap,
not a tuning problem — no amount of adjusting thresholds on a system that
isn't in the decision path changes anything.

## A decision layer, not another special case

Rather than bolting each workstream onto the sizing code separately (which
is how the first one got wired in, and exactly why the second and third
never followed), this became one small, pure decision function: take a
trade's direction, take whatever the independent research signals say, and
return a single size multiplier plus a suppress/allow verdict. Aligned
conviction across signals scales a position up; disagreement scales it
down; a genuinely hard contradiction — a live macro release the market
hasn't priced yet pointing the opposite way, or a headline that contradicts
what the physical data shows — blocks the entry outright rather than just
shrinking it. Every threshold that decides "how much" is a plain
configuration value, not a hardcoded literal, so it can be adjusted without
touching the decision logic itself. The whole thing is unit-tested against
synthetic inputs with no network call in sight, the same way the portfolio
risk-cap logic elsewhere in this codebase is tested — the aggregation math
and the live data fetch are two separate, separately-verifiable things.

## A genuine information-lag edge, not just more analysis

Added a new independent research input built on the same "physical/timing
reality beats the narrative" principle the macro-verification workstream
already uses, just applied to a different asymmetry: a home-market economic
data release lands hours before the instruments most exposed to it, trading
on a different exchange, have had a full session to react. The signal
distinguishes a release that's still inside that reaction lag (worth acting
on directionally) from one that's already been fully priced (worth treating
as confirmation only, not a trigger) — using an existing "has this
propagated to the outlets that would cover it" check as the freshness test.

## Does the strategy survive being wrong sometimes?

Separately, and directly prompted by wanting to know what happens under
real-world operating conditions rather than a clean backtest: added an
error-injection mode to the backtester. It takes the exact trade history a
clean run produces and replays it with three specific operational failures
injected at a chosen rate — a duplicate fill that never got reconciled, an
exit rule that silently failed to fire, a fill that landed materially worse
than the model price — each modeled as its *actual* counterfactual (what
would this trade's P&L have been if it had genuinely ridden to expiry
instead of exiting early, not a guessed penalty) rather than an assumed
haircut. The report is a direct side-by-side: clean P&L vs. P&L with errors,
and a plain profitable/not-profitable verdict. The point isn't to simulate a
specific incident — it's to price the exposure that exists *because* the
defenses built earlier in this project (order reconciliation, exit
monitoring, fill discipline) are the thing keeping these failure rates near
zero in practice.

## What's still open

Everything here is verified against synthetic/mocked inputs, not a single
live network call yet — same shape as every other phase of this project:
build and unit-test against a controlled input first, then watch it run for
real before trusting the result. That watching starts now; the plan is
multiple months of live paper-trading observation before revisiting any of
these thresholds.
