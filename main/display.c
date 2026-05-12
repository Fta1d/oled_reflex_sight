#include "display.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---- Pin assignment ----------------------------------------------------------
// Adjust these to match your wiring.
#define PIN_SCLK    GPIO_NUM_4
#define PIN_MOSI    GPIO_NUM_6
#define PIN_CS      GPIO_NUM_7
#define PIN_DC      GPIO_NUM_3
#define PIN_RST     GPIO_NUM_10  // set to -1 if RST not wired

#define SPI_CLK_HZ      8000000
#define LVGL_PALETTE_BYTES  8   // I1 format: 2 palette entries × 4 bytes each

static const char *TAG = "display";

static spi_device_handle_t s_spi;
// Page-format framebuffer in DMA-capable SRAM (required for SPI transfers > 64 bytes).
static uint8_t *s_buf;

// Pre-transfer callback: drive DC low for commands, high for data.
static void IRAM_ATTR pre_transfer_cb(spi_transaction_t *t)
{
    gpio_set_level(PIN_DC, (int)(uintptr_t)t->user);
}

static void ssd1306_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length  = 8,
        .tx_data = { cmd },
        .flags   = SPI_TRANS_USE_TXDATA,
        .user    = (void *)0,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void ssd1306_data(const uint8_t *data, size_t len)
{
    if (!len) return;
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
        .user      = (void *)1,
    };
    spi_device_polling_transmit(s_spi, &t);
}

esp_err_t display_init(void)
{
    // Configure DC and (optionally) RST as outputs.
    uint64_t pin_mask = (1ULL << PIN_DC);
    if (PIN_RST >= 0) pin_mask |= (1ULL << PIN_RST);
    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "GPIO config failed");

    if (PIN_RST >= 0) {
        gpio_set_level(PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = OLED_WIDTH * OLED_HEIGHT / 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SPI_CLK_HZ,
        .mode           = 0,
        .spics_io_num   = PIN_CS,
        .queue_size     = 4,
        .pre_cb         = pre_transfer_cb,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi),
                        TAG, "SPI device add failed");

    s_buf = heap_caps_calloc(1, OLED_WIDTH * OLED_HEIGHT / 8,
                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf) return ESP_ERR_NO_MEM;

    // SSD1306 initialisation sequence (display stays off until display_on()).
    static const uint8_t init_seq[] = {
        0xAE,               // display off
        0xD5, 0x80,         // clock: divide=1, osc-freq=8
        0xA8, 0x3F,         // multiplex ratio: 64 rows
        0xD3, 0x00,         // display offset: 0
        0x40,               // start line: 0
        0x8D, 0x14,         // charge pump enable
        0xA0,               // segment remap: normal (no X-flip)
        0xC8,               // COM scan: reversed (Y-flip for correct orientation)
        0xDA, 0x12,         // COM pins: alternative configuration
        0x81, 0x7F,         // contrast: 127
        0xD9, 0x22,         // pre-charge period
        0xDB, 0x40,         // VCOMH deselect level
        0xA4,               // entire display: follow GDDRAM
        0xA6,               // normal polarity (1 = pixel on)
    };
    for (size_t i = 0; i < sizeof(init_seq); i++) ssd1306_cmd(init_seq[i]);

    ESP_LOGI(TAG, "SSD1306 ready");
    return ESP_OK;
}

void display_on(void)
{
    ssd1306_cmd(0xAF);
}

void display_set_contrast(uint8_t level)
{
    ssd1306_cmd(0x81);
    ssd1306_cmd(level);
}

// Convert LVGL I1 framebuffer (row-major, MSB-left) to SSD1306 page format
// (page-major, column-per-byte, bit-0 = top row of page) and push to display.
void display_flush(const lv_area_t *area, uint8_t *px_map)
{
    px_map += LVGL_PALETTE_BYTES;

    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            bool lit = px_map[(OLED_WIDTH >> 3) * y + (x >> 3)] & (1 << (7 - (x & 7)));
            uint8_t *p = s_buf + OLED_WIDTH * (y >> 3) + x;
            if (lit) *p |=  (1u << (y & 7));
            else     *p &= ~(1u << (y & 7));
        }
    }

    // Horizontal addressing mode: one contiguous 1024-byte transfer.
    ssd1306_cmd(0x20); ssd1306_cmd(0x00);              // horizontal addressing
    ssd1306_cmd(0x21); ssd1306_cmd(0x00); ssd1306_cmd(0x7F);  // cols 0-127
    ssd1306_cmd(0x22); ssd1306_cmd(0x00); ssd1306_cmd(0x07);  // pages 0-7
    ssd1306_data(s_buf, OLED_WIDTH * OLED_HEIGHT / 8);
}
