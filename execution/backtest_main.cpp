// backtest_main.cpp — Nox Options Strategy Backtester
//
// Replays OptionsSignalGenerator logic on historical OHLCV from Yahoo Finance,
// simulates option P&L using Black-Scholes re-pricing with daily mark-to-model,
// and reports win rates, avg P&L, and directional accuracy per strategy/ticker.
//
// Build:
//   g++ -std=c++17 -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -I. \
//       -o nox_backtest backtest_main.cpp -lssl -lcrypto -lpthread
//
// Usage:
//   ./nox_backtest [key=value ...]
//
//   watchlist=SPY,QQQ,AAPL,TSLA,NVDA
//   range=2y          (Yahoo Finance range: 1y, 2y, 5y)
//   scan=5            (scan every N trading days — 5 = weekly)
//   profit=0.50       (exit at 50% of max profit)
//   stop=2.0          (exit at 2x debit/credit paid on a SHORT position — a
//                      long's value floors at 0, so mult>=1.0 clamps to a
//                      100%-loss/worthless exit; use stop<1.0 to bail earlier
//                      on longs, e.g. stop=0.5 exits at a 50% premium loss)
//   capital=35000     (determines strategy tier gate)
//   profile=personal  (use aggressive personal profile; default = bot)
//   fillmodel=adverse_selection  (also model asymmetric fills — see below)
//   fillgamma=0.2      fillqueuemult=3.0      fillseed=<n>
//
// Methodology:
//   - No real historical options chain: IV is proxied as HRV30 × 1.15
//     (HRV plus a modest variance-risk-premium assumption).
//   - All Greeks and prices are Black-Scholes European; no early-exercise value.
//   - Naive fill (default): every PROFIT_TARGET/STOP_LOSS touch fills at 100%.
//     Commissions and bid/ask spread are NOT modelled either way.
//   - Optional adverse-selection fill model (fillmodel=adverse_selection, see
//     BacktestFillModel.hpp): a touch that CROSSES the level always fills (the
//     "price moved against you" case); a touch that reverses without crossing
//     only fills probabilistically, scaled by underlying daily volume as a
//     liquidity PROXY (no real option-level volume/depth exists in this data
//     source). This corrects the naive model's tendency to always "catch" the
//     favorable reversals a real resting order would have missed.
//   - Use results to assess signal quality and strategy direction, not exact P&L.

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "OptionEngine.hpp"
#include "OptionsSignalTypes.hpp"
#include "BacktestFillModel.hpp"
#include "BacktestErrorModel.hpp"
#include "BacktestKellySizing.hpp"
#include "BacktestWalkForward.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using json = nlohmann::json;
using namespace nox::options;
using namespace nox::options_signal;
using namespace nox::backtest;

// ─── Data types ──────────────────────────────────────────────────────────────

struct Bar {
    std::string date;
    double high = 0.0, low = 0.0, close = 0.0;
    double volume = 0.0; // underlying volume; used only as a liquidity PROXY
};

struct BacktestConfig {
    std::vector<std::string> watchlist    = {"SPY","QQQ","AAPL","TSLA","NVDA"};
    std::string              range        = "2y";
    int    scan_every_n_days              = 5;
    double profit_target_pct             = 0.50;
    double stop_loss_mult                = 2.00;
    double initial_capital               = 35000.0; // ADVANCED tier by default
    double rfr                           = 0.05;
    // Audit §3 H5: entries were priced frictionless at the BS mid — "the
    // single biggest omitted cost, 2-10% each way retail" per the audit.
    // Both 0.0 by default (RULE-D5 no-op); set via CLI (haircutpct=/
    // commissionpercontract=) to see the cost-adjusted edge.
    double bid_ask_haircut_pct           = 0.0; // fraction of entry premium given up to the spread
    double commission_per_contract       = 0.0; // flat $ per contract per leg, charged at entry
    RiskProfile profile                  = RiskProfile::bot();
    FillModelConfig fill                 = FillModelConfig::fromEnv();
    ErrorConfig     errors               = ErrorConfig::fromEnv();
    KellySizingConfig kelly              = KellySizingConfig::fromEnv();
    WalkForwardConfig wfo                = WalkForwardConfig::fromEnv();
};

struct Trade {
    std::string ticker, strategy;
    std::string entry_date, exit_date, exit_reason;
    double spot_entry  = 0.0, spot_exit  = 0.0;
    double iv_entry    = 0.0, hrv_entry  = 0.0, rsi_entry = 0.0;
    double entry_price = 0.0, exit_price = 0.0;
    double pnl         = 0.0; // per underlying share; × 100 for dollar P&L
    // Counterfactual P&L had the position been held to expiry instead of
    // exiting on the profit-target / stop rule — the honest input a
    // "missed exit" error model needs (see BacktestErrorModel.hpp). Equals
    // pnl for trades that already ran to EXPIRY.
    double pnl_if_held_to_expiry = 0.0;
    bool   bias_right  = false;
    bool   is_long     = true;
    // Adverse-selection variant only: a profit-target touch that reversed
    // before ever getting filled (the naive model would have "caught" it).
    bool   target_touched_not_filled = false;
};

// ─── OHLCV fetch (Yahoo Finance) ──────────────────────────────────────────────

std::vector<Bar> fetchBars(const std::string& symbol, const std::string& range) {
    std::cerr << "  Fetching " << symbol << " (" << range << ")..." << std::flush;

    httplib::Client cli("https://query1.finance.yahoo.com");
    cli.set_connection_timeout(std::chrono::seconds(12));
    cli.set_read_timeout(std::chrono::seconds(20));

    std::string path = "/v8/finance/chart/" + symbol + "?interval=1d&range=" + range;
    auto res = cli.Get(path.c_str());
    if (!res || res->status != 200) {
        std::cerr << " FAILED (HTTP " << (res ? std::to_string(res->status) : "timeout") << ")\n";
        return {};
    }

    try {
        auto body      = json::parse(res->body);
        const auto& r  = body.at("chart").at("result").at(0);
        const auto& ts = r.at("timestamp");
        const auto& q  = r.at("indicators").at("quote").at(0);
        const auto& H  = q.at("high");
        const auto& L  = q.at("low");
        const auto& C  = q.at("close");
        const auto& V  = q.at("volume");

        std::vector<Bar> bars;
        bars.reserve(ts.size());
        for (size_t i = 0; i < ts.size(); ++i) {
            if (C[i].is_null()) continue;
            time_t t = ts[i].get<time_t>();
            std::tm buf{};
            gmtime_r(&t, &buf);
            std::ostringstream oss;
            oss << std::put_time(&buf, "%Y-%m-%d");
            bars.push_back({
                oss.str(),
                H[i].is_null() ? C[i].get<double>() : H[i].get<double>(),
                L[i].is_null() ? C[i].get<double>() : L[i].get<double>(),
                C[i].get<double>(),
                (i < V.size() && !V[i].is_null()) ? V[i].get<double>() : 0.0
            });
        }
        std::cerr << " " << bars.size() << " bars\n";
        return bars;
    } catch (...) {
        std::cerr << " parse error\n";
        return {};
    }
}

// Approximate day count since a fixed epoch, from a "YYYY-MM-DD" string.
// Used both for annualization spans AND (critically) to measure real ELAPSED
// CALENDAR DAYS between bars — bars are trading days, not calendar days, so a
// bar-index delta is NOT a day delta. A few days of calendar drift from
// ignoring real month lengths is immaterial at backtest scale. Single
// implementation (RULE-D6) — this used to be duplicated as kellyDayNumber/
// approxDayNumber further down.
long dateOrdinal(const std::string& date) {
    if (date.size() < 10) return 0;
    int y = std::stoi(date.substr(0, 4));
    int m = std::stoi(date.substr(5, 2));
    int d = std::stoi(date.substr(8, 2));
    return static_cast<long>(y) * 365 + m * 30 + d;
}

// ─── Technical indicators (strict no-lookahead: only bars[0..end_idx]) ───────

// Wilder's RSI-14: seed on 14 bars, smooth over the rest of the 50-bar window
double calcRSI(const std::vector<Bar>& b, size_t end) {
    if (end < 50) return 50.0;
    size_t seed = end - 49;
    double ag = 0.0, al = 0.0;
    for (size_t i = seed + 1; i <= seed + 14; ++i) {
        double d = b[i].close - b[i-1].close;
        if (d > 0) ag += d; else al -= d;
    }
    ag /= 14.0; al /= 14.0;
    for (size_t i = seed + 15; i <= end; ++i) {
        double d = b[i].close - b[i-1].close;
        ag = (ag * 13.0 + (d > 0 ? d : 0.0)) / 14.0;
        al = (al * 13.0 + (d < 0 ? -d : 0.0)) / 14.0;
    }
    return al < 1e-9 ? 100.0 : 100.0 - 100.0 / (1.0 + ag / al);
}

double calcSMA(const std::vector<Bar>& b, size_t end, int n) {
    if (end < static_cast<size_t>(n - 1)) return b[end].close;
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += b[end - i].close;
    return s / n;
}

// Rolling average underlying volume — used only as a liquidity PROXY for the
// adverse-selection fill model; no real option-level volume exists here.
double calcAvgVolume(const std::vector<Bar>& b, size_t end, int n = 20) {
    if (end < static_cast<size_t>(n - 1)) return b[end].volume;
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += b[end - i].volume;
    return s / n;
}

double calcATR(const std::vector<Bar>& b, size_t end, int n = 14) {
    if (end < static_cast<size_t>(n)) return 0.01 * b[end].close;
    double s = 0.0;
    for (int i = 0; i < n; ++i) {
        size_t idx = end - i;
        s += std::max({b[idx].high - b[idx].low,
                       std::abs(b[idx].high - b[idx-1].close),
                       std::abs(b[idx].low  - b[idx-1].close)});
    }
    return s / n;
}

// 30-day annualized close-to-close HRV (mean=0 assumption)
double calcHRV(const std::vector<Bar>& b, size_t end, int n = 30) {
    if (end < static_cast<size_t>(n + 1)) return 0.20;
    double sq = 0.0;
    for (int i = 1; i <= n; ++i) {
        double r = std::log(b[end - i + 1].close / b[end - i].close);
        sq += r * r;
    }
    return std::sqrt(sq / n * 252.0);
}

// ─── Signal logic (mirrors live engine) ──────────────────────────────────────

enum class Bias { Bullish, Bearish, Neutral };

Bias computeBias(double price, double sma20, double sma50, double rsi) {
    bool above20 = price > sma20, above50 = price > sma50;
    bool rbull = rsi >= 40.0 && rsi <= 70.0;
    bool rbear = rsi >= 30.0 && rsi <= 55.0;
    if (above20 && above50 && rbull) return Bias::Bullish;
    if (!above20 && !above50 && rbear) return Bias::Bearish;
    return Bias::Neutral;
}

std::string pickStrategy(Bias bias, double iv, double hrv,
                         const RiskProfile& prof, const std::string& tier) {
    bool vol_rich  = hrv > 0.01 && iv > hrv * 1.20;
    bool prefer_sell = vol_rich; // sell when options overprice realized vol
    // if not rich, default to buying premium
    bool prefer_buy  = !prefer_sell;

    auto ok = [&](const std::string& s) -> bool {
        if (!prof.enforce_tier_gates) return true;
        if (tier == "STARTER")  return s == "LONG_CALL" || s == "LONG_PUT";
        if (tier == "STANDARD") return s=="LONG_CALL"||s=="LONG_PUT"||s=="CSP"||s=="CC";
        return true; // ADVANCED / FREE_CAPITAL
    };

    if (bias == Bias::Bullish) {
        if (prefer_sell && ok("CSP"))            return "CSP";
        if (ok("BULL_CALL_SPREAD"))              return "BULL_CALL_SPREAD";
        return "LONG_CALL";
    }
    if (bias == Bias::Bearish) {
        if (prefer_sell && ok("CC"))             return "CC";
        if (ok("BEAR_PUT_SPREAD"))               return "BEAR_PUT_SPREAD";
        return "LONG_PUT";
    }
    // Neutral — vol play
    if (prefer_sell && ok("STRANGLE"))           return "STRANGLE";
    if (prefer_buy  && ok("STRADDLE"))           return "STRADDLE";
    if (ok("CSP"))                               return "CSP";
    return "LONG_CALL";
}

// Number of option legs opened for a strategy — only used to scale a flat
// per-contract commission (audit §3 H5). Matches buildEntrySetup()'s own
// strategy set; anything not listed defaults to 1 (the single-leg shape).
int legsForStrategy(const std::string& strat) {
    if (strat == "BULL_CALL_SPREAD" || strat == "BEAR_PUT_SPREAD" ||
        strat == "STRADDLE"         || strat == "STRANGLE") return 2;
    return 1;
}

// ─── Strike selection (delta-targeted, standard listed increments) ────────────

double findStrikeForDelta(double spot, double expiry_yrs, double iv,
                          double target_delta, OptionType type, double rfr) {
    double step = (spot < 25.0) ? 0.50 : (spot < 200.0) ? 1.0 : 5.0;
    double atm  = std::round(spot / step) * step;
    double best = atm, best_diff = 1e9;
    for (int off = -30; off <= 30; ++off) {
        double s = atm + off * step;
        if (s <= 0.0) continue;
        OptionContract c;
        c.underlying = spot; c.strike = s; c.expiry = expiry_yrs;
        c.risk_free_rate = rfr; c.volatility = iv; c.type = type;
        double diff = std::abs(std::abs(bs_delta(c, iv)) - target_delta);
        if (diff < best_diff) { best_diff = diff; best = s; }
    }
    return best;
}

// ─── Position valuation using BS re-pricing ──────────────────────────────────

// Returns fair value of the position at (spot, t_rem, iv). Per-share, before ×100.
// For short positions (CSP/CC) this is the cost to close.
double valuePosition(const std::string& strat,
                     double spot, double t_rem, double iv, double rfr,
                     double K1, double K2, OptionType leg1_type) {
    auto bsp = [&](double K, OptionType t) -> double {
        if (t_rem <= 0.0)
            return t == OptionType::Call ? std::max(0.0, spot - K)
                                         : std::max(0.0, K - spot);
        OptionContract c;
        c.underlying = spot; c.strike = K; c.expiry = t_rem;
        c.risk_free_rate = rfr; c.volatility = iv; c.type = t;
        return bs_price(c, iv);
    };

    if (strat == "LONG_CALL" || strat == "CSP")  return bsp(K1, OptionType::Call);
    if (strat == "LONG_PUT"  || strat == "CC")   return bsp(K1, OptionType::Put);
    if (strat == "BULL_CALL_SPREAD")  return bsp(K1, OptionType::Call) - bsp(K2, OptionType::Call);
    if (strat == "BEAR_PUT_SPREAD")   return bsp(K1, OptionType::Put)  - bsp(K2, OptionType::Put);
    if (strat == "STRADDLE")          return bsp(K1, OptionType::Call) + bsp(K1, OptionType::Put);
    if (strat == "STRANGLE")          return bsp(K1, OptionType::Call) + bsp(K2, OptionType::Put);
    // fallback
    return bsp(K1, leg1_type);
}

// ─── Entry setup (strike/DTE selection, shared by both simulate variants) ────

struct EntrySetup {
    double K1 = 0.0, K2 = 0.0;
    OptionType leg1_type = OptionType::Call;
    int    dte           = 0;
    double expiry_yrs    = 0.0;
    double entry_price   = 0.0;
    bool   is_long       = true;
    bool   bias_bullish  = false;
    bool   bias_bearish  = false;
};

EntrySetup buildEntrySetup(const std::string& strat, double spot0,
                          double iv_entry, double rfr, const RiskProfile& prof) {
    EntrySetup es;

    // DTE from profile
    es.dte = prof.dte_long;
    if (strat == "CSP" || strat == "CC")
        es.dte = prof.dte_income;
    else if (strat == "BULL_CALL_SPREAD" || strat == "BEAR_PUT_SPREAD" ||
             strat == "STRADDLE"         || strat == "STRANGLE")
        es.dte = prof.dte_spread;
    es.expiry_yrs = es.dte / 365.0;

    // Strike selection
    double K1 = spot0, K2 = 0.0;
    OptionType leg1_type = OptionType::Call;

    if (strat == "LONG_CALL") {
        K1 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_long, OptionType::Call, rfr);
        leg1_type = OptionType::Call;
    } else if (strat == "LONG_PUT") {
        K1 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_long, OptionType::Put, rfr);
        leg1_type = OptionType::Put;
    } else if (strat == "CSP") {
        K1 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_income, OptionType::Put, rfr);
        leg1_type = OptionType::Put;
    } else if (strat == "CC") {
        K1 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_income, OptionType::Call, rfr);
        leg1_type = OptionType::Call;
    } else if (strat == "BULL_CALL_SPREAD") {
        K1 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_long,        OptionType::Call, rfr);
        K2 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_spread_wing, OptionType::Call, rfr);
        if (K2 <= K1) K2 = K1 + ((spot0 < 200.0) ? 1.0 : 5.0);
        leg1_type = OptionType::Call;
    } else if (strat == "BEAR_PUT_SPREAD") {
        K1 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_long,        OptionType::Put, rfr);
        K2 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_spread_wing, OptionType::Put, rfr);
        if (K2 >= K1) K2 = K1 - ((spot0 < 200.0) ? 1.0 : 5.0);
        leg1_type = OptionType::Put;
    } else if (strat == "STRADDLE") {
        K1 = std::round(spot0);
        leg1_type = OptionType::Call;
    } else if (strat == "STRANGLE") {
        K1 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_income, OptionType::Call, rfr);
        K2 = findStrikeForDelta(spot0, es.expiry_yrs, iv_entry, prof.delta_income, OptionType::Put,  rfr);
        leg1_type = OptionType::Call;
    }

    es.K1 = K1; es.K2 = K2; es.leg1_type = leg1_type;
    es.entry_price = valuePosition(strat, spot0, es.expiry_yrs, iv_entry, rfr, K1, K2, leg1_type);

    // Long positions pay debit; short positions receive credit. STRANGLE is
    // sold for a credit too — the live engine now routes it as a real short
    // strangle (Track 2 §2 C3), so the backtester's sign convention has to
    // match or it's pricing a position the live system never takes.
    es.is_long = (strat != "CSP" && strat != "CC" && strat != "STRANGLE");

    // Directional check: did price go the right way?
    es.bias_bullish = (strat == "LONG_CALL" || strat == "BULL_CALL_SPREAD" || strat == "CSP");
    es.bias_bearish = (strat == "LONG_PUT"  || strat == "BEAR_PUT_SPREAD"  || strat == "CC");

    return es;
}

// Applies the bid-ask haircut + flat per-leg commission to a freshly-built
// entry price (audit §3 H5 — "entries frictionless at BS mid ... the single
// biggest omitted cost"). Both knobs default to 0.0 (RULE-D5 no-op), so this
// is an identity transform unless a caller opts in via CLI. Long positions
// pay more; short/credit positions collect less (and can go to 0 if costs
// exceed the whole credit — same as a real trade that isn't worth opening).
void applyEntryCosts(EntrySetup& es, const std::string& strat, const BacktestConfig& cfg) {
    if (cfg.bid_ask_haircut_pct <= 0.0 && cfg.commission_per_contract <= 0.0) return;
    double commission_per_share = legsForStrategy(strat) * cfg.commission_per_contract / 100.0;
    if (es.is_long) {
        es.entry_price = es.entry_price * (1.0 + cfg.bid_ask_haircut_pct) + commission_per_share;
    } else {
        es.entry_price = std::max(0.0, es.entry_price * (1.0 - cfg.bid_ask_haircut_pct) - commission_per_share);
    }
}

// ─── Simulate one trade ───────────────────────────────────────────────────────

Trade simulateTrade(const std::string& ticker,
                    const std::string& strat,
                    const std::vector<Bar>& bars,
                    size_t entry_idx,
                    const BacktestConfig& cfg,
                    double iv_entry, double hrv_entry, double rsi_entry,
                    // Audit §3 H2: walk-forward train-window trades used to read
                    // bars all the way to bars.size(), so a trade opened near the
                    // end of the train window could resolve using TEST-window
                    // price action — the grid search that picks (profit_target,
                    // stop_loss) off train P&L was therefore partly informed by
                    // out-of-sample data. max_bar_idx caps how far the exit search
                    // is allowed to read; SIZE_MAX (default) is the old, unbounded
                    // behavior used everywhere except the WFO train-window call.
                    size_t max_bar_idx = static_cast<size_t>(-1))
{
    const RiskProfile& prof = cfg.profile;
    double spot0 = bars[entry_idx].close;
    double rfr   = cfg.rfr;

    EntrySetup es = buildEntrySetup(strat, spot0, iv_entry, rfr, prof);
    applyEntryCosts(es, strat, cfg);
    if (es.entry_price <= 0.01) {
        // Spread collapsed or invalid IV, or costs consumed the whole credit — skip
        Trade t; t.exit_reason = "INVALID"; return t;
    }

    Trade t;
    t.ticker      = ticker;
    t.strategy    = strat;
    t.entry_date  = bars[entry_idx].date;
    t.spot_entry  = spot0;
    t.iv_entry    = iv_entry;
    t.hrv_entry   = hrv_entry;
    t.rsi_entry   = rsi_entry;
    t.entry_price = es.entry_price;
    t.is_long     = es.is_long;
    t.exit_reason = "EXPIRY";

    double exit_price = es.entry_price;
    size_t exit_idx   = entry_idx;
    size_t last_in_dte_idx = entry_idx; // last bar seen with elapsed calendar days <= dte

    // Bars are TRADING days, but es.dte is CALENDAR days (a real listed
    // option's expiration is a calendar date, not a bar count) — stepping by
    // bar index and calling that index "day" understates real elapsed time by
    // ~7/5x, so theta decay lagged the realized-vol clock it was priced
    // against (audit §3 C1: ~9.5% structural variance subsidy to long
    // premium). Use the bars' own dates to measure real elapsed calendar days.
    long entry_ord = dateOrdinal(bars[entry_idx].date);
    bool capped = false;
    for (size_t idx = entry_idx + 1; idx < bars.size() && idx <= max_bar_idx; ++idx) {
        long elapsed_days = dateOrdinal(bars[idx].date) - entry_ord;
        if (elapsed_days > es.dte) break; // past the option's real expiration
        last_in_dte_idx = idx;

        double spot   = bars[idx].close;
        double t_rem  = std::max(0.0, es.expiry_yrs - elapsed_days / 365.0);
        double hrv_d  = calcHRV(bars, idx);
        double iv_d   = hrv_d * 1.15;

        double cur    = valuePosition(strat, spot, t_rem, iv_d, rfr, es.K1, es.K2, es.leg1_type);
        double pnl_d  = es.is_long ? (cur - es.entry_price) : (es.entry_price - cur);

        if (pnl_d >= es.entry_price * cfg.profit_target_pct) {
            exit_price = cur; exit_idx = idx; t.exit_reason = "PROFIT_TARGET"; break;
        }
        if (pnl_d <= -(es.entry_price * cfg.stop_loss_mult)) {
            exit_price = cur; exit_idx = idx; t.exit_reason = "STOP_LOSS"; break;
        }

        exit_price = cur;
        exit_idx   = idx;
        capped     = (idx == max_bar_idx);
    }
    // Loop stopped at the window boundary before a natural PROFIT_TARGET/
    // STOP_LOSS/expiry — mark-to-market at that bar rather than mislabeling
    // it "EXPIRY" (it wasn't). Only ever reachable when a caller passes a
    // real max_bar_idx (the WFO train-window grid search); every other
    // caller keeps the old unbounded behavior.
    if (capped && t.exit_reason == "EXPIRY") t.exit_reason = "WINDOW_END";

    t.exit_price = exit_price;
    t.exit_date  = bars[exit_idx].date;
    t.spot_exit  = bars[exit_idx].close;
    t.pnl        = es.is_long ? (exit_price - es.entry_price) : (es.entry_price - exit_price);

    // Counterfactual: value the position at the last reachable bar within its
    // real (calendar-day) DTE (the held-to-expiry outcome) regardless of the
    // exit rule that fired, so an error model can ask "what if the exit had
    // been missed?" honestly instead of guessing a give-back multiplier.
    {
        size_t exp_idx = last_in_dte_idx;
        long elapsed_exp = dateOrdinal(bars[exp_idx].date) - entry_ord;
        double t_rem_exp = std::max(0.0, es.expiry_yrs - elapsed_exp / 365.0);
        double hrv_exp   = calcHRV(bars, exp_idx);
        double iv_exp    = hrv_exp * 1.15;
        double val_exp   = valuePosition(strat, bars[exp_idx].close, t_rem_exp, iv_exp, rfr,
                                         es.K1, es.K2, es.leg1_type);
        t.pnl_if_held_to_expiry = es.is_long ? (val_exp - es.entry_price)
                                             : (es.entry_price - val_exp);
    }

    if (es.bias_bullish)      t.bias_right = t.spot_exit > t.spot_entry;
    else if (es.bias_bearish) t.bias_right = t.spot_exit < t.spot_entry;
    else {
        // Vol play: right if actual move > expected one-SD move. es.expiry_yrs
        // is already a calendar-year fraction (dte/365) — HRV's sqrt(252)
        // annualization converts trading-day variance to ANNUAL (calendar)
        // variance, so scaling by sqrt(expiry_yrs) is the consistent basis
        // (dte/252 here double-counted the trading/calendar mismatch, same
        // class of bug as the decay loop above).
        double expected_move = hrv_entry * t.spot_entry * std::sqrt(es.expiry_yrs);
        t.bias_right = std::abs(t.spot_exit - t.spot_entry) > expected_move;
    }

    return t;
}

// Reprices the position at a day's underlying low/high/close to build an
// approximate daily value RANGE for the option position — there is no real
// option-level OHLC in this data source, so this is a modeled proxy, not a
// traded range.
OptionBarRange syntheticOptionBar(const std::string& strat, double t_rem, double iv, double rfr,
                                  double K1, double K2, OptionType leg1_type,
                                  double spot_low, double spot_high, double spot_close) {
    double v_low   = valuePosition(strat, spot_low,   t_rem, iv, rfr, K1, K2, leg1_type);
    double v_high  = valuePosition(strat, spot_high,  t_rem, iv, rfr, K1, K2, leg1_type);
    double v_close = valuePosition(strat, spot_close, t_rem, iv, rfr, K1, K2, leg1_type);
    OptionBarRange r;
    r.low         = std::min(v_low, v_high);
    r.high        = std::max(v_low, v_high);
    r.close_value = v_close;
    return r;
}

// Adverse-selection variant of simulateTrade(): the PROFIT_TARGET exit is a
// resting limit order that only fills for certain when price crosses through
// it; STOP_LOSS is treated as a stop-market order (fills for certain once
// touched, force_certain=true). See BacktestFillModel.hpp for the fill math.
Trade simulateTradeAdverseSelection(const std::string& ticker,
                                    const std::string& strat,
                                    const std::vector<Bar>& bars,
                                    size_t entry_idx,
                                    const BacktestConfig& cfg,
                                    double iv_entry, double hrv_entry, double rsi_entry,
                                    std::mt19937& rng)
{
    const RiskProfile& prof = cfg.profile;
    double spot0 = bars[entry_idx].close;
    double rfr   = cfg.rfr;

    EntrySetup es = buildEntrySetup(strat, spot0, iv_entry, rfr, prof);
    applyEntryCosts(es, strat, cfg);
    if (es.entry_price <= 0.01) {
        Trade t; t.exit_reason = "INVALID"; return t;
    }

    Trade t;
    t.ticker      = ticker;
    t.strategy    = strat;
    t.entry_date  = bars[entry_idx].date;
    t.spot_entry  = spot0;
    t.iv_entry    = iv_entry;
    t.hrv_entry   = hrv_entry;
    t.rsi_entry   = rsi_entry;
    t.entry_price = es.entry_price;
    t.is_long     = es.is_long;
    t.exit_reason = "EXPIRY";

    double target_level = profitTargetLevel(es.entry_price, cfg.profit_target_pct, es.is_long);
    double stop_level    = stopLossLevel(es.entry_price, cfg.stop_loss_mult, es.is_long);
    bool target_from_above = approachedFromAboveForTarget(es.is_long);
    bool stop_from_above    = approachedFromAboveForStop(es.is_long);

    double exit_price = es.entry_price;
    size_t exit_idx   = entry_idx;

    // See simulateTrade()'s comment on entry_ord/elapsed_days — same
    // trading-day-vs-calendar-day fix, same class of bug (RULE-D6).
    long entry_ord = dateOrdinal(bars[entry_idx].date);
    for (size_t idx = entry_idx + 1; idx < bars.size(); ++idx) {
        long elapsed_days = dateOrdinal(bars[idx].date) - entry_ord;
        if (elapsed_days > es.dte) break;

        double t_rem  = std::max(0.0, es.expiry_yrs - elapsed_days / 365.0);
        double hrv_d  = calcHRV(bars, idx);
        double iv_d   = hrv_d * 1.15;

        OptionBarRange ob = syntheticOptionBar(strat, t_rem, iv_d, rfr, es.K1, es.K2, es.leg1_type,
                                               bars[idx].low, bars[idx].high, bars[idx].close);
        double day_vol = bars[idx].volume;
        double avg_vol = calcAvgVolume(bars, idx, 20);

        FillOutcome fo_target = simulateFill(ob, target_level, target_from_above,
                                             day_vol, avg_vol, cfg.fill, rng, false);
        if (fo_target.touched && !fo_target.filled) t.target_touched_not_filled = true;
        if (fo_target.filled) {
            exit_price = target_level; exit_idx = idx; t.exit_reason = "PROFIT_TARGET"; break;
        }

        FillOutcome fo_stop = simulateFill(ob, stop_level, stop_from_above,
                                           day_vol, avg_vol, cfg.fill, rng, true);
        if (fo_stop.filled) {
            exit_price = stop_level; exit_idx = idx; t.exit_reason = "STOP_LOSS"; break;
        }

        exit_price = ob.close_value;
        exit_idx   = idx;
    }

    t.exit_price = exit_price;
    t.exit_date  = bars[exit_idx].date;
    t.spot_exit  = bars[exit_idx].close;
    t.pnl        = es.is_long ? (exit_price - es.entry_price) : (es.entry_price - exit_price);

    if (es.bias_bullish)      t.bias_right = t.spot_exit > t.spot_entry;
    else if (es.bias_bearish) t.bias_right = t.spot_exit < t.spot_entry;
    else {
        double expected_move = hrv_entry * t.spot_entry * std::sqrt(es.expiry_yrs);
        t.bias_right = std::abs(t.spot_exit - t.spot_entry) > expected_move;
    }

    return t;
}

// ─── Run full backtest ────────────────────────────────────────────────────────

struct BacktestResult {
    std::vector<Trade> baseline; // naive 100%-fill simulation — always computed
    std::vector<Trade> variant;  // adverse-selection fill simulation — only when requested
};

BacktestResult runBacktest(const BacktestConfig& cfg) {
    // Capital tier from initial_capital
    std::string tier = "STARTER";
    if      (cfg.initial_capital >= 75000.0) tier = "FREE_CAPITAL";
    else if (cfg.initial_capital >= 30000.0) tier = "ADVANCED";
    else if (cfg.initial_capital >= 5000.0)  tier = "STANDARD";

    bool run_variant = (cfg.fill.mode != FillMode::Naive);
    std::mt19937 rng(cfg.fill.rng_seed != 0 ? cfg.fill.rng_seed
                                             : static_cast<unsigned>(std::random_device{}()));

    BacktestResult result;

    for (const auto& ticker : cfg.watchlist) {
        auto bars = fetchBars(ticker, cfg.range);
        if (bars.size() < 55) {
            std::cerr << "  Skipping " << ticker << " — too few bars (" << bars.size() << ")\n";
            continue;
        }

        size_t n_signals = 0;
        for (size_t i = 51; i + 5 < bars.size(); i += static_cast<size_t>(cfg.scan_every_n_days)) {
            double spot  = bars[i].close;
            double sma20 = calcSMA(bars, i, 20);
            double sma50 = calcSMA(bars, i, 50);
            double rsi   = calcRSI(bars, i);
            double hrv   = calcHRV(bars, i);
            double iv    = hrv * 1.15; // IV proxy: HRV + 15% variance premium

            Bias bias        = computeBias(spot, sma20, sma50, rsi);
            std::string strat = pickStrategy(bias, iv, hrv, cfg.profile, tier);

            Trade t = simulateTrade(ticker, strat, bars, i, cfg, iv, hrv, rsi);
            if (t.exit_reason != "INVALID") {
                result.baseline.push_back(t);
                ++n_signals;
                if (run_variant) {
                    Trade tv = simulateTradeAdverseSelection(ticker, strat, bars, i, cfg, iv, hrv, rsi, rng);
                    result.variant.push_back(tv);
                }
            }
        }
        std::cerr << "  " << ticker << ": " << n_signals << " signals\n";
    }

    return result;
}

// ─── Walk-forward optimization ────────────────────────────────────────────────
//
// Generates signals+trades over one contiguous bar-index range [start, end)
// using the given (possibly parameter-overridden) config. Shared by both the
// train-window grid search and the test-window out-of-sample application
// below so the scan-and-simulate loop isn't duplicated between them.
std::vector<Trade> simulateSignalsInRange(const std::vector<Bar>& bars, size_t start, size_t end,
                                          const BacktestConfig& cfg, const std::string& ticker,
                                          const std::string& tier,
                                          // See simulateTrade()'s comment (audit §3 H2). Pass
                                          // fold.train_end-1 from the WFO train-window grid
                                          // search; leave at the default everywhere else
                                          // (including the WFO test/out-of-sample call, which
                                          // is meant to run a trade to its natural exit).
                                          size_t max_bar_idx = static_cast<size_t>(-1)) {
    std::vector<Trade> trades;
    size_t i = std::max<size_t>(start, 51);
    for (; i < end && i + 5 < bars.size(); i += static_cast<size_t>(cfg.scan_every_n_days)) {
        double spot  = bars[i].close;
        double sma20 = calcSMA(bars, i, 20);
        double sma50 = calcSMA(bars, i, 50);
        double rsi   = calcRSI(bars, i);
        double hrv   = calcHRV(bars, i);
        double iv    = hrv * 1.15;

        Bias bias        = computeBias(spot, sma20, sma50, rsi);
        std::string strat = pickStrategy(bias, iv, hrv, cfg.profile, tier);

        Trade t = simulateTrade(ticker, strat, bars, i, cfg, iv, hrv, rsi, max_bar_idx);
        if (t.exit_reason != "INVALID") trades.push_back(t);
    }
    return trades;
}

struct WalkForwardFoldResult {
    std::string ticker;
    int    fold_index    = 0;
    double chosen_profit  = 0.0;
    double chosen_stop    = 0.0;
    int    train_trades    = 0;
    int    test_trades     = 0;
    double test_pnl        = 0.0; // per-share; ×100 for dollars, same convention as Trade::pnl
};

struct WalkForwardReport {
    std::vector<Trade> oos_trades; // out-of-sample trades ONLY — never train-window trades
    std::vector<WalkForwardFoldResult> fold_results;
};

// Runs the walk-forward loop described at the top of this section: for each
// fold, grid-search (profit_target, stop_loss) on the TRAIN window only (by
// total train P&L — simple and transparent rather than a fancier objective
// that would need its own justification), then apply that winning combo to
// the immediately-following TEST window and keep only the test-window
// trades. A trade opened near the end of the train or test window is allowed
// to run to its natural DTE exit even if that reads bars past the window
// boundary — same as a real position does not stop existing at an arbitrary
// calendar cutoff; only the PARAMETER CHOICE is constrained to train-window
// information, not the trade's own lifetime.
WalkForwardReport runWalkForward(const BacktestConfig& cfg) {
    std::string tier = "STARTER";
    if      (cfg.initial_capital >= 75000.0) tier = "FREE_CAPITAL";
    else if (cfg.initial_capital >= 30000.0) tier = "ADVANCED";
    else if (cfg.initial_capital >= 5000.0)  tier = "STANDARD";

    WalkForwardReport report;

    for (const auto& ticker : cfg.watchlist) {
        auto bars = fetchBars(ticker, cfg.range);
        if (bars.size() < 55) {
            std::cerr << "  Skipping " << ticker << " — too few bars (" << bars.size() << ")\n";
            continue;
        }

        auto folds = generateFolds(bars.size(), cfg.wfo.train_days, cfg.wfo.test_days, cfg.wfo.step_days);
        if (folds.empty()) {
            std::cerr << "  Skipping " << ticker << " — not enough history for even one WFO fold "
                      << "(need " << (cfg.wfo.train_days + cfg.wfo.test_days) << " bars, have "
                      << bars.size() << ")\n";
            continue;
        }

        for (size_t f = 0; f < folds.size(); ++f) {
            const auto& fold = folds[f];

            // Grid search on the TRAIN window only.
            double best_profit = cfg.wfo.profit_grid.front();
            double best_stop   = cfg.wfo.stop_grid.front();
            double best_total_pnl = -1e18;
            int    best_train_count = 0;

            for (double p : cfg.wfo.profit_grid) {
                for (double s : cfg.wfo.stop_grid) {
                    BacktestConfig trial = cfg;
                    trial.profit_target_pct = p;
                    trial.stop_loss_mult    = s;
                    // Cap at fold.train_end-1 (audit §3 H2): a trade opened near the
                    // train window's end must not resolve using test-window bars, or
                    // the grid search that picks (profit_target, stop_loss) from train
                    // P&L would be partly informed by out-of-sample price action.
                    auto train_trades = simulateSignalsInRange(bars, fold.train_start, fold.train_end,
                                                               trial, ticker, tier, fold.train_end - 1);
                    double total = 0.0;
                    for (const auto& t : train_trades) total += t.pnl;
                    if (total > best_total_pnl) {
                        best_total_pnl  = total;
                        best_profit      = p;
                        best_stop        = s;
                        best_train_count = static_cast<int>(train_trades.size());
                    }
                }
            }

            // Apply the winning train-window combo to the TEST window — this
            // is the only part of each fold that ends up in the report.
            BacktestConfig chosen = cfg;
            chosen.profit_target_pct = best_profit;
            chosen.stop_loss_mult    = best_stop;
            auto test_trades = simulateSignalsInRange(bars, fold.test_start, fold.test_end,
                                                      chosen, ticker, tier);

            double test_pnl = 0.0;
            for (const auto& t : test_trades) test_pnl += t.pnl;

            report.fold_results.push_back({ticker, static_cast<int>(f), best_profit, best_stop,
                                           best_train_count, static_cast<int>(test_trades.size()), test_pnl});
            report.oos_trades.insert(report.oos_trades.end(), test_trades.begin(), test_trades.end());
        }

        std::cerr << "  " << ticker << ": " << folds.size() << " WFO fold(s)\n";
    }

    return report;
}

// ─── Fractional-Kelly equity-curve simulation ─────────────────────────────────
//
// Answers "can I backtest different Kelly rules": runs the SAME baseline
// trade sequence through a single compounding equity curve, sizing each
// trade via calculateKellyContracts() at a given kelly_fraction, so the
// sweep_fractions in KellySizingConfig can be compared side by side on
// ending equity / CAGR / max drawdown — exactly the fractional-Kelly
// variance/growth trade-off (Half-Kelly vs quarter-Kelly, etc.).

struct KellyEquityResult {
    double kelly_fraction    = 0.0;
    double starting_equity    = 0.0;
    double ending_equity      = 0.0;
    double cagr               = 0.0;
    double max_drawdown_pct    = 0.0;
    int    trades_sized        = 0;
    int    trades_halted       = 0; // negative-Kelly / sub-1-contract — RULE-005
};

static long kellyDayNumber(const std::string& date) { return dateOrdinal(date); }

KellyEquityResult simulateKellyEquityCurve(std::vector<Trade> trades, // sorted copy
                                           const KellySizingConfig& kcfg,
                                           double starting_equity,
                                           double kelly_fraction) {
    std::sort(trades.begin(), trades.end(), [](const Trade& a, const Trade& b) {
        return a.entry_date < b.entry_date; // "YYYY-MM-DD" sorts lexicographically
    });

    KellyEquityResult r;
    r.kelly_fraction  = kelly_fraction;
    r.starting_equity = starting_equity;

    double equity = starting_equity;
    double peak = equity, max_dd_pct = 0.0;
    RollingTradeStats stats(kcfg.rolling_window);

    long first_day = trades.empty() ? 0 : kellyDayNumber(trades.front().entry_date);
    long last_day  = first_day;

    for (const auto& t : trades) {
        double win_rate, wlr;
        if (stats.count() >= kcfg.min_trades_for_stats) {
            std::tie(win_rate, wlr) = stats.stats();
            if (wlr <= 0.0) { win_rate = kcfg.seed_win_rate; wlr = kcfg.seed_win_loss_ratio; }
        } else {
            win_rate = kcfg.seed_win_rate;
            wlr      = kcfg.seed_win_loss_ratio;
        }

        int contracts = calculateKellyContracts(equity, t.entry_price * 100.0,
                                                 win_rate, wlr, kelly_fraction, kcfg.hard_cap);
        if (contracts < 0) {
            ++r.trades_halted;
        } else {
            ++r.trades_sized;
            equity += t.pnl * 100.0 * contracts;
            peak = std::max(peak, equity);
            if (peak > 0.0) max_dd_pct = std::max(max_dd_pct, (peak - equity) / peak);
        }

        // Record the outcome AFTER sizing this trade — stats used to size a
        // trade must never include that same trade's own result.
        stats.record(t.pnl);
        last_day = std::max(last_day, kellyDayNumber(t.exit_date));
    }

    r.ending_equity   = equity;
    r.max_drawdown_pct = max_dd_pct;
    double years = std::max(1.0 / 365.0, static_cast<double>(last_day - first_day) / 365.0);
    r.cagr = (starting_equity > 0.0 && equity > 0.0)
                 ? std::pow(equity / starting_equity, 1.0 / years) - 1.0
                 : -1.0;
    return r;
}

std::vector<KellyEquityResult> runKellySweep(const std::vector<Trade>& trades,
                                             const KellySizingConfig& kcfg,
                                             double starting_equity) {
    std::vector<KellyEquityResult> results;
    results.reserve(kcfg.sweep_fractions.size());
    for (double frac : kcfg.sweep_fractions)
        results.push_back(simulateKellyEquityCurve(trades, kcfg, starting_equity, frac));
    return results;
}

// ─── Formatted report ─────────────────────────────────────────────────────────

namespace {

std::string pct(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << v * 100.0 << "%";
    return o.str();
}

std::string dollar(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    o << (v >= 0 ? "+" : "") << v;
    return o.str();
}

std::string line(char c, int n) { return n > 0 ? std::string(n, c) : ""; }

long approxDayNumber(const std::string& date) { return dateOrdinal(date); }

// Sharpe ratio over the trade P&L series, annualized by the backtest's own
// trade frequency (n trades / span in years) rather than a fixed 252 — a
// weekly-scan backtest and a daily-scan backtest do not have the same
// trades-per-year, and using 252 unconditionally would overstate Sharpe for
// a low-frequency scan. This is a TRADE-level Sharpe (variance across
// per-trade P&L), not a daily-return Sharpe — reported as such below.
double tradeSharpe(const std::vector<Trade>& trades) {
    size_t n = trades.size();
    if (n < 2) return 0.0;

    double mean = 0.0;
    for (const auto& t : trades) mean += t.pnl;
    mean /= static_cast<double>(n);

    double var = 0.0;
    for (const auto& t : trades) var += (t.pnl - mean) * (t.pnl - mean);
    var /= static_cast<double>(n - 1);
    double stdev = std::sqrt(var);
    if (stdev < 1e-9) return 0.0;

    long first = approxDayNumber(trades.front().entry_date);
    long last  = approxDayNumber(trades.back().exit_date);
    for (const auto& t : trades) {
        first = std::min(first, approxDayNumber(t.entry_date));
        last  = std::max(last,  approxDayNumber(t.exit_date));
    }
    double span_years = std::max(1.0 / 365.0, static_cast<double>(last - first) / 365.0);
    double trades_per_year = static_cast<double>(n) / span_years;

    return (mean / stdev) * std::sqrt(trades_per_year);
}

void printSection(const std::string& title) {
    int pad = 55 - 4 - static_cast<int>(title.size());
    std::cout << "\n-- " << title << " " << line('-', pad > 0 ? pad : 2) << "\n";
}

void printStratTable(const std::map<std::string, std::vector<double>>& data,
                     const std::string& col1_header, int col1_w) {
    std::cout << std::left
              << std::setw(col1_w)  << col1_header
              << std::setw(8)       << "Trades"
              << std::setw(10)      << "Win %"
              << std::setw(14)      << "Avg P&L"
              << std::setw(14)      << "Total P&L"
              << "\n";
    std::cout << line('-', 56) << "\n";
    for (const auto& [key, pnls] : data) {
        int w = 0; double sum = 0.0;
        for (double p : pnls) { if (p > 0) w++; sum += p; }
        double avg = sum / static_cast<double>(pnls.size());
        std::cout << std::left
                  << std::setw(col1_w)  << key
                  << std::setw(8)       << pnls.size()
                  << std::setw(10)      << pct(static_cast<double>(w) / pnls.size())
                  << std::setw(14)      << ("$" + dollar(avg * 100.0))
                  << "$" << dollar(sum * 100.0)
                  << "\n";
    }
}

} // anonymous namespace

void printReport(const std::vector<Trade>& trades, const BacktestConfig& cfg) {
    std::cout << "\n" << line('=', 62) << "\n";
    std::cout << "  NOX OPTIONS BACKTESTER\n";
    std::cout << "  Profile : " << cfg.profile.name
              << " | Range: " << cfg.range
              << " | Scan: every " << cfg.scan_every_n_days << " days\n";
    std::cout << "  Capital : $" << static_cast<int>(cfg.initial_capital)
              << " | ProfitTarget: " << static_cast<int>(cfg.profit_target_pct * 100) << "%"
              << " | StopLoss: " << cfg.stop_loss_mult << "×\n";
    std::cout << "  Tickers : ";
    for (const auto& t : cfg.watchlist) std::cout << t << " ";
    std::cout << "\n" << line('=', 62) << "\n";

    if (trades.empty()) { std::cout << "  No trades generated.\n" << line('=', 62) << "\n"; return; }

    int n    = static_cast<int>(trades.size());
    int wins = 0;
    double total_pnl = 0.0;
    int exit_profit = 0, exit_loss = 0, exit_expiry = 0;
    int dir_bull = 0, dir_bull_right = 0;
    int dir_bear = 0, dir_bear_right = 0;
    int vol_trades = 0, vol_right = 0;

    std::map<std::string, std::vector<double>> by_strat, by_ticker;

    // Running drawdown
    double running = 0.0, peak = 0.0, max_dd = 0.0;

    for (const auto& t : trades) {
        double pnl_d = t.pnl; // per-share; × 100 for dollars
        if (t.pnl > 0.0) wins++;
        total_pnl += pnl_d;
        by_strat[t.strategy].push_back(pnl_d);
        by_ticker[t.ticker].push_back(pnl_d);

        running += pnl_d * 100.0;
        peak     = std::max(peak, running);
        max_dd   = std::min(max_dd, running - peak);

        if (t.exit_reason == "PROFIT_TARGET")   ++exit_profit;
        else if (t.exit_reason == "STOP_LOSS")  ++exit_loss;
        else                                    ++exit_expiry;

        bool bull = (t.strategy=="LONG_CALL"||t.strategy=="BULL_CALL_SPREAD"||t.strategy=="CSP");
        bool bear = (t.strategy=="LONG_PUT" ||t.strategy=="BEAR_PUT_SPREAD" ||t.strategy=="CC");
        if (bull) { ++dir_bull; if (t.bias_right) ++dir_bull_right; }
        else if (bear) { ++dir_bear; if (t.bias_right) ++dir_bear_right; }
        else { ++vol_trades; if (t.bias_right) ++vol_right; }
    }

    printSection("Overall");
    std::cout << std::left
              << std::setw(26) << "Total trades"     << ": " << n << "\n"
              << std::setw(26) << "Win rate"          << ": " << pct(static_cast<double>(wins) / n)
              << "  (" << wins << " W / " << (n - wins) << " L)\n"
              << std::setw(26) << "Avg P&L per trade" << ": $" << dollar(total_pnl * 100.0 / n) << "\n"
              << std::setw(26) << "Total P&L (1 ctr)" << ": $" << dollar(total_pnl * 100.0) << "\n"
              << std::setw(26) << "Max drawdown"      << ": $" << dollar(max_dd) << "\n"
              << std::setw(26) << "Sharpe (trade-level)" << ": " << std::fixed << std::setprecision(2)
              << tradeSharpe(trades) << "\n"
              << std::setw(26) << "Exit: profit target" << ": " << exit_profit
              << "  (" << pct(static_cast<double>(exit_profit) / n) << ")\n"
              << std::setw(26) << "Exit: stop loss"   << ": " << exit_loss
              << "  (" << pct(static_cast<double>(exit_loss)   / n) << ")\n"
              << std::setw(26) << "Exit: held to expiry" << ": " << exit_expiry
              << "  (" << pct(static_cast<double>(exit_expiry) / n) << ")\n";

    printSection("By Strategy");
    printStratTable(by_strat, "Strategy", 24);

    printSection("By Ticker");
    printStratTable(by_ticker, "Ticker", 10);

    printSection("Directional Accuracy");
    if (dir_bull > 0)
        std::cout << "Bullish signals : " << dir_bull_right << " / " << dir_bull
                  << " (" << pct(static_cast<double>(dir_bull_right) / dir_bull) << ") correct\n";
    if (dir_bear > 0)
        std::cout << "Bearish signals : " << dir_bear_right << " / " << dir_bear
                  << " (" << pct(static_cast<double>(dir_bear_right) / dir_bear) << ") correct\n";
    if (vol_trades > 0)
        std::cout << "Vol plays       : " << vol_right << " / " << vol_trades
                  << " (" << pct(static_cast<double>(vol_right) / vol_trades) << ") hit expected move\n";
    std::cout << "\n  Note: 50% = coin flip. Look for >52% on ≥30 signals to be meaningful.\n";

    printSection("HRV vs IV (vol richness at entry)");
    {
        int rich = 0, cheap = 0, fair = 0;
        double pnl_rich = 0.0, pnl_cheap = 0.0, pnl_fair = 0.0;
        for (const auto& t : trades) {
            bool r = t.hrv_entry > 0.01 && t.iv_entry > t.hrv_entry * 1.20;
            bool c = t.hrv_entry > 0.01 && t.iv_entry < t.hrv_entry * 0.90;
            if (r)      { ++rich;  pnl_rich  += t.pnl; }
            else if (c) { ++cheap; pnl_cheap += t.pnl; }
            else        { ++fair;  pnl_fair  += t.pnl; }
        }
        auto row = [&](const std::string& label, int cnt, double pnl) {
            if (cnt == 0) return;
            std::cout << std::left << std::setw(22) << label << ": "
                      << std::setw(5) << cnt << " trades | avg $"
                      << dollar(pnl * 100.0 / cnt) << "\n";
        };
        row("Vol RICH  (IV>HRV×1.20)", rich,  pnl_rich);
        row("Vol CHEAP (IV<HRV×0.90)", cheap, pnl_cheap);
        row("Vol FAIR",                fair,  pnl_fair);
        std::cout << "\n  If RICH avg > FAIR avg: variance premium signal has real edge.\n";
    }

    printSection("Methodology");
    std::cout << "  Sharpe     : TRADE-level (variance across per-trade P&L, annualized by\n"
              << "               this backtest's own trades/year) — NOT a daily-return Sharpe.\n"
              << "               Comparable across runs of this backtester only, not to a\n"
              << "               live daily_ledger Sharpe (see heartbeat/alpha_decay_monitor.py).\n"
              << "  IV proxy   : HRV30 × 1.15 (no historical options chain available)\n"
              << "  Pricing    : Black-Scholes European, re-priced daily at mark-to-model\n"
              << "  Fill model : NAIVE (this report) — every PROFIT_TARGET/STOP_LOSS touch\n"
              << "               fills at 100%, no slippage/commissions. Rerun with\n"
              << "               fillmodel=adverse_selection to see a fill model that only\n"
              << "               guarantees fills on the trades that moved against you.\n"
              << "  P&L scale  : per 1 contract = × $100. Multiply by your contract count.\n"
              << "  When ready : integrate a real historical options chain (Polygon.io,\n"
              << "               CBOE DataShop) to replace the IV proxy with real market prices.\n";

    std::cout << "\n" << line('=', 62) << "\n\n";
}

// Prints a side-by-side comparison of the naive baseline vs. the
// adverse-selection variant — only called when fillmodel=adverse_selection
// was requested, so a run without it never shows a meaningless "variant ==
// baseline" section.
void printFillComparison(const std::vector<Trade>& baseline, const std::vector<Trade>& variant,
                         const FillModelConfig& fill_cfg) {
    printSection("Fill-Model Comparison (Naive vs. Adverse-Selection)");
    if (baseline.empty() || variant.empty()) {
        std::cout << "  Not enough trades to compare.\n" << line('=', 62) << "\n\n";
        return;
    }

    auto stats = [](const std::vector<Trade>& trades) {
        int wins = 0; double total = 0.0;
        for (const auto& t : trades) { if (t.pnl > 0.0) ++wins; total += t.pnl; }
        int n = static_cast<int>(trades.size());
        return std::make_tuple(static_cast<double>(wins) / n, total * 100.0 / n, total * 100.0);
    };
    auto [wr_base, avg_base, tot_base] = stats(baseline);
    auto [wr_var,  avg_var,  tot_var]  = stats(variant);

    int touched_not_filled = 0;
    for (const auto& t : variant) if (t.target_touched_not_filled) ++touched_not_filled;

    std::cout << "  gamma=" << fill_cfg.gamma << "  queue_mult=" << fill_cfg.queue_mult << "\n\n";
    std::cout << std::left << std::setw(22) << "" << std::setw(14) << "Naive"
              << "Adverse-Selection\n";
    std::cout << std::left << std::setw(22) << "Win rate"
              << std::setw(14) << pct(wr_base) << pct(wr_var) << "\n";
    std::cout << std::left << std::setw(22) << "Avg P&L per trade"
              << std::setw(14) << ("$" + dollar(avg_base)) << ("$" + dollar(avg_var)) << "\n";
    std::cout << std::left << std::setw(22) << "Total P&L (1 ctr)"
              << std::setw(14) << ("$" + dollar(tot_base)) << ("$" + dollar(tot_var)) << "\n";
    std::cout << "\n  Profit-target touches never filled: " << touched_not_filled
              << " / " << variant.size() << " trades\n"
              << "  (these are fills the naive model always \"caught\" that a real\n"
              << "   resting limit order would have missed)\n";
    std::cout << "\n" << line('=', 62) << "\n\n";
}

// Error-injection resilience comparison: the clean baseline vs. the same trades
// with operational errors (ghost fills, missed exits, adverse fills) injected.
// Answers "does the edge survive the drawdown errors inflict, or does it flip
// the strategy to a loss?" — reported as P&L retention + a survival verdict.
void printErrorInjectionReport(const std::vector<Trade>& trades, const ErrorConfig& ecfg) {
    printSection("Error-Injection Resilience (Clean vs. Errors)");
    if (trades.empty()) {
        std::cout << "  No trades to stress.\n" << line('=', 62) << "\n\n";
        return;
    }

    std::vector<TradeView> views;
    views.reserve(trades.size());
    for (const auto& t : trades)
        views.push_back({t.entry_price, t.pnl, t.pnl_if_held_to_expiry, t.exit_reason});

    ErrorImpact impact = applyErrors(views, ecfg);
    ErrorSummary s     = summarize(views, impact);

    std::cout << "  Injected rates : ghost_fill=" << pct(ecfg.ghost_fill_rate)
              << "  missed_exit=" << pct(ecfg.missed_exit_rate)
              << "  adverse_fill=" << pct(ecfg.adverse_fill_rate)
              << " (slip " << pct(ecfg.adverse_slippage_pct) << ")\n";
    std::cout << "  Seed           : " << ecfg.seed << "  (deterministic)\n";
    std::cout << "  Errors fired   : " << impact.ghost_fills << " ghost fill(s), "
              << impact.missed_exits << " missed exit(s), "
              << impact.adverse_fills << " adverse fill(s)\n\n";

    std::cout << std::left << std::setw(26) << "" << std::setw(16) << "Clean"
              << "With Errors\n";
    std::cout << std::left << std::setw(26) << "Total P&L (1 ctr)"
              << std::setw(16) << ("$" + dollar(s.baseline_total * 100.0))
              << ("$" + dollar(s.injected_total * 100.0)) << "\n";
    std::cout << std::left << std::setw(26) << "Max drawdown"
              << std::setw(16) << ("$" + dollar(s.max_drawdown_baseline * 100.0))
              << ("$" + dollar(s.max_drawdown_injected * 100.0)) << "\n";
    std::cout << std::left << std::setw(26) << "P&L retained"
              << pct(s.retention) << " of the clean run's profit\n\n";

    if (s.still_profitable)
        std::cout << "  VERDICT: ✅ STILL PROFITABLE through injected errors "
                  << "(kept " << pct(s.retention) << " of clean P&L).\n";
    else
        std::cout << "  VERDICT: ❌ NOT profitable once these errors hit — the edge does\n"
                  << "           not absorb this error rate. Tighten the defense that\n"
                  << "           prevents whichever error dominates above.\n";

    std::cout << "\n  Note: errors are the UN-defended counterfactual — Phase 1's ghost-fill\n"
              << "  reconciliation, the exit monitor, and the fill model exist precisely to\n"
              << "  keep these rates near zero live. This measures exposure if one lapses.\n";
    std::cout << "\n" << line('=', 62) << "\n\n";
}

// Compares several fractional-Kelly rules against the SAME baseline trade
// sequence, each run through its own compounding equity curve — this is the
// direct answer to "can I backtest different Kelly rules".
void printKellySweepReport(const std::vector<KellyEquityResult>& sweep, const KellySizingConfig& kcfg) {
    printSection("Fractional-Kelly Sweep (compounding equity curve per fraction)");
    if (sweep.empty()) {
        std::cout << "  No trades to size.\n" << line('=', 62) << "\n\n";
        return;
    }
    std::cout << "  Rolling W/R window: " << kcfg.rolling_window << " trades  |  Hard cap: "
              << pct(kcfg.hard_cap) << "  |  Seed W/R (cold start): "
              << pct(kcfg.seed_win_rate) << " / " << kcfg.seed_win_loss_ratio << "\n\n";
    std::cout << std::left
              << std::setw(10) << "Fraction" << std::setw(16) << "Ending Equity"
              << std::setw(10) << "CAGR" << std::setw(10) << "Max DD" << std::setw(8) << "Sized"
              << "Halted\n";
    std::cout << line('-', 62) << "\n";
    for (const auto& r : sweep) {
        std::ostringstream frac; frac << r.kelly_fraction << "x";
        std::cout << std::left
                  << std::setw(10) << frac.str()
                  << std::setw(16) << ("$" + dollar(r.ending_equity))
                  << std::setw(10) << pct(r.cagr)
                  << std::setw(10) << pct(r.max_drawdown_pct)
                  << std::setw(8)  << r.trades_sized
                  << r.trades_halted << "\n";
    }
    std::cout << "\n  Halted = RULE-005: negative Kelly or sub-1-contract allocation, trade\n"
              << "  skipped rather than forced. Higher fractions compound faster but with\n"
              << "  deeper drawdowns — this is the actual variance/growth trade-off, not\n"
              << "  a fixed multiplier applied after the fact.\n";
    std::cout << "\n" << line('=', 62) << "\n\n";
}

// Reports ONLY the out-of-sample (test-window) performance across every WFO
// fold — this is the honest number. A per-fold table shows which
// (profit_target, stop_loss) combo won each train window, so a parameter
// that keeps winning across folds (real signal) is visible from one that
// flops fold to fold (overfit to that specific train window).
void printWalkForwardReport(const WalkForwardReport& wfo, const BacktestConfig& cfg) {
    printSection("Walk-Forward Optimization — Out-of-Sample Only");
    if (wfo.fold_results.empty()) {
        std::cout << "  No folds produced — need at least "
                  << (cfg.wfo.train_days + cfg.wfo.test_days) << " bars of history per ticker.\n"
                  << line('=', 62) << "\n\n";
        return;
    }
    std::cout << "  Train: " << cfg.wfo.train_days << " bars | Test: " << cfg.wfo.test_days
              << " bars | Step: " << cfg.wfo.step_days << " bars\n"
              << "  Profit grid: ";
    for (double p : cfg.wfo.profit_grid) std::cout << pct(p) << " ";
    std::cout << " | Stop grid: ";
    for (double s : cfg.wfo.stop_grid) std::cout << s << "x ";
    std::cout << "\n\n";

    std::cout << std::left
              << std::setw(8)  << "Ticker" << std::setw(6) << "Fold" << std::setw(10) << "Profit*"
              << std::setw(8)  << "Stop*"  << std::setw(8) << "Train"  << std::setw(8) << "Test"
              << "Test P&L\n";
    std::cout << line('-', 62) << "\n";
    for (const auto& r : wfo.fold_results) {
        std::cout << std::left
                  << std::setw(8) << r.ticker << std::setw(6) << r.fold_index
                  << std::setw(10) << pct(r.chosen_profit) << std::setw(8) << r.chosen_stop
                  << std::setw(8) << r.train_trades << std::setw(8) << r.test_trades
                  << "$" << dollar(r.test_pnl * 100.0) << "\n";
    }

    if (wfo.oos_trades.empty()) {
        std::cout << "\n  No out-of-sample trades were generated by any fold.\n";
    } else {
        int n = static_cast<int>(wfo.oos_trades.size());
        int wins = 0; double total = 0.0;
        for (const auto& t : wfo.oos_trades) { if (t.pnl > 0.0) ++wins; total += t.pnl; }
        std::cout << "\n  Aggregate out-of-sample: " << n << " trades | win rate "
                  << pct(static_cast<double>(wins) / n) << " | total P&L $"
                  << dollar(total * 100.0) << " | avg P&L $" << dollar(total * 100.0 / n) << "\n"
                  << "\n  This is the number to trust over the single-run backtest above — every\n"
                  << "  trade here was generated with parameters chosen from data STRICTLY\n"
                  << "  before it, never from the window it was then graded on.\n";
    }
    std::cout << "\n" << line('=', 62) << "\n\n";
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    BacktestConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout <<
                "Usage: nox_backtest [key=value ...]\n\n"
                "  watchlist=SPY,QQQ,AAPL     comma-separated tickers\n"
                "  range=2y                   Yahoo Finance range (1y, 2y, 5y)\n"
                "  scan=5                     scan every N trading days\n"
                "  profit=0.50                exit at X% of max profit\n"
                "  stop=2.0                   stop loss at X× debit paid\n"
                "  capital=35000              starting capital (sets tier gate)\n"
                "  haircutpct=0.05            bid-ask haircut at entry, fraction of premium (0=frictionless)\n"
                "  commissionpercontract=0.65 flat $ commission per contract per leg at entry (0=none)\n"
                "  profile=personal           use aggressive personal profile\n"
                "  fillmodel=adverse_selection  also model asymmetric fills (see header)\n"
                "  fillgamma=0.2              touch-fill liquidity scale factor\n"
                "  fillqueuemult=3.0          queue-ahead size, × rolling avg volume\n"
                "  fillseed=<n>               seed the fill-model RNG (0 = random)\n"
                "  errghostrate=0.05          inject ghost-fill (double-lot) errors at this rate\n"
                "  errmissedrate=0.10         inject missed-exit (rode to expiry) errors\n"
                "  erradverserate=0.15        inject adverse-fill (slippage) errors\n"
                "  errslippct=0.15            adverse-fill haircut, fraction of entry premium\n"
                "  errseed=<n>                seed the error-injection RNG (deterministic)\n"
                "  kelly=1                    size trades via fractional Kelly, sweep report\n"
                "  kellysweep=0.1,0.25,0.5,1.0  fractions to compare (default shown)\n"
                "  kellywindow=20             trailing closed trades used for causal W/R\n"
                "  kellycap=0.10              hard cap on Kelly-adjusted risk per trade\n"
                "  kellyseedwr=0.50           cold-start win rate before window fills\n"
                "  kellyseedwlr=1.5           cold-start win/loss ratio before window fills\n"
                "  wfo=1                      run walk-forward optimization instead of a static backtest\n"
                "  wfotrain=252               train-window size in bars (~1 trading year)\n"
                "  wfotest=63                 test-window size in bars (~1 quarter)\n"
                "  wfostep=63                 slide increment between folds, in bars\n"
                "  wfoprofitgrid=0.3,0.5,0.75   profit-target grid to search per fold\n"
                "  wfostopgrid=1.5,2.0,3.0      stop-loss-multiple grid to search per fold\n"
                "\nExample:\n"
                "  nox_backtest watchlist=AAPL,NVDA range=2y capital=50000\n"
                "  nox_backtest watchlist=AAPL range=2y fillmodel=adverse_selection\n";
            return 0;
        }

        auto eq = arg.find('=');
        if (eq == std::string::npos) continue;
        std::string key = arg.substr(0, eq);
        std::string val = arg.substr(eq + 1);

        if (key == "watchlist") {
            cfg.watchlist.clear();
            std::istringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) cfg.watchlist.push_back(tok);
        } else if (key == "range")   { cfg.range               = val; }
        else if (key == "scan")      { cfg.scan_every_n_days    = std::stoi(val); }
        else if (key == "profit")    { cfg.profit_target_pct    = std::stod(val); }
        else if (key == "stop")      { cfg.stop_loss_mult       = std::stod(val); }
        else if (key == "capital")   { cfg.initial_capital      = std::stod(val); }
        else if (key == "haircutpct")             { cfg.bid_ask_haircut_pct     = std::stod(val); }
        else if (key == "commissionpercontract")  { cfg.commission_per_contract = std::stod(val); }
        else if (key == "profile" && val == "personal") {
            cfg.profile = RiskProfile::personal();
        }
        else if (key == "fillmodel") {
            cfg.fill.mode = (val == "adverse_selection") ? FillMode::AdverseSelection : FillMode::Naive;
        }
        else if (key == "fillgamma")     { cfg.fill.gamma       = std::stod(val); }
        else if (key == "fillqueuemult") { cfg.fill.queue_mult  = std::stod(val); }
        else if (key == "fillseed")      { cfg.fill.rng_seed    = static_cast<unsigned>(std::stoul(val)); }
        else if (key == "errghostrate")  { cfg.errors.ghost_fill_rate      = std::stod(val); }
        else if (key == "errmissedrate") { cfg.errors.missed_exit_rate     = std::stod(val); }
        else if (key == "erradverserate"){ cfg.errors.adverse_fill_rate    = std::stod(val); }
        else if (key == "errslippct")    { cfg.errors.adverse_slippage_pct = std::stod(val); }
        else if (key == "errseed")       { cfg.errors.seed = static_cast<std::uint32_t>(std::stoul(val)); }
        else if (key == "kelly")         { cfg.kelly.enabled = (val == "1" || val == "true"); }
        else if (key == "kellysweep") {
            std::vector<double> fractions;
            std::istringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) fractions.push_back(std::stod(tok));
            if (!fractions.empty()) cfg.kelly.sweep_fractions = fractions;
        }
        else if (key == "kellywindow")   { cfg.kelly.rolling_window       = std::stoi(val); }
        else if (key == "kellycap")      { cfg.kelly.hard_cap             = std::stod(val); }
        else if (key == "kellyseedwr")   { cfg.kelly.seed_win_rate        = std::stod(val); }
        else if (key == "kellyseedwlr")  { cfg.kelly.seed_win_loss_ratio  = std::stod(val); }
        else if (key == "wfo")           { cfg.wfo.enabled     = (val == "1" || val == "true"); }
        else if (key == "wfotrain")      { cfg.wfo.train_days  = std::stoi(val); }
        else if (key == "wfotest")       { cfg.wfo.test_days   = std::stoi(val); }
        else if (key == "wfostep")       { cfg.wfo.step_days   = std::stoi(val); }
        else if (key == "wfoprofitgrid") {
            std::vector<double> grid;
            std::istringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) grid.push_back(std::stod(tok));
            if (!grid.empty()) cfg.wfo.profit_grid = grid;
        }
        else if (key == "wfostopgrid") {
            std::vector<double> grid;
            std::istringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) grid.push_back(std::stod(tok));
            if (!grid.empty()) cfg.wfo.stop_grid = grid;
        }
    }

    if (cfg.wfo.enabled) {
        std::cerr << "\nFetching historical OHLCV (walk-forward)...\n";
        auto wfo_report = runWalkForward(cfg);
        printWalkForwardReport(wfo_report, cfg);
        return 0;
    }

    std::cerr << "\nFetching historical OHLCV...\n";
    auto result = runBacktest(cfg);
    printReport(result.baseline, cfg);
    if (cfg.fill.mode != FillMode::Naive)
        printFillComparison(result.baseline, result.variant, cfg.fill);
    else
        std::cout << "  Adverse-selection fill model available — rerun with\n"
                  << "  fillmodel=adverse_selection to compare against this naive baseline.\n\n";

    if (cfg.errors.active())
        printErrorInjectionReport(result.baseline, cfg.errors);
    else
        std::cout << "  Error-injection resilience test available — rerun with e.g.\n"
                  << "  errghostrate=0.05 errmissedrate=0.10 erradverserate=0.15 to see\n"
                  << "  whether the strategy still profits through operational errors.\n\n";

    if (cfg.kelly.enabled) {
        auto sweep = runKellySweep(result.baseline, cfg.kelly, cfg.initial_capital);
        printKellySweepReport(sweep, cfg.kelly);
    } else {
        std::cout << "  Fractional-Kelly sizing sweep available — rerun with kelly=1 to\n"
                  << "  compare " << cfg.kelly.sweep_fractions.size() << " Kelly fractions on a "
                  << "compounding equity curve.\n\n";
    }
    return 0;
}
