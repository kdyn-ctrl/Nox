# Journal: July 1-3, 2026 — Reliability, a Real Leak, and Undoing My Own Over-Engineering

*This entry covers the broader arc of this window; see [2026_07_02_tradingview_pivot.md](2026_07_02_tradingview_pivot.md) for the same-day documentation-vs-reality correction in more detail.*

## What I was building

Retry/backoff on every external data fetch (a market data outage or a flaky API call had been failing silently and getting treated the same as "there's genuinely nothing to report"), and reconciling ~19 commits' worth of features between my private working branch and the public-facing repo.

## The dead code that mattered most

The function responsible for actually recording an executed options position into the tracked-positions table was never called. Every options trade placed through the automated path was invisible to the exit logic meant to manage it — no take-profit, no stop-loss, nothing, because the position was never in the table those rules read from. The trade ledger table was empty for the same reason, which meant every performance report I'd generated that week was structurally reporting "no trades" regardless of what had actually happened in the market. I tested the fix against a throwaway copy of the database before touching the real one, which is a habit I'm glad I'd built up by this point rather than something I did by luck.

In the same pass, I caught something worse than a missing feature: a newer rule-based exit path bypassed a settlement-timing safeguard for one specific market, which could have triggered an actual regulatory violation (an illegal same-day round-trip) rather than just a bad trade. Caught and fixed before it ran live.

## The leak, done right this time

A security sweep before merging private work into the public branch found that the merge had actually pulled real trading-edge code — a broker integration meant to stay private, an entire second market's data engine, and a personal email address — onto the public repo. My first instinct was to just delete the leaked files. I stopped myself before doing that, because the code was load-bearing — deleting it outright would have broken a live, running pipeline, not just removed a nice-to-have. I re-scoped the fix instead: a proper denylist, an automated secret-scanner, and a pre-push check so this class of leak gets caught before it ships rather than after. Then cleaned the history.

I'm including this next to the June 30 leak specifically because the response was different and better: not reactive panic-deletion, but a pause to ask what the fix would actually break, then a structural fix (automation) instead of a one-time manual cleanup.

## Undoing a decision I'd made too eagerly

The private/public repo split had been a reasonable security response earlier in the project. By this point it was actively hurting my own workflow — files ending up on the wrong branch, a rebase that diverged badly enough I had to abort it. I said so plainly (to myself, in the actual conversation) and consolidated back to a single working tree with two remotes and an automated pre-push safety check instead of a structural branch split. Correct decisions can stop being correct once the situation changes, and recognizing that is not the same as admitting the original decision was wrong.

## A smaller, honest correction

I misread a request to update one specific career-planning document as a request to abandon and restructure a much larger plan, and pushed that wholesale rewrite before checking. It was caught quickly and reverted to the narrow, correct change. Small mistake, but worth keeping in here — not every error in this project is a production incident; some are just "I moved faster than I understood the ask."

## Where things stand, honestly

The system had a real live test run this week that exposed genuine flaws in the exit logic — a stock got bought roughly a dozen times in a loop because the rule-based exits never got a chance to fire before a broker-side trailing stop closed the position first, and every exit that fired that week was a loss, not a gain. That's not a flattering result and I'm not going to describe it as one. It's exactly the kind of thing paper-trading is supposed to surface before it costs anything real, and it's currently being worked through rather than solved.

## What I'd tell someone about this period, and what's still open

I still have a half-life configuration change that I made inconsistently (one part of it reads from environment variables, another part quietly reverted to hardcoded defaults, and I haven't gone back to reconcile which one actually wins). I still have two retry-logic implementations doing the same job in different places that should be one. The system is not finished, and I don't think "finished" is the right frame for something that trades — it's closer to "currently correct, under continuous adversarial testing against real market behavior," and I expect the exit-logic failures from this week specifically to keep shaping near-term work: getting real fill/exit data logged well enough that I can say something evidence-based about what actually works, not just what I intended to work.
