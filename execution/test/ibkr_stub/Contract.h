#pragma once
#include <string>
#include <memory>
#include <vector>
#include "ComboLeg.h"
struct Contract {
    int conId = 0;
    std::string symbol;
    std::string secType;
    std::string lastTradeDateOrContractMonth;
    double strike = 0.0;
    std::string right;
    std::string multiplier;
    std::string exchange;
    std::string primaryExchange;
    std::string currency;
    std::string localSymbol;
    std::string tradingClass;
    ComboLegListSPtr comboLegs;
};
