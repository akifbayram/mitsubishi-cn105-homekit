#pragma once
#include <cstdint>
#include <cstddef>

typedef int uart_port_t;
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2

typedef int uart_word_length_t;
typedef int uart_parity_t;

#define UART_DATA_8_BITS     0x03
#define UART_PARITY_EVEN     0x02
#define UART_STOP_BITS_1     0x01
#define UART_HW_FLOWCTRL_DISABLE 0x00
#define UART_PIN_NO_CHANGE   (-1)
#define UART_SCLK_DEFAULT    0
#define UART_SCLK_XTAL       1

typedef struct {
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    int source_clk;
} uart_config_t;

#define pdMS_TO_TICKS(ms) (ms)

// Stub function declarations — never called (UartInterface bypasses them)
inline int uart_param_config(uart_port_t, const uart_config_t*) { return 0; }
inline int uart_set_pin(uart_port_t, int, int, int, int) { return 0; }
inline int uart_driver_install(uart_port_t, int, int, int, void*, int) { return 0; }
inline int uart_get_baudrate(uart_port_t, uint32_t* out) { *out = 2400; return 0; }
inline int uart_get_word_length(uart_port_t, uart_word_length_t* out) { *out = 0; return 0; }
inline int uart_get_parity(uart_port_t, uart_parity_t* out) { *out = 0; return 0; }
inline int uart_flush_input(uart_port_t) { return 0; }
inline int uart_write_bytes(uart_port_t, const void*, size_t) { return 0; }
inline int uart_wait_tx_done(uart_port_t, int) { return 0; }
inline int uart_read_bytes(uart_port_t, void*, size_t, int) { return 0; }
inline int uart_get_buffered_data_len(uart_port_t, size_t* out) { *out = 0; return 0; }
