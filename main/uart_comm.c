#include "uart_comm.h"
#include "frame.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// Set to 1 for hardware UART pins, 0 for USB Serial/JTAG
#define USE_PIN_UART  1

#if USE_PIN_UART
#include "driver/uart.h"
#include "driver/gpio.h"
#define UART_PORT           UART_NUM_1
#define UART_BAUD           115200
#define PIN_TX              GPIO_NUM_21
#define PIN_RX              GPIO_NUM_20
#define UART_RX_BUF         1024
#define UART_TX_BUF         256
#else
#include "driver/usb_serial_jtag.h"
#define USB_RX_BUF          1024
#define USB_TX_BUF          256
#endif

#define FRAME_QUEUE_LEN     2
#define UART_RX_TASK_STACK  2048
#define UART_RX_TASK_PRIO   5

static const char *TAG = "uart_comm";
static QueueHandle_t s_frame_queue;

static void uart_rx_task(void *arg) {
    uint8_t raw[FRAME_SIZE];
    uint8_t byte;
    int     filled = 0;

    while (1) {
#if USE_PIN_UART
        int rx = uart_read_bytes(UART_PORT, &byte, 1, pdMS_TO_TICKS(1000));
#else
        int rx = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(1000));
#endif
        if (rx == 0) {
#if USE_PIN_UART
            uart_write_bytes(UART_PORT, "\xBE", 1);
#else
            usb_serial_jtag_write_bytes("\xBE", 1, 0);
#endif
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
                xQueueSend(s_frame_queue, raw, 0);
                filled = 0;
            }
        }
    }
}

esp_err_t uart_comm_init(void) {
#if USE_PIN_UART
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0));
    ESP_LOGI(TAG, "UART%d ready — TX GPIO%d  RX GPIO%d  %dbaud",
             UART_PORT, PIN_TX, PIN_RX, UART_BAUD);
#else
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = USB_RX_BUF,
        .tx_buffer_size = USB_TX_BUF,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    ESP_LOGI(TAG, "USB Serial/JTAG ready");
#endif

    s_frame_queue = xQueueCreate(FRAME_QUEUE_LEN, FRAME_SIZE);
    assert(s_frame_queue);
    xTaskCreate(uart_rx_task, "uart_rx", UART_RX_TASK_STACK, NULL, UART_RX_TASK_PRIO, NULL);
    return ESP_OK;
}

void uart_comm_send(const uint8_t *data, size_t len) {
#if USE_PIN_UART
    uart_write_bytes(UART_PORT, data, len);
#else
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
#endif
}

QueueHandle_t uart_comm_get_frame_queue(void) {
    return s_frame_queue;
}
