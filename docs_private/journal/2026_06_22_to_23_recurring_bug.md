# Private Journal: June 22-23, 2026 — The Bug I Fixed Three Times

*Private mirror of [docs/journal/2026_06_22_to_23_recurring_bug.md](../../docs/journal/2026_06_22_to_23_recurring_bug.md) with real specifics kept out of the public version.*

## Concrete detail

- The `/status` bug: a Python f-string producing MarkdownV2 used the literal sequence `\-` as a "manual escape," which Python doesn't recognize — it silently produced a literal backslash-dash, which the real `esc()` helper then double-escaped into garbage. Fix #1 (June 22 evening) wrapped one occurrence in `esc()`. Fix #2 (minutes later, fresh session with no memory of fix #1) found fix #1 itself was broken (the `\-` was still there, now double-wrapped). Fix #3 (June 23 overnight) found `esc()` existed and was correct but simply wasn't being called on the values causing the malformed output in a third location. Only fully closed once every message-assembly path was audited for the same shape of bug, not just the one throwing an error that session.
- `.env` deletion: a stray `.swp` file suggested a crash mid-edit. Recovered live values by `docker exec`-ing into already-running containers and reading env vars directly, since the containers hadn't restarted yet. No real secrets-backup existed at this point — this is a gap, not a system.
- Near-leak: a VPS IP + webhook secret were briefly hardcoded into a Pine Script strategy file and also present in `INDEX.md`/`docker-compose.yml`. Caught before push, scrubbed via `git-filter-repo` across all 13 local commits at the time. Compare to the June 30 and July 2 entries — a similar leak *did* get past me later, so this catch, while good, wasn't yet backed by any automated check (that came later, July 2).
- Risk-profile design pushback: I (the user) rejected sharing one `RiskProfile` between personal trading and bot-driven trading — correctly, in retrospect — leading to two separate parameterized presets. Good example of a design decision I drove, not just accepted.

## Interview angle

The three-times bug is probably the single best "tell me about a time you had to debug something tricky" answer in the whole project — it's small enough to fully explain in two minutes, has a clear moment of being wrong twice, and ends with an actual lesson (patch the pattern, not the instance) that generalizes. Use it for [[03_bot_interview_questions]] item 17 as a *secondary* example if the June 28 silent-crash incident feels too heavy for the moment — this one is lower-stakes and shows the same underlying discipline.
