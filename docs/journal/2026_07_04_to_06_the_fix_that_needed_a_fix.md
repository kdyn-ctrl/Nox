# Journal: July 4-6, 2026 — The Fix That Needed a Fix

## What I was building

Continuing straight off the trading-day gating work from July 3: closing the
gaps it exposed, patching a stale macro data feed a wider report-quality audit
had flagged, and starting the first piece of a longer infrastructure-hardening
push.

## The bug my own bug-fix shipped

The trading-day gate I'd just shipped on July 3 was supposed to stop the bot
from generating signals when the market was closed. It had its own bug: this
year's Independence Day fell on a Saturday, which US markets observe by
closing the preceding Friday instead — a detail my hardcoded holiday list
didn't account for. The list said "closed Saturday," so the gate correctly
blocked Saturday but incorrectly let the bot run and post signals on Friday,
when the market was actually closed. A feature meant to prevent trading on
non-trading days let the bot trade on a non-trading day, for one specific
date, because of exactly the kind of manual-list drift the feature itself was
supposed to guard against.

The honest fix wasn't "add another special case" — it was to stop hardcoding
holiday dates at all. I switched the trading-day check to fetch the official
market calendar directly from the broker's own calendar endpoint (which
already accounts for weekend-observance shifts), keeping the old hardcoded
list only as a fallback if that call fails. That closes the whole class of
bug, not just this one date.

The same week, the strict "don't send a report if any data source is
unavailable" rule I'd built for the after-hours narrative report turned out to
conflict with the point of having an after-hours report at all — on the
holiday itself, every data source was quiet because the market was closed, so
the report correctly found nothing to validate and correctly sent nothing,
leaving the user with silence on exactly the day they'd want a status update.
Loosened it: on a real trading day, still refuse to report on incomplete
data; on a non-trading day, send what's available with an explicit
"incomplete" warning instead of nothing.

## A slower-moving data problem

A broader audit of report quality (not this week's main focus, but surfaced
by it) caught a macro data feed serving the identical value for over five
consecutive days with no staleness warning anywhere — the upstream source was
returning valid-looking, successful responses that had simply stopped
updating weeks earlier. No fetch failure, no error, nothing that would trip
an "is this working" check, because the check I had only asked "did the
request succeed," not "is this number actually moving." Added an explicit
staleness threshold and a fallback data source, and made the report label
which source actually served the number, so a future freeze is visible on
its face instead of silent.

## Also this week

Started an isolated backtester skeleton for a market I trade under different
structural rules (settlement timing, price-move limits, transaction tax) than
the main US-equities path — deliberately strategy-agnostic and not wired into
anything live yet, just the scaffolding so those rules exist in one tested
place before any strategy gets layered on top. And wrote up the
execution-philosophy and infrastructure roadmap that the rest of this month's
work follows. One deliberate decision in it worth flagging: the system is
designed to *not* retry on its own when an order's fate is unclear, trusting
the next scan cycle to regenerate a signal naturally if the underlying
condition still holds. That's a considered call, cross-checked against
outside critique before committing to it, not an oversight — the one place
that philosophy doesn't apply is knowing for certain whether an order
actually reached the broker, which is where the next stretch of work went.

## What I'd tell someone about this period

The Friday-trading bug is the one I'd lead with if asked about a mistake in
this window — not because it caused damage (caught the same day, nothing was
actually mispriced by it) but because it's a clean example of a fix creating
a smaller version of the exact problem it solved. The right response wasn't
a patch, it was removing the reason a human-maintained list could drift from
reality in the first place. I'd rather point to "I replaced the failure mode"
than "I patched around it."
