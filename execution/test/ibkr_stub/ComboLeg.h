#pragma once
#include <memory>
#include <string>
struct ComboLeg {
    int conId = 0;
    int ratio = 0;
    std::string action;
    std::string exchange;
    int openClose = 0;
    int shortSaleSlot = 0;
    std::string designatedLocation;
    int exemptCode = -1;
};
typedef std::shared_ptr<ComboLeg> ComboLegSPtr;
typedef std::vector<ComboLegSPtr> ComboLegList;
typedef std::shared_ptr<ComboLegList> ComboLegListSPtr;
