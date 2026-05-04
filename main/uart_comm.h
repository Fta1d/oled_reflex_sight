#ifndef UART_COMM_H
#define UART_COMM_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stddef.h>

/** Install USB Serial/JTAG driver and start the RX/framing task.
 *  Console must be on UART0 (CONFIG_ESP_CONSOLE_UART_DEFAULT=y), not USB. */
esp_err_t uart_comm_init(void);

/** Send raw bytes over UART0. Safe to call from any task. */
void uart_comm_send(const uint8_t *data, size_t len);

/** Returns the queue of complete frame_t payloads (FRAME_SIZE bytes each).
 *  The parser task should block on this queue. */
QueueHandle_t uart_comm_get_frame_queue(void);

#endif // UART_COMM_H
