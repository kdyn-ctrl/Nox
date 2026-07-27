"""Barbell (core / satellite) allocation logic for personal trades.

Pure, dependency-free helpers so they can be unit-tested without importing the
telebot/anthropic-heavy `monitor` module (the same reason `retry_utils` and
`trading_day_utils` are standalone).
`monitor.py` owns the DB and Telegram I/O; this module owns the
classification and the soft-warn math.

Design (2026-07-18, user decision — a Barbell / Core-Satellite split of the
*personal* trading sleeve, enforced in SOFT-WARN mode: warn, never refuse):

  - Every personal trade is tagged ``core`` or ``satellite``. Default ``core``.
  - **Core** is the disciplined base (steady, high-probability setups).
    **Satellite** is the moonshot sleeve — 0DTE / OTM / high-IV lotto tickets
    the user knowingly accepts a low win rate on for asymmetric payoff.
  - The whole point of the tag is to keep the two *measured apart*
    (see feedback_signal_trust_diversity): a low-win-rate satellite bet is a
    success by design, and blending it into one "personal win rate" both
    flatters the sleeve that's supposed to lose often and slanders the core.
  - Soft-warn (never block) fires when:
      1. satellite open exposure exceeds its capital cap (over-allocated to
         lotto risk), or
      2. a new satellite entry re-funds a *net-negative* satellite sleeve from
         fresh capital instead of skimmed core profit — the "never re-fund the
         satellite from the core" rule of the barbell.

Nothing here sizes or gates an actual order. It tags the user's manually
logged personal trades and surfaces warnings; execution is untouched.

Core-scope soft-warn (see core_scope_warning() below, added 2026-07-18):
CORE is supposed to be the sleeve backed by measured evidence, not just
"not satellite" by default. The user's own bar for core status: "a real
chains-tested, cost-adjusted, positive backtest — not before." Three
real-option-price structures have been tested against that bar so far
(research/premium_selling_test.py's ATM straddle,
research/defined_risk_wrapper_test.py's iron condor,
research/naked_strangle_live_params_test.py's naked strangle at Nox's exact
live delta/DTE/margin sizing) — all three found a real PRE-cost
variance-risk-premium on the SPY/QQQ/IWM/GLD/XLF index/ETF basket, and all
three had that edge erased by a moderate (not worst-case) transaction-cost
assumption (see research/naked_strangle_live_params_findings_2026_07_18.md
for the latest and most decision-relevant of the three — it matches Nox's
real trade exactly). **Nothing has cleared the bar yet** — CORE_EDGE_TICKERS
defaults to empty until something does. A core-bucket trade is therefore
unproven right now, not "wrong" — same SOFT-WARN philosophy as the
satellite checks above: warn, never refuse.
"""

import os

CORE = "core"
SATELLITE = "satellite"
VALID_BUCKETS = (CORE, SATELLITE)

# The ticker/strategy combination that has cleared a real-chains,
# cost-adjusted, POSITIVE backtest at Nox's real sizing — per the user's own
# bar for core status. As of 2026-07-18, that's an EMPTY set: every
# structure tested so far (straddle, iron condor, naked strangle) had its
# real pre-cost edge erased by realistic transaction costs (see the module
# docstring). Defaulting to empty means core_scope_warning() below warns on
# EVERY core trade until something actually clears the bar — this is the
# honest reading of the evidence, not a placeholder (RULE-D5: this default
# is measured, not invented). Env-tunable (RULE-D11) so a future passing
# result can be wired in without a code change.
CORE_EDGE_TICKERS = frozenset(
    t.strip().upper()
    for t in os.getenv("PERSONAL_CORE_EDGE_TICKERS", "").split(",")
    if t.strip()
)
CORE_EDGE_STRATEGY = os.getenv("PERSONAL_CORE_EDGE_STRATEGY", "STRANGLE")

# Bucket keyword aliases. Recognized only after an explicit ``bucket:`` prefix
# (see extract_bucket_override) so a bare "moon" in a free-text notes field
# can never be mistaken for a bucket selector.
_SATELLITE_ALIASES = {"satellite", "sat", "moon", "moonshot", "lotto", "spec"}
_CORE_ALIASES = {"core", "base"}

# Actions that represent an entry (money going out / a position opening) vs a
# close (position settled, realized pnl booked). Mirrors monitor's own
# vocabulary written by cmd_trade / cmd_close.
_ENTRY_ACTIONS = {"OPEN", "BUY"}
_CLOSE_ACTIONS = {"CLOSE", "SELL"}


def normalize_bucket(raw):
    """Map a user-typed bucket token to 'core'/'satellite', or None if it isn't
    a recognized bucket keyword. Case-insensitive."""
    if raw is None:
        return None
    tok = str(raw).strip().lower()
    if tok in _SATELLITE_ALIASES:
        return SATELLITE
    if tok in _CORE_ALIASES:
        return CORE
    return None


def extract_bucket_override(text, default=CORE):
    """Strip an optional ``bucket:<alias>`` token out of a raw /trade command
    string and return (cleaned_text, bucket).

    Deliberately prefixed-only (like ``date:``), never a bare keyword: /trade's
    trailing notes field is free text, and a bare "moon"/"spec" there must not
    be silently reclassified as a bucket. Only the first ``bucket:`` token is
    honored; any later one is left in place. An unrecognized value
    (``bucket:foo``) is stripped but falls back to ``default`` rather than
    inventing a third bucket.
    """
    tokens = text.split()
    cleaned = []
    bucket = None
    for tok in tokens:
        low = tok.lower()
        if bucket is None and low.startswith("bucket:"):
            bucket = normalize_bucket(low.split(":", 1)[1])
            continue  # strip the token either way (recognized or not)
        cleaned.append(tok)
    return " ".join(cleaned), (bucket or default)


def _row_bucket(row):
    """A row's bucket, tolerating NULL/legacy rows (pre-migration) as core."""
    return normalize_bucket(row.get("bucket")) or CORE


def _cost_basis(row, contract_multiplier):
    """Absolute dollar cost basis of an entry row, or None if qty/price are
    missing (the /trade s:47 form logs an execution without them)."""
    qty = row.get("quantity")
    price = row.get("price")
    if qty is None or price is None:
        return None
    mult = contract_multiplier(row.get("strategy"), row.get("asset_class"))
    return abs(float(price) * float(qty) * mult)


def snapshot(rows, *, contract_multiplier, capital, cap_pct):
    """Aggregate personal_trades rows into per-bucket standing figures.

    rows: iterable of dict-like rows with keys id, bucket, action, quantity,
          price, pnl, asset_class, strategy, closes_trade_id.
    contract_multiplier: callable(strategy, asset_class) -> int (100 for
          options, else 1). Passed in so this module never has to duplicate
          monitor's options-strategy sets (RULE-D6: one source of that truth).

    Returns a dict with cap fields and a per-bucket sub-dict of:
      open_exposure (sum |cost basis| of still-open entries),
      open_count, uncomputable_open (open entries with no qty/price),
      realized_pnl, wins, losses, closed (count of settled trades).
    """
    rows = list(rows)
    closed_ids = {r.get("closes_trade_id") for r in rows
                  if r.get("closes_trade_id") is not None}

    buckets = {
        CORE: dict(open_exposure=0.0, open_count=0, uncomputable_open=0,
                   realized_pnl=0.0, wins=0, losses=0, closed=0),
        SATELLITE: dict(open_exposure=0.0, open_count=0, uncomputable_open=0,
                        realized_pnl=0.0, wins=0, losses=0, closed=0),
    }

    for r in rows:
        b = buckets[_row_bucket(r)]
        action = (r.get("action") or "").upper()
        if action in _ENTRY_ACTIONS and r.get("id") not in closed_ids:
            # Still-open position: contributes to standing exposure.
            b["open_count"] += 1
            cost = _cost_basis(r, contract_multiplier)
            if cost is None:
                b["uncomputable_open"] += 1
            else:
                b["open_exposure"] += cost
        elif action in _CLOSE_ACTIONS:
            pnl = r.get("pnl")
            if pnl is not None:
                b["closed"] += 1
                b["realized_pnl"] += float(pnl)
                if pnl > 0:
                    b["wins"] += 1
                elif pnl < 0:
                    b["losses"] += 1

    return {
        "capital": float(capital),
        "cap_pct": float(cap_pct),
        "cap_dollars": float(capital) * float(cap_pct) / 100.0,
        CORE: buckets[CORE],
        SATELLITE: buckets[SATELLITE],
    }


def satellite_soft_warnings(snap):
    """Soft-warn strings for a satellite entry given a snapshot AFTER the new
    row is included. Empty list = within the barbell's rules. Never a hard stop.
    """
    sat = snap[SATELLITE]
    cap = snap["cap_dollars"]
    warns = []
    if sat["open_exposure"] > cap:
        warns.append(
            f"⚠️ Satellite exposure ${sat['open_exposure']:,.0f} exceeds your "
            f"moonshot cap ${cap:,.0f} ({snap['cap_pct']:.0f}% of "
            f"${snap['capital']:,.0f}) — you're over-allocated to lotto risk."
        )
    if sat["realized_pnl"] < 0:
        warns.append(
            f"⚠️ Satellite sleeve is net ${sat['realized_pnl']:+,.0f} realized. "
            f"Adding here re-funds it from fresh capital, not skimmed core "
            f"profit (barbell rule #3: never re-fund the satellite from the core)."
        )
    if sat["uncomputable_open"]:
        warns.append(
            f"ℹ️ {sat['uncomputable_open']} open satellite trade(s) have no "
            f"qty/price logged, so the exposure figure above is a floor, not "
            f"the full picture."
        )
    return warns


def core_scope_warning(ticker, strategy):
    """Soft-warn string (or None) for a CORE-bucket entry whose ticker/strategy
    isn't the measured-edge combination — CORE_EDGE_TICKERS x CORE_EDGE_STRATEGY
    (see module docstring). None = within the measured basket, no warning.
    Never blocks; everything else just stays satellite-by-earning, per the
    2026-07-18 barbell/core-sleeve plan, until IT gets its own real-chains,
    cost-adjusted, positive backtest.
    """
    tkr = (ticker or "").strip().upper()
    strat = (strategy or "").strip().upper()
    if tkr in CORE_EDGE_TICKERS and strat == CORE_EDGE_STRATEGY:
        return None
    if not CORE_EDGE_TICKERS:
        return (
            f"🧱 Core-scope note: nothing has cleared a real-chains, "
            f"cost-adjusted, POSITIVE backtest yet (straddle/iron-condor/naked-"
            f"strangle all had their pre-cost edge erased by realistic costs — "
            f"see research/naked_strangle_live_params_findings_2026_07_18.md). "
            f"{tkr or 'This'} {strat or '(no strategy)'} is logged as core, but "
            f"every core trade is unproven right now. Consider bucket:satellite "
            f"until something actually earns core status."
        )
    return (
        f"🧱 Core-scope note: {tkr or '?'} {strat or '(no strategy)'} isn't the "
        f"measured-edge combination ({CORE_EDGE_STRATEGY} on "
        f"{'/'.join(sorted(CORE_EDGE_TICKERS))}) — logged as core, but it isn't "
        f"backed by a real-chains-tested edge yet. Consider bucket:satellite "
        f"until it earns core status the same way."
    )


def _bucket_line(name, b):
    closed = b["closed"]
    wr = (b["wins"] / closed * 100.0) if closed else None
    wr_str = f"{wr:.0f}% WR" if wr is not None else "no closes yet"
    return (
        f"  {name:<9} open ${b['open_exposure']:,.0f} ({b['open_count']}) | "
        f"realized ${b['realized_pnl']:+,.0f} | "
        f"W:{b['wins']} L:{b['losses']} ({wr_str})"
    )


def render_status(snap):
    """A human-readable /barbell dashboard block."""
    sat = snap[SATELLITE]
    cap = snap["cap_dollars"]
    used_pct = (sat["open_exposure"] / cap * 100.0) if cap else 0.0
    over = sat["open_exposure"] > cap
    headroom = cap - sat["open_exposure"]
    cap_line = (
        f"Satellite cap: ${cap:,.0f} ({snap['cap_pct']:.0f}% of "
        f"${snap['capital']:,.0f}) — using {used_pct:.0f}%"
    )
    if over:
        cap_line += f"  🔴 OVER by ${-headroom:,.0f}"
    else:
        cap_line += f"  🟢 ${headroom:,.0f} headroom"
    return (
        "🎯 *Barbell — personal allocation*\n"
        f"{cap_line}\n```\n"
        f"{_bucket_line('Core', snap[CORE])}\n"
        f"{_bucket_line('Satellite', sat)}\n```"
    )
