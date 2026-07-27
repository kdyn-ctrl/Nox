#pragma once
#include "EWrapper.h"
class DefaultEWrapper : public EWrapper {
public:
    void nextValidId(OrderId) override {}
    void orderStatus(OrderId, const std::string&, double, double, double,
                     int, int, double, int, const std::string&, double) override {}
    void openOrder(OrderId, const Contract&, const Order&, const OrderState&) override {}
    void execDetails(int, const Contract&, const Execution&) override {}
    void error(int, int, const std::string&) override {}
    void tickPrice(TickerId, TickType, double, const TickAttrib&) override {}
    void tickSize(TickerId, TickType, int) override {}
    void contractDetails(int, const ContractDetails&) override {}
    void contractDetailsEnd(int) override {}
};
