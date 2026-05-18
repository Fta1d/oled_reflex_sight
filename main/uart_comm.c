#include "uart_comm.h"
#include "frame.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define USB_RX_BUF          1024
#define USB_TX_BUF          1024
#define FRAME_QUEUE_LEN     2
#define UART_RX_TASK_STACK  2048
#define UART_RX_TASK_PRIO   5

static const char *TAG = "uart_comm";

static QueueHandle_t s_frame_queue;

// Sync state machine — scans the byte stream for SYNC0/SYNC1 then reads
// the remaining (FRAME_SIZE - 2) bytes to complete the frame.
static void uart_rx_task(void *arg) {
    uint8_t raw[FRAME_SIZE];
    uint8_t byte;
    int     filled = 0;

    while (1) {
        // 1-second timeout: send heartbeat so PC can verify TX works
        if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(1000)) == 0) {
            usb_serial_jtag_write_bytes("\xBE", 1, 0);
            continue;
        }

        if (filled == 0) {
            if (byte == FRAME_SYNC0) raw[filled++] = byte;
        } else if (filled == 1) {
            if (byte == FRAME_SYNC1) raw[filled++] = byte;
            else filled = 0;
        } else {
            raw[filled++] = byte;
            if (filled == FRAME_SIZE) {
                xQueueSend(s_frame_queue, raw, 0); // non-blocking; drop if parser is behind
                filled = 0;
            }
        }
    }
}

esp_err_t uart_comm_init(void) {
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = USB_RX_BUF,
        .tx_buffer_size = USB_TX_BUF,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));

    s_frame_queue = xQueueCreate(FRAME_QUEUE_LEN, FRAME_SIZE);
    assert(s_frame_queue);

    xTaskCreate(uart_rx_task, "uart_rx", UART_RX_TASK_STACK, NULL, UART_RX_TASK_PRIO, NULL);

    ESP_LOGI(TAG, "USB Serial/JTAG ready");
    return ESP_OK;
}

void uart_comm_send(const uint8_t *data, size_t len) {
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
}

QueueHandle_t uart_comm_get_frame_queue(void) {
    return s_frame_queue;
}
