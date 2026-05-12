#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"
#include "lvgl.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 64

esp_err_t display_init(void);
void display_flush(const lv_area_t *area, uint8_t *px_map);
void display_set_contrast(uint8_t level);
void display_on(void);

#endif // DISPLAY_H
