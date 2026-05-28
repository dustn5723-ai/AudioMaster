#ifndef UART_COMM_H
#define UART_COMM_H

#include <stdbool.h>
#include <stdint.h>

void send_uart_message(
    bool trigger,
    float rms_db,
    float drms_db,
    int16_t peak
);

#endif