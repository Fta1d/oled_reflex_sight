#include "ui.h"
#include "display.h"
#include "frame.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart_comm.h"
#include <math.h>

#define CROSSHAIR_ARM    10
#define ARROW_HEAD_LEN    5

static const char *TAG = "ui";

static lv_display_t *s_disp;

static lv_obj_t *s_bbox;
static lv_obj_t *s_pred_dot;
static lv_obj_t *s_dbg_label;

static lv_obj_t *s_ch_h_line;
static lv_obj_t *s_ch_v_line;
static lv_point_precise_t s_ch_h_pts[2];
static lv_point_precise_t s_ch_v_pts[2];

static lv_obj_t *s_arrow_line;
static lv_point_precise_t s_arrow_pts[2];
static lv_obj_t *s_arrow_h1_line;
static lv_point_precise_t s_arrow_h1_pts[2];
static lv_obj_t *s_arrow_h2_line;
static lv_point_precise_t s_arrow_h2_pts[2];

static uint8_t s_cx = OLED_WIDTH  / 2;
static uint8_t s_cy = OLED_HEIGHT / 2;
static uint8_t s_arrow_tip_x;
static uint8_t s_arrow_tip_y;

// --- Public API --------------------------------------------------------------

esp_err_t ui_init(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel) {
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = OLED_WIDTH * OLED_HEIGHT,
        .double_buffer = false,
        .hres          = OLED_WIDTH,
        .vres          = OLED_HEIGHT,
        .monochrome    = true,
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) return ESP_FAIL;

    ESP_LOGI(TAG, "LVGL ready");
    return ESP_OK;
}

void ui_lock(void)         { lvgl_port_lock(0); }
void ui_unlock(void)       { lvgl_port_unlock(); }
void ui_notify_frame(void) { /* port wakes its task after each flush automatically */ }

// --- Screens -----------------------------------------------------------------

void ui_build_reticle(void) {
    lv_obj_t *scr = lv_display_get_screen_active(s_disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_cx = OLED_WIDTH  / 2;
    s_cy = OLED_HEIGHT / 2;

    s_ch_h_pts[0] = (lv_point_precise_t){ s_cx - CROSSHAIR_ARM, s_cy };
    s_ch_h_pts[1] = (lv_point_precise_t){ s_cx + CROSSHAIR_ARM, s_cy };
    s_ch_v_pts[0] = (lv_point_precise_t){ s_cx, s_cy - CROSSHAIR_ARM };
    s_ch_v_pts[1] = (lv_point_precise_t){ s_cx, s_cy + CROSSHAIR_ARM };

    s_ch_h_line = lv_line_create(scr);
    lv_line_set_points(s_ch_h_line, s_ch_h_pts, 2);
    lv_obj_set_style_line_color(s_ch_h_line, lv_color_white(), 0);
    lv_obj_set_style_line_width(s_ch_h_line, 1, 0);

    s_ch_v_line = lv_line_create(scr);
    lv_line_set_points(s_ch_v_line, s_ch_v_pts, 2);
    lv_obj_set_style_line_color(s_ch_v_line, lv_color_white(), 0);
    lv_obj_set_style_line_width(s_ch_v_line, 1, 0);

    s_dbg_label = lv_label_create(scr);
    lv_label_set_text(s_dbg_label, "rx:0");
    lv_obj_set_style_text_color(s_dbg_label, lv_color_white(), 0);
    lv_obj_align(s_dbg_label, LV_ALIGN_TOP_LEFT, 0, 0);

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

    s_arrow_h1_pts[0] = s_arrow_h1_pts[1] = (lv_point_precise_t){ s_cx, s_cy };
    s_arrow_h1_line = lv_line_create(scr);
    lv_line_set_points(s_arrow_h1_line, s_arrow_h1_pts, 2);
    lv_obj_set_style_line_color(s_arrow_h1_line, lv_color_white(), 0);
    lv_obj_set_style_line_width(s_arrow_h1_line, 2, 0);
    lv_obj_add_flag(s_arrow_h1_line, LV_OBJ_FLAG_HIDDEN);

    s_arrow_h2_pts[0] = s_arrow_h2_pts[1] = (lv_point_precise_t){ s_cx, s_cy };
    s_arrow_h2_line = lv_line_create(scr);
    lv_line_set_points(s_arrow_h2_line, s_arrow_h2_pts, 2);
    lv_obj_set_style_line_color(s_arrow_h2_line, lv_color_white(), 0);
    lv_obj_set_style_line_width(s_arrow_h2_line, 2, 0);
    lv_obj_add_flag(s_arrow_h2_line, LV_OBJ_FLAG_HIDDEN);
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
    s_arrow_pts[0] = (lv_point_precise_t){ s_cx, s_cy };
    s_arrow_pts[1] = (lv_point_precise_t){ s_cx, s_cy };
    lv_line_set_points(s_arrow_line, s_arrow_pts, 2);
    lv_obj_add_flag(s_arrow_h1_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrow_h2_line, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_crosshair(uint8_t cx, uint8_t cy)
{
    s_cx = cx;
    s_cy = cy;
    s_ch_h_pts[0] = (lv_point_precise_t){ cx - CROSSHAIR_ARM, cy };
    s_ch_h_pts[1] = (lv_point_precise_t){ cx + CROSSHAIR_ARM, cy };
    s_ch_v_pts[0] = (lv_point_precise_t){ cx, cy - CROSSHAIR_ARM };
    s_ch_v_pts[1] = (lv_point_precise_t){ cx, cy + CROSSHAIR_ARM };
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

    float dx = (float)tip_x - s_cx;
    float dy = (float)tip_y - s_cy;
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 1.0f) {
        float ux = dx / len;
        float uy = dy / len;
        // Perpendicular unit vector
        float px = -uy;
        float py =  ux;
        float hl = ARROW_HEAD_LEN;

        int w1x = LV_CLAMP(0, (int)(tip_x - hl * ux + hl * px), OLED_WIDTH  - 1);
        int w1y = LV_CLAMP(0, (int)(tip_y - hl * uy + hl * py), OLED_HEIGHT - 1);
        int w2x = LV_CLAMP(0, (int)(tip_x - hl * ux - hl * px), OLED_WIDTH  - 1);
        int w2y = LV_CLAMP(0, (int)(tip_y - hl * uy - hl * py), OLED_HEIGHT - 1);

        s_arrow_h1_pts[0] = (lv_point_precise_t){ tip_x, tip_y };
        s_arrow_h1_pts[1] = (lv_point_precise_t){ w1x,   w1y   };
        lv_line_set_points(s_arrow_h1_line, s_arrow_h1_pts, 2);
        lv_obj_clear_flag(s_arrow_h1_line, LV_OBJ_FLAG_HIDDEN);

        s_arrow_h2_pts[0] = (lv_point_precise_t){ tip_x, tip_y };
        s_arrow_h2_pts[1] = (lv_point_precise_t){ w2x,   w2y   };
        lv_line_set_points(s_arrow_h2_line, s_arrow_h2_pts, 2);
        lv_obj_clear_flag(s_arrow_h2_line, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_invalidate(lv_display_get_screen_active(s_disp));
}

void ui_clear_arrow(void)
{
    lv_obj_add_flag(s_arrow_line,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrow_h1_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrow_h2_line, LV_OBJ_FLAG_HIDDEN);
}

void ui_debug_frame_count(uint32_t count)
{
    static char buf[24];
    lv_snprintf(buf, sizeof(buf), "%lu a:%d,%d", count, s_arrow_tip_x, s_arrow_tip_y);
    lv_label_set_text(s_dbg_label, buf);
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
