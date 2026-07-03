# Journal: June 24-27, 2026 — "Already Done" Isn't the Same as Working

## What I was building

An 8-angle review of the options engine to get it ready to show as a real portfolio piece, plus early IBKR client scaffolding and a broader codebase sweep.

## The pattern that defined this week

Almost every session this week started from a belief that something was already finished, and an audit found it wasn't:

- An "IV rank" calculation had been computing rank from a single snapshot's min/max instead of real 52-week history — it always returned roughly the same middle value regardless of actual market conditions, silently feeding wrong numbers into position-sizing math.
- A position-monitoring system had real exit logic (profit targets, stop-losses, DTE limits) written and looking complete, but the method that was supposed to register a new position into that table was never actually called anywhere. Every executed options trade was invisible to its own exit logic.
- The engine did not compile at all for a period — a shipped binary was several days stale because two struct members were used in code but never declared. I had been running (and reporting on) a binary that didn't reflect any of the recent source changes.
- A parallel statistics test was silently dropping most of its permutations and using a test statistic that didn't actually depend on the data, always returning a meaningless result while looking like it worked.
- A qty field coming back from an API as a string was silently coercing to zero in one specific check, defeating a safety validation without ever raising an error.

Fixing the IV-rank issue also surfaced that the *real* fix lived only in a different service (Python) and was never reachable from the C++ order path that actually needed it — meaning my first "fix" earlier in the week was still silently using the old broken logic underneath.

## A real loss, handled as well as I could

A private planning document — including a candid section of my own weaknesses and interview prep notes — leaked onto the public repo. I ran a full history rewrite to scrub it, which had a side effect I didn't anticipate: it rewrote history on every branch, including the private one, and wiped the same file there too. A recovery attempt made it worse, and the file came back empty with its content permanently gone. I made the pragmatic call to accept the loss, delete the empty placeholder, and rewrite the internal-facing document from scratch rather than chase a recovery further.

## What I'd tell someone about this period

The through-line here isn't any single bug — it's that "I wrote this" and "this runs correctly in production" turned out to be different claims, repeatedly, and I only found the gap by auditing rather than trusting my own memory of what was done. The git-history incident is the clearest lesson: a destructive fix for one problem (a leak) can create a second, unrelated problem (permanent data loss) if I don't fully think through its blast radius first. I know that now specifically because I didn't think it through the first time.
