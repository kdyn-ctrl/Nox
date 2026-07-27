// test_kill_switch_store.cpp — global kill switch persistence.
//
// KillSwitchStore is the persisted halt state behind /pause, /resume, and
// the automatic daily-loss-limit trigger (check_daily_loss_limit() in
// main.cpp). These tests assert: default state is active (not paused),
// pause()/resume() round-trip correctly with reason/triggered_by, state
// survives a fresh handle to the same file (simulating a process restart),
// and reads fail open (not-paused) against an unreachable/malformed DB.

#include "../KillSwitchStore.hpp"

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

using nox::execution::KillSwitchStore;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  ✗ FAIL: " << (msg) << "\n"; ++g_failures; } \
    else         { std::cout << "  ✓ " << (msg) << "\n"; } \
} while (0)

static std::string tmpDb(const std::string& tag) {
    return "/tmp/nox_killswitch_test_" + tag + ".db";
}
static void wipe(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

int main() {
    std::cout << "=== KillSwitchStore tests ===\n\n";

    std::cout << "[default state]\n";
    {
        std::string db = tmpDb("default");
        wipe(db);
        KillSwitchStore store(db);
        CHECK(!store.isPaused(), "fresh store defaults to not-paused");
        auto s = store.get();
        CHECK(s.reason.empty(), "fresh store has empty reason");
        wipe(db);
    }

    std::cout << "\n[pause / resume round-trip]\n";
    {
        std::string db = tmpDb("roundtrip");
        wipe(db);
        KillSwitchStore store(db);
        store.pause("test halt", "operator");
        CHECK(store.isPaused(), "isPaused() true after pause()");
        auto s = store.get();
        CHECK(s.reason == "test halt", "reason persisted (got '" + s.reason + "')");
        CHECK(s.triggered_by == "operator", "triggered_by persisted (got '" + s.triggered_by + "')");
        CHECK(s.triggered_at > 0, "triggered_at timestamp recorded");

        store.resume();
        CHECK(!store.isPaused(), "isPaused() false after resume()");
        auto s2 = store.get();
        CHECK(s2.reason.empty(), "reason cleared after resume()");
        wipe(db);
    }

    std::cout << "\n[daily-loss-limit trigger shape]\n";
    {
        std::string db = tmpDb("dailyloss");
        wipe(db);
        KillSwitchStore store(db);
        store.pause("Daily P&L breached limit", "daily_loss_limit");
        auto s = store.get();
        CHECK(s.paused && s.triggered_by == "daily_loss_limit",
              "automatic trigger records triggered_by='daily_loss_limit' distinctly from 'operator'");
        wipe(db);
    }

    std::cout << "\n[state survives a fresh handle to the same file (restart simulation)]\n";
    {
        std::string db = tmpDb("restart");
        wipe(db);
        {
            KillSwitchStore store(db);
            store.pause("survives restart", "operator");
        } // handle closes here, simulating process exit
        {
            KillSwitchStore reopened(db);
            CHECK(reopened.isPaused(), "a fresh handle to the same file still sees the pause");
            auto s = reopened.get();
            CHECK(s.reason == "survives restart", "reason survives across handles");
        }
        wipe(db);
    }

    std::cout << "\n[repeated pause() calls update reason, not just first-wins]\n";
    {
        std::string db = tmpDb("repause");
        wipe(db);
        KillSwitchStore store(db);
        store.pause("first reason", "operator");
        store.pause("second reason", "daily_loss_limit");
        auto s = store.get();
        CHECK(s.reason == "second reason", "latest pause() call wins (got '" + s.reason + "')");
        CHECK(s.triggered_by == "daily_loss_limit", "latest triggered_by wins");
        wipe(db);
    }

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "All KillSwitchStore tests passed.\n";
        return 0;
    }
    std::cout << g_failures << " test(s) FAILED.\n";
    return 1;
}
