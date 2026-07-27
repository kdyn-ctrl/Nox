#pragma once
#include <string>
#include "CommonDefs.h"
#include "Contract.h"
#include "ContractDetails.h"
#include "Order.h"
#include "OrderState.h"
#include "Execution.h"
#include "TickAttrib.h"
class EWrapper {
public:
    virtual ~EWrapper() {}
    virtual void nextValidId(OrderId orderId) = 0;
    virtual void orderStatus(OrderId orderId, const std::string& status,
                             double filled, double remaining, double avgFillPrice,
                             int permId, int parentId, double lastFillPrice,
                             int clientId, const std::string& whyHeld,
                             double mktCapPrice) = 0;
    virtual void openOrder(OrderId orderId, const Contract& contract,
                           const Order& order, const OrderState& state) = 0;
    virtual void execDetails(int reqId, const Contract& contract,
                             const Execution& execution) = 0;
    virtual void error(int id, int errorCode, const std::string& errorString) = 0;
    virtual void tickPrice(TickerId tickerId, TickType field, double price,
                           const TickAttrib& attribs) = 0;
    virtual void tickSize(TickerId tickerId, TickType field, int size) = 0;
    virtual void contractDetails(int reqId, const ContractDetails& details) = 0;
    virtual void contractDetailsEnd(int reqId) = 0;
};
