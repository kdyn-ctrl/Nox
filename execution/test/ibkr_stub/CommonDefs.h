// execution/test/ibkr_stub/ — HAND-WRITTEN STAND-INS for the real Interactive
// Brokers TWS API C++ client (vendored separately per IBKR_MIGRATION.md /
// execution/setup_ibkr_vendor.sh, not committed to this repo). These exist
// SOLELY so IBKRClient.hpp/.cpp and IBKROrderRouter.hpp can be compiled and
// unit-tested for contract/order/combo-leg SHAPE without a real IB Gateway
// or the actual vendor source. EClientSocket's reqContractDetails/placeOrder
// here are no-ops — they never invoke IBKRWrapper's callbacks, so anything
// that depends on a real broker round-trip (a live fill, a real conId) is
// explicitly NOT covered by tests built against this directory. Do not add
// -Iexecution/test/ibkr_stub to the production build.
#pragma once
typedef long OrderId;
typedef long TickerId;
typedef int TickType;
