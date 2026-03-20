#pragma once

#ifndef UNIT_TEST

#include "uart_interface.h"
#include <driver/uart.h>
#include <driver/gpio.h>
#include "board_profile.h"
#include "logging.h"

/// ESP-IDF UART implementation of UartInterface.
class HardwareUart : public UartInterface {
public:
    HardwareUart(uart_port_t uartNum, int rxPin, int txPin, uint32_t baudRate) : _uartNum(uartNum) {
        static const char *TAG = "uart";

        uart_config_t uart_config = {};
        uart_config.baud_rate = baudRate;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity    = UART_PARITY_EVEN;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
// ESP32-C6/C3 have UART_SCLK_XTAL for precise low-baud clocking.
// Classic ESP32 only has UART_SCLK_APB/REF_TICK; UART_SCLK_XTAL doesn't exist.
#if UART_USE_XTAL_CLK && defined(SOC_UART_SUPPORT_XTAL_CLK)
        uart_config.source_clk = UART_SCLK_XTAL;
#else
        uart_config.source_clk = UART_SCLK_DEFAULT;
#endif

        esp_err_t err;
        err = uart_param_config(_uartNum, &uart_config);
        LOG_INFO("uart_param_config: %s", esp_err_to_name(err));

        err = uart_set_pin(_uartNum, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        LOG_INFO("uart_set_pin(TX=%d, RX=%d): %s", txPin, rxPin, esp_err_to_name(err));

        err = uart_driver_install(_uartNum, 256, 256, 10, &_eventQueue, 0);
        LOG_INFO("uart_driver_install: %s", esp_err_to_name(err));

#if UART_NEEDS_RX_PULLUP
        gpio_set_pull_mode((gpio_num_t)rxPin, GPIO_PULLUP_ONLY);
#endif

        uint32_t baud;
        uart_get_baudrate(_uartNum, &baud);
        uart_word_length_t data_bits_v;
        uart_get_word_length(_uartNum, &data_bits_v);
        uart_parity_t parity_v;
        uart_get_parity(_uartNum, &parity_v);
        LOG_INFO("Verified: baud=%lu, data_bits=%d, parity=%d", baud, data_bits_v, parity_v);

        uart_flush_input(_uartNum);
    }

    int read(uint8_t *buf, size_t len) override {
        return uart_read_bytes(_uartNum, buf, len, 0);
    }

    void write(const uint8_t *buf, size_t len) override {
        uart_write_bytes(_uartNum, buf, len);
        uart_wait_tx_done(_uartNum, pdMS_TO_TICKS(200));
    }

    size_t available() override {
        size_t avail = 0;
        uart_get_buffered_data_len(_uartNum, &avail);
        return avail;
    }

    void flush() override {
        uart_flush_input(_uartNum);
    }

    bool waitForData(uint32_t timeoutMs) override {
        if (!_eventQueue) return false;
        uart_event_t event;
        if (xQueueReceive(_eventQueue, &event, pdMS_TO_TICKS(timeoutMs))) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                uart_flush_input(_uartNum);
                xQueueReset(_eventQueue);
                return false;
            }
            return (event.type == UART_DATA);
        }
        return false;
    }

private:
    uart_port_t _uartNum;
    QueueHandle_t _eventQueue = nullptr;
};

#endif // UNIT_TEST
