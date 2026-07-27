# Journal: July 18, 2026 — A Barbell for the Human, and the Gate I Refused to Build

## Verify before you build

The session opened with a long list of "fixes" spanning five phases —
operational plumbing, execution/sizing bugs, strategy-scoring logic, data
pipelines, performance tracking. The temptation with a list like that is to
start typing. Instead the first move was to check the *actual code* for every
item, because a lot of the list had already been quietly addressed in earlier
work and the notes describing it were stale. Roughly ten of sixteen items
turned out to already be done and verified in the source. That's not wasted
effort — knowing what's already true is what lets you spend the rest of the
session on what genuinely isn't.

## The barbell belongs to the human, not the bot

The idea was to run personal trading as a barbell: a disciplined "core"
sleeve doing the high-probability setups, and a small, capped "satellite"
sleeve for speculative moonshots — with a hard firewall between them so a
string of lottery-ticket losses can never bleed into the serious capital. The
insight worth writing down is that this system already has the machinery to
make that a *mechanical* rule instead of a matter of willpower. Every manually
logged trade now carries a bucket tag; the speculative sleeve has a capital
cap; and crossing it (or re-funding a losing speculative sleeve from fresh
money) throws a warning. It warns rather than blocks — a deliberate choice to
keep the human in control while still making the discipline visible.

The subtle part was in the reporting. A moonshot sleeve is *supposed* to lose
most of the time and win big occasionally; that's the whole point of the
asymmetry. So blending it into one "personal win rate" both flatters the
sleeve that should lose often and slanders the disciplined core. The fix was
to measure the two apart and compare only the core, apples-to-apples, against
the automated system.

## The gate I was asked to build, and didn't

A sharp analysis came in arguing the scoring logic was "intoxicated" by the
gap between implied and realized volatility, and that the fix was a hard rule:
if a trade's mathematical expected value is negative, force its quality score
to zero. The expected-value math in the argument was airtight *on its own
terms*.

But checking it against the real code told a different story. First, the
volatility term was a fifth of what the analysis assumed — it explained only a
small slice of the ranking gap it was blamed for; the real driver was a
trend-distance term entirely. Second, and more important: an expected-value
gate needs a *win probability*, and this system has no measured directional
edge to supply one. Its own research proved that months ago. Feeding a gate a
made-up probability would produce something that *looks* rigorous while being
astrology — and there's a standing rule here against exactly that: an invented
number must never be given the power to size or suppress a trade.

So I built the honest half instead. The poor risk-to-reward veto that the
system *already computed* but only ever displayed is now an actual gate — and
risk-to-reward is a real, measured quantity from the strikes, not a guess. And
a post-earnings buffer: don't buy directional options into the volatility
crush right after a report, when the move is most exhausted. That premise — the
earnings date — is a known fact, so it's allowed to gate. The expected-value
gate, with its unmeasurable input, stayed on the shelf.

## Two more traps, both structural

Short-dated options got a structural rule: under two weeks to expiry, the long
leg must be in-the-money. An out-of-the-money short-dated option is a theta
trap — decay annihilates it before the underlying can travel far enough to
matter. Again, both inputs (days-to-expiry, delta) are measured, so the rule
is honest.

And a data-pipeline bug worth its own paragraph: the fundamentals screen was
reading cumulative year-to-date cash-flow figures out of quarterly filings and
treating them as single-quarter numbers — overstating a company's cash burn by
two or three times and firing false fragility alerts on any second- or
third-quarter filing. The fix isolates the actual single quarter, preferring a
discrete quarterly figure when the filing provides one and otherwise
subtracting the prior cumulative period. When it can't isolate cleanly, it
returns nothing rather than a confidently wrong number.

## The theme

Every gate that shipped this session judges something *measured* — realized
risk-to-reward, days to expiry, an earnings date, a period duration. The one
that didn't ship was the one that needed a number nobody can measure. The best
version of "make the bot smarter" was, in one case, refusing to.
