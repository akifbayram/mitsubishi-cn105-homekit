#pragma once
#include "uart_interface.h"
#include <vector>
#include <cstring>
#include <algorithm>

class MockUart : public UartInterface {
public:
    void feedBytes(const uint8_t *data, size_t len) {
        _rxQueue.insert(_rxQueue.end(), data, data + len);
    }

    const std::vector<uint8_t>& getTxLog() const { return _txLog; }
    void clearTxLog() { _txLog.clear(); }

    int read(uint8_t *buf, size_t len) override {
        size_t toRead = std::min(len, _rxQueue.size());
        if (toRead == 0) return 0;
        memcpy(buf, _rxQueue.data(), toRead);
        _rxQueue.erase(_rxQueue.begin(), _rxQueue.begin() + toRead);
        return (int)toRead;
    }

    void write(const uint8_t *buf, size_t len) override {
        _txLog.insert(_txLog.end(), buf, buf + len);
    }

    size_t available() override {
        return _rxQueue.size();
    }

    void flush() override {
        _rxQueue.clear();
    }

private:
    std::vector<uint8_t> _rxQueue;
    std::vector<uint8_t> _txLog;
};
