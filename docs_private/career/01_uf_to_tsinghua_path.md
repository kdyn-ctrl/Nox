# Path: UF (Chem Eng) → Tsinghua → Better Job Than Current

Status as of 2026-07-02: transferring to UF, chem eng major, currently in Calc 1 (1 of 8 prereqs).

## The honest framing first

Chem eng + a working quant trading system is an unusual but real combination: process engineers who can code are valuable in commodities/energy trading (physical + financial), and quant shops occasionally want people who understand physical markets (shipping, chokepoints, refining) rather than pure math PhDs. Your `alt_macro.py` chokepoint work (Hormuz/Red Sea/tanker traffic) is literally a chem-eng-adjacent skill nobody else in a CS bootcamp has. Don't hide the chem eng degree — lean into "physical markets + quant infra" as your niche.

## Stage 1 — UF prereqs (now → ~18-24 months)

Order matters for keeping momentum and for what you can pair with project work:

1. **Calc 1 (current)** → Calc 2 → Calc 3 → Differential Equations. These are also the literal math prerequisites for anything quant (stochastic calc, PDEs for option pricing later).
2. **Gen Chem 1/2** — standard chem eng requirement, no crossover with the bot, treat as a box to check efficiently.
3. **Physics 1/2 (calc-based)** — Physics 1 (mechanics) has real crossover: the liquidity-gate z-score logic and regime decay math are the same "rate of change / conservation" thinking as physics problem sets. Use physics as a place to practice translating real systems into differential equations — directly transferable to your `HalfLifeDecay` (λ = ln2/half-life) work.
4. **Statics** (chem/mechanical eng requirement) — least crossover, treat as a box to check.
5. Whichever gen-ed/writing requirement fills your 8th slot — if you have any elective flexibility at all, take an intro **statistics** or **linear algebra** course here instead of a pure gen-ed; it's disproportionately useful for both UF admission (shows quant intent) and the bot.

**Parallel track while doing prereqs:** don't wait until you're at UF to build the "quant" side of your resume. See [[04_bot_master_guide]] and [[02_quant_evaluation_criteria]] — spend prereq-era time turning the existing bot into a backtested, documented system with real performance numbers. That artifact matters more for job-hunting than your GPA in Calc 1.

## Stage 2 — At UF (Chem Eng major)

- **Minor or certificate to stack:** UF has a Quantitative Finance-adjacent path through the Warrington College of Business (finance electives are usually open to non-business majors as free electives) and a Statistics minor through the math/stats department. A Statistics minor is the highest-leverage add for a chem eng major aiming at quant/trading roles — it's the single credential line recruiters scan for when the primary major isn't math/CS/finance.
- **Target electives, in priority order:** Probability & Statistics (calc-based) → Linear Algebra → an Intro to Programming/Data Structures course (even if "for engineers") → Numerical Methods (chem eng usually has one — this is your bridge course, it's literally solving the same ODEs/PDEs you'd use for pricing models) → a Derivatives/Investments elective if business college allows cross-registration.
- **Research/lab angle:** Chem eng departments often have process control / systems labs (PID controllers, feedback loops). Volunteering in one is a resume line that maps directly to your liquidity-gate and regime-reset logic — controls theory and trading-system risk gating are the same math (feedback, thresholds, hysteresis). This is a genuinely good talking point in interviews: "I built a control-loop-style abort mechanism for order execution, and here's the process-control class where I learned the underlying theory."
- **Keep building the bot the whole time** as your one continuous portfolio piece — a 3-4 year build history on one non-trivial system is more impressive than a new class project every semester.

## Stage 3 — Tsinghua-Columbia Dual Master's Degree in Business Analytics (grad study)

Target program: **Tsinghua-Columbia Dual Master's Degree Program in Business Analytics** — dual credential from top-tier China and US financial hubs.

- **Program structure:** Columbia's 1-year MS Business Analytics + Tsinghua's Master's program in Business Analytics or SEM (School of Economics and Management). The dual degree offers concentrated quant curriculum across both institutions, with the strategic advantage of finishing Columbia's quantitative core (derivatives, stochastic calc, statistical learning) in Year 1, then Tsinghua's market microstructure and policy-regime modules in Year 2 — directly applicable to your multi-market trading system.
- **Application strength:** your bot (live multi-market system, China data integration, documented backtesting) is the exact credential these programs hunt for. Dual-degree applicants are rare; most are pure finance/math. A chem-eng-to-quant narrative + shipped trading system is a genuinely differentiated story.
- **Chinese-language:** most English-taught programs don't require fluency for admission, but HSK 4-5 substantially improves internship access and post-degree job prospects. Your existing work on Chinese data sources is evidence of intent.
- **GRE target:** 170 quant (99th percentile). This is the threshold for top-tier dual-degree and research-track roles post-grad.
- **Curriculum cross-stack:** Use Columbia for derivatives pricing, stochastic processes, and statistical learning. Use Tsinghua for China equity/futures microstructure, policy-driven regime dynamics, and cross-border market infrastructure — all directly testable in your bot.
- **Post-grad leverage:** the dual master's + 3-4 year bot history + internships at quant firms is the credential package for research-track roles (Citadel, Two Sigma, D.E. Shaw) or China-facing quant shops (Harvest Fund Management, Qiyuan, Orenda).

## What "better job than current, before finishing school" actually looks like

You don't need to wait for Tsinghua, or even for the UF degree, to get a better job than what you have now. See [[06_job_leads_remote_jax]] for concrete near-term targets. The realistic sequence:
1. Now → next 6-12 months: use the bot (backtested, documented, interview-ready per [[04_bot_master_guide]]) to land a **junior/remote trading-infra, data-eng, or quant-dev-adjacent role** — this doesn't require a finished degree, just demonstrated shipped work.
2. Through UF: keep that job or a similar one part-time/remote while finishing prereqs + degree, upgrading roles as the resume grows.
3. Tsinghua: use as the credential jump for research-track or higher-tier quant roles post-grad-school, or as the pivot point into China-facing trading/commodities roles if the chem-eng-into-physical-markets angle is the one you want to chase.
