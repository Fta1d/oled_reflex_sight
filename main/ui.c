#include "ui.h"
#include "display.h"
#include "frame.h"

#include "lvgl.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart_comm.h"
#include <sys/lock.h>
#include <sys/param.h>

#define LVGL_TICK_MS        5
#define LVGL_TASK_STACK     (6 * 1024)
#define LVGL_TASK_PRIO      2
#define HOLD_INSET_X        35
#define HOLD_INSET_Y        12
#define CROSSHAIR_ARM       1
#define ARROW_TIP_D         10

static const char *TAG = "ui";

static _lock_t      s_lvgl_lock;
static TaskHandle_t s_lvgl_task_handle;
static lv_display_t *s_disp;

static lv_obj_t *s_bbox;
static lv_obj_t *s_pred_dot;
// static lv_obj_t *s_dbg_label;

static lv_obj_t *s_ch_h_line;
static lv_obj_t *s_ch_v_line;
static lv_point_precise_t s_ch_h_pts[2];
static lv_point_precise_t s_ch_v_pts[2];

static lv_obj_t *s_arrow_line;
static lv_point_precise_t s_arrow_pts[2];
static lv_obj_t *s_arrow_tip_circle;

static lv_obj_t *s_hold_frame;
static lv_obj_t *s_hold_label;

static uint8_t s_cx;
static uint8_t s_cy;
static uint8_t s_arrow_tip_x;
static uint8_t s_arrow_tip_y;

// --- LVGL flush callback -----------------------------------------------------

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    display_flush(area, px_map);
    lv_display_flush_ready(disp);
    uart_comm_send((const uint8_t*)"\xAC", 1);
}

// --- LVGL tick + task --------------------------------------------------------

static void lvgl_tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_MS); }

static void lvgl_task(void *arg) {
    while (1) {
        _lock_acquire(&s_lvgl_lock);
        uint32_t delay_ms = lv_timer_handler();
        _lock_release(&s_lvgl_lock);
        delay_ms = MAX(delay_ms, 1000 / CONFIG_FREERTOS_HZ);
        delay_ms = MIN(delay_ms, 500);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}

// --- Public API --------------------------------------------------------------

esp_err_t ui_init(void) {
    lv_init();

    s_disp = lv_display_create(OLED_WIDTH, OLED_HEIGHT);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    lv_timer_set_period(lv_display_get_refr_timer(s_disp), 5);

    size_t buf_sz = OLED_WIDTH * OLED_HEIGHT / 8 + 8;  // +8: I1 palette (2 entries × 4 bytes)
    void *lvgl_buf = heap_caps_calloc(1, buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!lvgl_buf) return ESP_ERR_NO_MEM;

    lv_display_set_buffers(s_disp, lvgl_buf, NULL, buf_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_render_mode(s_disp, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    esp_timer_handle_t tick_timer;
    const esp_timer_create_args_t tick_args = { .callback = lvgl_tick_cb, .name = "lvgl_tick" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer),                 TAG, "Tick timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000), TAG, "Tick timer start failed");

    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, &s_lvgl_task_handle);

    ESP_LOGI(TAG, "LVGL ready");
    return ESP_OK;
}

void ui_lock(void)         { _lock_acquire(&s_lvgl_lock); }
void ui_unlock(void)       { _lock_release(&s_lvgl_lock); }
void ui_notify_frame(void) {
    _lock_acquire(&s_lvgl_lock);
    lv_timer_ready(lv_display_get_refr_timer(s_disp));
    _lock_release(&s_lvgl_lock);
    xTaskNotifyGive(s_lvgl_task_handle);
}

// --- Screens -----------------------------------------------------------------

void ui_build_ui(void) {
    lv_obj_t *scr = lv_display_get_screen_active(s_disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_cx = OLED_WIDTH  / 2;
    s_cy = OLED_HEIGHT / 2;

    s_ch_h_pts[0] = (lv_point_precise_t){ s_cx - CROSSHAIR_ARM, s_cy };
    s_ch_h_pts[1] = (lv_point_precise_t){ s_cx + CROSSHAIR_ARM + 1, s_cy };
    s_ch_v_pts[0] = (lv_point_precise_t){ s_cx, s_cy - CROSSHAIR_ARM };
    s_ch_v_pts[1] = (lv_point_precise_t){ s_cx, s_cy + CROSSHAIR_ARM + 1 };

    s_ch_h_line = lv_line_create(scr);
    lv_line_set_points(s_ch_h_line, s_ch_h_pts, 2);
    lv_obj_set_style_line_color(s_ch_h_line, lv_color_white(), 0);
    lv_obj_set_style_line_width(s_ch_h_line, 1, 0);

    s_ch_v_line = lv_line_create(scr);
    lv_line_set_points(s_ch_v_line, s_ch_v_pts, 2);
    lv_obj_set_style_line_color(s_ch_v_line, lv_color_white(), 0);
    lv_obj_set_style_line_width(s_ch_v_line, 1, 0);

    // s_dbg_label = lv_label_create(scr);
    // lv_label_set_text(s_dbg_label, "rx:0");
    // lv_obj_set_style_text_color(s_dbg_label, lv_color_white(), 0);
    // lv_obj_align(s_dbg_label, LV_ALIGN_TOP_LEFT, 0, 0);
    // lv_obj_add_flag(s_dbg_label, LV_OBJ_FLAG_HIDDEN);

    s_bbox = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(s_bbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_bbox, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_bbox, 1, 0);
    lv_obj_set_style_radius(s_bbox, 0, 0);
    lv_obj_set_style_pad_all(s_bbox, 0, 0);
    lv_obj_add_flag(s_bbox, LV_OBJ_FLAG_HIDDEN);

    s_pred_dot = lv_obj_create(scr);
    lv_obj_set_size(s_pred_dot, 3, 3);
    lv_obj_set_style_bg_color(s_pred_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_pred_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pred_dot, 0, 0);
    lv_obj_set_style_radius(s_pred_dot, 0, 0);
    lv_obj_set_style_pad_all(s_pred_dot, 0, 0);
    lv_obj_add_flag(s_pred_dot, LV_OBJ_FLAG_HIDDEN);

    s_arrow_pts[0] = (lv_point_precise_t){ s_cx, s_cy };
    s_arrow_pts[1] = (lv_point_precise_t){ s_cx, s_cy };
    s_arrow_line = lv_line_create(scr);
    lv_line_set_points(s_arrow_line, s_arrow_pts, 2);
    lv_obj_set_style_line_color(s_arrow_line, lv_color_white(), 0);
    lv_obj_set_style_line_width(s_arrow_line, 2, 0);
    lv_obj_add_flag(s_arrow_line, LV_OBJ_FLAG_HIDDEN);

    s_arrow_tip_circle = lv_arc_create(scr);
    lv_obj_set_size(s_arrow_tip_circle, ARROW_TIP_D, ARROW_TIP_D);
    lv_arc_set_bg_angles(s_arrow_tip_circle, 0, 359);
    lv_arc_set_angles(s_arrow_tip_circle, 0, 0);
    lv_obj_set_style_arc_color(s_arrow_tip_circle, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arrow_tip_circle, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arrow_tip_circle, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arrow_tip_circle, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(s_arrow_tip_circle, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_arrow_tip_circle, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_arrow_tip_circle, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_arrow_tip_circle, LV_OBJ_FLAG_HIDDEN);

    s_hold_frame = lv_obj_create(scr);
    lv_obj_set_size(s_hold_frame, OLED_WIDTH - 2*HOLD_INSET_X, OLED_HEIGHT - 2*HOLD_INSET_Y);
    lv_obj_align(s_hold_frame, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_hold_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_hold_frame, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_hold_frame, 1, 0);
    lv_obj_set_style_radius(s_hold_frame, 0, 0);
    lv_obj_set_style_pad_all(s_hold_frame, 0, 0);
    // lv_obj_add_flag(s_hold_frame, LV_OBJ_FLAG_HIDDEN);

    s_hold_label = lv_label_create(scr);
    lv_label_set_text(s_hold_label, "H");
    lv_obj_set_style_text_color(s_hold_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_hold_label, &lv_font_unscii_8, 0);
    lv_obj_align_to(s_hold_label, s_hold_frame, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    // lv_obj_add_flag(s_hold_label, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_target(const frame_t *f)
{
    lv_obj_set_pos(s_bbox, f->bbox_x, f->bbox_y);
    lv_obj_set_size(s_bbox, f->bbox_w, f->bbox_h);
    lv_obj_clear_flag(s_bbox, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(s_pred_dot, f->pred_x - 1, f->pred_y - 1);
    lv_obj_clear_flag(s_pred_dot, LV_OBJ_FLAG_HIDDEN);
}

void ui_clear_target(void)
{
    lv_obj_add_flag(s_bbox,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pred_dot,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrow_line,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrow_tip_circle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hold_frame,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hold_label,       LV_OBJ_FLAG_HIDDEN);
}

void ui_set_crosshair(uint8_t cx, uint8_t cy)
{
    s_cx = cx;
    s_cy = cy;
    s_ch_h_pts[0] = (lv_point_precise_t){ cx - CROSSHAIR_ARM, cy };
    s_ch_h_pts[1] = (lv_point_precise_t){ cx + CROSSHAIR_ARM + 1, cy };
    s_ch_v_pts[0] = (lv_point_precise_t){ cx, cy - CROSSHAIR_ARM };
    s_ch_v_pts[1] = (lv_point_precise_t){ cx, cy + CROSSHAIR_ARM + 1 };
    lv_line_set_points(s_ch_h_line, s_ch_h_pts, 2);
    lv_line_set_points(s_ch_v_line, s_ch_v_pts, 2);
    s_arrow_pts[0] = (lv_point_precise_t){ cx, cy };
    lv_line_set_points(s_arrow_line, s_arrow_pts, 2);
}

void ui_show_arrow(uint8_t tip_x, uint8_t tip_y)
{
    s_arrow_tip_x = tip_x;
    s_arrow_tip_y = tip_y;

    s_arrow_pts[0] = (lv_point_precise_t){ s_cx,  s_cy  };
    s_arrow_pts[1] = (lv_point_precise_t){ tip_x, tip_y };
    lv_line_set_points(s_arrow_line, s_arrow_pts, 2);
    lv_obj_clear_flag(s_arrow_line, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(s_arrow_tip_circle, tip_x - ARROW_TIP_D/2, tip_y - ARROW_TIP_D/2);
    lv_obj_clear_flag(s_arrow_tip_circle, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_hold_frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hold_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_display_get_screen_active(s_disp));
}

void ui_clear_arrow(void)
{
    lv_obj_add_flag(s_arrow_line,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrow_tip_circle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hold_frame,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hold_label,       LV_OBJ_FLAG_HIDDEN);
}

void ui_show_hold(void)
{
    lv_obj_add_flag(s_arrow_line,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrow_tip_circle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hold_frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hold_label, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_hold_arrow(uint8_t tip_x, uint8_t tip_y)
{
    s_arrow_tip_x = tip_x;
    s_arrow_tip_y = tip_y;

    s_arrow_pts[0] = (lv_point_precise_t){ s_cx,  s_cy  };
    s_arrow_pts[1] = (lv_point_precise_t){ tip_x, tip_y };
    lv_line_set_points(s_arrow_line, s_arrow_pts, 2);
    lv_obj_clear_flag(s_arrow_line, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(s_arrow_tip_circle, tip_x - ARROW_TIP_D/2, tip_y - ARROW_TIP_D/2);
    lv_obj_clear_flag(s_arrow_tip_circle, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(s_hold_frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hold_label, LV_OBJ_FLAG_HIDDEN);
}

void ui_debug_frame_count(uint32_t count)
{
    static char buf[24];
    lv_snprintf(buf, sizeof(buf), "%lu a:%d,%d", count, s_arrow_tip_x, s_arrow_tip_y);
    // lv_label_set_text(s_dbg_label, buf);
}

// --- Demo animation ----------------------------------------------------------

static void ui_demo_task(void *arg) {
    const int box_w = 20, box_h = 12;
    int x = 0, y = 0;
    int vx = 2, vy = 1;

    while (1) {
        x += vx;
        y += vy;

        if (x < 0)                   { x = 0;                    vx = -vx; }
        if (x > OLED_WIDTH  - box_w) { x = OLED_WIDTH  - box_w; vx = -vx; }
        if (y < 0)                   { y = 0;                    vy = -vy; }
        if (y > OLED_HEIGHT - box_h) { y = OLED_HEIGHT - box_h; vy = -vy; }

        ui_lock();
        lv_obj_set_pos(s_bbox, x, y);
        lv_obj_set_size(s_bbox, box_w, box_h);
        lv_obj_clear_flag(s_bbox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_pred_dot, x + box_w / 2 - 1, y + box_h / 2 - 1);
        lv_obj_clear_flag(s_pred_dot, LV_OBJ_FLAG_HIDDEN);
        ui_unlock();

        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

void ui_demo_start(void) {
    xTaskCreate(ui_demo_task, "ui_demo", 2048, NULL, 1, NULL);
}
