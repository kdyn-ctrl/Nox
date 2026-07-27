# Journal: July 14, 2026 — Letting Conviction Change the Trade, Not Just Rank It

## What prompted this

Reviewed a real paper trade after the fact: a short-dated, out-of-the-money
debit spread that had no strong momentum behind it, and theta decay ate it
alive before the thesis had time to play out. The signal generator already
computed a composite quality score for every candidate (a blend of momentum,
volume, and trend agreement) — but only used it to *rank* candidates after
the fact. It never fed back into the two decisions that actually determine
whether a weak setup like that one should exist at all: how much time to
give the trade, and how much capital to risk on it.

## Quality score, moved earlier

The fix was mostly about sequencing, not new math: hoist the quality-score
computation to run right after direction and strategy are chosen, before
contract selection and position sizing happen, instead of after. Once that's
available early, two things become possible that weren't before:

- **DTE selection** for time-decay-sensitive strategies now widens the
  minimum days-to-expiry floor for low-quality setups and relaxes it (never
  below the existing hard floor) for high-quality ones — theta gets more
  room to be wrong on a weak thesis, not less.
- **Position sizing** gets a smooth multiplier from the same score, on top
  of the existing volatility-regime scaling, plus a hard ceiling on
  risk-per-trade for anything landing at or under a short-DTE threshold
  regardless of tier — a structural cap on convexity risk that a strong
  setup can partially earn back, but never fully bypass.

Covered/cash-secured strategies are explicitly excluded from the DTE part of
this — for those, decay is the edge being sold, not a risk to hedge against.

## A sector-level trend filter, finally

There was already a broad market-regime gate (VIX term structure, SPY vs.
its 200-day average) but nothing that checked whether a *single-name* signal
was fighting its own sector. Added an EMA20-vs-EMA50 trend check per sector
ETF, wired as the same kind of advisory-suppress gate the existing regime
checks use — a bullish signal on a name whose sector is trending down gets
flagged and held back, same mechanism, one more input.

## A backtest fill-model honesty check

Separately, revisited the backtester's fill assumptions. The default model
assumed every profit-target or stop-loss touch fills at exactly the modeled
price — which flatters a resting limit order in particular, since a real one
only fills for certain if price actually crosses through it, not just
touches and reverses. Added an alternate fill model that treats a stop as a
market order (fills for certain once touched) but a profit target as a
resting limit order that only fills probabilistically on a touch that
doesn't cross, scaled by the underlying's volume as a liquidity proxy — then
reports the two side by side so the gap between "how good does this look
naively" and "how good does this look under a more skeptical fill
assumption" is visible instead of assumed away.

## What's still open

All of the new thresholds (quality bands, sizing multipliers, DTE floors,
the short-DTE risk ceiling) are reasoned starting points, not backtested
against real quality-score distributions yet — that's the natural next pass
once enough live signals accumulate to check whether the bands are actually
where conviction should split.
