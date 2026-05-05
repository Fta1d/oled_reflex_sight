#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

// Hardware dimensions — shared with ui layer
#define OLED_WIDTH  128
#define OLED_HEIGHT 64

/**
 * Initialize I2C bus, panel IO, and SSD1306 panel.
 * Both output handles are required by the UI layer.
 */
esp_err_t display_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out);

/**
 * Convert LVGL I1 framebuffer to SSD1306 page format and push to panel.
 * Called from the LVGL flush callback in ui.c.
 */
void display_flush(esp_lcd_panel_handle_t panel, const lv_area_t *area, uint8_t *px_map);

/**
 * Set SSD1306 contrast (brightness). 0 = minimum, 255 = maximum.
 * Default from datasheet reset value is 0x7F.
 */
void display_set_contrast(uint8_t level);

// Turn the display on. Call this after LVGL has completed its first flush
// so the display becomes visible with clean content rather than GDDRAM noise.
void display_on(void);

#endif // DISPLAY_H