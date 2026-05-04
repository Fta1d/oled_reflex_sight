#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// Hardware dimensions — shared with ui layer
#define OLED_WIDTH  128
#define OLED_HEIGHT 64

/**
 * Initialize I2C bus, panel IO, and SSD1306 panel.
 * Both output handles are required by the UI layer.
 */
esp_err_t display_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out);

/**
 * Set SSD1306 contrast (brightness). 0 = minimum, 255 = maximum.
 * Default from datasheet reset value is 0x7F.
 */
void display_set_contrast(uint8_t level);

#endif // DISPLAY_H