#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "OptionEngine.hpp"
#include "OptionsSignalTypes.hpp"
#include "OptionsOrderRouter.hpp"
#include "../shared/RegimeStateMachine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace nox::options_signal {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class DirectionalBias { Bullish, Bearish, Neutral };

// ─── Structures ───────────────────────────────────────────────────────────────

struct UnderlyingData {
    double price    = 0.0;
    double sma20    = 0.0;
    double sma50    = 0.0;
    double rsi14    = 0.0;
    double atr14    = 0.0;
    bool   valid    = false;
};


// ─── OptionsSignalGenerator ───────────────────────────────────────────────────

class OptionsSignalGenerator {
public:
    // Profile-driven constructor — all risk parameters come from the RiskProfile.
    OptionsSignalGenerator(const std::string& alpacaUrl,
                           const std::string& apiKey,
                           const std::string& apiSec,
                           const std::string& tgToken,
                           const std::string& tgChatId,
                           RiskProfile        profile)
        : alpacaUrl_(alpacaUrl)
        , apiKey_(apiKey)
        , apiSec_(apiSec)
        , tgToken_(tgToken)
        , tgChatId_(tgChatId)
        , profile_(std::move(profile))
    {}

    // Entry point — called once per scan cycle from the engine's background thread.
    void run_scan(double live_equity) {
        const auto& watchlist = profile_.watchlist;
        double effective_capital = resolveCapital(live_equity);
        std::string tier         = computeCapitalTier(effective_capital);
        bool  fc_mode            = (profile_.free_capital_amount > 0.0);

        log("INFO", "[OPTIONS_SCAN][" + profile_.name + "] Tier=" + tier +
            " | Capital=$" + fmt(effective_capital, 0) +
            " | Tickers=" + std::to_string(watchlist.size()));

        // Fetch macro regime once per scan — reuse for all tickers
        double vix      = fetchVix();
        SpySnapshot spy = fetchSpy();

        AllocationStrategy regime{};
        if (vix > 0.0 && spy.valid) {
            regime = regimeMachine_.evaluate(vix, spy.price, spy.sma200);
            log("INFO", "[OPTIONS_SCAN] Regime: " + regime.log_message);
        } else {
            log("WARN", "[OPTIONS_SCAN] Could not fetch VIX/SPY — regime defaulting to TRANSITION.");
            regime.current_regime     = Regime::TRANSITION;
            regime.capital_multiplier = 0.5;
            regime.log_message        = "TRANSITION (data unavailable)";
            vix = 20.0;
        }

        for (const auto& ticker : watchlist) {
            try {
                scanTicker(ticker, effective_capital, tier, fc_mode, vix, spy, regime);
            } catch (const std::exception& e) {
                log("WARN", "[OPTIONS_SCAN] Exception on " + ticker + ": " + e.what());
            }
            // Brief pause between tickers to avoid rate limits
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    }

private:
    // ── Config ────────────────────────────────────────────────────────────────
    std::string alpacaUrl_;
    std::string apiKey_;
    std::string apiSec_;
    std::string tgToken_;
    std::string tgChatId_;
    RiskProfile profile_;
    RegimeStateMachine regimeMachine_;

    // ── Helpers ───────────────────────────────────────────────────────────────

    static void log(const std::string& level, const std::string& msg) {
        std::cout << "[" << level << "] " << msg << std::endl;
    }

    static std::string fmt(double v, int decimals = 2) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(decimals) << v;
        return oss.str();
    }

    // ── Capital / tier logic ──────────────────────────────────────────────────

    double resolveCapital(double live_equity) const {
        if (profile_.free_capital_amount > 0.0) return profile_.free_capital_amount;
        return live_equity;
    }

    static std::string computeCapitalTier(double capital) {
        if (capital >= 75000.0) return "FREE_CAPITAL";
        if (capital >= 30000.0) return "ADVANCED";
        if (capital >= 5000.0)  return "STANDARD";
        return "STARTER";
    }

    // Risk dollars per trade — uses profile-specific percentages
    double computeMaxRisk(double capital, const std::string& tier) const {
        if (tier == "FREE_CAPITAL") return capital * profile_.risk_pct_free;
        if (tier == "ADVANCED")     return capital * profile_.risk_pct_advanced;
        if (tier == "STANDARD")     return capital * profile_.risk_pct_standard;
        return capital * profile_.risk_pct_starter;
    }

    // Returns true if a strategy is allowed — tier gates honoured only when enforce_tier_gates is set
    bool strategyAllowed(const std::string& strategy, const std::string& tier) const {
        if (!profile_.enforce_tier_gates) return true; // personal: all strategies open

        if (tier == "STARTER") {
            return strategy == "LONG_CALL" || strategy == "LONG_PUT";
        }
        if (tier == "STANDARD") {
            return strategy == "LONG_CALL"  || strategy == "LONG_PUT" ||
                   strategy == "CSP"        || strategy == "CC";
        }
        return true; // ADVANCED / FREE_CAPITAL
    }

    // ── Regime confidence multiplier ──────────────────────────────────────────

    double regimeConfidence(Regime r, const std::string& strategy) const {
        bool is_long_premium = (strategy == "LONG_CALL"  || strategy == "LONG_PUT" ||
                                strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD" ||
                                strategy == "STRADDLE"   || strategy == "STRANGLE");

        if (r == Regime::RISK_OFF && is_long_premium) {
            // Bot: hard suppress. Personal: show warning (50% confidence) but don't block.
            return profile_.enforce_regime_gate ? 0.0 : 0.50;
        }
        if (r == Regime::TRANSITION) return 0.65;
        return 1.0; // RISK_ON
    }

    // ── Market data: VIX ──────────────────────────────────────────────────────

    double fetchVix() const {
        try {
            httplib::Client cli("https://query1.finance.yahoo.com");
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(12));

            auto res = cli.Get("/v8/finance/chart/%5EVIX?interval=1d&range=2d");
            if (!res || res->status != 200) return -1.0;

            auto body = json::parse(res->body);
            const auto& closes = body.at("chart").at("result").at(0)
                                      .at("indicators").at("quote").at(0).at("close");
            for (int i = static_cast<int>(closes.size()) - 1; i >= 0; --i) {
                if (!closes[i].is_null()) return closes[i].get<double>();
            }
        } catch (...) {}
        return -1.0;
    }

    // ── Market data: SPY ──────────────────────────────────────────────────────

    struct SpySnapshot {
        double price  = 0.0;
        double sma200 = 0.0;
        bool   valid  = false;
    };

    SpySnapshot fetchSpy() const {
        try {
            httplib::Client cli("https://query1.finance.yahoo.com");
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(15));

            auto res = cli.Get("/v8/finance/chart/SPY?interval=1d&range=1y");
            if (!res || res->status != 200) return {};

            auto body = json::parse(res->body);
            const auto& closes = body.at("chart").at("result").at(0)
                                      .at("indicators").at("quote").at(0).at("close");

            std::vector<double> valid_closes;
            for (const auto& c : closes) {
                if (!c.is_null()) valid_closes.push_back(c.get<double>());
            }
            if (valid_closes.size() < 200) return {};

            double price = valid_closes.back();
            double sum   = 0.0;
            for (size_t i = valid_closes.size() - 200; i < valid_closes.size(); ++i)
                sum += valid_closes[i];

            return {price, sum / 200.0, true};
        } catch (...) {}
        return {};
    }

    // ── Market data: Underlying OHLCV (Yahoo Finance) ─────────────────────────

    UnderlyingData fetchUnderlyingBars(const std::string& symbol) const {
        try {
            httplib::Client cli("https://query1.finance.yahoo.com");
            cli.set_connection_timeout(std::chrono::seconds(8));
            cli.set_read_timeout(std::chrono::seconds(15));

            std::string path = "/v8/finance/chart/" + symbol + "?interval=1d&range=1y";
            auto res = cli.Get(path.c_str());
            if (!res || res->status != 200) return {};

            auto body   = json::parse(res->body);
            const auto& ohlcv = body.at("chart").at("result").at(0)
                                     .at("indicators").at("quote").at(0);

            const auto& raw_closes = ohlcv.at("close");
            const auto& raw_highs  = ohlcv.at("high");
            const auto& raw_lows   = ohlcv.at("low");

            std::vector<double> closes, highs, lows;
            for (size_t i = 0; i < raw_closes.size(); ++i) {
                if (!raw_closes[i].is_null() && !raw_highs[i].is_null() && !raw_lows[i].is_null()) {
                    closes.push_back(raw_closes[i].get<double>());
                    highs.push_back(raw_highs[i].get<double>());
                    lows.push_back(raw_lows[i].get<double>());
                }
            }
            if (closes.size() < 50) return {};

            // SMA-20 and SMA-50
            double sma20 = 0.0, sma50 = 0.0;
            for (size_t i = closes.size() - 20; i < closes.size(); ++i) sma20 += closes[i];
            for (size_t i = closes.size() - 50; i < closes.size(); ++i) sma50 += closes[i];
            sma20 /= 20.0;
            sma50 /= 50.0;

            // RSI-14
            double gain = 0.0, loss = 0.0;
            size_t rsi_start = closes.size() - 15;
            for (size_t i = rsi_start + 1; i < closes.size(); ++i) {
                double diff = closes[i] - closes[i - 1];
                if (diff > 0) gain += diff; else loss -= diff;
            }
            gain /= 14.0; loss /= 14.0;
            double rsi = (loss < 1e-9) ? 100.0 : 100.0 - (100.0 / (1.0 + gain / loss));

            // ATR-14: average of true range over last 14 bars
            double atr_sum = 0.0;
            size_t atr_start = closes.size() - 14;
            for (size_t i = atr_start; i < closes.size(); ++i) {
                double hl  = highs[i] - lows[i];
                double hpc = std::abs(highs[i] - closes[i - 1]);
                double lpc = std::abs(lows[i]  - closes[i - 1]);
                atr_sum += std::max({hl, hpc, lpc});
            }

            UnderlyingData d;
            d.price  = closes.back();
            d.sma20  = sma20;
            d.sma50  = sma50;
            d.rsi14  = rsi;
            d.atr14  = atr_sum / 14.0;
            d.valid  = true;
            return d;
        } catch (...) {}
        return {};
    }

    // ── IV Rank via Alpaca options snapshot (falls back to VIX proxy) ─────────

    double fetchIVRank(const std::string& symbol, double vix_fallback) const {
        try {
            // Alpaca options snapshot endpoint (requires market data subscription)
            httplib::Client cli(alpacaUrl_);
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            httplib::Headers headers = {
                {"APCA-API-KEY-ID",     apiKey_},
                {"APCA-API-SECRET-KEY", apiSec_}
            };

            std::string path = "/v2/options/snapshots/" + symbol + "?limit=50&feed=indicative";
            auto res = cli.Get(path.c_str(), headers);
            if (!res || res->status != 200) throw std::runtime_error("snapshot unavailable");

            auto body = json::parse(res->body);
            const auto& snapshots = body.at("snapshots");

            double iv_min = 9999.0, iv_max = 0.0, iv_sum = 0.0;
            int    iv_count = 0;
            for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
                const auto& snap = it.value();
                if (snap.contains("greeks") && !snap["greeks"]["iv"].is_null()) {
                    double iv = snap["greeks"]["iv"].get<double>();
                    iv_min    = std::min(iv_min, iv);
                    iv_max    = std::max(iv_max, iv);
                    iv_sum   += iv;
                    ++iv_count;
                }
            }
            if (iv_count == 0 || (iv_max - iv_min) < 1e-6) throw std::runtime_error("no IV data");

            double iv_current = iv_sum / iv_count;
            return (iv_current - iv_min) / (iv_max - iv_min) * 100.0;

        } catch (...) {
            // VIX proxy: VIX < 15 → ~20% rank, VIX 15–25 → mid, VIX > 30 → high
            if (vix_fallback < 15.0)  return 15.0;
            if (vix_fallback > 30.0)  return 70.0;
            return (vix_fallback - 15.0) / 15.0 * 55.0 + 15.0;
        }
    }

    // ── Directional bias from technicals ─────────────────────────────────────

    static DirectionalBias computeBias(const UnderlyingData& d) {
        bool above_20  = d.price > d.sma20;
        bool above_50  = d.price > d.sma50;
        bool rsi_bull  = d.rsi14 >= 40.0 && d.rsi14 <= 65.0;
        bool rsi_bear  = d.rsi14 >= 35.0 && d.rsi14 <= 60.0;
        bool near_sma  = std::abs(d.price - d.sma20) < d.atr14;

        if (above_20 && above_50 && rsi_bull) return DirectionalBias::Bullish;
        if (!above_20 && !above_50 && rsi_bear) return DirectionalBias::Bearish;
        if (near_sma && d.rsi14 >= 40.0 && d.rsi14 <= 60.0) return DirectionalBias::Neutral;
        return DirectionalBias::Neutral;
    }

    // ── Strategy selection ────────────────────────────────────────────────────
    //
    // Matrix: bias × iv_rank → strategy candidates (in preference order)
    // Long premium preferred when IV Rank < 30 (cheap vol)
    // Short premium preferred when IV Rank > 50 (expensive vol)

    std::string selectStrategy(DirectionalBias bias, double iv_rank,
                               const std::string& tier) const {
        const double buy_max  = profile_.iv_rank_buy_max;
        const double sell_min = profile_.iv_rank_sell_min;

        if (bias == DirectionalBias::Bullish) {
            if (iv_rank <= buy_max) {
                if (strategyAllowed("BULL_CALL_SPREAD", tier)) return "BULL_CALL_SPREAD";
                return "LONG_CALL";
            }
            if (iv_rank >= sell_min) {
                if (strategyAllowed("CSP", tier)) return "CSP";
                return "LONG_CALL";
            }
            // neutral zone between buy_max and sell_min
            if (strategyAllowed("BULL_CALL_SPREAD", tier)) return "BULL_CALL_SPREAD";
            return "LONG_CALL";
        }

        if (bias == DirectionalBias::Bearish) {
            if (iv_rank <= buy_max) {
                if (strategyAllowed("BEAR_PUT_SPREAD", tier)) return "BEAR_PUT_SPREAD";
                return "LONG_PUT";
            }
            if (iv_rank >= sell_min) {
                if (strategyAllowed("CC", tier)) return "CC";
                return "LONG_PUT";
            }
            if (strategyAllowed("BEAR_PUT_SPREAD", tier)) return "BEAR_PUT_SPREAD";
            return "LONG_PUT";
        }

        // Neutral — vol expansion play or income
        if (iv_rank < buy_max * 0.8) { // well below buy threshold → straddle
            if (strategyAllowed("STRADDLE", tier)) return "STRADDLE";
        }
        if (iv_rank >= sell_min) {
            if (strategyAllowed("STRANGLE", tier)) return "STRANGLE";
        }
        if (strategyAllowed("CSP", tier)) return "CSP";
        return "LONG_CALL";
    }

    // ── Contract parameter construction ───────────────────────────────────────
    //
    // Selects strike(s) and DTE based on strategy type.
    // Uses delta targets computed via Black-Scholes.
    // Returns false if no viable contract found.

    struct ContractParams {
        double strike  = 0.0;
        double strike2 = 0.0; // 0 = single-leg
        double expiry  = 0.0; // years to expiration
        std::string expiry_date;
    };

    ContractParams buildContractParams(const std::string& strategy,
                                       double spot, double atr,
                                       double rfr, double iv_sigma) const {
        ContractParams p;

        // Target DTE from profile
        int target_dte = profile_.dte_long;
        if (strategy == "CSP" || strategy == "CC")
            target_dte = profile_.dte_income;
        else if (strategy == "STRADDLE" || strategy == "STRANGLE" ||
                 strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD")
            target_dte = profile_.dte_spread;

        p.expiry = target_dte / 365.0;

        // Approximate expiry date string (calendar days from today)
        auto now    = std::chrono::system_clock::now();
        auto exp_tp = now + std::chrono::hours(24 * target_dte);
        auto exp_t  = std::chrono::system_clock::to_time_t(exp_tp);
        std::tm* tm_ptr = std::gmtime(&exp_t);
        std::ostringstream oss;
        oss << std::put_time(tm_ptr, "%Y-%m-%d");
        p.expiry_date = oss.str();

        // Target strikes via delta search
        // Long/spread: Δ ≈ 0.45 (near-ATM)
        // Income (short): Δ ≈ 0.25 (OTM)
        // Spread second leg: Δ ≈ 0.15

        auto findStrikeForDelta = [&](double target_delta,
                                      nox::options::OptionType opt_type) -> double {
            // Walk strikes in $1 increments from deep OTM toward ATM
            double best_strike = spot;
            double best_diff   = 1e9;
            for (int offset = -50; offset <= 50; ++offset) {
                double s = std::round(spot / atr) * atr + offset * (atr * 0.5);
                if (s <= 0) continue;
                nox::options::OptionContract c;
                c.underlying     = spot;
                c.strike         = s;
                c.expiry         = p.expiry;
                c.risk_free_rate = rfr;
                c.volatility     = iv_sigma;
                c.type           = opt_type;
                double d = std::abs(nox::options::bs_delta(c, iv_sigma));
                if (std::abs(d - target_delta) < best_diff) {
                    best_diff   = std::abs(d - target_delta);
                    best_strike = s;
                }
            }
            return best_strike;
        };

        const double d_long = profile_.delta_long;
        const double d_inc  = profile_.delta_income;
        const double d_wing = profile_.delta_spread_wing;

        if (strategy == "LONG_CALL") {
            p.strike = findStrikeForDelta(d_long, nox::options::OptionType::Call);
        } else if (strategy == "LONG_PUT") {
            p.strike = findStrikeForDelta(d_long, nox::options::OptionType::Put);
        } else if (strategy == "CSP") {
            p.strike = findStrikeForDelta(d_inc, nox::options::OptionType::Put);
        } else if (strategy == "CC") {
            p.strike = findStrikeForDelta(d_inc, nox::options::OptionType::Call);
        } else if (strategy == "BULL_CALL_SPREAD") {
            p.strike  = findStrikeForDelta(d_long, nox::options::OptionType::Call);
            p.strike2 = findStrikeForDelta(d_wing, nox::options::OptionType::Call);
            if (p.strike2 <= p.strike) p.strike2 = p.strike + atr;
        } else if (strategy == "BEAR_PUT_SPREAD") {
            p.strike  = findStrikeForDelta(d_long, nox::options::OptionType::Put);
            p.strike2 = findStrikeForDelta(d_wing, nox::options::OptionType::Put);
            if (p.strike2 >= p.strike) p.strike2 = p.strike - atr;
        } else if (strategy == "STRADDLE") {
            p.strike = std::round(spot);
        } else if (strategy == "STRANGLE") {
            p.strike  = findStrikeForDelta(d_inc, nox::options::OptionType::Call);
            p.strike2 = findStrikeForDelta(d_inc, nox::options::OptionType::Put);
        }
        return p;
    }

    // ── Signal assembly ───────────────────────────────────────────────────────

    OptionsSignal assembleSignal(const std::string& ticker,
                                 const UnderlyingData& d,
                                 const std::string& strategy,
                                 const ContractParams& cp,
                                 double iv_rank, double iv_sigma,
                                 double rfr, double confidence,
                                 const std::string& tier,
                                 bool fc_mode, double allocated_capital) const
    {
        using namespace nox::options;

        OptionsSignal sig;
        sig.underlying        = ticker;
        sig.strategy          = strategy;
        sig.expiry_date       = cp.expiry_date;
        sig.strike            = cp.strike;
        sig.strike2           = cp.strike2;
        sig.iv_rank           = iv_rank;
        sig.rsi               = d.rsi14;
        sig.atr               = d.atr14;
        sig.confidence        = confidence;
        sig.capital_tier      = tier;
        sig.free_capital_mode = fc_mode;
        sig.allocated_capital = allocated_capital;

        // Primary leg
        OptionContract primary;
        primary.underlying     = d.price;
        primary.strike         = cp.strike;
        primary.expiry         = cp.expiry;
        primary.risk_free_rate = rfr;
        primary.volatility     = iv_sigma;

        bool is_call = (strategy == "LONG_CALL" || strategy == "CC" ||
                        strategy == "BULL_CALL_SPREAD" || strategy == "STRADDLE");
        primary.type   = is_call ? OptionType::Call : OptionType::Put;
        sig.option_type = primary.type;

        OptionGreeks g1 = compute_greeks(primary);

        double max_risk   = computeMaxRisk(allocated_capital, tier);
        double contracts  = std::max(1.0, std::floor(max_risk / (g1.price * 100.0)));

        // Per-contract P&L geometry
        if (strategy == "LONG_CALL" || strategy == "LONG_PUT") {
            sig.entry_price = g1.price;
            sig.max_risk    = g1.price * 100.0 * contracts;
            sig.max_reward  = 999999.0; // theoretically unlimited — sentinel
            sig.breakeven   = (strategy == "LONG_CALL")
                              ? cp.strike + g1.price
                              : cp.strike - g1.price;
            sig.greeks      = g1;

        } else if (strategy == "CSP" || strategy == "CC") {
            // Short premium — receive credit, max risk is the spread width
            sig.entry_price = g1.price;
            sig.max_risk    = (strategy == "CSP")
                              ? (cp.strike - g1.price) * 100.0 * contracts
                              : (d.price   - g1.price) * 100.0 * contracts;
            sig.max_reward  = g1.price * 100.0 * contracts;
            sig.breakeven   = (strategy == "CSP")
                              ? cp.strike - g1.price
                              : cp.strike + g1.price;
            sig.greeks      = g1;

        } else if (strategy == "BULL_CALL_SPREAD" || strategy == "BEAR_PUT_SPREAD") {
            OptionContract sell_leg = primary;
            sell_leg.strike = cp.strike2;
            OptionGreeks g2 = compute_greeks(sell_leg);

            double debit       = g1.price - g2.price;
            double spread_width = std::abs(cp.strike2 - cp.strike);
            sig.entry_price = debit;
            sig.max_risk    = debit * 100.0 * contracts;
            sig.max_reward  = (spread_width - debit) * 100.0 * contracts;
            sig.breakeven   = (strategy == "BULL_CALL_SPREAD")
                              ? cp.strike + debit
                              : cp.strike - debit;
            // Net Greeks
            sig.greeks.price = debit;
            sig.greeks.delta = g1.delta - g2.delta;
            sig.greeks.gamma = g1.gamma - g2.gamma;
            sig.greeks.theta = g1.theta - g2.theta;
            sig.greeks.vega  = g1.vega  - g2.vega;
            sig.greeks.implied_volatility = iv_sigma;

        } else if (strategy == "STRADDLE") {
            OptionContract put_leg = primary;
            put_leg.type = OptionType::Put;
            OptionGreeks gp = compute_greeks(put_leg);

            double total_debit = g1.price + gp.price;
            sig.entry_price = total_debit;
            sig.max_risk    = total_debit * 100.0 * contracts;
            sig.max_reward  = 999999.0;
            sig.breakeven   = cp.strike + total_debit; // upper breakeven (lower = strike - debit)
            sig.greeks.price = total_debit;
            sig.greeks.delta = g1.delta + gp.delta;
            sig.greeks.gamma = g1.gamma + gp.gamma;
            sig.greeks.theta = g1.theta + gp.theta;
            sig.greeks.vega  = g1.vega  + gp.vega;
            sig.greeks.implied_volatility = iv_sigma;

        } else if (strategy == "STRANGLE") {
            OptionContract put_leg = primary;
            put_leg.type   = OptionType::Put;
            put_leg.strike = cp.strike2;
            OptionGreeks gp = compute_greeks(put_leg);

            double total_debit = g1.price + gp.price;
            sig.entry_price = total_debit;
            sig.max_risk    = total_debit * 100.0 * contracts;
            sig.max_reward  = 999999.0;
            sig.breakeven   = cp.strike + total_debit;
            sig.greeks.price = total_debit;
            sig.greeks.delta = g1.delta + gp.delta;
            sig.greeks.gamma = g1.gamma + gp.gamma;
            sig.greeks.theta = g1.theta + gp.theta;
            sig.greeks.vega  = g1.vega  + gp.vega;
            sig.greeks.implied_volatility = iv_sigma;
        }

        // Rationale
        sig.rationale    = buildRationale(strategy, d, iv_rank);
        sig.profile_name = profile_.name;
        return sig;
    }

    static std::string buildRationale(const std::string& strategy,
                                      const UnderlyingData& d, double iv_rank)
    {
        std::string r = strategy + " on " +
                        (d.price > d.sma20 ? "bullish" : "bearish") + " bias. ";
        r += "RSI=" + fmt(d.rsi14) + ", ATR=" + fmt(d.atr14) + ". ";
        r += "IV Rank=" + fmt(iv_rank, 0) + "% — ";
        r += (iv_rank < 30) ? "cheap vol, buy premium."
           : (iv_rank > 50) ? "expensive vol, sell premium."
           : "neutral vol zone.";
        return r;
    }

    // ── Telegram formatting ───────────────────────────────────────────────────

    std::string formatAlert(const OptionsSignal& s, double vix,
                            const AllocationStrategy& regime) const
    {
        std::string leg2_str = (s.strike2 > 0)
            ? " / $" + fmt(s.strike2, 0)
            : "";

        std::string strategy_label = s.strategy;
        // Human-readable strategy names
        if (s.strategy == "LONG_CALL")          strategy_label = "Long Call";
        else if (s.strategy == "LONG_PUT")       strategy_label = "Long Put";
        else if (s.strategy == "CSP")            strategy_label = "Cash-Secured Put";
        else if (s.strategy == "CC")             strategy_label = "Covered Call";
        else if (s.strategy == "BULL_CALL_SPREAD") strategy_label = "Bull Call Spread";
        else if (s.strategy == "BEAR_PUT_SPREAD")  strategy_label = "Bear Put Spread";
        else if (s.strategy == "STRADDLE")       strategy_label = "Long Straddle";
        else if (s.strategy == "STRANGLE")       strategy_label = "Long Strangle";

        bool unlimited_reward = (s.max_reward > 999990.0);
        std::string reward_str = unlimited_reward ? "Unlimited" : "$" + fmt(s.max_reward, 0);

        std::string rr_str;
        if (!unlimited_reward && s.max_risk > 0) {
            rr_str = "\n📊 *R:R Ratio:* " + fmt(s.max_reward / s.max_risk, 1) + ":1";
        }

        std::string regime_emoji = (regime.current_regime == Regime::RISK_ON)    ? "✅"
                                 : (regime.current_regime == Regime::RISK_OFF)   ? "🔴"
                                 : "🟡";

        std::string fc_footer = s.free_capital_mode
            ? "\n⚡ _Free Capital Mode — $" + fmt(s.allocated_capital, 0) + " allocated_"
            : "";

        int conf_pct = static_cast<int>(std::round(s.confidence * 100.0));

        std::string iv_zone = (s.iv_rank < 30) ? "LOW — buy premium zone ✅"
                            : (s.iv_rank > 50) ? "HIGH — sell premium zone ✅"
                            : "NEUTRAL";

        std::string alert =
            "📊 *OPTIONS SIGNAL — " + s.underlying + "* [" + s.profile_name + " · " + s.capital_tier + "]\n"
            "────────────────────────────────────\n"
            "🎯 *Strategy:* " + strategy_label + "\n"
            "📅 *Expiry:* " + s.expiry_date + "\n"
            "💵 *Strike(s):* $" + fmt(s.strike, 0) + leg2_str + "\n"
            "💰 *Entry:* $" + fmt(s.entry_price) +
                " | Max Risk: $" + fmt(s.max_risk, 0) +
                " | Max Gain: " + reward_str +
            rr_str + "\n"
            "⚖️ *Breakeven:* $" + fmt(s.breakeven) + "\n"
            "\n📐 *Greeks*\n"
            "• Delta: " + fmt(s.greeks.delta, 3) +
            " | Gamma: "  + fmt(s.greeks.gamma, 4) + "\n"
            "• Theta: $" + fmt(s.greeks.theta, 4) + "/day"
            " | Vega: "  + fmt(s.greeks.vega, 3) + "\n"
            "• IV Rank: " + fmt(s.iv_rank, 0) + "% ← " + iv_zone + "\n"
            "\n📈 *Technicals — " + s.underlying + "*\n"
            "• RSI(14): " + fmt(s.rsi) +
            " | ATR(14): $" + fmt(s.atr) + "\n"
            "\n🌐 *Macro Regime:* " + regime.log_message + " " + regime_emoji +
            " (VIX " + fmt(vix, 1) + ")\n"
            "🎯 *Signal Confidence:* " + std::to_string(conf_pct) + "%\n"
            "\n📋 _" + s.rationale + "_" +
            fc_footer + "\n"
            "⚠️ _Advisory only — manual execution required._";

        return alert;
    }

    // ── Telegram dispatch ─────────────────────────────────────────────────────

    void sendTelegram(const std::string& message) const {
        if (tgToken_.empty() || tgChatId_.empty()) return;
        try {
            httplib::Client cli("https://api.telegram.org");
            cli.set_connection_timeout(std::chrono::seconds(5));
            cli.set_read_timeout(std::chrono::seconds(10));

            const size_t MAX_LEN = 4000;
            size_t offset = 0;
            while (offset < message.size()) {
                std::string chunk = message.substr(offset, MAX_LEN);
                offset += MAX_LEN;

                json body = {
                    {"chat_id",    tgChatId_},
                    {"text",       chunk},
                    {"parse_mode", "Markdown"}
                };
                std::string path = "/bot" + tgToken_ + "/sendMessage";
                cli.Post(path.c_str(), body.dump(), "application/json");
            }
        } catch (...) {
            log("WARN", "[OPTIONS_SIGNAL] Telegram delivery failed.");
        }
    }

    // ── Per-ticker scan orchestrator ──────────────────────────────────────────

    void scanTicker(const std::string& ticker,
                    double effective_capital, const std::string& tier, bool fc_mode,
                    double vix, const SpySnapshot& spy,
                    const AllocationStrategy& regime)
    {
        log("INFO", "[OPTIONS_SCAN] Scanning " + ticker + "...");

        UnderlyingData d = fetchUnderlyingBars(ticker);
        if (!d.valid) {
            log("WARN", "[OPTIONS_SCAN] No bar data for " + ticker + " — skipping.");
            return;
        }

        double iv_rank  = fetchIVRank(ticker, vix);

        // IV sigma for Black-Scholes: convert IV rank to approximate σ
        // Heuristic: IV rank 0→0.15 σ, rank 50→0.25, rank 100→0.45
        double iv_sigma = 0.15 + (iv_rank / 100.0) * 0.30;

        double rfr = 0.05; // US risk-free rate (approximate)

        DirectionalBias bias     = computeBias(d);
        std::string     strategy = selectStrategy(bias, iv_rank, tier);

        double conf = regimeConfidence(regime.current_regime, strategy);
        if (conf < 1e-6) {
            log("INFO", "[OPTIONS_SCAN] " + ticker + " / " + strategy +
                " suppressed by RISK_OFF regime.");
            return;
        }

        ContractParams cp = buildContractParams(strategy, d.price, d.atr14, rfr, iv_sigma);
        if (cp.strike <= 0.0) {
            log("WARN", "[OPTIONS_SCAN] Could not determine valid strike for " + ticker);
            return;
        }

        OptionsSignal sig = assembleSignal(ticker, d, strategy, cp,
                                           iv_rank, iv_sigma, rfr, conf,
                                           tier, fc_mode, effective_capital);

        std::string alert = formatAlert(sig, vix, regime);
        sendTelegram(alert);
        log("INFO", "[OPTIONS_SCAN] Signal emitted: " + ticker + " / " + strategy +
            " | conf=" + fmt(conf * 100.0, 0) + "%");

        if (profile_.auto_execute) {
            executeSignal(sig);
        }
    }

    // ── Live order execution via OptionsOrderRouter ───────────────────────────

    void executeSignal(const OptionsSignal& sig) const {
        // Include the router here (inside the function) to break the forward-declaration
        // dependency. OptionsOrderRouter.hpp must be included in main.cpp before this
        // header for ODR compliance; the router is instantiated per-signal, which is
        // fine given the low frequency of options scans.
        nox::options_router::OptionsOrderRouter router(alpacaUrl_, apiKey_, apiSec_);
        auto result = router.route(sig, profile_.qty_contracts);

        if (result.success) {
            log("INFO", "[OPTIONS_EXEC] Order placed — " + result.message);
            sendTelegram(
                "✅ *OPTIONS ORDER PLACED*\n"
                "────────────────────────\n"
                "• *Ticker:* " + sig.underlying + "\n"
                "• *Strategy:* " + sig.strategy + "\n"
                "• *Contracts:* " + std::to_string(profile_.qty_contracts) + "\n"
                "• *Expiry:* " + sig.expiry_date + "\n"
                "• *Order ID:* `" + result.order_id + "`"
            );
        } else {
            log("WARN", "[OPTIONS_EXEC] Order failed — " + result.message);
            sendTelegram(
                "🚨 *OPTIONS ORDER FAILED*\n"
                "────────────────────────\n"
                "• *Ticker:* " + sig.underlying + "\n"
                "• *Strategy:* " + sig.strategy + "\n"
                "• *Reason:* `" + result.message + "`"
            );
        }
    }
};

} // namespace nox::options_signal
