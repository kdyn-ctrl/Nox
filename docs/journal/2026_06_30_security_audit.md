# Journal: June 30, 2026 — Auditing My Own Blast Radius

## What I was building

A full self-directed audit: leak scan, correctness review, and cleanup, after realizing I'd been building fast enough that I wasn't confident I could name every risk in the system from memory anymore.

## What the audit found

Running the audit against my own assumptions (rather than just adding features) turned up several things I would not have caught by continuing to build forward:

- A position-safety check (no naked short calls) existed for one broker integration but was completely missing for a second one I'd added more recently — an inconsistency I'd introduced without noticing, because I fixed the first instance and assumed the pattern would carry over.
- A monitoring loop had no crash supervision — if it died, nothing would restart it or tell me. I flagged this myself as "the exact failure mode from two days ago" (see the June 28-29 entry), which was an uncomfortable but useful thing to notice: I'd fixed one instance of silent failure and shipped a second one in different code within 48 hours.
- An order-execution endpoint was reachable from the public internet, protected only by a shared secret in the request body rather than any real authentication.
- A rule meant only for one specific market's share-lot sizing was being applied globally, silently rounding stock orders for other markets down to zero shares — trades that looked like they were happening (no error) but never actually placed an order.
- A placeholder domain name I'd used temporarily (rather than pay for a real one yet) was still sitting in outbound request headers and documentation, which I scrubbed once I noticed it didn't belong in anything meant to be shown to anyone else.

I fixed all of the above in the same session, prioritizing by which ones could actually lose money or leak access, not by which were easiest.

## A mistake that happened live, and how I handled it

Near the end of a long session, I pasted what were actually live trading API credentials directly into a chat window — not the codebase, just carelessness in a text field I didn't think through. I caught it (with help) essentially immediately: stopped, was told plainly to treat the credentials as compromised regardless of intent, and rotated them right away rather than assuming "I'll fix it later" was good enough. It's not a flattering thing to include, but it's true, and how I responded to it (immediate rotation, no rationalizing that it was probably fine) matters more to me than pretending it never happened.

## What I'd tell someone about this period

The most useful realization from this day wasn't any individual bug — it was noticing that a fix I'd shipped for one incident (silent crashes) hadn't actually been applied everywhere it needed to be, in a different service, less than 48 hours later. Fixing an instance of a problem and fixing the *pattern* are not the same thing, and I now try to ask "where else does this exact shape of bug exist" every time I close one out, instead of only closing the ticket in front of me.
