# Journal: June 22-23, 2026 — The Bug I Fixed Three Times

## What I was building

Statistical validation tooling (a Monte Carlo permutation test to check whether a strategy's edge is distinguishable from randomly shuffled returns), plus the first pass at options trading logic — position sizing, IV-based signals, order routing.

## The bug that wouldn't stay fixed

A Telegram `/status` command kept breaking with malformed output. I diagnosed it, patched it, and moved on — three separate times across two days, and it came back every time.

- **First pass:** found a Markdown-escaping issue and wrapped the offending string in the existing escape helper.
- **Second pass, hours later:** still broken. Turned out my first "fix" introduced a new bug — I'd used an invalid Python escape sequence, which produced garbage that then got double-escaped.
- **Third pass, the next day:** still broken. The escape helper existed and worked correctly, it just wasn't actually being called on the values causing the problem — the fix I thought I'd applied wasn't fully wired in.

Each of the first two "fixes" addressed a symptom in one specific code path rather than the systemic issue: escaping wasn't applied consistently everywhere it needed to be. It only actually stopped recurring once I stopped patching the immediate error and looked at every place that assembled the message.

## Other real issues from this stretch

- Test infrastructure had drifted from the code it was testing — a Kelly-sizing test called an API that no longer existed in that shape, and regime-classification tests had wrong expected values that needed correcting against actual (verified) behavior, not the other way around.
- While wiring an early webhook integration, I nearly shipped a VPS IP address and a secret baked directly into a config file that would have been pushed to the repo. I caught it before it went out and scrubbed local history — a good catch, but in hindsight not yet a systematic habit, since a similar class of leak got past me later in the project (see the June 30 and July 2 entries).
- `.env` got deleted mid-session (looked like a crash during an edit). I recovered the values by reading them live out of already-running Docker containers rather than from disk. That worked because the containers happened to still be up — a lucky save, not a designed recovery path, and a reminder that I didn't have a real secrets-backup story at this point.
- A stale flag was silently suppressing a scheduled report whenever I'd triggered one manually — a small thing, but the kind of "the code is technically doing what I told it to" bug that's easy to miss because nothing throws an error.

## What I'd tell someone about this period

The three-times bug is the most honest artifact from this week. It's a clean example of the difference between patching a symptom and fixing the root cause, and I only recognized the difference after being wrong twice.
