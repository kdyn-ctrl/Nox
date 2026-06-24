#pragma once

// Shared types used by both OptionsSignalGenerator and OptionsOrderRouter.
// Extracted to break the circular include dependency between those two headers.

#include "OptionEngine.hpp"
#include <string>
#include <vector>

namespace nox::options_signal {

// ─── OptionsSignal ────────────────────────────────────────────────────────────

struct OptionsSignal {
    std::string underlying;
    std::string strategy;       // LONG_CALL / LONG_PUT / CSP / CC /
                                // BULL_CALL_SPREAD / BEAR_PUT_SPREAD /
                                // STRADDLE / STRANGLE
    std::string expiry_date;    // "YYYY-MM-DD"
    double strike       = 0.0; // primary leg
    double strike2      = 0.0; // second leg (spreads/straddles); 0 = single-leg
    nox::options::OptionType option_type = nox::options::OptionType::Call;
    double entry_price  = 0.0; // BS theoretical mid
    double max_risk     = 0.0; // max dollar loss (position-sized)
    double max_reward   = 0.0; // max dollar gain
    double breakeven    = 0.0;
    nox::options::OptionGreeks greeks{};
    double iv_rank      = 50.0;
    double iv_level     = 0.20; // actual implied volatility (annualized sigma from snapshot)
    double hrv30        = 0.20; // 30-day historical realized volatility
    double rsi          = 50.0;
    double atr          = 0.0;
    double confidence   = 1.0; // 0–1, regime-adjusted
    std::string capital_tier;  // STARTER / STANDARD / ADVANCED / FREE_CAPITAL
    std::string rationale;
    bool free_capital_mode  = false;
    double allocated_capital = 0.0;
    std::string profile_name; // "PERSONAL" or "BOT" — for Telegram labelling
};

// ─── RiskProfile ──────────────────────────────────────────────────────────────
//
// All parameters that differ between personal (high-risk-tolerance) and bot
// (conservative) signal generation. Injected into OptionsSignalGenerator at
// construction. Does not change at runtime.

struct RiskProfile {
    std::string name; // "PERSONAL" or "BOT" — shown in every Telegram alert

    // ── Contract selection ──────────────────────────────────────────────────
    double delta_long         = 0.45; // target Δ for directional longs (calls/puts)
    double delta_income       = 0.25; // target Δ for income/short legs (CSP, CC)
    double delta_spread_wing  = 0.15; // target Δ for the short wing of spreads

    int    dte_long           = 45;   // DTE for directional longs
    int    dte_income         = 30;   // DTE for income strategies
    int    dte_spread         = 45;   // DTE for spreads/multi-leg

    // ── IV thresholds ───────────────────────────────────────────────────────
    double iv_rank_buy_max    = 30.0; // IV rank ceiling to buy premium (cheap vol zone)
    double iv_rank_sell_min   = 50.0; // IV rank floor to sell premium (expensive vol zone)

    // ── Risk sizing (% of allocated capital per trade) ──────────────────────
    double risk_pct_starter   = 0.010; // STARTER tier  (< $5k)
    double risk_pct_standard  = 0.015; // STANDARD tier ($5k–$30k)
    double risk_pct_advanced  = 0.020; // ADVANCED tier ($30k–$75k)
    double risk_pct_free      = 0.015; // FREE_CAPITAL  (≥ $75k or custom)

    // ── Behavioural gates ───────────────────────────────────────────────────
    bool   enforce_tier_gates  = true;  // false = all strategy types always allowed
    bool   enforce_regime_gate = true;  // false = RISK_OFF doesn't suppress long premium

    // ── Execution config (set per-instance, not preset) ─────────────────────
    bool   auto_execute          = false;
    int    qty_contracts         = 1;
    double free_capital_amount   = 0.0;
    std::vector<std::string> watchlist;
    int    scan_interval_minutes = 30;

    // ── Factory: conservative defaults for the automated bot ────────────────
    static RiskProfile bot() {
        RiskProfile p;
        p.name                  = "BOT";
        p.delta_long            = 0.45;
        p.delta_income          = 0.25;
        p.delta_spread_wing     = 0.15;
        p.dte_long              = 45;
        p.dte_income            = 30;
        p.dte_spread            = 45;
        p.iv_rank_buy_max       = 30.0;
        p.iv_rank_sell_min      = 50.0;
        p.risk_pct_starter      = 0.010;
        p.risk_pct_standard     = 0.015;
        p.risk_pct_advanced     = 0.020;
        p.risk_pct_free         = 0.015;
        p.enforce_tier_gates    = true;
        p.enforce_regime_gate   = true;
        p.watchlist             = {"SPY", "QQQ", "AAPL", "TSLA", "NVDA"};
        p.scan_interval_minutes = 30;
        return p;
    }

    // ── Factory: aggressive defaults for personal advisory signals ───────────
    //
    // Key differences vs bot:
    //   - Higher delta targets (more directional, more leverage)
    //   - Shorter DTE (gamma plays, faster resolution)
    //   - Wider IV rank buy window (willing to pay for vol)
    //   - Higher risk % per trade
    //   - No tier gates — all strategies available
    //   - Regime gate relaxed — RISK_OFF reduces confidence but never suppresses
    static RiskProfile personal() {
        RiskProfile p;
        p.name                  = "PERSONAL";
        p.delta_long            = 0.60; // ITM-biased — more intrinsic, less theta drag
        p.delta_income          = 0.30; // slightly closer to the money for more premium
        p.delta_spread_wing     = 0.20; // tighter spread = higher max gain ratio
        p.dte_long              = 14;   // gamma plays — 2-week expiry
        p.dte_income            = 21;   // enough theta without overexposure
        p.dte_spread            = 21;   // tighter timeline = more decisive outcome
        p.iv_rank_buy_max       = 50.0; // willing to buy even moderately expensive vol
        p.iv_rank_sell_min      = 40.0; // sell premium at a lower threshold
        p.risk_pct_starter      = 0.020; // 2% — still disciplined but meaningfully sized
        p.risk_pct_standard     = 0.025;
        p.risk_pct_advanced     = 0.030;
        p.risk_pct_free         = 0.025;
        p.enforce_tier_gates    = false; // personal capital — no tier restrictions
        p.enforce_regime_gate   = false; // you decide — confidence shown but not blocking
        p.watchlist             = {"SPY", "QQQ", "AAPL", "TSLA", "NVDA", "AMZN", "META"};
        p.scan_interval_minutes = 30;
        return p;
    }
};

} // namespace nox::options_signal
