# Interview Question Bank — Nox/Nocturnal Trading System

Semi up-to-date as of 2026-07-02 (post: trade ledger/exits, data-fetch reliability, docker logging fixes, WS1-6 complete). Re-derive/update this after major architecture changes — it will go stale as the code evolves.

## Tier 1 — "Walk me through it" (expect this first, always)
1. What does this system actually do end to end, from data ingestion to a live order?
2. Why two separate engines for US and China? What's structurally different between the markets that required that split?
3. What's your Sharpe/drawdown, out of sample, after costs? *(Have a real number ready — see [[02_quant_evaluation_criteria]] item 1. If you don't have this yet, say so honestly and explain what you'd need to compute it — don't bluff.)*
4. Why C++ for execution/analyst but Python for data engines? (Answer: latency/determinism for the order path vs. iteration speed and library ecosystem for scraping/NLP-lite pipelines — be ready to defend, not just describe.)

## Tier 2 — Architecture / design decisions
5. Explain the regime decay math — why exponential decay, why per-`SignalCategory`, how did you pick λ (=ln2/half-life)?
6. What is `trigger_regime_reset` and `detect_volatility_catalyst` actually detecting, and why key off VIX jumps specifically?
7. Walk me through the contradiction vector: why does bullish text + bearish (positive) skew resolve to IGNORE rather than CONTRADICT?
8. Why discard Rule 10b5-1 filings in the insider cluster filter? What information do pre-scheduled sales/buys actually carry (or not carry)?
9. Why require ≥2 distinct officers within 48h for the insider cluster signal instead of any single large filing?
10. Explain the liquidity gate: what's being z-scored, over what window, and why fail-open on bad data instead of fail-closed?
11. Why is the liquidity gate placed after the notional ceiling check in the execution order, not before?
12. What's the chokepoint fusion logic in `alt_macro.py` doing when physical and political signals disagree, and why does physical data win?
13. Why lexicon-based sentiment scoring instead of an embedding/transformer model? What's the accuracy tradeoff and why did you accept it?
14. How does the weekend batch job differ from the live pipeline — why run Skeptic report generation on a cron instead of continuously?

## Tier 3 — Failure modes / stress questions (they want to see if you've thought about what breaks)
15. What happens if a data source (SEC EDGAR, news feed, market data) goes down mid-session? Walk through your retry/backoff and what happens if it never recovers.
16. You mentioned reports refuse to generate on incomplete data — what's the actual definition of "incomplete," and what's the failure mode if that gate is wrong (too strict = no reports, too loose = bad reports)?
17. Tell me about a real incident — what broke, how did you find out, what did you fix. *(Your actual answer: containers crashed silently, analyst/execution went dark, you added persistent logging to docker-compose. Have specifics: what logs, what compose changes, how you verified it was fixed.)*
18. What happens on a partial fill? On a rejected order? On an IBKR disconnect mid-position?
19. How do you reconcile paper vs. live equity positions — what happens if they drift?
20. What's your single point of failure? If forced to name one, what is it and why haven't you fixed it yet?

## Tier 4 — Skepticism / "prove it" questions (senior quant will go here)
21. How do you know your Form 4 / news / IV-skew joins don't have look-ahead bias — i.e., are you using filing timestamp or event date?
22. Your fusion logic is rule-based (CONFIRM/CONTRADICT/IGNORE) — why not a learned/weighted ensemble? What would you need to make that case?
23. Have you back-tested the insider cluster and alt-macro signals independently against forward returns, or only as part of the combined pipeline? What's each one's standalone hit rate?
24. What's your estimate of strategy capacity — at what AUM does your liquidity gate start meaningfully constraining you?
25. If I gave you 10x the capital, what breaks first?
26. Is any part of this curve-fit to a specific historical period (e.g., regime detection tuned to a known volatility event)? How would you know?

## Tier 5 — Behavioral / "why you" (given non-traditional background)
27. You're a chem eng major — why trading systems, and what does the chem eng background actually give you here? *(Real answer available: process-control/feedback-loop framing of the liquidity gate and regime reset, physical-markets literacy for the chokepoint/tanker-traffic work.)*
28. What would you build next if you had another 3 months?
29. What's the worst design decision in this system that you'd do differently if starting over?
30. What don't you know how to do yet that you'd need to learn on the job?

## How to prep against this list
- For every question you can't currently answer with a specific number or artifact (esp. Tier 1 #3, Tier 4 #21-26), that's your build queue — see [[02_quant_evaluation_criteria]].
- Practice the Tier 3 incident story (#17) out loud — it's your best "have you actually operated something in production" evidence and most candidates have nothing comparable.
- Never bluff a number. "I don't have that measured yet, here's how I'd compute it" is a stronger answer than a made-up Sharpe ratio to anyone who's actually a quant.
