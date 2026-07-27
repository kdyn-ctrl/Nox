// test_phantom_reversal.cpp — reconciliation reverses an optimistically-booked
// position when the originating order resolves to a CONFIRMED zero fill.
//
// The execution recorder books a position on the broker's ACCEPT ack; if that
// order is later rejected/expired/canceled with no fill, that booked position
// is a phantom. remove_phantom_single_leg / remove_phantom_spread reverse it,
// keyed by the same contract tuple the position-exists gate uses. A partial
// fill (filled_qty > 0) must NEVER be reversed — that guard lives in main.cpp's
// reconciler; here we test the PositionManager removal primitives it calls.

#include "../OptionsOrderRouter.hpp"
#include "../PositionManager.hpp"

#include <cstdio>
#include <iostream>
#include <string>

using nox::options_router::OptionsOrderRouter;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpdb() {
    char buf[] = "/tmp/nox_phantom_XXXXXX";
    int fd = mkstemp(buf);
    if (fd >= 0) close(fd);
    return std::string(buf);
}

int main() {
    OptionsOrderRouter router("http://127.0.0.1:1", "k", "s"); // never called

    std::cout << "[single-leg] reverse a booked phantom by contract tuple\n";
    {
        std::string db = tmpdb();
        PositionManager pm(db, router);
        pm.add_position("SPY", "call", 500.0, 3, 1.25, "2026-08-01", "long", "2026-09-18");
        CHECK(pm.has_open_position("SPY", "call", 500.0, "2026-09-18"),
              "position booked and visible to the gate");

        int removed = pm.remove_phantom_single_leg("SPY", "call", 500.0, "2026-09-18");
        CHECK(removed == 1, "remove_phantom_single_leg reports exactly 1 removed");
        CHECK(!pm.has_open_position("SPY", "call", 500.0, "2026-09-18"),
              "phantom gone — gate no longer blocks regeneration");
        std::remove(db.c_str());
    }

    std::cout << "[single-leg] no-match removal is a safe no-op\n";
    {
        std::string db = tmpdb();
        PositionManager pm(db, router);
        pm.add_position("SPY", "call", 500.0, 3, 1.25, "2026-08-01", "long", "2026-09-18");
        // Different strike / type / expiry must NOT touch the real position.
        CHECK(pm.remove_phantom_single_leg("SPY", "call", 505.0, "2026-09-18") == 0,
              "different strike removes nothing");
        CHECK(pm.remove_phantom_single_leg("SPY", "put", 500.0, "2026-09-18") == 0,
              "different type removes nothing");
        CHECK(pm.remove_phantom_single_leg("SPY", "call", 500.0, "2026-10-16") == 0,
              "different expiry removes nothing");
        CHECK(pm.has_open_position("SPY", "call", 500.0, "2026-09-18"),
              "the real position is untouched by non-matching reversals");
        std::remove(db.c_str());
    }

    std::cout << "[spread] reverse a booked phantom spread by key\n";
    {
        std::string db = tmpdb();
        PositionManager pm(db, router);
        std::vector<SpreadLeg> legs = {
            {"call", 500.0, "buy"}, {"call", 510.0, "sell"}};
        pm.add_spread_position("SPY", "BULL_CALL_SPREAD", 2, 3.00, "2026-08-01",
                               "2026-09-18", legs);
        CHECK(pm.has_open_spread_position("SPY", "BULL_CALL_SPREAD", "2026-09-18"),
              "spread booked and visible to the gate");

        int removed = pm.remove_phantom_spread("SPY", "BULL_CALL_SPREAD", "2026-09-18");
        CHECK(removed == 1, "remove_phantom_spread reports exactly 1 removed");
        CHECK(!pm.has_open_spread_position("SPY", "BULL_CALL_SPREAD", "2026-09-18"),
              "phantom spread gone");
        std::remove(db.c_str());
    }

    std::cout << "[spread] no-match removal is a safe no-op\n";
    {
        std::string db = tmpdb();
        PositionManager pm(db, router);
        std::vector<SpreadLeg> legs = {
            {"call", 500.0, "buy"}, {"call", 510.0, "sell"}};
        pm.add_spread_position("SPY", "BULL_CALL_SPREAD", 2, 3.00, "2026-08-01",
                               "2026-09-18", legs);
        CHECK(pm.remove_phantom_spread("QQQ", "BULL_CALL_SPREAD", "2026-09-18") == 0,
              "different underlying removes nothing");
        CHECK(pm.remove_phantom_spread("SPY", "BEAR_PUT_SPREAD", "2026-09-18") == 0,
              "different strategy removes nothing");
        CHECK(pm.remove_phantom_spread("SPY", "BULL_CALL_SPREAD", "2026-10-16") == 0,
              "different expiry removes nothing");
        CHECK(pm.has_open_spread_position("SPY", "BULL_CALL_SPREAD", "2026-09-18"),
              "the real spread is untouched by non-matching reversals");
        std::remove(db.c_str());
    }

    if (g_failures == 0) {
        std::cout << "\n✅ ALL PHANTOM-REVERSAL TESTS PASSED\n";
        return 0;
    }
    std::cout << "\n❌ " << g_failures << " phantom-reversal test(s) FAILED\n";
    return 1;
}
