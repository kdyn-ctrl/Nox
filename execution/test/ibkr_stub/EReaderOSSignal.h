#pragma once
class EReaderOSSignal {
public:
    explicit EReaderOSSignal(int waitTimeoutMs) : timeout_(waitTimeoutMs) {}
    void waitForSignal() {}
    void issueSignal() {}
private:
    int timeout_;
};
