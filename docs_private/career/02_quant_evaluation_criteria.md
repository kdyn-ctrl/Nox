# What Quants Actually Look At — And How To Wire It Into Nox/Nocturnal

This is the technical checklist behind the "senior quant" perspective. Each item: what they check, why, and a concrete integration you can make in this codebase.

## 1. Out-of-sample, cost-aware performance
**What they check:** Sharpe/Sortino, max drawdown, turnover, and — critically — whether the backtest used only information available at decision time (no look-ahead). They will ask "what's your out-of-sample Sharpe after realistic slippage."
**Integrate:** Add a `backtest/report_tearsheet.py` (or C++ equivalent) that runs the full signal pipeline (WS1-6) against historical data with a hard cutoff date, applies a slippage model derived from your existing `LiquidityGate` spread data (use realized historical spreads, not zero-cost fills), and outputs Sharpe/Sortino/MDD/turnover/capacity. This is the single highest-value artifact you're missing.

## 2. Signal decay / half-life justification
**What they check:** Is signal decay assumed or estimated? You already have `HalfLifeDecay` — the question will be "how did you pick the half-life parameter per category?"
**Integrate:** Log realized signal-to-return correlation over rolling windows per `SignalCategory` and fit the empirical half-life instead of (or alongside) the hand-set value. Even a simple regression of |correlation| vs. time-since-signal gives you a defensible number instead of "I picked 24h."

## 3. Ensemble/fusion methodology
**What they check:** Rule-based fusion (your CONFIRM/CONTRADICT/IGNORE verdicts) invites "why not a learned weighting?" They're checking if you understand the tradeoff, not demanding you rebuild it.
**Integrate:** You don't need to replace the rules — add a thin evaluation layer: log every verdict alongside the forward N-day return, and periodically compute hit-rate per verdict type. This turns "I used heuristics" into "I used heuristics and I have data showing CONTRADICT_bullish_bearish_skew predicts negative forward returns at 63% hit rate over N samples" — the second sentence is what gets you hired.

## 4. Look-ahead and survivorship bias in the data pipeline
**What they check:** Insider cluster filter, Form 4 scraping, news sentiment — all vulnerable to using restated/late-arriving data as if it were known in real time. SEC filings have disclosure lag; make sure your backtest uses filing *timestamp*, not event date.
**Integrate:** Audit `fetch_form4_filings()` and news ingestion to confirm every backtest join uses the data's actual availability timestamp, not the economic event date. Document this explicitly — it's a common question and a common flaw.

## 5. Execution realism
**What they check:** Do you model partial fills, queue position, and adverse selection, or do you assume instant full fills at mid?
**Integrate:** Your `LiquidityGate` already tracks relative-spread z-scores — extend the backtest to fill at bid/ask (not mid) scaled by the same z-score regime (worse spread regime → worse assumed fill price), rather than a flat cost assumption.

## 6. Capital/regime robustness
**What they check:** Does the strategy still work at 10x size? Does it depend on one regime (e.g., only works in high-vol)?
**Integrate:** Since you already have `detect_volatility_catalyst` (VIX jump) and regime-reset machinery, segment your tearsheet by regime (pre/post reset) and report Sharpe conditional on regime — shows you understand regime-dependence rather than reporting one blended number.

## 7. Risk controls as a first-class citizen
**What they check:** Notional ceilings, kill switches, fail-open vs fail-closed behavior under bad data. You have this — the liquidity gate fail-open behavior is actually a good discussion point (why fail-open instead of fail-closed here, what's the tradeoff).
**Integrate:** Nothing to build — just be ready to explain the fail-open choice explicitly (a stuck gate blocking all trading is itself a risk; you traded "occasionally trade through bad liquidity data" against "systematically block all trading on a data hiccup"). Document this reasoning in a `docs/risk_controls.md` so it's not just tribal knowledge in your head.

## 8. Reproducibility / engineering hygiene
**What they check (esp. quant dev / infra roles):** tests, CI, logging, incident response. You have `tests/test_regime.cpp`, persistent logging added after a real incident, and a documented equity reconciliation flow. This is already a strength — make sure it's visible (a clean `docs/architecture.md` summarizing WS1-6 + execution + reconciliation flow) rather than only living in your memory of the build history.

## Priority order if you can only do a few of these before interviewing
1. Tearsheet with realistic costs (#1) — non-negotiable, everything else is secondary without this.
2. Verdict hit-rate logging (#3) — cheap to add, turns heuristics into evidence.
3. Look-ahead audit (#4) — costs you nothing to check, costs you the interview if you're wrong and don't know it.
4. Regime-segmented reporting (#6) — makes #1 much more credible.
