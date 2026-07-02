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

## Stage 3 — Tsinghua (grad study)

Realistic paths from a UF chem eng degree into Tsinghua:
- **Tsinghua-Berkeley Shenzhen Institute (TBSI)** or **Tsinghua SEM (School of Economics and Management)** master's programs (e.g., Master of Quantitative Finance, or a Financial Engineering-flavored program) — these accept strong quant/engineering undergrads from non-finance majors if the application shows math + demonstrated quant work. Your bot is exactly the "demonstrated quant work" a chem eng transcript otherwise lacks.
- **Tsinghua Department of Chemical Engineering** grad programs also exist if you want to keep the chem eng thread and pivot into commodities/energy trading later (physical trading desks at firms like Trafigura, Vitol, Glencore explicitly value chem/process eng grads who also understand derivatives — this is a real and less-crowded path into trading than pure quant finance).
- Chinese-language requirement: most English-taught master's programs at Tsinghua (SEM's international programs) don't require fluency, but HSK 4-5 substantially widens options and matters for actually working in China afterward if that's part of the plan.
- **Application leverage:** GRE/GMAT quant score, a strong SOP that explicitly connects "built a live multi-market trading system spanning US and China" (you have a China data engine — mention that you engaged with Chinese market structure specifically) to why Tsinghua/China market expertise matters to you. This is a genuinely differentiated narrative — most applicants don't have a working system that already touches Chinese equities.

## What "better job than current, before finishing school" actually looks like

You don't need to wait for Tsinghua, or even for the UF degree, to get a better job than what you have now. See [[06_job_leads_remote_jax]] for concrete near-term targets. The realistic sequence:
1. Now → next 6-12 months: use the bot (backtested, documented, interview-ready per [[04_bot_master_guide]]) to land a **junior/remote trading-infra, data-eng, or quant-dev-adjacent role** — this doesn't require a finished degree, just demonstrated shipped work.
2. Through UF: keep that job or a similar one part-time/remote while finishing prereqs + degree, upgrading roles as the resume grows.
3. Tsinghua: use as the credential jump for research-track or higher-tier quant roles post-grad-school, or as the pivot point into China-facing trading/commodities roles if the chem-eng-into-physical-markets angle is the one you want to chase.
