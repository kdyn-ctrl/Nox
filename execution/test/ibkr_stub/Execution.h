#pragma once
#include <string>
struct Execution {
    OrderId orderId = 0;
    std::string execId;
    std::string side;
    double shares = 0.0;
    double price = 0.0;
    std::string time;
};
