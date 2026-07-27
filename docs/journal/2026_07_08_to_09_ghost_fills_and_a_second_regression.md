# Journal: July 8-9, 2026 — Ghost Fills, and a Second Regression

## What I was building

Two deliberate infrastructure phases planned out earlier in the week: making
sure the bot can never lose track of whether an order actually reached the
broker, and making the audit trail for *why* a signal did or didn't fire
something I could query instead of infer from silence.

## The one thing signal-driven logic can't self-correct

The execution philosophy from earlier this week is deliberately light on
retry logic — the belief being that a bot trading multi-day option positions
doesn't need to race a broken connection, because the next scan cycle will
regenerate the trade if the setup is still valid. That philosophy has exactly
one blind spot: if an order fills at the broker and the confirmation response
gets lost on the way back (a timeout, a dropped connection), the local system
has no way to notice that on its own — it just looks like nothing happened,
and the next cycle would happily submit the same order again. Signal
intelligence can't correct a fact it never received.

Built the fix around that specific blind spot rather than a general-purpose
retry system: every order now gets a locally-generated ID written to a ledger
*before* the request goes out, a reconciliation check against the broker's
own order records runs at the start of every scan cycle, and a short-lived
duplicate-submission block catches anything that slips through in between.
Tested against a mock broker rigged to simulate exactly the failure modes
that matter: a rate limit, a server error, a lost response after the order
actually filled, and a malformed success response — all four resolve safely,
and exactly one order ever reaches the (mock) broker in the fill-then-lost-
response case, which was the actual point of the exercise.

## The second regression

While reviewing the signal-scoring logic this infrastructure sits on top of,
I found something worse than a bug: a commit from six days earlier, labeled
as adding retry utilities, had silently deleted several hundred lines of
unrelated scoring and risk logic from the options signal generator and
replaced it with a much thinner version — no crash, no error, nothing that
would show up in a log, because the file still compiled and ran fine. It just
made worse decisions the entire time, including a full trading-strategy path
that had gone completely dormant while its configuration sat there looking
active. Caught it by noticing a commit's diff size didn't match its stated
purpose, then confirmed by diffing it against its parent commit directly
rather than trusting the message.

Fixed it by manually re-integrating the missing logic into the current file
rather than a blind revert — the file had also grown legitimate new logic
since the regression (the ghost-fill work above, for one), and a revert would
have thrown that out along with the bug. Same day, found and fixed a related
but separate scoring bug: an oversold-condition threshold had been drawn wide
enough to misclassify a legitimate rebound setup as bearish, which would have
pointed the bot toward the wrong side of a trade at the worst possible
moment.

## Making silence legible

The last piece this week was aimed at a specific gap: the system could tell
you a signal fired, but had no record of *why* a signal didn't. Added an
event log that captures every suppression point a signal can hit on its way
to an order — blocked by a volatility-structure gate, blocked by a liquidity
check, blocked because a position cap was already hit, blocked because a
near-identical order was already pending — so "did this stay quiet because
the setup passed but got gated, or because the underlying condition never
came back" is now a query instead of a guess. Paired it with a live,
persisted daily P&L figure for both unrealized and realized moves (previously
this only existed as console output during a live monitoring loop, gone the
moment that loop's window scrolled past it), and replaced a same-day-snapshot
proxy for how "expensive" an option's volatility is with a calculation
against its own trailing year of history — the proxy could tell you
volatility was high right now, not whether it was high *for this specific
instrument*, which is the actual question the position sizing built on top
of it needed answered.

## What's still open, honestly

All of this — the ghost-fill handling and the audit trail both — is tested
against a mock broker and seeded data, not the real thing. The natural next
validation step for both is a paper-trading window against the live broker
API, and that hasn't happened yet. I'm noting that explicitly rather than
letting "tests pass" read as "verified in production" — those are different
claims, and I want to keep being careful about which one I'm actually making.
