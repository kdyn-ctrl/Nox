// test_futures_signal_generator.cpp — futures signal phase 1 (CLAUDE.md):
// signals only, no order routing. Asserts the bias-combination math and the
// futures_signals schema; there is no order-submission code path to test
// against because FuturesSignalGenerator has none (see its header comment).

#include "../FuturesSignalStore.hpp"
#include "../FuturesSignalGenerator.hpp"

#include <cstdio>
#include <iostream>
#include <sqlite3.h>
#include <string>

using nox::execution::FuturesSignalStore;
using nox::execution::FuturesSignal;
using nox::execution::MassiveFuturesClient;
using nox::futures_signal::FuturesSignalGenerator;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb(const std::string& tag) {
    return "/tmp/nox_futures_signal_test_" + tag + ".db";
}
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

static void test_macro_confirm_drives_direction_from_bias() {
    std::cout << "\n[confirm] physical+political agree — direction follows bias, quality=|physical_stress|\n";
    MassiveFuturesClient::Bar bar; bar.close = 80.0; bar.prevClose = 79.0; bar.valid = true;
    nlohmann::json region = {
        {"verdict", "CONFIRM"}, {"bias", "BULLISH_OIL"},
        {"physical_stress", 0.72}, {"political_signal", 0.65},
        {"reason", "Physical supply data and political signal agree."}
    };
    auto sig = FuturesSignalGenerator::computeSignal("CL", bar, region, 1000);
    CHECK(sig.direction == "BULLISH", "CONFIRM + BULLISH_OIL bias maps to a BULLISH direction");
    CHECK(std::abs(sig.quality_score - 0.72) < 1e-9, "quality score takes the physical_stress magnitude when present");
    CHECK(sig.macro_verdict == "CONFIRM", "macro_verdict is carried through unchanged");
}

static void test_contradiction_trusts_physical_over_political() {
    std::cout << "\n[contradiction] text vs. physical disagree — bias already resolved to physical by alt_macro.py\n";
    MassiveFuturesClient::Bar bar; bar.close = 80.0; bar.prevClose = 79.0; bar.valid = true;
    nlohmann::json region = {
        {"verdict", "TEXT_CONTRADICTS_PHYSICAL"}, {"bias", "BEARISH_OIL"},
        {"physical_stress", -0.55}, {"political_signal", 0.60},
        {"reason", "Political narrative implies BULLISH_OIL but physical supply data implies BEARISH_OIL."}
    };
    auto sig = FuturesSignalGenerator::computeSignal("CL", bar, region, 1000);
    CHECK(sig.direction == "BEARISH", "generator trusts alt_macro.py's already-resolved physical-side bias, not the raw political sign");
}

static void test_no_macro_data_falls_back_to_momentum() {
    std::cout << "\n[momentum-only] alt_macro region absent/NO_DATA — direction comes from price momentum\n";
    MassiveFuturesClient::Bar bar; bar.close = 82.0; bar.prevClose = 80.0; bar.valid = true; // +2.5%
    nlohmann::json region; // null — no matching region this cycle
    auto sig = FuturesSignalGenerator::computeSignal("CL", bar, region, 1000);
    CHECK(sig.direction == "BULLISH", "positive momentum with no macro data still yields a directional bias");
    CHECK(sig.macro_verdict.empty(), "no macro_verdict is recorded when there's no macro data");

    nlohmann::json noDataRegion = {{"verdict", "NO_DATA"}};
    MassiveFuturesClient::Bar barDown; barDown.close = 78.0; barDown.prevClose = 80.0; barDown.valid = true;
    auto sigDown = FuturesSignalGenerator::computeSignal("CL", barDown, noDataRegion, 1000);
    CHECK(sigDown.direction == "BEARISH", "an explicit NO_DATA verdict is treated the same as no region at all");
}

static void test_no_data_at_all_is_neutral() {
    std::cout << "\n[no-data] no bar and no macro data — NEUTRAL, quality 0, never a false directional call\n";
    MassiveFuturesClient::Bar bar; // valid=false, default
    nlohmann::json region;
    auto sig = FuturesSignalGenerator::computeSignal("CL", bar, region, 1000);
    CHECK(sig.direction == "NEUTRAL", "no market data and no macro signal never fabricates a direction");
    CHECK(sig.quality_score == 0.0, "quality score is 0 when there's nothing to base a call on");
}

static void test_quality_score_clamped_to_one() {
    std::cout << "\n[clamp] quality score never exceeds 1.0 regardless of input magnitude\n";
    MassiveFuturesClient::Bar bar; bar.close = 100.0; bar.prevClose = 50.0; bar.valid = true; // +100% — extreme
    nlohmann::json region;
    auto sig = FuturesSignalGenerator::computeSignal("CL", bar, region, 1000);
    CHECK(sig.quality_score <= 1.0, "momentum-only quality score is clamped to [0,1] even for an extreme move");
}

static void test_store_schema_and_insert() {
    std::cout << "\n[store] futures_signals table accepts a full row and persists it\n";
    std::string db = tmpDb("schema"); wipe(db);
    FuturesSignalStore store(db);

    FuturesSignal sig;
    sig.contract = "CL"; sig.direction = "BULLISH"; sig.price = 81.23;
    sig.physical_stress = 0.5; sig.political_signal = 0.4;
    sig.macro_verdict = "CONFIRM"; sig.macro_bias = "BULLISH_OIL";
    sig.quality_score = 0.5; sig.reason = "test"; sig.scan_at = 1234567890;

    CHECK(store.insert(sig), "insert() succeeds against a freshly-created table");

    sqlite3* raw = nullptr;
    sqlite3_open(db.c_str(), &raw);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(raw, "SELECT contract, direction, quality_score FROM futures_signals WHERE scan_at = 1234567890;", -1, &stmt, 0);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    CHECK(found, "the inserted row is readable back out of the shared DB file");
    if (found) {
        std::string contract = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        CHECK(contract == "CL", "contract column round-trips correctly");
    }
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    wipe(db);
}

int main() {
    std::cout << "═══ Futures signal generator (signals-only) tests ═══\n";
    test_macro_confirm_drives_direction_from_bias();
    test_contradiction_trusts_physical_over_political();
    test_no_macro_data_falls_back_to_momentum();
    test_no_data_at_all_is_neutral();
    test_quality_score_clamped_to_one();
    test_store_schema_and_insert();

    std::cout << "\n"
              << (g_failures == 0 ? "✅ ALL FUTURES SIGNAL TESTS PASSED\n"
                                  : "❌ " + std::to_string(g_failures) + " CHECK(S) FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
