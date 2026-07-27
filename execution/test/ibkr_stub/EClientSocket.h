#pragma once
#include <string>
#include <vector>
#include "EWrapper.h"
#include "EReaderOSSignal.h"
#include "Contract.h"
#include "Order.h"

// Stub reqContractDetails responds SYNCHRONOUSLY and deterministically (no
// real socket/thread involved in this test double) so tests can assert on
// exactly which conId got assigned to which leg: conId = strike*10 + (1 for
// call, 2 for put). placeOrder records every (Contract, Order) pair it was
// given so a test can inspect the final combo/BAG shape that would have gone
// over the wire.
class EClientSocket {
public:
    EClientSocket(EWrapper* wrapper, EReaderOSSignal* signal) : wrapper_(wrapper), signal_(signal) {}
    virtual ~EClientSocket() {}
    bool eConnect(const char*, int, int, bool) { return true; }
    void eDisconnect() {}
    bool isConnected() const { return connected_; }

    void reqContractDetails(int reqId, const Contract& contract) {
        ContractDetails details;
        details.contract = contract;
        details.contract.conId = static_cast<int>(contract.strike * 10) +
                                 (contract.right == "C" ? 1 : 2);
        wrapper_->contractDetails(reqId, details);
        wrapper_->contractDetailsEnd(reqId);
    }

    void placeOrder(OrderId oid, const Contract& contract, const Order& order) {
        placed_orders_.push_back({oid, contract, order});
    }

    struct PlacedOrder { OrderId oid; Contract contract; Order order; };
    static std::vector<PlacedOrder> placed_orders_;

protected:
    EWrapper* wrapper_;
    EReaderOSSignal* signal_;
    bool connected_ = true;
};

inline std::vector<EClientSocket::PlacedOrder> EClientSocket::placed_orders_;
