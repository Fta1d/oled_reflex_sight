#include "display.h"
#include "ui.h"
#include "uart_comm.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void app_main(void)
{
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;

    ESP_ERROR_CHECK(display_init(&io, &panel));
    display_set_contrast(0xFF);
    ESP_ERROR_CHECK(ui_init(io, panel));
    ESP_ERROR_CHECK(uart_comm_init());

    ui_lock();
    ui_build_reticle();
    ui_unlock();
    // ui_demo_start();

    QueueHandle_t q = uart_comm_get_frame_queue();
    frame_t f;
    uint32_t rx_count = 0;

    while (1) {
        if (!xQueueReceive(q, &f, portMAX_DELAY)) continue;

        rx_count++;
        ui_lock();
        ui_debug_frame_count(rx_count);
        switch (f.type) {
            case FRAME_TYPE_TARGET:    ui_show_target(&f);                    break;
            case FRAME_TYPE_LOST:      ui_clear_target();                     break;
            case FRAME_TYPE_CROSSHAIR: ui_set_crosshair(f.bbox_x, f.bbox_y); break;
            case FRAME_TYPE_ARROW:     ui_show_arrow(f.bbox_x, f.bbox_y);    break;
            default: break;
        }
        ui_unlock();
        // ui_notify_frame();
        uart_comm_send((uint8_t[]){0xAC}, 1);
    }
}
