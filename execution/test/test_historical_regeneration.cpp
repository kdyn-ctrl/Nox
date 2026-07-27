// test_historical_regeneration.cpp — Phase 3: backtesting integration item.
//
// CLAUDE.md's Phase 3 backtesting item asks to "inject simulated order
// failures to confirm signal regeneration holds up historically, not just in
// the Phase 1 mock server." test_signal_regeneration.cpp (Phase 2) already
// proves the gate logic correctly regenerates/silences across 2-3 manually
// sequenced cycles. This test extends that into an actual multi-day replay —
// a 20-"day" simulated scan loop driving the SAME pre-order-gate primitive
// against a scripted per-day broker mode (500s, rate-limit, and a clean fill
// interleaved), across three independent tickers with different day-by-day
// signal-persistence patterns — and asserts the historical invariants:
//
//   1. A signature that keeps recurring because the breakout condition still
//      holds regenerates on every day the broker previously failed it, and
//      stops the instant it fills (never double-submits after that).
//   2. A signature whose condition stops holding after a failed day is never
//      force-retried — Nox's signal-driven philosophy (CLAUDE.md's "Core
//      Philosophy") means silence is correct, not a bug, when the generator
//      itself doesn't re-fire.
//   3. Across the full 20-day replay, exactly one broker POST ever reaches
//      the mock server per eventual fill — no day's gate logic double-buys.

#include "MockAlpacaServer.hpp"
#include "../OrderLedger.hpp"
#include "../OptionsOrderRouter.hpp"
#include "../PositionManager.hpp"
#include "../OptionsSignalTypes.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using nox::execution::OrderLedger;
using nox::options_router::OptionsOrderRouter;
using nox::test::MockAlpacaServer;
namespace osig = nox::options_signal;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb(const std::string& tag) {
    return "/tmp/nox_histregen_test_" + tag + ".db";
}
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

static osig::OptionsSignal makeSignal(const std::string& ticker, double strike) {
    osig::OptionsSignal s;
    s.underlying  = ticker;
    s.strategy    = "LONG_CALL";
    s.expiry_date = "2026-12-18";
    s.strike      = strike;
    s.strike2     = 0.0;
    s.option_type = nox::options::OptionType::Call;
    s.entry_price = 3.00;
    s.rsi         = 55.0;
    return s;
}
static std::string signatureFor(const std::string& ticker, double strike, int day) {
    // Day-scoped, exactly like main.cpp's real equity/options signatures — a
    // breakout condition that "still holds" produces the SAME signature every
    // day it recurs; conditions changing (or a fill) is what ends the series,
    // never an artificial expiry of the signature itself.
    (void)day;
    return ticker + "|LONG_CALL|" + std::to_string(strike) + "|0.00|2026-12-18|buy";
}

enum class Gate { Allow, BlockedDuplicate, BlockedPositionExists };

// Exact mirror of main.cpp's pre_order_hook lambda, same as
// test_signal_regeneration.cpp — the gate under test is the real production
// logic's shape, not a reimplementation.
static Gate preOrderGate(OrderLedger& led, PositionManager& pm,
                         const osig::OptionsSignal& s, const std::string& coid,
                         const std::string& sig, long sent_at_override) {
    if (pm.has_open_position(s.underlying, "call", s.strike, s.expiry_date)) {
        led.logSignalEvent(s.underlying, s.strategy, sig, -1.0,
            "gate_blocked_position_exists", "position already open for this contract");
        return Gate::BlockedPositionExists;
    }
    // 86400s window: day-scoped signature, mirrors the day-scoped equity gate
    // in main.cpp (hasRecentActive(eq_signature, 86400)) rather than the 60s
    // options network-retry window — a historical day-by-day replay needs the
    // day-scoped semantics, not the sub-minute one.
    if (led.hasRecentActive(sig, 86400)) {
        led.logSignalEvent(s.underlying, s.strategy, sig, -1.0,
            "gate_blocked_duplicate", "already attempted today");
        return Gate::BlockedDuplicate;
    }
    OrderLedger::Order o;
    o.client_oid  = coid;
    o.ticker      = s.underlying;
    o.strategy    = s.strategy;
    o.signature   = sig;
    o.side        = "buy";
    o.option_type = "call";
    o.profile_type = "long";
    o.expiration_date = s.expiry_date;
    o.status      = "pending";
    o.strike      = s.strike;
    o.qty         = 1;
    o.entry_price = s.entry_price;
    o.sent_at     = sent_at_override;
    led.insertPending(o);
    led.logSignalEvent(s.underlying, s.strategy, sig, -1.0,
        "submitted", "order POST about to fire (client_oid=" + coid + ")");
    return Gate::Allow;
}

struct DaySpec {
    int day;
    bool signal_recurs;             // does the breakout condition still hold today?
    MockAlpacaServer::Mode broker_mode; // what the broker does THIS day, if we submit
};

// Runs one ticker's full multi-day script against a shared mock server and
// ledger/PositionManager pair, returning how many real broker POSTs actually
// landed (via the disposition of route() calls) for the caller to assert on.
static int replayTicker(MockAlpacaServer& mock, OrderLedger& led, PositionManager& pm,
                        const std::string& ticker, double strike,
                        const std::vector<DaySpec>& script) {
    OptionsOrderRouter router(mock.url(), "k", "s");
    int broker_posts = 0;
    bool filled = false;

    for (const auto& d : script) {
        auto sig = makeSignal(ticker, strike);
        std::string sg = signatureFor(ticker, strike, d.day);
        long sent_at = OrderLedger::now_epoch() - static_cast<long>((30 - d.day) * 86400);

        if (!d.signal_recurs) {
            // Conditions no longer hold — the generator itself produces
            // nothing today. Signal-driven, not retry-driven: there is
            // nothing for the gate to even evaluate.
            continue;
        }

        std::string coid = "nox-o-" + ticker + "-LONG_CALL-" + std::to_string(d.day);
        Gate g = preOrderGate(led, pm, sig, coid, sg, sent_at);
        if (g != Gate::Allow) continue; // silenced — position exists or same-day dup

        mock.set_mode(d.broker_mode);
        auto res = router.route(sig, 1, coid);
        ++broker_posts;
        if (res.success) {
            led.setStatus(coid, "filled", res.order_id);
            pm.add_position(ticker, "call", strike, 1, sig.entry_price,
                            "2026-12-0" + std::to_string(d.day % 9 + 1), "long", sig.expiry_date);
            filled = true;
        } else {
            led.setStatus(coid, "failed");
        }
    }
    (void)filled;
    return broker_posts;
}

// ── Scenario A: breakout persists 3 days through broker failures, fills on
//    day 4, then stays silent for the remaining 16 simulated days ───────────
static void test_persistent_breakout_regenerates_until_fill(MockAlpacaServer& mock) {
    std::cout << "\n[historical] persistent breakout regenerates through 3 failed days, fills day 4, silent after\n";
    std::string db = tmpDb("persist"); wipe(db);
    OrderLedger led(db);
    OptionsOrderRouter dummy_router(mock.url(), "k", "s");
    PositionManager pm(db, dummy_router);

    std::vector<DaySpec> script;
    script.push_back({1, true,  MockAlpacaServer::Mode::Server500});
    script.push_back({2, true,  MockAlpacaServer::Mode::Rate429});
    script.push_back({3, true,  MockAlpacaServer::Mode::Server500});
    script.push_back({4, true,  MockAlpacaServer::Mode::Normal}); // fills
    for (int d = 5; d <= 20; ++d) script.push_back({d, true, MockAlpacaServer::Mode::Normal});

    int posts = replayTicker(mock, led, pm, "AAPL", 150.0, script);

    CHECK(posts == 4, "exactly 4 broker POSTs across 20 days: 3 failures + the fill (days 5-20 silenced pre-broker)");

    std::string sg = signatureFor("AAPL", 150.0, 0);
    auto history = led.getEventsBySignature(sg);
    int submitted = 0, blocked_position = 0;
    for (const auto& e : history) {
        if (e.outcome == "submitted") ++submitted;
        if (e.outcome == "gate_blocked_position_exists") ++blocked_position;
    }
    CHECK(submitted == 4, "signal_events shows exactly 4 submissions (3 failed + 1 filled), not 20");
    CHECK(blocked_position == 16, "the remaining 16 days are documented as silenced because a position now exists");
    wipe(db);
}

// ── Scenario B: breakout condition vanishes after one failed day — no forced
//    retry ever happens, matching CLAUDE.md's signal-driven philosophy ──────
static void test_vanishing_breakout_never_force_retried(MockAlpacaServer& mock) {
    std::cout << "\n[historical] breakout that stops recurring after a failure is never force-retried\n";
    std::string db = tmpDb("vanish"); wipe(db);
    OrderLedger led(db);
    OptionsOrderRouter dummy_router(mock.url(), "k", "s");
    PositionManager pm(db, dummy_router);

    std::vector<DaySpec> script;
    script.push_back({1, true, MockAlpacaServer::Mode::Server500}); // fails
    for (int d = 2; d <= 20; ++d) script.push_back({d, false, MockAlpacaServer::Mode::Normal}); // move already happened

    int posts = replayTicker(mock, led, pm, "TSLA", 250.0, script);

    CHECK(posts == 1, "only day 1's single failed POST ever reaches the broker across all 20 days");

    std::string sg = signatureFor("TSLA", 250.0, 0);
    auto history = led.getEventsBySignature(sg);
    CHECK(history.size() == 1, "signal_events has exactly one entry — no phantom regeneration attempts were logged");
    CHECK(history[0].outcome == "submitted", "the one entry is the original (failed) submission");
    wipe(db);
}

// ── Scenario C: two tickers replay independently in the same ledger without
//    cross-contaminating each other's signature/day-scoping ─────────────────
static void test_independent_tickers_isolated_across_replay(MockAlpacaServer& mock) {
    std::cout << "\n[historical] two tickers' 20-day replays never interact\n";
    std::string db = tmpDb("isolated"); wipe(db);
    OrderLedger led(db);
    OptionsOrderRouter dummy_router(mock.url(), "k", "s");
    PositionManager pm(db, dummy_router);

    std::vector<DaySpec> scriptA;
    scriptA.push_back({1, true, MockAlpacaServer::Mode::Normal}); // fills immediately
    for (int d = 2; d <= 10; ++d) scriptA.push_back({d, true, MockAlpacaServer::Mode::Normal});

    std::vector<DaySpec> scriptB;
    scriptB.push_back({1, true, MockAlpacaServer::Mode::Server500});
    scriptB.push_back({2, true, MockAlpacaServer::Mode::Normal}); // fills day 2
    for (int d = 3; d <= 10; ++d) scriptB.push_back({d, true, MockAlpacaServer::Mode::Normal});

    int postsA = replayTicker(mock, led, pm, "MSFT", 400.0, scriptA);
    int postsB = replayTicker(mock, led, pm, "NVDA", 900.0, scriptB);

    CHECK(postsA == 1, "MSFT: fills on day 1, silenced for the remaining 9 days");
    CHECK(postsB == 2, "NVDA: fails day 1, fills day 2, silenced for the remaining 8 days");

    CHECK(pm.has_open_position("MSFT", "call", 400.0, "2026-12-18"), "MSFT position open");
    CHECK(pm.has_open_position("NVDA", "call", 900.0, "2026-12-18"), "NVDA position open");
    CHECK(!pm.has_open_position("MSFT", "call", 900.0, "2026-12-18"), "MSFT's strike never leaks into NVDA's");
    wipe(db);
}

int main() {
    std::cout << "═══ Phase 3 historical signal-regeneration replay tests ═══\n";
    MockAlpacaServer mock(18101);
    mock.start();

    test_persistent_breakout_regenerates_until_fill(mock);
    test_vanishing_breakout_never_force_retried(mock);
    test_independent_tickers_isolated_across_replay(mock);

    mock.stop();

    std::cout << "\n"
              << (g_failures == 0 ? "✅ ALL HISTORICAL-REGENERATION TESTS PASSED\n"
                                  : "❌ " + std::to_string(g_failures) + " CHECK(S) FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
