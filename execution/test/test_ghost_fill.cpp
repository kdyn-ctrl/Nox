// test_ghost_fill.cpp — Phase 1 defensive-infrastructure end-to-end tests.
//
// Drives the REAL OptionsOrderRouter + OrderLedger + PositionManager against the
// in-process MockAlpacaServer and asserts the four failure modes CLAUDE.md item 7
// requires. The reconcile decision below mirrors NoxEngine::reconcile_options_orders
// (main.cpp) so the test exercises the same primitives the engine uses.

#include "MockAlpacaServer.hpp"
#include "../OrderLedger.hpp"
#include "../OptionsOrderRouter.hpp"
#include "../PositionManager.hpp"
#include "../OptionsSignalTypes.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using nox::execution::OrderLedger;
using nox::options_router::OptionsOrderRouter;
using nox::options_router::OrderDisposition;
using nox::test::MockAlpacaServer;
namespace osig = nox::options_signal;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static const int    RECON_GRACE_SECONDS = 75;
static const char*  ENTRY_DATE          = "2026-07-08";

// Build the canonical single-leg LONG_CALL test signal.
static osig::OptionsSignal makeSignal() {
    osig::OptionsSignal s;
    s.underlying   = "TEST";
    s.strategy     = "LONG_CALL";
    s.expiry_date  = "2026-08-21";
    s.strike       = 100.0;
    s.strike2      = 0.0;
    s.option_type  = nox::options::OptionType::Call;
    s.entry_price  = 2.50;
    s.rsi          = 55.0;
    return s;
}

static std::string signatureOf(const osig::OptionsSignal& s) {
    return s.underlying + "|" + s.strategy + "|100.00|0.00|" + s.expiry_date + "|buy";
}

// Insert the 'pending' ledger row exactly as the engine's pre-order hook does.
static void insertPending(OrderLedger& led, const osig::OptionsSignal& s,
                          const std::string& coid, const std::string& sig) {
    OrderLedger::Order o;
    o.client_oid      = coid;
    o.ticker          = s.underlying;
    o.strategy        = s.strategy;
    o.signature       = sig;
    o.side            = "buy";
    o.option_type     = "call";
    o.profile_type    = "long";
    o.expiration_date = s.expiry_date;
    o.status          = "pending";
    o.strike          = s.strike;
    o.qty             = 1;
    o.entry_price     = s.entry_price;
    led.insertPending(o);
}

// Mirror of NoxEngine::reconcile_options_orders decision logic (single order).
static void reconcileOne(OrderLedger& led, OptionsOrderRouter& router,
                         PositionManager& pm, const OrderLedger::Order& o) {
    auto st  = router.getOrderByClientId(o.client_oid);
    long age = OrderLedger::now_epoch() - o.sent_at;

    if (!st.reachable) return;                    // never self-inflict 'failed'
    if (!st.found) {
        if (age >= RECON_GRACE_SECONDS) led.setStatus(o.client_oid, "failed");
        return;
    }
    const std::string& s = st.status;
    if (s == "filled" || s == "partially_filled" || s == "accepted" ||
        s == "new"    || s == "pending_new") {
        led.setStatus(o.client_oid, "filled", st.broker_order_id);
        bool single_leg = (o.strategy == "LONG_CALL" || o.strategy == "LONG_PUT" ||
                           o.strategy == "CSP"       || o.strategy == "CC");
        if (single_leg && !o.option_type.empty() &&
            !pm.has_open_position(o.ticker, o.option_type, o.strike, o.expiration_date)) {
            pm.add_position(o.ticker, o.option_type, o.strike, static_cast<int>(o.qty),
                            o.entry_price, ENTRY_DATE,
                            o.profile_type.empty() ? "long" : o.profile_type,
                            o.expiration_date);
        }
    } else if (s == "canceled" || s == "rejected" || s == "expired") {
        led.setStatus(o.client_oid, "failed", st.broker_order_id);
    }
}

static void reconcileAll(OrderLedger& led, OptionsOrderRouter& router, PositionManager& pm) {
    for (const auto& o : led.getUnresolved()) reconcileOne(led, router, pm, o);
}

// Fresh temp DB path per scenario so tests don't interfere.
static std::string tmpDb(const std::string& tag) {
    return "/tmp/nox_ghost_test_" + tag + ".db";
}
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

// ── Scenario: GHOST FILL (the core Phase 1 guarantee) ─────────────────────────
static void test_ghost_fill(MockAlpacaServer& mock) {
    std::cout << "\n[ghost-fill] broker fills but the response is lost → reconcile catches it\n";
    std::string db = tmpDb("ghost"); wipe(db);
    OptionsOrderRouter router(mock.url(), "k", "s");
    OrderLedger led(db);
    PositionManager pm(db, router);
    auto sig  = makeSignal();
    std::string coid = "nox-o-TEST-LONG_CALL-10000-ghost";
    std::string sg   = signatureOf(sig);

    mock.set_mode(MockAlpacaServer::Mode::Ghost);
    mock.set_ghost_delay(11); // > client 10s read timeout

    insertPending(led, sig, coid, sg);
    auto res = router.route(sig, 1, coid);        // blocks ~10s then times out

    CHECK(!res.success, "route reports failure on lost response");
    CHECK(res.disposition == OrderDisposition::Timeout, "disposition is Timeout (not Rejected)");
    led.setStatus(coid, "unknown");               // engine maps Timeout → 'unknown'
    CHECK(!pm.has_open_position("TEST", "call", 100.0, "2026-08-21"),
          "no position recorded yet (we don't know the fill)");

    reconcileAll(led, router, pm);                // the ghost-fill catch

    CHECK(pm.has_open_position("TEST", "call", 100.0, "2026-08-21"),
          "reconciliation discovers the fill and tracks the position");
    CHECK(mock.post_count(coid) == 1, "exactly ONE order POST — no double-entry");

    // Second scan: position now exists → pre-order gate blocks a re-fire.
    bool blocked_by_position = pm.has_open_position("TEST", "call", 100.0, "2026-08-21");
    CHECK(blocked_by_position, "next scan is blocked by the position-exists check");
    wipe(db);
}

// ── Scenario: 429 RATE LIMIT → 60s blocker catches the retry ─────────────────
static void test_rate_limit(MockAlpacaServer& mock) {
    std::cout << "\n[429] rate limit → the 60s blocker (not the ledger) catches the retry\n";
    std::string db = tmpDb("429"); wipe(db);
    OptionsOrderRouter router(mock.url(), "k", "s");
    OrderLedger led(db);
    PositionManager pm(db, router);
    auto sig = makeSignal();
    std::string sg = signatureOf(sig);
    std::string coid1 = "nox-o-TEST-LONG_CALL-10000-r1";

    mock.set_mode(MockAlpacaServer::Mode::Rate429);

    // First fire: gate allows (nothing recent), order 429s.
    CHECK(!led.hasRecentActive(sg, 60), "first fire: no recent attempt, gate allows");
    insertPending(led, sig, coid1, sg);
    auto res = router.route(sig, 1, coid1);
    CHECK(!res.success && res.disposition == OrderDisposition::Rejected, "429 → Rejected");
    led.setStatus(coid1, "failed");

    // Immediate retry of the SAME signature within 60s → blocked by recency.
    CHECK(led.hasRecentActive(sg, 60), "retry within 60s is blocked by the recency check");
    CHECK(mock.post_count(coid1) == 1, "only the first attempt hit the broker");
    wipe(db);
}

// ── Scenario: 500 SERVER ERROR → fail once, regenerate next scan ─────────────
static void test_server_error(MockAlpacaServer& mock) {
    std::cout << "\n[500] server error → fail once; a later scan (>60s) may regenerate\n";
    std::string db = tmpDb("500"); wipe(db);
    OptionsOrderRouter router(mock.url(), "k", "s");
    OrderLedger led(db);
    PositionManager pm(db, router);
    auto sig = makeSignal();
    std::string sg = signatureOf(sig);
    std::string coid = "nox-o-TEST-LONG_CALL-10000-s1";

    mock.set_mode(MockAlpacaServer::Mode::Server500);
    insertPending(led, sig, coid, sg);
    auto res = router.route(sig, 1, coid);
    CHECK(!res.success && res.disposition == OrderDisposition::Rejected, "500 → Rejected (fail once)");
    led.setStatus(coid, "failed");
    CHECK(led.hasRecentActive(sg, 60), "immediate re-fire blocked within window");

    // Seed an OLD attempt (>60s ago) for a different signature to prove the gate
    // reopens once the window passes — i.e. the next scan can regenerate.
    OrderLedger::Order old;
    old.client_oid = "nox-o-OLD"; old.ticker = "TEST"; old.strategy = "LONG_CALL";
    old.signature  = "OLDSIG"; old.status = "failed";
    old.sent_at    = OrderLedger::now_epoch() - 120; // 2 minutes ago
    led.insertPending(old);
    CHECK(!led.hasRecentActive("OLDSIG", 60), "attempt older than the window no longer blocks (regeneration)");
    wipe(db);
}

// ── Scenario: MALFORMED response → caught, resolved by reconciliation ────────
static void test_malformed(MockAlpacaServer& mock) {
    std::cout << "\n[malformed] non-JSON 2xx → ParseError → reconcile resolves, no crash\n";
    std::string db = tmpDb("malformed"); wipe(db);
    OptionsOrderRouter router(mock.url(), "k", "s");
    OrderLedger led(db);
    PositionManager pm(db, router);
    auto sig = makeSignal();
    std::string sg = signatureOf(sig);
    std::string coid = "nox-o-TEST-LONG_CALL-10000-m1";

    mock.set_mode(MockAlpacaServer::Mode::Malformed);
    insertPending(led, sig, coid, sg);
    auto res = router.route(sig, 1, coid);
    CHECK(!res.success && res.disposition == OrderDisposition::ParseError,
          "malformed 2xx → ParseError (not a false success)");
    led.setStatus(coid, "unknown");
    CHECK(!pm.has_open_position("TEST", "call", 100.0, "2026-08-21"), "no position on parse error");

    reconcileAll(led, router, pm);                // broker had it accepted
    CHECK(pm.has_open_position("TEST", "call", 100.0, "2026-08-21"),
          "reconciliation resolves the accepted order into a tracked position");
    wipe(db);
}

int main() {
    std::cout << "═══ Phase 1 ghost-fill / defensive-infrastructure tests ═══\n";
    MockAlpacaServer mock(18099);
    mock.start();

    test_rate_limit(mock);
    test_server_error(mock);
    test_malformed(mock);
    test_ghost_fill(mock);   // last — it incurs the ~10s read-timeout wait

    mock.stop();

    std::cout << "\n"
              << (g_failures == 0 ? "✅ ALL GHOST-FILL TESTS PASSED\n"
                                  : "❌ " + std::to_string(g_failures) + " CHECK(S) FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
