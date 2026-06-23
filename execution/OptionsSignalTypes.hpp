#pragma once

// Shared types used by both OptionsSignalGenerator and OptionsOrderRouter.
// Extracted to break the circular include dependency between those two headers.

#include "OptionEngine.hpp"
#include <string>

namespace nox::options_signal {

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
    double rsi          = 50.0;
    double atr          = 0.0;
    double confidence   = 1.0; // 0–1, regime-adjusted
    std::string capital_tier;  // STARTER / STANDARD / ADVANCED / FREE_CAPITAL
    std::string rationale;
    bool free_capital_mode = false;
    double allocated_capital = 0.0; // effective capital for sizing
};

} // namespace nox::options_signal
