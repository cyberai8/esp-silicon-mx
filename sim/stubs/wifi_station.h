#pragma once

class WifiStation {
public:
    static WifiStation& GetInstance() {
        static WifiStation inst;
        return inst;
    }
    bool IsConnected() const { return true; }
};
