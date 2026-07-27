#pragma once
#include <string>
struct Order {
    OrderId orderId = 0;
    int clientId = 0;
    int permId = 0;
    std::string action;
    double totalQuantity = 0.0;
    std::string orderType;
    double lmtPrice = 0.0;
    double auxPrice = 0.0;
    std::string tif;
    bool transmit = true;
    OrderId parentId = 0;
};
