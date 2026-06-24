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
//   stop=2.0          (exit at 2× debit/credit paid — i.e. lose 100% + premium)
//   capital=35000     (determines strategy tier gate)
//   profile=personal  (use aggressive personal profile; default = bot)
//
// Methodology:
//   - No real historical options chain: IV is proxied as HRV30 × 1.15
//     (HRV plus a modest variance-risk-premium assumption).
//   - All Greeks and prices are Black-Scholes European; no early-exercise value.
//   - Slippage, commissions, and bid/ask spread are NOT modelled.
//   - Use results to assess signal quality and strategy direction, not exact P&L.

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "OptionEngine.hpp"
#include "OptionsSignalTypes.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace nox::options;
using namespace nox::options_signal;

// ─── Data types ──────────────────────────────────────────────────────────────

struct Bar {
    std::string date;
    double high = 0.0, low = 0.0, close = 0.0;
};

struct BacktestConfig {
    std::vector<std::string> watchlist    = {"SPY","QQQ","AAPL","TSLA","NVDA"};
    std::string              range        = "2y";
    int    scan_every_n_days              = 5;
    double profit_target_pct             = 0.50;
    double stop_loss_mult                = 2.00;
    double initial_capital               = 35000.0; // ADVANCED tier by default
    double rfr                           = 0.05;
    RiskProfile profile                  = RiskProfile::bot();
};

struct Trade {
    std::string ticker, strategy;
    std::string entry_date, exit_date, exit_reason;
    double spot_entry  = 0.0, spot_exit  = 0.0;
    double iv_entry    = 0.0, hrv_entry  = 0.0, rsi_entry = 0.0;
    double entry_price = 0.0, exit_price = 0.0;
    double pnl         = 0.0; // per underlying share; × 100 for dollar P&L
    bool   bias_right  = false;
    bool   is_long     = true;
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
                C[i].get<double>()
            });
        }
        std::cerr << " " << bars.size() << " bars\n";
        return bars;
    } catch (...) {
        std::cerr << " parse error\n";
        return {};
    }
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

// ─── Simulate one trade ───────────────────────────────────────────────────────

Trade simulateTrade(const std::string& ticker,
                    const std::string& strat,
                    const std::vector<Bar>& bars,
                    size_t entry_idx,
                    const BacktestConfig& cfg,
                    double iv_entry, double hrv_entry, double rsi_entry)
{
    const RiskProfile& prof = cfg.profile;
    double spot0 = bars[entry_idx].close;
    double rfr   = cfg.rfr;

    // DTE from profile
    int dte = prof.dte_long;
    if (strat == "CSP" || strat == "CC")
        dte = prof.dte_income;
    else if (strat == "BULL_CALL_SPREAD" || strat == "BEAR_PUT_SPREAD" ||
             strat == "STRADDLE"         || strat == "STRANGLE")
        dte = prof.dte_spread;
    double expiry_yrs = dte / 365.0;

    // Strike selection
    double K1 = spot0, K2 = 0.0;
    OptionType leg1_type = OptionType::Call;

    if (strat == "LONG_CALL") {
        K1 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_long, OptionType::Call, rfr);
        leg1_type = OptionType::Call;
    } else if (strat == "LONG_PUT") {
        K1 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_long, OptionType::Put, rfr);
        leg1_type = OptionType::Put;
    } else if (strat == "CSP") {
        K1 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_income, OptionType::Put, rfr);
        leg1_type = OptionType::Put;
    } else if (strat == "CC") {
        K1 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_income, OptionType::Call, rfr);
        leg1_type = OptionType::Call;
    } else if (strat == "BULL_CALL_SPREAD") {
        K1 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_long,        OptionType::Call, rfr);
        K2 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_spread_wing, OptionType::Call, rfr);
        if (K2 <= K1) K2 = K1 + ((spot0 < 200.0) ? 1.0 : 5.0);
        leg1_type = OptionType::Call;
    } else if (strat == "BEAR_PUT_SPREAD") {
        K1 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_long,        OptionType::Put, rfr);
        K2 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_spread_wing, OptionType::Put, rfr);
        if (K2 >= K1) K2 = K1 - ((spot0 < 200.0) ? 1.0 : 5.0);
        leg1_type = OptionType::Put;
    } else if (strat == "STRADDLE") {
        K1 = std::round(spot0);
        leg1_type = OptionType::Call;
    } else if (strat == "STRANGLE") {
        K1 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_income, OptionType::Call, rfr);
        K2 = findStrikeForDelta(spot0, expiry_yrs, iv_entry, prof.delta_income, OptionType::Put,  rfr);
        leg1_type = OptionType::Call;
    }

    double entry_price = valuePosition(strat, spot0, expiry_yrs, iv_entry, rfr, K1, K2, leg1_type);
    if (entry_price <= 0.01) {
        // Spread collapsed or invalid IV — skip
        Trade t; t.exit_reason = "INVALID"; return t;
    }

    // Long positions pay debit; short positions receive credit.
    bool is_long = (strat != "CSP" && strat != "CC");

    // Directional check: did price go the right way?
    bool bias_bullish = (strat == "LONG_CALL" || strat == "BULL_CALL_SPREAD" || strat == "CSP");
    bool bias_bearish = (strat == "LONG_PUT"  || strat == "BEAR_PUT_SPREAD"  || strat == "CC");

    Trade t;
    t.ticker      = ticker;
    t.strategy    = strat;
    t.entry_date  = bars[entry_idx].date;
    t.spot_entry  = spot0;
    t.iv_entry    = iv_entry;
    t.hrv_entry   = hrv_entry;
    t.rsi_entry   = rsi_entry;
    t.entry_price = entry_price;
    t.is_long     = is_long;
    t.exit_reason = "EXPIRY";

    double exit_price = entry_price;
    size_t exit_idx   = entry_idx;

    for (int day = 1; day <= dte; ++day) {
        size_t idx = entry_idx + static_cast<size_t>(day);
        if (idx >= bars.size()) break;

        double spot   = bars[idx].close;
        double t_rem  = std::max(0.0, expiry_yrs - day / 365.0);
        double hrv_d  = calcHRV(bars, idx);
        double iv_d   = hrv_d * 1.15;

        double cur    = valuePosition(strat, spot, t_rem, iv_d, rfr, K1, K2, leg1_type);
        double pnl_d  = is_long ? (cur - entry_price) : (entry_price - cur);

        if (pnl_d >= entry_price * cfg.profit_target_pct) {
            exit_price = cur; exit_idx = idx; t.exit_reason = "PROFIT_TARGET"; break;
        }
        if (pnl_d <= -(entry_price * cfg.stop_loss_mult)) {
            exit_price = cur; exit_idx = idx; t.exit_reason = "STOP_LOSS"; break;
        }

        exit_price = cur;
        exit_idx   = idx;
    }

    t.exit_price = exit_price;
    t.exit_date  = bars[exit_idx].date;
    t.spot_exit  = bars[exit_idx].close;
    t.pnl        = is_long ? (exit_price - entry_price) : (entry_price - exit_price);

    if (bias_bullish)      t.bias_right = t.spot_exit > t.spot_entry;
    else if (bias_bearish) t.bias_right = t.spot_exit < t.spot_entry;
    else {
        // Vol play: right if actual move > expected one-SD move
        double expected_move = hrv_entry * t.spot_entry * std::sqrt(dte / 252.0);
        t.bias_right = std::abs(t.spot_exit - t.spot_entry) > expected_move;
    }

    return t;
}

// ─── Run full backtest ────────────────────────────────────────────────────────

std::vector<Trade> runBacktest(const BacktestConfig& cfg) {
    // Capital tier from initial_capital
    std::string tier = "STARTER";
    if      (cfg.initial_capital >= 75000.0) tier = "FREE_CAPITAL";
    else if (cfg.initial_capital >= 30000.0) tier = "ADVANCED";
    else if (cfg.initial_capital >= 5000.0)  tier = "STANDARD";

    std::vector<Trade> all_trades;

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
                all_trades.push_back(t);
                ++n_signals;
            }
        }
        std::cerr << "  " << ticker << ": " << n_signals << " signals\n";
    }

    return all_trades;
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
    std::cout << "  IV proxy   : HRV30 × 1.15 (no historical options chain available)\n"
              << "  Pricing    : Black-Scholes European, re-priced daily at mark-to-model\n"
              << "  Slippage   : NOT modelled — real fills will be worse by $0.05-0.30/share\n"
              << "  P&L scale  : per 1 contract = × $100. Multiply by your contract count.\n"
              << "  When ready : integrate a real historical options chain (Polygon.io,\n"
              << "               CBOE DataShop) to replace the IV proxy with real market prices.\n";

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
                "  profile=personal           use aggressive personal profile\n"
                "\nExample:\n"
                "  nox_backtest watchlist=AAPL,NVDA range=2y capital=50000\n";
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
        else if (key == "profile" && val == "personal") {
            cfg.profile = RiskProfile::personal();
        }
    }

    std::cerr << "\nFetching historical OHLCV...\n";
    auto trades = runBacktest(cfg);
    printReport(trades, cfg);
    return 0;
}
