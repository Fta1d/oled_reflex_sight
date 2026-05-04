#include "display.h"

#include "driver/i2c_master.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_check.h"
#include "esp_log.h"

#define PIN_SDA         GPIO_NUM_8
#define PIN_SCL         GPIO_NUM_9
#define OLED_ADDR       0x3C

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;

esp_err_t display_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out) {
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

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = OLED_ADDR,
        .scl_speed_hz        = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 6,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, io_out), TAG, "Panel IO init failed");

    esp_lcd_panel_ssd1306_config_t ssd1306_cfg = { .height = OLED_HEIGHT };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config  = &ssd1306_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(*io_out, &panel_cfg, panel_out), TAG, "SSD1306 init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel_out),               TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel_out),                TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(*panel_out, false, true), TAG, "Panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel_out, true),   TAG, "Display on failed");

    s_io = *io_out;
    ESP_LOGI(TAG, "SSD1306 ready");
    return ESP_OK;
}

void display_set_contrast(uint8_t level) {
    esp_lcd_panel_io_tx_param(s_io, 0x81, &level, 1);
}
