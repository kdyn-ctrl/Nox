#include "PositionManager.hpp"
#include "OptionsOrderRouter.hpp"
#include "OptionEngine.hpp"
#include "PortfolioRiskManager.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "../shared/TelegramNotifier.hpp"
#include <thread>
#include <chrono>
#include <algorithm>

using TelegramNotifier = nox::TelegramNotifier;


// Helper to get current date as YYYY-MM-DD
std::string get_current_date() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d");
    return ss.str();
}

// Helper to calculate days between two dates
int days_between(const std::string& date1_str, const std::string& date2_str) {
    std::tm date1_tm = {};
    std::tm date2_tm = {};
    std::stringstream ss1(date1_str);
    std::stringstream ss2(date2_str);
    ss1 >> std::get_time(&date1_tm, "%Y-%m-%d");
    ss2 >> std::get_time(&date2_tm, "%Y-%m-%d");

    auto time1 = std::mktime(&date1_tm);
    auto time2 = std::mktime(&date2_tm);

    return std::abs(time2 - time1) / (60 * 60 * 24);
}

// Approximate US options market-hours check (Mon-Fri 09:30-16:00 ET,
// DST-approx) — same shape as main.cpp's is_us_market_hours() for equities.
// Quote fetches fail (404/empty) outside these hours since Alpaca's options
// feed has nothing fresh to return, which previously fired a Telegram error
// alert every 5 minutes overnight for no actionable reason; gating the whole
// monitor cycle on this avoids that noise instead of just muting the alert.
static bool is_options_market_hours() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&tt, &utc);
    if (utc.tm_wday == 0 || utc.tm_wday == 6) return false;
    int offset_h = (utc.tm_mon >= 3 && utc.tm_mon <= 9) ? 4 : 5; // EDT vs EST
    int et_mins  = ((utc.tm_hour - offset_h + 24) % 24) * 60 + utc.tm_min;
    return et_mins >= 9 * 60 + 30 && et_mins < 16 * 60;
}

double get_option_quote_mid(const std::string& api_key,
                             const std::string& api_secret,
                             const std::string& occ_symbol)
{
    try {
        // Options quotes live on Alpaca's market-data host, never the
        // trading-API base URL (paper-api.alpaca.markets/live-api) — that
        // host doesn't serve /v1beta1/options/*, it 404s on every call.
        httplib::Client cli("https://data.alpaca.markets");
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(10));

        httplib::Headers headers = {
            {"APCA-API-KEY-ID",     api_key},
            {"APCA-API-SECRET-KEY", api_secret}
        };

        std::string path = "/v1beta1/options/quotes/latest?symbols=" + occ_symbol;
        auto res = cli.Get(path.c_str(), headers);

        if (!res || res->status != 200) {
            std::cerr << "[POS_MANAGER] Quote fetch failed for " << occ_symbol
                      << " — HTTP " << (res ? std::to_string(res->status) : "timeout") << std::endl;
            return -1.0;
        }

        json body = json::parse(res->body);

        // Alpaca returns quotes in format: {"quotes": {"OCC_SYMBOL": {...}}}
        if (!body.contains("quotes") || body["quotes"].empty()) {
            std::cerr << "[POS_MANAGER] No quotes returned for " << occ_symbol << std::endl;
            return -1.0;
        }

        auto& quote = body["quotes"][occ_symbol];

        // Use bid price for short positions (we want to close at bid),
        // ask price for long positions (we want to close at ask).
        // For simplicity, use mid-point: (bid + ask) / 2
        double bid = quote.value("bp", 0.0);  // bid price
        double ask = quote.value("ap", 0.0);  // ask price

        if (bid <= 0.0 && ask <= 0.0) {
            std::cerr << "[POS_MANAGER] Invalid quote data for " << occ_symbol
                      << " (bid=" << bid << ", ask=" << ask << ")" << std::endl;
            return -1.0;
        }

        // Mid-point price
        double current_price = (bid > 0 && ask > 0) ? (bid + ask) / 2.0 : (bid > 0 ? bid : ask);

        std::cout << "[POS_MANAGER] Quote for " << occ_symbol << ": bid=" << bid
                  << ", ask=" << ask << ", mid=" << current_price << std::endl;

        return current_price;

    } catch (const std::exception& e) {
        std::cerr << "[POS_MANAGER] Exception fetching quote for " << occ_symbol
                  << ": " << e.what() << std::endl;
        return -1.0;
    }
}

// Phase 4, item 1: current underlying (stock) price, needed to recompute an
// open option position's Greeks against TODAY's spot rather than the
// entry-time snapshot OptionsSignalGenerator computed once at signal time.
// Same Alpaca data endpoint main.cpp's fetch_equity_spread() and
// OptionsSignalGenerator's fetchUnderlyingSpread() already use for stocks —
// unlike those two (which discard the mid for a spread ratio), this returns
// the mid price itself. Returns -1.0 on any failure.
double get_underlying_price_from_alpaca(const std::string& ticker,
                                         const std::string& api_key,
                                         const std::string& api_secret)
{
    try {
        httplib::Client cli("https://data.alpaca.markets");
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(10));

        httplib::Headers headers = {
            {"APCA-API-KEY-ID",     api_key},
            {"APCA-API-SECRET-KEY", api_secret}
        };
        std::string path = "/v2/stocks/" + ticker + "/quotes/latest?feed=iex";
        auto res = cli.Get(path.c_str(), headers);
        if (!res || res->status != 200) return -1.0;

        json body = json::parse(res->body);
        if (!body.contains("quote")) return -1.0;
        const auto& q = body["quote"];
        double bid = q.value("bp", 0.0);
        double ask = q.value("ap", 0.0);
        if (bid <= 0.0 || ask <= 0.0 || ask < bid) return -1.0;
        return (bid + ask) / 2.0;
    } catch (...) {
        return -1.0;
    }
}

// Phase 4, item 1: solves live Greeks for one open option position from its
// CURRENT underlying price + CURRENT option mark price (both already fetched
// this cycle) — as opposed to the entry-time IV/underlying baked into the
// signal at order time, which never changes again once the position is open.
// Returns false (and leaves `out` untouched) if the position has already
// expired (dte <= 0) or the IV solver doesn't converge against the current
// mark; the caller must not treat that as fatal — the exit-rule evaluation
// for this position is independent of whether Greeks refresh succeeded.
static bool solve_live_greeks(const OptionPosition& pos, double underlying_price,
                              double option_mark_price, nox::options::OptionGreeks& out)
{
    int dte = days_between(get_current_date(), pos.expiration_date);
    if (dte <= 0 || underlying_price <= 0.0 || option_mark_price <= 0.0) return false;

    nox::options::OptionContract c;
    c.symbol         = pos.ticker;
    c.strike         = pos.strike;
    c.underlying     = underlying_price;
    c.expiry         = dte / 365.0;
    c.risk_free_rate = 0.05; // matches OptionsSignalGenerator's entry-time rfr default
    c.volatility     = option_mark_price; // solve_iv=true treats this as a market price
    c.type           = (pos.option_type == "call") ? nox::options::OptionType::Call
                                                    : nox::options::OptionType::Put;
    try {
        out = nox::options::compute_greeks(c, /*solve_iv=*/true);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void PositionManager::monitor_positions() {
    int error_count = 0;
    int last_status_hour = -1;

    while (run_monitoring_) {
        // Monitor every 5 minutes instead of 30 to catch profit-taking opportunities early
        // wait_for returns true when the predicate (stop requested) becomes true → break the loop;
        // false on timeout → run the next cycle.
        {
            std::unique_lock<std::mutex> lock(monitor_lock_);
            if (monitor_cv_.wait_for(lock, std::chrono::minutes(5),
                                     [this] { return !run_monitoring_.load(); })) {
                break;
            }
        }

        auto open_positions = get_open_positions();

        // Log periodic status every hour
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        int current_hour = ::gmtime_r(&time_t, &tm_buf)->tm_hour;

        auto open_spreads = get_open_spread_positions();

        if (open_positions.empty() && open_spreads.empty()) {
            set_last_risk_breach(nox::risk::evaluate({}, risk_targets_));
            if (current_hour != last_status_hour) {
                std::cout << "[POS_MANAGER] Hourly status: No open positions to monitor." << std::endl;
                last_status_hour = current_hour;
            }
            continue;
        }

        // Quote fetches return empty/404 outside market hours (nothing fresh to
        // return), which used to fire a Telegram error alert every cycle overnight
        // with nothing actionable to do about it. Skip the quote-dependent work
        // entirely rather than just muting the alert.
        if (!is_options_market_hours()) {
            if (current_hour != last_status_hour) {
                std::cout << "[POS_MANAGER] Market closed — holding " << open_positions.size()
                          << " single-leg + " << open_spreads.size()
                          << " spread position(s), skipping quote checks until open." << std::endl;
                last_status_hour = current_hour;
            }
            continue;
        }

        if (current_hour != last_status_hour) {
            std::stringstream ss;
            ss << "[POS_MANAGER] Hourly status: Monitoring " << open_positions.size()
               << " single-leg + " << open_spreads.size() << " spread position(s)";
            std::cout << ss.str() << std::endl;
            last_status_hour = current_hour;
        }

        int cycle_errors = 0;

        // Phase 4: results are collected here rather than closed inline, so the
        // portfolio-risk breach (evaluated after every position's live Greeks
        // are in) can flag ONE additional position to force-close before any
        // actual close/alert/ledger action runs for this cycle.
        struct PositionEvalResult {
            OptionPosition pos;
            std::string    occ_symbol;
            double         current_price;
            bool           exit_triggered;
            std::string    exit_reason;
            std::string    detail;
        };
        std::vector<PositionEvalResult> results;
        std::vector<nox::risk::PositionGreekContribution> contributions;

        for (const auto& pos : open_positions) {
            double current_price = -1.0;
            std::string occ_symbol;
            std::string error_detail;

            try {
                // We need the exact OCC symbol to get a quote and to close the order.
                auto contract = order_router_.lookupContract(
                    pos.ticker, pos.strike, pos.expiration_date, pos.option_type
                );
                if (!contract.valid) {
                    error_detail = "Contract lookup failed for " + pos.ticker +
                                   " (strike=" + std::to_string(pos.strike) +
                                   ", exp=" + pos.expiration_date +
                                   ", type=" + pos.option_type + ")";
                    std::cerr << "[POS_MANAGER] ❌ " << error_detail << std::endl;
                    cycle_errors++;
                    error_count++;

                    std::stringstream tg_msg;
                    tg_msg << "🚨 *POSITION MONITOR ERROR*\n"
                           << "────────────────────────\n"
                           << "• *Position ID:* " << pos.id << "\n"
                           << "• *Ticker:* " << pos.ticker << "\n"
                           << "• *Error:* Contract not found\n"
                           << "• *Details:* Strike=" << pos.strike
                           << ", Expiry=" << pos.expiration_date
                           << ", Type=" << pos.option_type << "\n"
                           << "• *Impact:* Position cannot be monitored for exits\n"
                           << "• *Action Required:* Manual verification needed";
                    TelegramNotifier::sendMessage(tg_msg.str());
                    continue;
                }
                occ_symbol = contract.occ_symbol;

                // Fetch real-time market price from Alpaca for the option contract
                const char* api_key_env = std::getenv("ALPACA_API_KEY");
                const char* api_sec_env = std::getenv("ALPACA_SECRET_KEY");
                const char* base_url_env = std::getenv("ALPACA_BASE_URL");

                if (!api_key_env || !api_sec_env || !base_url_env) {
                    error_detail = "Missing ALPACA API credentials in environment";
                    std::cerr << "[POS_MANAGER] ❌ " << error_detail << std::endl;
                    cycle_errors++;
                    error_count++;

                    std::stringstream tg_msg;
                    tg_msg << "🚨 *CRITICAL: POSITION MONITOR DISABLED*\n"
                           << "────────────────────────\n"
                           << "• *Error:* Missing Alpaca API credentials\n"
                           << "• *Position ID:* " << pos.id << "\n"
                           << "• *Ticker:* " << pos.ticker << "\n"
                           << "• *Impact:* NO positions can be monitored\n"
                           << "• *Action Required:* Check ALPACA_API_KEY, ALPACA_SECRET_KEY, ALPACA_BASE_URL";
                    TelegramNotifier::sendMessage(tg_msg.str());
                    continue;
                }

                current_price = get_option_quote_mid(
                    std::string(api_key_env),
                    std::string(api_sec_env),
                    occ_symbol
                );

            } catch (const std::exception& e) {
                error_detail = "Exception during contract lookup or price fetch: " + std::string(e.what());
                std::cerr << "[POS_MANAGER] ❌ " << error_detail << " [ticker=" << pos.ticker << "]" << std::endl;
                cycle_errors++;
                error_count++;

                std::stringstream tg_msg;
                tg_msg << "⚠️ *POSITION MONITOR ERROR*\n"
                       << "────────────────────────\n"
                       << "• *Position ID:* " << pos.id << "\n"
                       << "• *Ticker:* " << pos.ticker << "\n"
                       << "• *Strike:* " << pos.strike << "\n"
                       << "• *Error:* `" << e.what() << "`\n"
                       << "• *Impact:* This position cannot be checked for exits this cycle\n"
                       << "• *Note:* Will retry in 5 minutes";
                TelegramNotifier::sendMessage(tg_msg.str());
                continue;
            }

            if (current_price < 0) {
                error_detail = "Price fetch returned invalid value (" + std::to_string(current_price) + ")";
                std::cerr << "[POS_MANAGER] ❌ " << error_detail << " [ticker=" << pos.ticker << "]" << std::endl;
                cycle_errors++;
                error_count++;

                std::stringstream tg_msg;
                tg_msg << "⚠️ *POSITION MONITOR ERROR*\n"
                       << "────────────────────────\n"
                       << "• *Position ID:* " << pos.id << "\n"
                       << "• *Ticker:* " << pos.ticker << "\n"
                       << "• *OCC Symbol:* " << occ_symbol << "\n"
                       << "• *Error:* Failed to fetch price from Alpaca\n"
                       << "• *Impact:* Cannot evaluate exit rules this cycle\n"
                       << "• *Note:* Will retry in 5 minutes";
                TelegramNotifier::sendMessage(tg_msg.str());
                continue;
            }

            // Phase 2, item B: refresh the live unrealized-P&L snapshot for this
            // position every cycle, regardless of whether an exit fires below —
            // this is the only place a current option mark is fetched, so it's
            // the natural point to persist it instead of leaving it in stdout only.
            std::string detail = pos.option_type + " " +
                std::to_string(pos.strike) + " " + pos.expiration_date;
            double unrealized_pnl_snapshot = (pos.profile_type == "short_premium")
                ? (pos.entry_price - current_price) * pos.quantity * 100.0
                : (current_price - pos.entry_price) * pos.quantity * 100.0;
            upsert_unrealized(get_current_date(), pos.ticker, "option", detail,
                              static_cast<double>(pos.quantity), pos.entry_price,
                              current_price, unrealized_pnl_snapshot);

            // Phase 4, item 1: live Greeks, solved from THIS cycle's underlying
            // price + option mark — best-effort; a failure here (quote feed
            // hiccup, near-expiry contract) never blocks the exit-rule
            // evaluation below, it just means this position doesn't contribute
            // to this cycle's portfolio-risk snapshot.
            {
                const char* k = std::getenv("ALPACA_API_KEY");
                const char* s = std::getenv("ALPACA_SECRET_KEY");
                nox::options::OptionGreeks live_greeks{};
                double underlying_price = (k && s) ? get_underlying_price_from_alpaca(pos.ticker, k, s) : -1.0;
                if (underlying_price > 0.0 &&
                    solve_live_greeks(pos, underlying_price, current_price, live_greeks)) {
                    upsert_live_greeks(pos.id, pos.ticker, underlying_price,
                                       live_greeks.delta, live_greeks.gamma,
                                       live_greeks.theta, live_greeks.vega,
                                       live_greeks.implied_volatility);

                    double sign = (pos.profile_type == "short_premium") ? -1.0 : 1.0;
                    nox::risk::PositionGreekContribution contrib;
                    contrib.position_id        = pos.id;
                    contrib.ticker              = pos.ticker;
                    contrib.delta_contribution = live_greeks.delta * pos.quantity * 100.0 * sign;
                    contrib.vega_contribution  = live_greeks.vega  * pos.quantity * 100.0 * sign;
                    contrib.notional           = std::abs(underlying_price * pos.quantity * 100.0);
                    contributions.push_back(contrib);
                } else {
                    std::cout << "[POS_MANAGER] Live Greeks unavailable for position " << pos.id
                              << " (" << pos.ticker << ") this cycle — excluded from portfolio-risk snapshot."
                              << std::endl;
                }
            }

            bool exit_triggered = false;
            std::string exit_reason;

            // Rule evaluation with detailed logging
            if (pos.profile_type == "long" && current_price >= pos.entry_price * 1.50) {
                exit_triggered = true;
                exit_reason = "50% Profit Rule (Long)";
            } else if (pos.profile_type == "short_premium" && current_price <= pos.entry_price * 0.50) {
                exit_triggered = true;
                exit_reason = "50% Profit Rule (Short Premium)";
            }

            if (!exit_triggered && pos.profile_type == "short_premium") {
                if (days_between(get_current_date(), pos.expiration_date) <= 21) {
                    exit_triggered = true;
                    exit_reason = "21 DTE Rule";
                    int dte = days_between(get_current_date(), pos.expiration_date);
                    std::cout << "[POS_MANAGER] 21-DTE rule triggered for " << pos.ticker
                              << " (ID: " << pos.id << ", DTE=" << dte << ")" << std::endl;
                }
            }

            if (!exit_triggered) {
                if (pos.profile_type == "long" && current_price <= pos.entry_price * 0.50) {
                    exit_triggered = true;
                    exit_reason = "Stop Loss Rule (Long)";
                } else if (pos.profile_type == "short_premium" && current_price >= pos.entry_price * 2.0) {
                    exit_triggered = true;
                    exit_reason = "Stop Loss Rule (Short Premium)";
                }
            }

            // Log position status for debugging
            if (!exit_triggered) {
                std::cout << "[POS_MANAGER] Position " << pos.id << " (" << pos.ticker
                          << " ID=" << pos.id << "): entry=" << pos.entry_price
                          << ", current=" << current_price
                          << ", unrealized_pnl=$" << unrealized_pnl_snapshot << std::endl;
            }

            results.push_back({pos, occ_symbol, current_price, exit_triggered, exit_reason, detail});
        }

        // Phase 4, item 2: portfolio circuit breaker — evaluated once per cycle
        // from every position whose live Greeks refreshed successfully above.
        // Blocking new orders happens elsewhere (the options pre-order gate
        // reads get_last_risk_breach()); here, if breached, the single
        // position that is the LARGEST contributor to the breach is flagged to
        // force-close — a position already within its share of target is left
        // running through the normal 50%/stop/21-DTE rules untouched.
        // NOTE: `contributions` holds SINGLE-LEG positions only at this point;
        // spread contributions are added in the spread loop below. This breach is
        // used for the single-leg force-close pass (the only kind it can act on).
        // The AUTHORITATIVE gate snapshot (single + spread) is set at cycle end,
        // after spreads are priced, so new-entry blocking sees the whole book.
        nox::risk::RiskBreach breach = nox::risk::evaluate(contributions, risk_targets_);
        // Positions that must be closed together as one linked cluster (the
        // breach target plus any opposite-side leg on the same underlying+
        // expiry it's covering) rather than one-by-one — closing a leg that
        // covers another open leg in isolation gets rejected by the broker as
        // leaving the other leg uncovered (see the July 13 sell-failure
        // incident: a long-call leg hedging two short-call legs booked as
        // separate open_positions rows).
        std::vector<long> force_close_cluster_ids;
        if (breach.breached && breach.position_to_close != -1) {
            const PositionEvalResult* target = nullptr;
            for (auto& r : results) {
                if (r.pos.id == breach.position_to_close) { target = &r; break; }
            }
            if (target && !target->exit_triggered) {
                force_close_cluster_ids.push_back(target->pos.id);
                bool target_is_short = (target->pos.profile_type == "short_premium");
                std::string ticker = target->pos.ticker, expiry = target->pos.expiration_date;
                for (auto& r : results) {
                    if (r.pos.id == target->pos.id || r.exit_triggered) continue;
                    bool r_is_short = (r.pos.profile_type == "short_premium");
                    if (r.pos.ticker == ticker && r.pos.expiration_date == expiry &&
                        r_is_short != target_is_short) {
                        force_close_cluster_ids.push_back(r.pos.id);
                    }
                }
                std::string reason = "Portfolio Risk Breach (" + breach.reason + ")";
                for (auto& r : results) {
                    if (std::find(force_close_cluster_ids.begin(), force_close_cluster_ids.end(),
                                  r.pos.id) != force_close_cluster_ids.end()) {
                        r.exit_triggered = true;
                        r.exit_reason    = reason;
                    }
                }
                std::cout << "[POS_MANAGER] ⚠️ Portfolio risk breach — force-closing position "
                          << target->pos.id << " (" << target->pos.ticker << ")"
                          << (force_close_cluster_ids.size() > 1
                              ? " plus " + std::to_string(force_close_cluster_ids.size() - 1) +
                                " linked hedge leg(s)"
                              : "")
                          << ": " << breach.reason << std::endl;
                TelegramNotifier::sendMessage(
                    "🚨 *PORTFOLIO RISK BREACH — FORCE-CLOSING POSITION*\n"
                    "────────────────────────\n"
                    "• *Ticker:* " + target->pos.ticker + "\n"
                    "• *Position ID:* " + std::to_string(target->pos.id) + "\n"
                    "• *Reason:* " + breach.reason + "\n" +
                    (force_close_cluster_ids.size() > 1
                        ? "• *Linked legs closing with it:* " +
                          std::to_string(force_close_cluster_ids.size() - 1) + "\n"
                          "_These legs cover each other — closing the risk-driving leg alone "
                          "would leave the others uncovered, so the whole cluster closes together._"
                        : "_This position is the largest contributor to the breach — "
                          "positions within target are left untouched._")
                );
            }
        }

        if (force_close_cluster_ids.size() > 1) {
            std::vector<nox::options_router::OptionsOrderRouter::CloseLegSpec> cluster_legs;
            std::vector<const PositionEvalResult*> cluster_results;
            std::string underlying, expiry;
            for (const auto& r : results) {
                if (std::find(force_close_cluster_ids.begin(), force_close_cluster_ids.end(),
                              r.pos.id) == force_close_cluster_ids.end()) continue;
                underlying = r.pos.ticker;
                expiry     = r.pos.expiration_date;
                bool is_short = (r.pos.profile_type == "short_premium");
                cluster_legs.push_back({r.pos.option_type, r.pos.strike,
                                         is_short ? "sell" : "buy", r.pos.quantity});
                cluster_results.push_back(&r);
            }

            auto result = order_router_.closeSpreadPosition(underlying, expiry, cluster_legs, 1);

            if (result.success) {
                std::cout << "[POS_MANAGER] ✅ Closed linked hedge cluster (" << cluster_legs.size()
                          << " legs) for " << underlying << ". Order ID: " << result.order_id << std::endl;
                std::stringstream tg_msg;
                tg_msg << "✅ *LINKED HEDGE CLUSTER CLOSED*\n"
                       << "────────────────────────\n"
                       << "• *Underlying:* " << underlying << "\n"
                       << "• *Legs closed:* " << cluster_legs.size() << "\n"
                       << "• *Order ID:* `" << result.order_id << "`";
                TelegramNotifier::sendMessage(tg_msg.str());

                for (const auto* rp : cluster_results) {
                    const auto& pos = rp->pos;
                    double realized_pnl = (pos.profile_type == "short_premium")
                        ? (pos.entry_price - rp->current_price) * pos.quantity * 100.0
                        : (rp->current_price - pos.entry_price) * pos.quantity * 100.0;
                    record_trade(pos.ticker, "CLOSE", "option",
                                 static_cast<double>(pos.quantity), rp->current_price,
                                 0.0, 0.0, realized_pnl,
                                 pos.option_type + " " + std::to_string(pos.strike) + " | " + rp->exit_reason);
                    add_realized(get_current_date(), pos.ticker, "option", rp->detail, realized_pnl);
                    remove_position(pos.id);
                }
            } else {
                std::cerr << "[POS_MANAGER] ❌ FAILED to close linked hedge cluster for "
                          << underlying << ". Reason: " << result.message << std::endl;
                cycle_errors++;
                error_count++;
                TelegramNotifier::sendMessage(
                    "🚨 *CRITICAL: LINKED CLUSTER CLOSE FAILED — Manual Action Required*\n"
                    "────────────────────────\n"
                    "• *Underlying:* " + underlying + "\n"
                    "• *Legs:* " + std::to_string(cluster_legs.size()) + "\n"
                    "• *Error:* " + result.message + "\n"
                    "• *Action Required:* Close these linked positions manually immediately"
                );
            }
        }

        for (const auto& r : results) {
            if (force_close_cluster_ids.size() > 1 &&
                std::find(force_close_cluster_ids.begin(), force_close_cluster_ids.end(),
                          r.pos.id) != force_close_cluster_ids.end()) {
                continue; // handled by the linked-cluster close above
            }
            const auto& pos           = r.pos;
            const auto& occ_symbol    = r.occ_symbol;
            double      current_price = r.current_price;
            const auto& exit_reason   = r.exit_reason;
            const auto& detail        = r.detail;

            if (r.exit_triggered) {
                std::cout << "[POS_MANAGER] ✅ EXIT TRIGGERED for " << pos.ticker
                          << " (ID: " << pos.id << "): " << exit_reason << std::endl;

                bool is_short = (pos.profile_type == "short_premium");
                auto result = order_router_.closePosition(occ_symbol, pos.quantity, is_short);

                if (result.success) {
                    std::cout << "[POS_MANAGER] ✅ Successfully closed position " << pos.id
                              << ". Order ID: " << result.order_id << std::endl;

                    // Fire Telegram alert
                    std::stringstream tg_msg;
                    double realized_pnl = (pos.profile_type == "short_premium")
                        ? (pos.entry_price - current_price) * pos.quantity * 100.0
                        : (current_price - pos.entry_price) * pos.quantity * 100.0;

                    tg_msg << "✅ *PROFIT LOCKED - Position Closed*\n"
                           << "────────────────────────\n"
                           << "• *Ticker:* " << pos.ticker << "\n"
                           << "• *Position ID:* " << pos.id << "\n"
                           << "• *Type:* " << pos.option_type << " @ $" << std::fixed << std::setprecision(2) << pos.strike << "\n"
                           << "• *Exit Rule:* " << exit_reason << "\n"
                           << "• *Entry Price:* $" << pos.entry_price << "\n"
                           << "• *Exit Price:* $" << current_price << "\n"
                           << "• *Realized P&L:* $" << realized_pnl << "\n"
                           << "• *Quantity:* " << pos.quantity << " contract(s)\n"
                           << "• *Order ID:* `" << result.order_id << "`";
                    TelegramNotifier::sendMessage(tg_msg.str());

                    record_trade(pos.ticker, "CLOSE", "option",
                                 static_cast<double>(pos.quantity), current_price,
                                 0.0, 0.0, realized_pnl,
                                 pos.option_type + " " + std::to_string(pos.strike) + " | " + exit_reason);
                    add_realized(get_current_date(), pos.ticker, "option", detail, realized_pnl);

                    // Remove from database
                    remove_position(pos.id);
                } else {
                    std::cerr << "[POS_MANAGER] ❌ FAILED to close position " << pos.id
                              << ". Reason: " << result.message << std::endl;
                    cycle_errors++;
                    error_count++;

                    std::stringstream err_msg;
                    err_msg << "🚨 *CRITICAL: PROFIT-TAKING FAILED - Manual Action Required*\n"
                            << "────────────────────────\n"
                            << "• *Ticker:* " << pos.ticker << "\n"
                            << "• *Position ID:* " << pos.id << "\n"
                            << "• *Entry Price:* $" << pos.entry_price << "\n"
                            << "• *Current Price:* $" << current_price << "\n"
                            << "• *Strike:* $" << pos.strike << "\n"
                            << "• *Expiry:* " << pos.expiration_date << "\n"
                            << "• *Exit Rule Triggered:* " << exit_reason << "\n"
                            << "• *Error Details:* " << result.message << "\n"
                            << "• *Action Required:* Close this position manually immediately";
                    TelegramNotifier::sendMessage(err_msg.str());
                }
            }
        }

        // Multi-leg spreads/straddles/strangles/reverse-iron-condors. Every
        // strategy OptionsOrderRouter opens is a net debit (see the comment on
        // SpreadPosition), so one rule set covers all of them: 50%-profit /
        // 50%-stop against the net value of every leg combined, plus the same
        // 21-DTE rule short_premium single-leg positions use.
        for (const auto& sp : open_spreads) {
            const char* api_key_env  = std::getenv("ALPACA_API_KEY");
            const char* api_sec_env  = std::getenv("ALPACA_SECRET_KEY");
            const char* base_url_env = std::getenv("ALPACA_BASE_URL");
            if (!api_key_env || !api_sec_env || !base_url_env) {
                cycle_errors++;
                error_count++;
                continue;
            }

            double net_value = 0.0;
            bool quote_ok = true;
            // Live Greeks for the whole spread, summed across legs with the ENTRY
            // side's sign (buy = long = +, sell = short = -). Solved from THIS
            // cycle's marks, same as the single-leg path. Previously spreads were
            // never priced for Greeks at all: live_greeks stayed empty and, since
            // every filled options position is a spread, the portfolio breaker
            // saw an empty book and failed open (audit Phase 4). One underlying
            // spot fetch per spread feeds every leg's solve.
            double underlying_price =
                get_underlying_price_from_alpaca(sp.underlying, api_key_env, api_sec_env);
            double g_delta = 0.0, g_gamma = 0.0, g_theta = 0.0, g_vega = 0.0, g_iv_sum = 0.0;
            int g_iv_count = 0;
            bool greeks_ok = (underlying_price > 0.0);
            for (const auto& leg : sp.legs) {
                auto contract = order_router_.lookupContract(sp.underlying, leg.strike,
                                                              sp.expiration_date, leg.option_type);
                if (!contract.valid) { quote_ok = false; break; }
                double price = get_option_quote_mid(api_key_env, api_sec_env,
                                                    contract.occ_symbol);
                if (price < 0) { quote_ok = false; break; }
                // Closing value: sell back what was bought, buy back what was sold.
                net_value += (leg.side == "buy") ? price : -price;

                if (greeks_ok) {
                    OptionPosition leg_pos;
                    leg_pos.ticker          = sp.underlying;
                    leg_pos.option_type     = leg.option_type;
                    leg_pos.strike          = leg.strike;
                    leg_pos.expiration_date = sp.expiration_date;
                    nox::options::OptionGreeks lg{};
                    if (solve_live_greeks(leg_pos, underlying_price, price, lg)) {
                        double sign = (leg.side == "buy") ? 1.0 : -1.0;
                        g_delta  += lg.delta * sign;
                        g_gamma  += lg.gamma * sign;
                        g_theta  += lg.theta * sign;
                        g_vega   += lg.vega  * sign;
                        g_iv_sum += lg.implied_volatility;
                        g_iv_count++;
                    } else {
                        greeks_ok = false; // an unsolvable leg makes the spread Greeks incomplete
                    }
                }
            }

            if (!quote_ok) {
                std::cerr << "[POS_MANAGER] ❌ Spread quote fetch failed for " << sp.underlying
                          << " (" << sp.strategy << ", ID=" << sp.id << ")" << std::endl;
                cycle_errors++;
                error_count++;
                continue;
            }

            // Persist the summed spread Greeks (keyed by the NEGATED spread id so
            // it can't collide with a single-leg open_positions row of the same
            // id in live_greeks) and contribute to this cycle's portfolio-risk
            // snapshot. Only when every leg solved — a partial Greek picture would
            // understate the book's risk.
            if (greeks_ok && g_iv_count > 0) {
                upsert_live_greeks(-sp.id, sp.underlying, underlying_price,
                                   g_delta, g_gamma, g_theta, g_vega,
                                   g_iv_sum / g_iv_count);
                nox::risk::PositionGreekContribution contrib;
                contrib.position_id        = -sp.id;
                contrib.ticker              = sp.underlying;
                contrib.delta_contribution = g_delta * sp.quantity * 100.0;
                contrib.vega_contribution  = g_vega  * sp.quantity * 100.0;
                contrib.notional           = std::abs(underlying_price * sp.quantity * 100.0);
                contributions.push_back(contrib);
            }

            std::string detail = sp.strategy + " " + std::to_string(sp.legs.size()) +
                                 "-leg " + sp.expiration_date;
            // entry_debit and net_value share one sign convention (buy=+price,
            // sell=-price), so this pnl formula is correct for both net-debit
            // spreads (entry_debit > 0) and net-credit strangles (entry_debit < 0)
            // without a separate branch — see signed_entry_debit_for() in main.cpp.
            double unrealized_pnl = (net_value - sp.entry_debit) * sp.quantity * 100.0;
            upsert_unrealized(get_current_date(), sp.underlying, "option", detail,
                              static_cast<double>(sp.quantity), sp.entry_debit,
                              net_value, unrealized_pnl);

            bool exit_triggered = false;
            std::string exit_reason;
            int dte = days_between(get_current_date(), sp.expiration_date);
            // is_credit: a net-credit structure (short strangle) — the 1.5x/0.5x
            // debit-side thresholds below are meaningless against a negative
            // entry_debit, so mirror the short_premium single-leg convention
            // instead: profit at 50% decay of credit collected, stop at 2x.
            bool is_credit = sp.entry_debit < 0.0;
            if (is_credit) {
                double credit_received = -sp.entry_debit;
                double cost_to_close   = -net_value;
                if (cost_to_close <= credit_received * 0.50) {
                    exit_triggered = true;
                    exit_reason = "50% Profit Rule (Spread, credit)";
                } else if (cost_to_close >= credit_received * 2.0) {
                    exit_triggered = true;
                    exit_reason = "Stop Loss Rule (Spread, credit)";
                } else if (dte <= 21) {
                    exit_triggered = true;
                    exit_reason = "21 DTE Rule (Spread)";
                }
            } else if (net_value >= sp.entry_debit * 1.50) {
                exit_triggered = true;
                exit_reason = "50% Profit Rule (Spread)";
            } else if (net_value <= sp.entry_debit * 0.50) {
                exit_triggered = true;
                exit_reason = "Stop Loss Rule (Spread)";
            } else if (dte <= 21) {
                exit_triggered = true;
                exit_reason = "21 DTE Rule (Spread)";
            }

            if (!exit_triggered) {
                std::cout << "[POS_MANAGER] Spread " << sp.id << " (" << sp.underlying << " "
                          << sp.strategy << "): entry_debit=" << sp.entry_debit
                          << ", current=" << net_value << ", unrealized_pnl=$" << unrealized_pnl
                          << std::endl;
                continue;
            }

            std::cout << "[POS_MANAGER] ✅ EXIT TRIGGERED for spread " << sp.underlying
                      << " (ID: " << sp.id << "): " << exit_reason << std::endl;

            std::vector<nox::options_router::OptionsOrderRouter::CloseLegSpec> close_legs;
            for (const auto& leg : sp.legs) close_legs.push_back({leg.option_type, leg.strike, leg.side});
            auto result = order_router_.closeSpreadPosition(sp.underlying, sp.expiration_date,
                                                             close_legs, sp.quantity);

            if (result.success) {
                double realized_pnl = (net_value - sp.entry_debit) * sp.quantity * 100.0;
                std::cout << "[POS_MANAGER] ✅ Successfully closed spread " << sp.id
                          << ". Order ID: " << result.order_id << std::endl;

                std::stringstream tg_msg;
                tg_msg << "✅ *SPREAD CLOSED*\n"
                       << "────────────────────────\n"
                       << "• *Underlying:* " << sp.underlying << "\n"
                       << "• *Strategy:* " << sp.strategy << "\n"
                       << "• *Position ID:* " << sp.id << "\n"
                       << "• *Exit Rule:* " << exit_reason << "\n"
                       << "• *Entry Debit:* $" << sp.entry_debit << "\n"
                       << "• *Exit Value:* $" << net_value << "\n"
                       << "• *Realized P&L:* $" << realized_pnl << "\n"
                       << "• *Quantity:* " << sp.quantity << " contract(s)\n"
                       << "• *Order ID:* `" << result.order_id << "`";
                TelegramNotifier::sendMessage(tg_msg.str());

                record_trade(sp.underlying, "CLOSE", "option",
                             static_cast<double>(sp.quantity), net_value,
                             0.0, 0.0, realized_pnl, sp.strategy + " | " + exit_reason);
                add_realized(get_current_date(), sp.underlying, "option", detail, realized_pnl);
                remove_spread_position(sp.id);
            } else {
                std::cerr << "[POS_MANAGER] ❌ FAILED to close spread " << sp.id
                          << ". Reason: " << result.message << std::endl;
                cycle_errors++;
                error_count++;

                std::stringstream err_msg;
                err_msg << "🚨 *CRITICAL: SPREAD EXIT FAILED - Manual Action Required*\n"
                        << "────────────────────────\n"
                        << "• *Underlying:* " << sp.underlying << "\n"
                        << "• *Strategy:* " << sp.strategy << "\n"
                        << "• *Position ID:* " << sp.id << "\n"
                        << "• *Exit Rule Triggered:* " << exit_reason << "\n"
                        << "• *Error Details:* " << result.message << "\n"
                        << "• *Action Required:* Close this position manually immediately";
                TelegramNotifier::sendMessage(err_msg.str());
            }
        }

        // Authoritative portfolio-risk snapshot for the pre-order gate — now
        // including multi-leg spreads (contributions gathered in both loops).
        // RULE-D4 darkness guard: if open positions exist but NOT ONE produced
        // live Greeks this cycle (total quote outage), do NOT overwrite a prior
        // real breach with a false 'clear' — reading darkness as "no breach"
        // would reopen new-entry gating exactly when the book is unobservable.
        // Leave the last known snapshot and flag it loudly in the logs.
        {
            size_t total_positions = open_positions.size() + open_spreads.size();
            if (!contributions.empty()) {
                set_last_risk_breach(nox::risk::evaluate(contributions, risk_targets_));
                if (contributions.size() < total_positions) {
                    std::cout << "[POS_MANAGER] ⚠️ Partial portfolio-risk visibility: "
                              << (total_positions - contributions.size()) << " of "
                              << total_positions << " open position(s) had no live Greeks "
                              << "this cycle and are excluded from the risk snapshot."
                              << std::endl;
                }
            } else if (total_positions > 0) {
                std::cout << "[POS_MANAGER] ⚠️ Portfolio-risk DARKNESS: " << total_positions
                          << " open position(s) but zero live Greeks this cycle (quote "
                          << "outage) — keeping the previous risk snapshot rather than "
                          << "reading darkness as 'no breach'." << std::endl;
            }
        }

        if (cycle_errors > 0) {
            std::cerr << "[POS_MANAGER] ⚠️ Monitoring cycle completed with " << cycle_errors
                      << " error(s). Total errors this session: " << error_count << std::endl;
        }
    }
}
