#!/bin/bash

# RULE-D10 / audit §5 H3: CI must run EVERY suite. Previously `set -e` plus a
# known test_regime failure aborted the run before ~20 later C++ tests, so a green
# exit proved almost nothing. We now collect per-suite failures and continue,
# exiting non-zero at the end if any failed. A failed g++ build leaves its binary
# missing/stale; the matching run_test then reports exit 127 rather than silently
# skipping. pipefail (not `set -e`) so build errors surface without aborting.
set -o pipefail

FAILED_TESTS=()
run_test() {
  local name="$1"; shift
  echo "🧪 Running $name..."
  if "$@"; then
    echo ""
  else
    local rc=$?
    echo "❌ $name FAILED (exit $rc)"
    FAILED_TESTS+=("$name")
    echo ""
  fi
}

echo "🏗️  Building test executables..."
mkdir -p build

# Compile tests
g++ -std=c++17 -pthread -o build/test_regime tests/test_regime.cpp
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
g++ -std=c++17 -pthread -o build/test_mcpt_example src/utils/mcpt/mcpt_example.cpp src/utils/mcpt/mcpt.cpp
g++ -std=c++17 -pthread -o build/mcpt_main src/utils/mcpt/main.cpp src/utils/mcpt/mcpt.cpp

# Phase 1 ghost-fill / defensive-infrastructure tests (needs OpenSSL + SQLite).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_ghost_fill.cpp execution/PositionManager.cpp \
    -o build/test_ghost_fill -lssl -lcrypto -lpthread -lsqlite3

# Phase 2, item A: signal-regeneration audit tests (needs OpenSSL + SQLite).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_signal_regeneration.cpp execution/PositionManager.cpp \
    -o build/test_signal_regeneration -lssl -lcrypto -lpthread -lsqlite3

# Phase 2, item B: live P&L / daily_ledger tests (needs OpenSSL + SQLite).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_daily_ledger.cpp execution/PositionManager.cpp \
    -o build/test_daily_ledger -lssl -lcrypto -lpthread -lsqlite3

# Phase 2, item C: historical IV rank store tests (needs SQLite only).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_iv_rank_store.cpp \
    -o build/test_iv_rank_store -lpthread -lsqlite3

# Phase 3: alpha-decay tier-down store tests (needs SQLite only).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_alpha_decay_store.cpp \
    -o build/test_alpha_decay_store -lpthread -lsqlite3

# Global kill switch persistence tests (needs SQLite only).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_kill_switch_store.cpp \
    -o build/test_kill_switch_store -lpthread -lsqlite3

# Phase 3: historical multi-day signal-regeneration replay tests (needs OpenSSL + SQLite).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_historical_regeneration.cpp execution/PositionManager.cpp \
    -o build/test_historical_regeneration -lssl -lcrypto -lpthread -lsqlite3

# Phase 3: IBKR combo/BAG order-construction tests — built against hand-written
# stub TWS headers (execution/test/ibkr_stub/), NOT the real vendored TWS API,
# so this validates contract/order shape only (see that directory's
# CommonDefs.h for what is and isn't covered).
g++ -std=c++17 -pthread -Iexecution -Iexecution/test/ibkr_stub \
    execution/test/test_ibkr_combo_router.cpp execution/IBKRClient.cpp \
    -o build/test_ibkr_combo_router -lsqlite3

# Phase 4, item 2: portfolio circuit breaker aggregation/breach math — pure
# logic, no I/O, so no SQLite/OpenSSL needed here.
g++ -std=c++17 -O2 -I. \
    execution/test/test_portfolio_risk_manager.cpp \
    -o build/test_portfolio_risk_manager -lpthread

# July 10: REVERSE_IRON_CONDOR strategy-selection reachability tests.
g++ -std=c++17 -pthread \
    execution/test/test_strategy_selection.cpp \
    -o build/test_strategy_selection -lpthread

# July 10: DTE-floor bypass fix + earnings fail-closed tests.
g++ -std=c++17 -pthread \
    execution/test/test_dte_earnings_fix.cpp \
    -o build/test_dte_earnings_fix -lpthread

# July 10: options_signals full-detail signal storage tests (needs SQLite only).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_options_signal_storage.cpp \
    -o build/test_options_signal_storage -lpthread -lsqlite3

# July 10: passive post-earnings-drift research trail (needs SQLite only).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_earnings_drift.cpp \
    -o build/test_earnings_drift -lpthread -lsqlite3

# Engine-wide prediction-quality logging (predictions_log write path — needs
# SQLite only).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_predictions_log.cpp \
    -o build/test_predictions_log -lpthread -lsqlite3

# Futures signal generator — phase 1, signals only, no order routing (needs
# OpenSSL for httplib's TU even though the test never makes a network call).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_futures_signal_generator.cpp \
    -o build/test_futures_signal_generator -lssl -lcrypto -lpthread -lsqlite3

# Quality-driven DTE selection + DTE/quality-tiered sizing (needs SQLite for
# AlphaDecayStore, exercised indirectly via assembleSignal()).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_quality_dte_sizing.cpp \
    -o build/test_quality_dte_sizing -lpthread -lsqlite3

# Sector/trend macro gate — pure EMA math + conflict-decision logic, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_sector_trend_gate.cpp \
    -o build/test_sector_trend_gate

# quality_score clamp — pure math, no I/O (httplib TU still needs OpenSSL to link).
g++ -std=c++17 -pthread \
    execution/test/test_quality_score_clamp.cpp \
    -o build/test_quality_score_clamp -lssl -lcrypto

# Skeptic intelligence decision layer — wires WS2 (alt-macro) + WS3 (insider) +
# China information-lag into sizing/gating. Pure aggregation math, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_skeptic_intelligence.cpp \
    -o build/test_skeptic_intelligence

# Backtester adverse-selection fill model — pure math, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_backtest_fill_model.cpp \
    -o build/test_backtest_fill_model

# Backtester error-injection resilience model — pure deterministic math, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_backtest_error_injection.cpp \
    -o build/test_backtest_error_injection

# Backtester fractional-Kelly sizing sweep — pure math, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_backtest_kelly_sizing.cpp \
    -o build/test_backtest_kelly_sizing

# Backtester walk-forward fold generation — pure math, no I/O.
g++ -std=c++17 -pthread \
    execution/test/test_backtest_walk_forward.cpp \
    -o build/test_backtest_walk_forward

# Track 2 §2 C1/C2/C3: sig.contracts wiring, zero-contract floor, short-strangle
# pricing/sizing — exercises assembleSignal() directly (needs SQLite for
# AlphaDecayStore, same as test_quality_dte_sizing).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_options_sizing_zero_and_strangle.cpp \
    -o build/test_options_sizing_zero_and_strangle -lpthread -lsqlite3

# Track 2 §2 C3: STRANGLE routes as a real short (both legs sell), plus the
# naked-options approval pre-flight check (needs OpenSSL + SQLite for httplib).
g++ -std=c++17 -O2 -I. -Ishared \
    execution/test/test_strangle_short_routing.cpp \
    -o build/test_strangle_short_routing -lssl -lcrypto -lpthread -lsqlite3

echo ""
echo "✅ Build complete!"
echo ""

# Run tests — every suite runs; failures are collected, not fatal (see run_test).
run_test "RegimeStateMachine tests" build/test_regime
run_test "Kelly sizing tests" build/test_kelly_sizing
run_test "MCPT example" build/test_mcpt_example
run_test "main MCPT demo" build/mcpt_main
run_test "Phase 1 ghost-fill / defensive-infrastructure tests" build/test_ghost_fill
run_test "Phase 2 signal-regeneration audit tests" build/test_signal_regeneration
run_test "Phase 2 daily_ledger (live P&L) tests" build/test_daily_ledger
run_test "Phase 2 IvRankStore (historical IV rank) tests" build/test_iv_rank_store
run_test "Phase 3 AlphaDecayStore (tier-down multiplier) tests" build/test_alpha_decay_store
run_test "global kill switch persistence tests" build/test_kill_switch_store
run_test "Phase 3 historical signal-regeneration replay tests" build/test_historical_regeneration
run_test "Phase 3 IBKR combo/BAG order-construction tests (stub headers)" build/test_ibkr_combo_router
run_test "Phase 4 PortfolioRiskManager (circuit breaker) tests" build/test_portfolio_risk_manager
run_test "REVERSE_IRON_CONDOR strategy-selection tests" build/test_strategy_selection
run_test "DTE-floor bypass + earnings fail-closed tests" build/test_dte_earnings_fix
run_test "options_signals storage tests" build/test_options_signal_storage
run_test "post-earnings-drift research trail tests" build/test_earnings_drift
run_test "predictions_log (engine-wide prediction-quality) tests" build/test_predictions_log
run_test "futures signal generator (signals-only) tests" build/test_futures_signal_generator
run_test "quality-driven DTE + sizing tests" build/test_quality_dte_sizing
run_test "sector/trend gate tests" build/test_sector_trend_gate
run_test "quality-score clamp tests" build/test_quality_score_clamp
run_test "Skeptic intelligence decision-layer tests" build/test_skeptic_intelligence
run_test "backtester adverse-selection fill model tests" build/test_backtest_fill_model
run_test "options sizing wiring / zero-contract floor / short-strangle math tests" build/test_options_sizing_zero_and_strangle
run_test "short-strangle routing / naked-options approval tests" build/test_strangle_short_routing
run_test "backtester error-injection resilience tests" build/test_backtest_error_injection
run_test "backtester fractional-Kelly sizing sweep tests" build/test_backtest_kelly_sizing
run_test "backtester walk-forward fold generation tests" build/test_backtest_walk_forward

echo ""
if [ ${#FAILED_TESTS[@]} -ne 0 ]; then
  echo "❌ ${#FAILED_TESTS[@]} test suite(s) FAILED:"
  for t in "${FAILED_TESTS[@]}"; do echo "   - $t"; done
  exit 1
fi
echo "✨ All tests passed successfully!"
