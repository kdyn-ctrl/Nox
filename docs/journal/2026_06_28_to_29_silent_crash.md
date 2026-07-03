# Journal: June 28-29, 2026 — The Incident That Changed How I Log

## What I was building

This is where the "Skeptic" architecture was conceived — the idea that no single signal should be trusted until it's been checked against an independent, harder-to-fake source. It started as a routine status check ("why aren't trades happening") that grew into designing a real signal-verification pipeline, plus a risk-prioritized sprint to close gaps that could lose real money: missing limit orders, no earnings-date gate, no duplicate-position protection, no account-level drawdown breaker.

## The incident

I noticed I hadn't gotten an analyst report in a while. Digging in, both the analyst and execution containers had restarted under Docker's auto-restart policy — meaning they had crashed silently at some point the day before, mid-cycle, and nobody (including me) knew until I happened to notice the silence. Because Docker's default logging is in-memory and gets wiped on restart, the actual root cause of the crash was already gone by the time I looked. I could not reconstruct what actually happened.

I want to be honest about what that means: I never found out why it crashed. What I could do was make sure it couldn't happen invisibly again. I added persistent, rotating log files to every service in the docker-compose stack, so a future crash leaves a trail instead of vanishing the moment the container restarts.

## What else broke this week

- A live options order started failing with a type error from the broker. Traced it to a serialization bug — some numeric fields were being converted to text before being sent as JSON, which the broker's API correctly rejected. Fixed it consistently across every order path that had the same pattern, not just the one that had actually failed yet.
- A `gmtime` call I'd been using elsewhere in the codebase (which uses a shared buffer and isn't safe when multiple things call it at once) turned up again in a spot I'd missed the first time I fixed this class of bug — caught by re-auditing for the same pattern rather than assuming one fix covered every instance.
- Realized the news pipeline had exactly one source with no fallback — a single point of failure I'd been trading on without noticing. Designed a fallback chain instead of a single provider.

## What I'd tell someone about this period

This is the incident I'd lead with if asked "tell me about a real production failure." Not because the fix was clever — adding persistent logging is about as basic as infrastructure gets — but because I can be specific about what "silent" actually meant here (no crash log, no alert, the system just stopped producing anything and I had to notice the absence myself), and specific about what changed so I'd know immediately today if it happened again. I never got the satisfaction of a root cause. I got the more useful outcome of not needing one next time.
