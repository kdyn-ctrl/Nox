// §2 C3 verification: STRANGLE was selected under the prefer_sell (income)
// thesis but routeStrangle() bought both legs — a long strangle, not the
// credit position selectStrategy() actually chose. Confirms the fix: both
// legs now submit "sell", and a naked-options-approval pre-flight check
// blocks execution when the account lacks level-3 (uncovered) approval
// instead of letting an opaque 403 (or worse, a silently wrong direction)
// be the only signal something's off.

#include "MockAlpacaServer.hpp"
#include "../OptionsOrderRouter.hpp"
#include "../OptionsSignalTypes.hpp"

#include <iostream>
#include <string>

using nox::options_router::OptionsOrderRouter;
using nox::test::MockAlpacaServer;
namespace osig = nox::options_signal;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static osig::OptionsSignal makeStrangleSignal() {
    osig::OptionsSignal s;
    s.underlying  = "TEST";
    s.strategy    = "STRANGLE";
    s.expiry_date = "2026-08-21";
    s.strike      = 110.0; // call strike
    s.strike2     = 90.0;  // put strike
    return s;
}

int main() {
    std::cout << "=== §2 C3 — short strangle routing + naked-options approval gate ===\n\n";

    MockAlpacaServer mock(18102);
    mock.start();
    OptionsOrderRouter router(mock.url(), "test-key", "test-secret");

    // ── Both legs submit as "sell", not "buy" ─────────────────────────────
    {
        auto sig = makeStrangleSignal();
        auto result = router.route(sig, /*qty_contracts=*/1, "test-strangle-1");
        CHECK(result.success, "strangle order accepted by mock broker");

        auto body = mock.last_order_body();
        CHECK(body.contains("legs") && body["legs"].size() == 2, "order carries exactly 2 legs");
        if (body.contains("legs") && body["legs"].size() == 2) {
            for (const auto& leg : body["legs"]) {
                CHECK(leg.value("side", "") == "sell",
                      "leg " + leg.value("symbol", "?") + " submitted as sell (got: " +
                      leg.value("side", "?") + ")");
            }
        }
    }

    // ── Naked-options approval check: level 3 → approved ──────────────────
    {
        mock.set_options_trading_level(3);
        CHECK(router.validateNakedOptionsApproval(),
              "level-3 (uncovered) account passes the naked-options approval check");
    }

    // ── Naked-options approval check: level < 3 → blocked ──────────────────
    {
        mock.set_options_trading_level(2);
        CHECK(!router.validateNakedOptionsApproval(),
              "level-2 account fails the naked-options approval check (would 403 at the broker)");
        mock.set_options_trading_level(3); // restore
    }

    mock.stop();

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "✅ All short-strangle routing tests passed.\n";
        return 0;
    }
    std::cout << "❌ " << g_failures << " test(s) failed.\n";
    return 1;
}
