#ifndef UI_H
#define UI_H

#include "esp_err.h"
#include "frame.h"

/**
 * Initialize LVGL, create display, start tick timer and LVGL task.
 * Must be called after display_init().
 */
esp_err_t ui_init(void);

/**
 * Acquire/release the LVGL mutex before making any lv_* calls from outside
 * the LVGL task (e.g. from the UART RX callback or another task).
 */
void ui_lock(void);
void ui_unlock(void);
void ui_notify_frame(void);  // wake LVGL task immediately after widget update

/**
 * Build the holosight ui screen (label + crosshair).
 * Call between ui_lock() / ui_unlock().
 */
void ui_build_ui(void);

/**
 * Show bbox + predicted lead point for a detected target.
 * Call between ui_lock() / ui_unlock().
 */
void ui_show_target(const frame_t *f);

/**
 * Hide all target overlays (target lost).
 * Call between ui_lock() / ui_unlock().
 */
void ui_clear_target(void);

/**
 * Reposition the crosshair center. Call between ui_lock() / ui_unlock().
 * Coordinates are in OLED space (x: 0-127, y: 0-63).
 */
void ui_set_crosshair(uint8_t cx, uint8_t cy);

/**
 * Show a guidance arrow from the crosshair center to (tip_x, tip_y).
 * Indicates which direction the operator should move the barrel.
 * Call between ui_lock() / ui_unlock().
 */
void ui_show_arrow(uint8_t tip_x, uint8_t tip_y);

/**
 * Hide the guidance arrow. Call between ui_lock() / ui_unlock().
 */
void ui_clear_arrow(void);

/**
 * Show hold indicator: aim is on the predicted target position.
 * Hides the arrow and shows a filled square at the crosshair center.
 * Call between ui_lock() / ui_unlock().
 */
void ui_show_hold(void);

/**
 * Show hold border + guidance circle in one call (FRAME_TYPE_HOLD_ARROW).
 * Combines ui_show_hold() and ui_show_arrow() so both render in the same LVGL tick.
 * Call between ui_lock() / ui_unlock().
 */
void ui_show_hold_arrow(uint8_t tip_x, uint8_t tip_y);

/**
 * Resize the hold frame by setting its insets from each edge.
 * The frame stays centered; width = OLED_WIDTH - 2*inset_x, height = OLED_HEIGHT - 2*inset_y.
 * Call between ui_lock() / ui_unlock().
 */
void ui_set_hold_inset(uint8_t inset_x, uint8_t inset_y);

/**
 * Set the arrow-tip circle diameter in pixels.
 * Call between ui_lock() / ui_unlock().
 */
void ui_set_circle_d(uint8_t d);

/** Debug: show received frame count in top-left corner. Remove when done. */
void ui_debug_frame_count(uint32_t count);

/** Start a local bouncing-box demo animation (no UART needed). */
void ui_demo_start(void);

#endif // UI_H