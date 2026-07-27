#pragma once
#include "EClientSocket.h"
#include "EReaderOSSignal.h"
class EReader {
public:
    EReader(EClientSocket*, EReaderOSSignal*) {}
    void start() {}
    void processMsgs() {}
};
