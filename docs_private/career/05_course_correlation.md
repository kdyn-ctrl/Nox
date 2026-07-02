# Course ↔ Bot Correlation Map

Use this to describe your coursework and project in the same breath during interviews and applications — the correlation is the story, not two separate resume lines.

| Course (UF Chem Eng path) | Bot Component It Maps To | The Actual Connection |
|---|---|---|
| Calc 1 (current) | Signal decay rate of change | Derivatives = the literal math behind "how fast does a signal lose value" — same limit/rate concept as `HalfLifeDecay`. |
| Calc 2 | Options/IV-skew intuition | Integration underlies expected value calculations used implicitly in options skew interpretation (area under a distribution). |
| Calc 3 (multivariable) | Multi-factor fusion (contradiction vector, alt-macro) | Combining multiple independent signals (text sentiment × skew, physical × political) is a multivariable problem — think of it as a function of several variables you're evaluating jointly. |
| Differential Equations | Regime decay & reset (WS4) | `HalfLifeDecay` λ=ln2/half-life is a first-order linear ODE solution (dN/dt = -λN) — literally the same equation as radioactive decay / first-order chemical reaction kinetics. This is your strongest, most literal course-to-code line. |
| Gen Chem 1/2 | Reaction kinetics analogy | First-order kinetics (rate ∝ concentration) is mathematically identical to your signal decay model — use this analogy explicitly, it's a genuinely good explanation device for non-technical interviewers too. |
| Physics 1 (mechanics) | Liquidity gate / control thresholds | z-score abort thresholds and warm-up periods are the same "system response to a forcing input, with a threshold/tolerance" thinking as physics problem sets (e.g., stress before failure, damped response). |
| Physics 2 | (weaker direct link) | Frame as general "quantitative modeling of physical systems" experience if asked to justify physics relevance. |
| Statics | (weakest direct link — chem eng requirement, not a bot analogy) | Fine to present as a degree requirement box-check; don't force a connection that isn't there. |
| Probability & Statistics (recommended elective) | Verdict hit-rate evaluation, z-scores, backtest Sharpe/Sortino | This is the single most directly load-bearing course for the "prove it with data" gaps in [[02_quant_evaluation_criteria]]. Prioritize this course. |
| Linear Algebra (recommended elective) | Multi-signal weighting / future learned-ensemble work | If you build the "learned weighting" upgrade mentioned in [[02_quant_evaluation_criteria]] item 3, it's a weighted linear combination — directly this course's material. |
| Numerical Methods (chem eng core, if in your curriculum) | Backtest engine / ODE solving for decay & regime models | Chem eng numerical methods courses solve the same class of ODEs (first-order decay, systems of equations) via the same numerical schemes (Euler, Runge-Kutta) your decay/regime code approximates in discrete time steps. Strongest "this class directly leveled up my project" story available to you. |
| Process Control / Instrumentation (if available as chem eng elective or lab) | Liquidity gate, regime-reset latch, fail-open design | PID control, feedback loops, hysteresis, and fail-safe design are the literal engineering vocabulary for what your execution risk gates already do. If UF offers this, take it — it's the highest-leverage single elective for this specific project. |

## How to use this table
- In a cover letter or interview, don't say "I study chem eng and separately build a trading bot." Say: "My chem eng coursework in reaction kinetics and numerical methods for ODEs is the same math underneath my trading system's signal decay model — I didn't learn quant finance and chemical engineering as two unrelated things."
- Update this table each semester as your actual UF schedule solidifies — it's meant to track real, currently-enrolled courses, not just plausible future ones.
