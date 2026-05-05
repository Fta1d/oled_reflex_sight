#include "display.h"

#include "driver/i2c_master.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_check.h"
#include "esp_log.h"
#include <string.h>

#define PIN_SDA             GPIO_NUM_8
#define PIN_SCL             GPIO_NUM_9
#define OLED_ADDR           0x3C
#define LVGL_PALETTE_BYTES  8   // I1 format: 2 palette entries × 4 bytes each

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static i2c_master_bus_handle_t   s_bus;

// Native SSD1306 page-format buffer (persists between flushes)
static uint8_t oled_buf[OLED_WIDTH * OLED_HEIGHT / 8];

// Clears all 1024 GDDRAM bytes via raw I2C, bypassing the panel component.
// esp_lcd_panel_io_tx_color does not reliably reach GDDRAM on this platform.
static void gram_clear(void)
{
    i2c_master_dev_handle_t dev;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OLED_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(s_bus, &cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "gram_clear: add device failed");
        return;
    }

    // One command transaction: horizontal addressing, col 0-127, page 0-7
    uint8_t cmds[] = {
        0x00,             // control byte: command stream (Co=0, D/C=0)
        0x20, 0x00,       // set memory addressing = horizontal
        0x21, 0x00, 0x7F, // column address 0..127
        0x22, 0x00, 0x07  // page address 0..7
    };
    i2c_master_transmit(dev, cmds, sizeof(cmds), 100);

    // Eight data transactions of 128 bytes each = 1024 bytes total
    static uint8_t page_buf[129]; // [0x40] + [0x00 × 128]
    page_buf[0] = 0x40;           // control byte: data stream (Co=0, D/C=1)
    memset(page_buf + 1, 0, 128);
    for (int p = 0; p < 8; p++) {
        i2c_master_transmit(dev, page_buf, sizeof(page_buf), 100);
    }

    i2c_master_bus_rm_device(dev);
    ESP_LOGI(TAG, "GRAM cleared");
}

esp_err_t display_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_SDA,
        .scl_io_num        = PIN_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C bus init failed");
    s_bus = bus;

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = OLED_ADDR,
        .scl_speed_hz        = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 6,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, io_out), TAG, "Panel IO init failed");
    s_io = *io_out;

    esp_lcd_panel_ssd1306_config_t ssd1306_cfg = { .height = OLED_HEIGHT };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config  = &ssd1306_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(*io_out, &panel_cfg, panel_out), TAG, "SSD1306 init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel_out),               TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel_out),                TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(*panel_out, false, true),  TAG, "Panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(*panel_out, true),   TAG, "Invert color failed");
    s_panel = *panel_out;

    gram_clear();

    // Display left OFF intentionally — call display_on() after LVGL's first flush.
    ESP_LOGI(TAG, "SSD1306 ready");
    return ESP_OK;
}

void display_on(void)
{
    esp_lcd_panel_disp_on_off(s_panel, true);
}

void display_set_contrast(uint8_t level)
{
    esp_lcd_panel_io_tx_param(s_io, 0x81, &level, 1);
}

// LVGL I1: MSB-first, row-major  (bit 7 = leftmost pixel of each byte)
// SSD1306:  page-format, col-major (bit 0 = topmost row of each page)
void display_flush(esp_lcd_panel_handle_t panel, const lv_area_t *area, uint8_t *px_map) {
    px_map += LVGL_PALETTE_BYTES;

    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            bool white = px_map[(OLED_WIDTH >> 3) * y + (x >> 3)] & (1 << (7 - x % 8));
            uint8_t *p = oled_buf + OLED_WIDTH * (y >> 3) + x;
            if (white) *p &= ~(1 << (y % 8));
            else        *p |=  (1 << (y % 8));
        }
    }
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, oled_buf);
}
