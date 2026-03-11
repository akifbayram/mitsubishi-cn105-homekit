#pragma once
#include <cstdint>
#include <cstddef>

/// Abstract UART interface for dependency injection.
/// Production uses HardwareUart (ESP-IDF); tests use MockUart.
class UartInterface {
public:
    virtual ~UartInterface() = default;

    /// Non-blocking read: returns number of bytes read (0 if none available)
    virtual int read(uint8_t *buf, size_t len) = 0;

    /// Write bytes (blocks until transmitted)
    virtual void write(const uint8_t *buf, size_t len) = 0;

    /// Returns number of bytes available to read
    virtual size_t available() = 0;

    /// Discard all buffered input
    virtual void flush() = 0;
};
