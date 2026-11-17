/*
 * LinSight Industrial CAN Analyzer UI (320x240, LVGL 8.3)
 * Dark Tactical HUD Theme — Handheld Field Tester Edition (Pure English).
 * Zero dynamic heap allocation on 20 Hz tick + Dirty-check rendering.
 */

#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "sd_store.h"
#include "twai.h"

/* -------------------------------------------------------------- geometry */
#define UI_W            320
#define UI_H            240
#define TOP_H           24
#define VIEWPORT_Y      24
#define VIEWPORT_H      180
#define BOT_Y           204
#define BOT_H           36

#define PAD_X           6
#define PAD_Y           6
#define GAP             6
#define CARD_FULL_W     (UI_W - (PAD_X * 2))               /* 308 */
#define CARD_HALF_W     ((CARD_FULL_W - GAP) / 2)          /* 151 */
#define CARD_RIGHT_X    (PAD_X + CARD_HALF_W + GAP)        /* 163 */

#define SPLASH_CONTENT_W 220
#define SPLASH_CONTENT_H 70

/* ---------------------------------------------------------------- colors */
#define COL_BG_ROOT       lv_color_hex(0x0B0F14)
#define COL_BG_CARD       lv_color_hex(0x151C24)
#define COL_BG_CARD_ACT   lv_color_hex(0x1E293B)
#define COL_BG_PRESSED    lv_color_hex(0x283548)
#define COL_BORDER        lv_color_hex(0x2A3644)

#define COL_TEXT_PRI      lv_color_hex(0xF1F5F9)
#define COL_TEXT_SEC      lv_color_hex(0x94A3B8)
#define COL_TEXT_MUTED    lv_color_hex(0x475569)

#define COL_OK_GREEN      lv_color_hex(0x10B981)
#define COL_WARN_AMBER    lv_color_hex(0xF59E0B)
#define COL_ERR_RED       lv_color_hex(0xEF4444)
#define COL_ACCENT_CYAN   lv_color_hex(0x06B6D4)

/* ----------------------------------------------------------------- types */
typedef enum {
    VIEW_DASH  = 0,
    VIEW_DIAG  = 1,
    VIEW_SET   = 2,
    VIEW_COUNT = 3
} view_id_t;

/* --------------------------------------------------------------- statics */
static lv_obj_t *s_views[VIEW_COUNT];
static view_id_t s_current_view = VIEW_DASH;

/* Top Bar (Decluttered 3-zone layout) */
static lv_obj_t *s_top_bar;
static lv_obj_t *s_top_state_pill, *s_top_state_lbl;
static lv_obj_t *s_top_cfg_lbl;
static lv_obj_t *s_top_badge_lbl;

/* Bottom Dock */
static lv_obj_t *s_dock;
static lv_obj_t *s_dock_btn[VIEW_COUNT];
static lv_obj_t *s_dock_line[VIEW_COUNT];
static lv_obj_t *s_dock_lbl[VIEW_COUNT];

/* View 0: DASH */
static lv_obj_t *s_dash_load_val, *s_dash_load_bar;
static lv_obj_t *s_dash_fps_val, *s_dash_fps_sub;
static lv_obj_t *s_dash_health_st, *s_dash_health_tec, *s_dash_health_err;
static lv_obj_t *s_dash_act_btn, *s_dash_act_lbl;
static lv_obj_t *s_dash_mark_btn, *s_dash_mark_lbl;

/* View 1: DIAG */
static lv_obj_t *s_diag_tec_val, *s_diag_tec_bar;
static lv_obj_t *s_diag_rec_val, *s_diag_rec_bar;
static lv_obj_t *s_diag_err_val, *s_diag_boff_val, *s_diag_ovf_val;
static lv_obj_t *s_diag_act_btn, *s_diag_act_lbl;
static lv_obj_t *s_diag_sd_st_val, *s_diag_sd_free_val, *s_diag_sd_drop_val;

/* View 2: SET */
static lv_obj_t *s_set_br_btn[4];
static lv_obj_t *s_set_mode_btn[3];
static lv_obj_t *s_set_bl_btn[4];
static lv_obj_t *s_set_host_link_lbl;

/* Toast */
static lv_obj_t *s_toast, *s_toast_lbl;
static uint32_t s_toast_tick = 0;

/* Static string buffers (0 heap allocation on 20 Hz tick) */
static char s_txt_top_state[16], s_txt_top_cfg[28], s_txt_top_badge[24];
static char s_txt_dash_load[16], s_txt_dash_fps[16], s_txt_dash_fps_sub[24];
static char s_txt_dash_health_st[20], s_txt_dash_health_tec[28], s_txt_dash_health_err[24];
static char s_txt_dash_act[20];
static char s_txt_diag_tec[12], s_txt_diag_rec[12];
static char s_txt_diag_err[16], s_txt_diag_boff[16], s_txt_diag_ovf[16];
static char s_txt_diag_act[20];
static char s_txt_diag_sd_st[20], s_txt_diag_sd_free[20], s_txt_diag_sd_drop[16];
static char s_txt_set_host_ep[56];
static char s_txt_set_host_link[48];
static char s_txt_toast[48];

/* Runtime states */
static uint32_t s_conn_mailbox = (uint32_t)UI_CONN_WIFI << 24;
static bool s_frozen = false;
static can_stats_t s_frozen_stats;

static const can_bitrate_t BITRATES[4]  = {CAN_BITRATE_125K, CAN_BITRATE_250K, CAN_BITRATE_500K, CAN_BITRATE_1M};
static const char *BITRATE_LABELS[4]    = {"125K", "250K", "500K", "1M"};
static const can_mode_t MODES[3]        = {CAN_MODE_LISTEN_ONLY, CAN_MODE_NORMAL, CAN_MODE_SELFTEST};
static const char *MODE_LABELS[3]       = {"LISTEN", "NORMAL", "SELFTEST"};
static const int BRIGHTNESS_LEVELS[4]   = {20, 50, 80, 100};
static const char *BRIGHTNESS_LABELS[4] = {"20%", "50%", "80%", "100%"};

/* ---------------------------------------------------- Dirty-Check Helpers */
static void obj_bg_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t selector)
{
    if (lv_obj_get_style_bg_color(obj, selector).full != color.full) {
        lv_obj_set_style_bg_color(obj, color, selector);
    }
}

static void label_color(lv_obj_t *label, lv_color_t color)
{
    if (lv_obj_get_style_text_color(label, 0).full != color.full) {
        lv_obj_set_style_text_color(label, color, 0);
    }
}

static void label_set_str(lv_obj_t *label, char *buf, size_t cap, const char *text)
{
    if (strcmp(buf, text) == 0) return;
    strlcpy(buf, text, cap);
    lv_label_set_text_static(label, buf);
}

static void label_printf(lv_obj_t *label, char *buf, size_t cap, const char *fmt, ...)
{
    char tmp[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);

    if (strcmp(buf, tmp) == 0) return;
    strlcpy(buf, tmp, cap);
    lv_label_set_text_static(label, buf);
}

static void bar_update(lv_obj_t *bar, int32_t val, lv_color_t ind_col)
{
    if (lv_bar_get_value(bar) != val) {
        lv_bar_set_value(bar, val, LV_ANIM_OFF);
    }
    obj_bg_color(bar, ind_col, LV_PART_INDICATOR);
}

#define LBL_SET(lbl, buf, text)       label_set_str((lbl), (buf), sizeof(buf), (text))
#define LBL_FMT(lbl, buf, fmt, ...)   label_printf((lbl), (buf), sizeof(buf), (fmt), __VA_ARGS__)

static void object_show(lv_obj_t *obj, bool show)
{
    if (!obj) return;
    if (show) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* --------------------------------------------------------- Widget Builders */
static lv_obj_t *card_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_BG_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, COL_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static lv_obj_t *btn_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                         lv_color_t bg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = card_new(parent, x, y, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, COL_BG_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, COL_ACCENT_CYAN, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

static lv_obj_t *lbl_new(lv_obj_t *parent, const char *txt, lv_coord_t x, lv_coord_t y,
                         const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text_static(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* Two-column key-value row inside a half-width card */
static lv_obj_t *kv_row_new(lv_obj_t *parent, const char *key_txt, lv_coord_t y,
                            char *val_buf, size_t cap, const char *init_val)
{
    lbl_new(parent, key_txt, 8, y, &lv_font_montserrat_12, COL_TEXT_SEC);
    snprintf(val_buf, cap, "%s", init_val);
    lv_obj_t *val_lbl = lbl_new(parent, val_buf, 8, y, &lv_font_montserrat_12, COL_TEXT_PRI);
    lv_obj_set_width(val_lbl, CARD_HALF_W - 16);
    lv_obj_set_style_text_align(val_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    return val_lbl;
}

static lv_obj_t *bar_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                         int32_t max, lv_color_t ind_col)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, max);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, COL_BG_ROOT, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, ind_col, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static void toast_show(const char *msg)
{
    if (!s_toast) return;
    LBL_SET(s_toast_lbl, s_txt_toast, msg);
    s_toast_tick = lv_tick_get();
    object_show(s_toast, true);
    lv_obj_move_foreground(s_toast);
}

/* ------------------------------------------------------------- View Switch */
static void view_show(view_id_t view)
{
    if (view >= VIEW_COUNT) view = VIEW_DASH;
    s_current_view = view;

    for (int i = 0; i < VIEW_COUNT; i++) {
        const bool active = (i == (int)view);
        object_show(s_views[i], active);
        if (active) {
            lv_obj_move_foreground(s_views[i]);
            lv_obj_invalidate(s_views[i]);
        }
        object_show(s_dock_line[i], active);
        obj_bg_color(s_dock_btn[i], active ? COL_BG_CARD_ACT : COL_BG_ROOT, 0);
        label_color(s_dock_lbl[i], active ? COL_TEXT_PRI : COL_TEXT_SEC);
    }

    if (s_top_bar) lv_obj_move_foreground(s_top_bar);
    if (s_dock) lv_obj_move_foreground(s_dock);
    if (s_toast) lv_obj_move_foreground(s_toast);
}

/* ------------------------------------------------------- Actions & Callbacks */
static void dock_click_cb(lv_event_t *e)
{
    view_show((view_id_t)(intptr_t)lv_event_get_user_data(e));
}

static void dash_action_cb(lv_event_t *e)
{
    (void)e;
    can_stats_t st;
    twai_stats_snapshot(&st);
    if (strcmp(st.state, "BUS_OFF") == 0) {
        toast_show(twai_capture_recover() == ESP_OK ? "BUS RECOVER OK" : "RECOVER FAILED");
    } else {
        s_frozen = !s_frozen;
        if (s_frozen) {
            twai_stats_snapshot(&s_frozen_stats);
        }
        toast_show(s_frozen ? "CAPTURE PAUSED" : "CAPTURE RESUMED");
    }
}

static void diag_action_cb(lv_event_t *e)
{
    (void)e;
    can_stats_t st;
    twai_stats_snapshot(&st);
    if (strcmp(st.state, "BUS_OFF") == 0) {
        toast_show(twai_capture_recover() == ESP_OK ? "BUS RECOVER OK" : "RECOVER FAILED");
    } else {
        twai_stats_clear();
        if (s_frozen) {
            twai_stats_snapshot(&s_frozen_stats);
        }
        toast_show("COUNTERS CLEARED");
    }
}

static void mark_event_cb(lv_event_t *e)
{
    (void)e;
    sd_store_mark("MANUAL", 0, 0);
    toast_show("EVENT MARKED");
}

static void bitrate_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (BITRATES[idx] == twai_current_bitrate()) return;
    if (twai_capture_restart(BITRATES[idx], twai_current_mode()) == ESP_OK) {
        s_frozen = false; /* Auto unfreeze on bus reconfiguration */
        char buf[32];
        snprintf(buf, sizeof(buf), "BITRATE: %s", BITRATE_LABELS[idx]);
        toast_show(buf);
    }
}

static void mode_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (MODES[idx] == twai_current_mode()) return;
    if (twai_capture_restart(twai_current_bitrate(), MODES[idx]) == ESP_OK) {
        s_frozen = false; /* Auto unfreeze on mode change */
        char buf[32];
        snprintf(buf, sizeof(buf), "MODE: %s", MODE_LABELS[idx]);
        toast_show(buf);
    }
}

static void brightness_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bsp_backlight_set(BRIGHTNESS_LEVELS[idx]);
    char buf[32];
    snprintf(buf, sizeof(buf), "BRIGHTNESS: %d%%", BRIGHTNESS_LEVELS[idx]);
    toast_show(buf);
}

/* ------------------------------------------------------------- View Builders */
static void build_top_bar(lv_obj_t *parent)
{
    s_top_bar = card_new(parent, 0, 0, UI_W, TOP_H);
    lv_obj_set_style_bg_color(s_top_bar, COL_BG_ROOT, 0);
    lv_obj_set_style_radius(s_top_bar, 0, 0);

    /* Zone 1 (Left): Wide State Pill (78px wide so "RUNNING" never clips) */
    s_top_state_pill = card_new(s_top_bar, PAD_X, 3, 78, 18);
    lv_obj_set_style_radius(s_top_state_pill, 3, 0);
    lv_obj_set_style_border_width(s_top_state_pill, 0, 0);
    lv_obj_set_style_bg_color(s_top_state_pill, COL_OK_GREEN, 0);

    snprintf(s_txt_top_state, sizeof(s_txt_top_state), "RUNNING");
    s_top_state_lbl = lbl_new(s_top_state_pill, s_txt_top_state, 0, 0, &lv_font_montserrat_12, COL_BG_ROOT);
    lv_obj_center(s_top_state_lbl);

    /* Zone 2 (Middle): Active CAN Bitrate & Mode */
    snprintf(s_txt_top_cfg, sizeof(s_txt_top_cfg), "500K / LISTEN");
    s_top_cfg_lbl = lbl_new(s_top_bar, s_txt_top_cfg, 92, 5, &lv_font_montserrat_12, COL_TEXT_PRI);

    /* Zone 3 (Right): Single Concise Status Badge (Rec / Link / SD) */
    snprintf(s_txt_top_badge, sizeof(s_txt_top_badge), "LOCAL");
    s_top_badge_lbl = lbl_new(s_top_bar, s_txt_top_badge, 214, 5, &lv_font_montserrat_12, COL_TEXT_MUTED);
    lv_obj_set_width(s_top_badge_lbl, 100);
    lv_obj_set_style_text_align(s_top_badge_lbl, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_bottom_dock(lv_obj_t *parent)
{
    s_dock = card_new(parent, 0, BOT_Y, UI_W, BOT_H);
    lv_obj_set_style_bg_color(s_dock, COL_BG_ROOT, 0);
    lv_obj_set_style_radius(s_dock, 0, 0);

    const char *names[VIEW_COUNT] = {"DASH", "DIAG", "SETUP"};
    const lv_coord_t w = 106;

    for (int i = 0; i < VIEW_COUNT; i++) {
        const lv_coord_t cur_w = (i == 2) ? 108 : w;
        s_dock_btn[i] = btn_new(s_dock, i * w, 0, cur_w, BOT_H, COL_BG_ROOT, dock_click_cb, (void *)(intptr_t)i);
        s_dock_line[i] = card_new(s_dock_btn[i], 0, 0, cur_w, 2);
        lv_obj_set_style_bg_color(s_dock_line[i], COL_ACCENT_CYAN, 0);
        lv_obj_set_style_border_width(s_dock_line[i], 0, 0);

        s_dock_lbl[i] = lbl_new(s_dock_btn[i], names[i], 0, 0, &lv_font_montserrat_14,
                                i == 0 ? COL_TEXT_PRI : COL_TEXT_SEC);
        lv_obj_center(s_dock_lbl[i]);
    }
}

static void build_dash_view(lv_obj_t *parent)
{
    /* Card 1 (Top Full): Primary Metrics (Bus Load % + Frame Rate) */
    lv_obj_t *c1 = card_new(parent, PAD_X, PAD_Y, CARD_FULL_W, 88);

    lbl_new(c1, "BUS LOAD", 10, 8, &lv_font_montserrat_12, COL_TEXT_SEC);
    snprintf(s_txt_dash_load, sizeof(s_txt_dash_load), "0.0 %%");
    s_dash_load_val = lbl_new(c1, s_txt_dash_load, 10, 26, &lv_font_montserrat_24, COL_TEXT_PRI);
    /* Range 0..1000 matches load_permille (0.0% .. 100.0%) */
    s_dash_load_bar = bar_new(c1, 10, 64, 134, 8, 1000, COL_OK_GREEN);

    /* Center vertical divider */
    lv_obj_t *div = card_new(c1, 154, 8, 1, 72);
    lv_obj_set_style_bg_color(div, COL_BORDER, 0);
    lv_obj_set_style_border_width(div, 0, 0);

    lbl_new(c1, "THROUGHPUT", 164, 8, &lv_font_montserrat_12, COL_TEXT_SEC);
    snprintf(s_txt_dash_fps, sizeof(s_txt_dash_fps), "0 fps");
    s_dash_fps_val = lbl_new(c1, s_txt_dash_fps, 164, 26, &lv_font_montserrat_24, COL_TEXT_PRI);
    snprintf(s_txt_dash_fps_sub, sizeof(s_txt_dash_fps_sub), "Rx Total: 0");
    s_dash_fps_sub = lbl_new(c1, s_txt_dash_fps_sub, 164, 60, &lv_font_montserrat_12, COL_TEXT_SEC);

    /* Card 2 (Bottom Left): Bus Health Summary */
    lv_obj_t *c2 = card_new(parent, PAD_X, 100, CARD_HALF_W, 74);
    lbl_new(c2, "BUS HEALTH", 8, 6, &lv_font_montserrat_12, COL_TEXT_SEC);
    snprintf(s_txt_dash_health_st, sizeof(s_txt_dash_health_st), "NORMAL (OK)");
    s_dash_health_st = lbl_new(c2, s_txt_dash_health_st, 8, 22, &lv_font_montserrat_14, COL_OK_GREEN);
    snprintf(s_txt_dash_health_tec, sizeof(s_txt_dash_health_tec), "TEC: 0 | REC: 0");
    s_dash_health_tec = lbl_new(c2, s_txt_dash_health_tec, 8, 41, &lv_font_montserrat_12, COL_TEXT_PRI);
    snprintf(s_txt_dash_health_err, sizeof(s_txt_dash_health_err), "Errors: 0");
    s_dash_health_err = lbl_new(c2, s_txt_dash_health_err, 8, 56, &lv_font_montserrat_12, COL_TEXT_MUTED);

    /* Zone 3 (Bottom Right): Tactical Dual Action Buttons */
    s_dash_act_btn = btn_new(parent, CARD_RIGHT_X, 100, CARD_HALF_W, 34, COL_BG_CARD, dash_action_cb, NULL);
    snprintf(s_txt_dash_act, sizeof(s_txt_dash_act), "PAUSE");
    s_dash_act_lbl = lbl_new(s_dash_act_btn, s_txt_dash_act, 0, 0, &lv_font_montserrat_14, COL_TEXT_PRI);
    lv_obj_center(s_dash_act_lbl);

    s_dash_mark_btn = btn_new(parent, CARD_RIGHT_X, 140, CARD_HALF_W, 34, COL_BG_CARD, mark_event_cb, NULL);
    lv_obj_set_style_border_color(s_dash_mark_btn, COL_WARN_AMBER, 0);
    s_dash_mark_lbl = lbl_new(s_dash_mark_btn, "MARK EVENT", 0, 0, &lv_font_montserrat_12, COL_WARN_AMBER);
    lv_obj_center(s_dash_mark_lbl);
}

static void build_diag_view(lv_obj_t *parent)
{
    /* Card 1 (Left): Physical Bus Diagnostics */
    lv_obj_t *c1 = card_new(parent, PAD_X, PAD_Y, CARD_HALF_W, 168);
    lbl_new(c1, "CAN DIAGNOSTICS", 8, 6, &lv_font_montserrat_12, COL_TEXT_PRI);

    s_diag_tec_val = kv_row_new(c1, "TEC (Tx):", 24, s_txt_diag_tec, sizeof(s_txt_diag_tec), "0");
    s_diag_tec_bar = bar_new(c1, 8, 40, CARD_HALF_W - 16, 5, 255, COL_OK_GREEN);

    s_diag_rec_val = kv_row_new(c1, "REC (Rx):", 50, s_txt_diag_rec, sizeof(s_txt_diag_rec), "0");
    s_diag_rec_bar = bar_new(c1, 8, 66, CARD_HALF_W - 16, 5, 255, COL_OK_GREEN);

    s_diag_err_val  = kv_row_new(c1, "Bus Errors:", 78,  s_txt_diag_err,  sizeof(s_txt_diag_err),  "0");
    s_diag_boff_val = kv_row_new(c1, "Bus-Offs:",   94,  s_txt_diag_boff, sizeof(s_txt_diag_boff), "0");
    s_diag_ovf_val  = kv_row_new(c1, "Rx Missed:",  110, s_txt_diag_ovf,  sizeof(s_txt_diag_ovf),  "0");

    s_diag_act_btn = btn_new(c1, 8, 130, CARD_HALF_W - 16, 30, COL_BG_CARD_ACT, diag_action_cb, NULL);
    snprintf(s_txt_diag_act, sizeof(s_txt_diag_act), "CLEAR STATS");
    s_diag_act_lbl = lbl_new(s_diag_act_btn, s_txt_diag_act, 0, 0, &lv_font_montserrat_12, COL_TEXT_PRI);
    lv_obj_center(s_diag_act_lbl);

    /* Card 2 (Right): SD Blackbox Storage */
    lv_obj_t *c2 = card_new(parent, CARD_RIGHT_X, PAD_Y, CARD_HALF_W, 168);
    lbl_new(c2, "SD BLACKBOX", 8, 6, &lv_font_montserrat_12, COL_TEXT_PRI);

    s_diag_sd_st_val   = kv_row_new(c2, "Status:",  30, s_txt_diag_sd_st,   sizeof(s_txt_diag_sd_st),   "NO SD");
    s_diag_sd_free_val = kv_row_new(c2, "Free:",    54, s_txt_diag_sd_free, sizeof(s_txt_diag_sd_free), "--");
    s_diag_sd_drop_val = kv_row_new(c2, "Dropped:", 78, s_txt_diag_sd_drop, sizeof(s_txt_diag_sd_drop), "0");

    lbl_new(c2, "Insert flag into log:", 8, 108, &lv_font_montserrat_12, COL_TEXT_MUTED);

    lv_obj_t *mark = btn_new(c2, 8, 130, CARD_HALF_W - 16, 30, COL_BG_CARD_ACT, mark_event_cb, NULL);
    lv_obj_set_style_border_color(mark, COL_WARN_AMBER, 0);
    lv_obj_t *mark_l = lbl_new(mark, "MARK EVENT", 0, 0, &lv_font_montserrat_12, COL_WARN_AMBER);
    lv_obj_center(mark_l);
}

static void build_set_view(lv_obj_t *parent)
{
    /* Row 1: Bitrate selector */
    lbl_new(parent, "BITRATE:", 8, 14, &lv_font_montserrat_12, COL_TEXT_SEC);
    for (int i = 0; i < 4; i++) {
        s_set_br_btn[i] = btn_new(parent, 74 + i * 60, 6, 56, 30, COL_BG_CARD, bitrate_click_cb, (void *)(intptr_t)i);
        lv_obj_t *l = lbl_new(s_set_br_btn[i], BITRATE_LABELS[i], 0, 0, &lv_font_montserrat_12, COL_TEXT_PRI);
        lv_obj_center(l);
    }

    /* Row 2: Controller Mode selector */
    lbl_new(parent, "MODE:", 8, 50, &lv_font_montserrat_12, COL_TEXT_SEC);
    for (int i = 0; i < 3; i++) {
        s_set_mode_btn[i] = btn_new(parent, 74 + i * 80, 42, 76, 30, COL_BG_CARD, mode_click_cb, (void *)(intptr_t)i);
        lv_obj_t *l = lbl_new(s_set_mode_btn[i], MODE_LABELS[i], 0, 0, &lv_font_montserrat_12, COL_TEXT_PRI);
        lv_obj_center(l);
    }

    /* Row 3: Backlight Brightness selector */
    lbl_new(parent, "BRIGHT:", 8, 86, &lv_font_montserrat_12, COL_TEXT_SEC);
    for (int i = 0; i < 4; i++) {
        s_set_bl_btn[i] = btn_new(parent, 74 + i * 60, 78, 56, 30, COL_BG_CARD, brightness_click_cb, (void *)(intptr_t)i);
        lv_obj_t *l = lbl_new(s_set_bl_btn[i], BRIGHTNESS_LABELS[i], 0, 0, &lv_font_montserrat_12, COL_TEXT_PRI);
        lv_obj_center(l);
    }

    /* Row 4: Host Telemetry (Wi-Fi STA / TCP endpoint & live link state) */
    lv_obj_t *host_card = card_new(parent, PAD_X, 116, CARD_FULL_W, 58);
    lbl_new(host_card, "HOST STREAM (Wi-Fi STA / TCP)", 8, 6, &lv_font_montserrat_12, COL_ACCENT_CYAN);

    snprintf(s_txt_set_host_ep, sizeof(s_txt_set_host_ep), "SSID: %.14s | %s:%u",
             CONFIG_LINSIGHT_WIFI_SSID, CONFIG_LINSIGHT_HOST_IP,
             (unsigned)CONFIG_LINSIGHT_HOST_PORT);
    lv_obj_t *ep_lbl = lbl_new(host_card, s_txt_set_host_ep, 8, 23, &lv_font_montserrat_12, COL_TEXT_SEC);
    lv_obj_set_width(ep_lbl, CARD_FULL_W - 16);
    lv_label_set_long_mode(ep_lbl, LV_LABEL_LONG_DOT);

    snprintf(s_txt_set_host_link, sizeof(s_txt_set_host_link), "Status: JOINING WI-FI...");
    s_set_host_link_lbl = lbl_new(host_card, s_txt_set_host_link, 8, 40, &lv_font_montserrat_12, COL_WARN_AMBER);
}

/* ----------------------------------------------------------- Boot Animation */
static void splash_bar_cb(void *var, int32_t value)
{
    lv_obj_set_width((lv_obj_t *)var, (lv_coord_t)value);
}

static void splash_fade_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void splash_ready_cb(lv_anim_t *a)
{
    lv_obj_del((lv_obj_t *)lv_anim_get_user_data(a));
}

static void splash_create(void)
{
    lv_obj_t *splash = card_new(lv_layer_top(), 0, 0, UI_W, UI_H);
    lv_obj_set_style_bg_color(splash, COL_BG_ROOT, 0);
    lv_obj_set_style_border_width(splash, 0, 0);
    lv_obj_set_style_radius(splash, 0, 0);
    lv_obj_add_flag(splash, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *content = card_new(splash, (UI_W - SPLASH_CONTENT_W) / 2, (UI_H - SPLASH_CONTENT_H) / 2,
                                 SPLASH_CONTENT_W, SPLASH_CONTENT_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);

    lv_obj_t *brand = lbl_new(content, "LinSight", 0, 0, &lv_font_montserrat_24, COL_TEXT_PRI);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *bar = card_new(content, 0, 36, 0, 3);
    lv_obj_set_style_bg_color(bar, COL_ACCENT_CYAN, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 2, 0);

    lv_obj_t *sub = lbl_new(content, "CYBER CAN ANALYZER", 0, 0, &lv_font_montserrat_12, COL_TEXT_SEC);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 48);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_values(&a, 0, SPLASH_CONTENT_W);
    lv_anim_set_time(&a, 700);
    lv_anim_set_exec_cb(&a, splash_bar_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, splash);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&fade, 900);
    lv_anim_set_time(&fade, 250);
    lv_anim_set_exec_cb(&fade, splash_fade_cb);
    lv_anim_set_ready_cb(&fade, splash_ready_cb);
    lv_anim_set_user_data(&fade, splash);
    lv_anim_start(&fade);
}

/* ------------------------------------------------------------- 20Hz Refresh */
static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Read lock-free remote connection mailbox */
    const uint32_t conn_packed = __atomic_load_n(&s_conn_mailbox, __ATOMIC_RELAXED);
    const linsight_conn_t conn = (linsight_conn_t)(conn_packed >> 24);
    const uint32_t rtt_ms = conn_packed & 0xFFFFFFu;

    /* Auto-hide toast after 1.5s */
    if (s_toast && !lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN)) {
        if (lv_tick_elaps(s_toast_tick) > 1500u) object_show(s_toast, false);
    }

    can_stats_t live_st;
    twai_stats_snapshot(&live_st);
    const can_stats_t *st = s_frozen ? &s_frozen_stats : &live_st;

    sd_status_t sd;
    sd_store_get_status(&sd);

    const bool is_bus_off = (strcmp(live_st.state, "BUS_OFF") == 0);
    const bool is_passive = (strcmp(live_st.state, "RECOVERING") == 0 || live_st.tec > 127u || live_st.rec > 127u);
    const can_mode_t cur_mode = twai_current_mode();

    /* 1. Top Bar Updates (Clean 3-zone) */
    if (is_bus_off) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "BUS-OFF");
        obj_bg_color(s_top_state_pill, COL_ERR_RED, 0);
        label_color(s_top_state_lbl, COL_TEXT_PRI);
    } else if (s_frozen) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "PAUSED");
        obj_bg_color(s_top_state_pill, COL_WARN_AMBER, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    } else if (is_passive) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "PASSIVE");
        obj_bg_color(s_top_state_pill, COL_WARN_AMBER, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    } else {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "RUNNING");
        obj_bg_color(s_top_state_pill, COL_OK_GREEN, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    }

    LBL_FMT(s_top_cfg_lbl, s_txt_top_cfg, "%s / %s",
            can_bitrate_name(twai_current_bitrate()),
            cur_mode == CAN_MODE_LISTEN_ONLY ? "LISTEN" : cur_mode == CAN_MODE_NORMAL ? "NORMAL" : "SELF");

    /* Top-right concise priority badge: REC > HOST LINK > SD READY > LOCAL */
    if (sd.recording) {
        LBL_FMT(s_top_badge_lbl, s_txt_top_badge, "REC #%02lu", (unsigned long)sd.files);
        label_color(s_top_badge_lbl, COL_ERR_RED);
    } else if (conn == UI_CONN_ONLINE) {
        LBL_SET(s_top_badge_lbl, s_txt_top_badge, "HOST LINK");
        label_color(s_top_badge_lbl, COL_OK_GREEN);
    } else if (sd.mounted) {
        LBL_SET(s_top_badge_lbl, s_txt_top_badge, "SD READY");
        label_color(s_top_badge_lbl, COL_ACCENT_CYAN);
    } else {
        LBL_SET(s_top_badge_lbl, s_txt_top_badge, "LOCAL");
        label_color(s_top_badge_lbl, COL_TEXT_MUTED);
    }

    /* 2. View 0: DASH */
    if (s_current_view == VIEW_DASH) {
        const uint32_t permille = st->load_permille > 1000u ? 1000u : st->load_permille;
        LBL_FMT(s_dash_load_val, s_txt_dash_load, "%lu.%lu %%",
                (unsigned long)(permille / 10u), (unsigned long)(permille % 10u));

        const lv_color_t load_col = permille < 500u ? COL_OK_GREEN : permille < 800u ? COL_WARN_AMBER : COL_ERR_RED;
        bar_update(s_dash_load_bar, (int32_t)permille, load_col);

        LBL_FMT(s_dash_fps_val, s_txt_dash_fps, "%lu fps", (unsigned long)st->fps);

        if (st->rx_total < 10000u) {
            LBL_FMT(s_dash_fps_sub, s_txt_dash_fps_sub, "Rx Total: %lu", (unsigned long)st->rx_total);
        } else {
            LBL_FMT(s_dash_fps_sub, s_txt_dash_fps_sub, "Rx Total: %.1fk", (double)st->rx_total / 1000.0);
        }

        if (is_bus_off) {
            LBL_SET(s_dash_health_st, s_txt_dash_health_st, "BUS-OFF");
            label_color(s_dash_health_st, COL_ERR_RED);
            LBL_SET(s_dash_act_lbl, s_txt_dash_act, "RECOVER BUS");
            obj_bg_color(s_dash_act_btn, COL_ERR_RED, 0);
            label_color(s_dash_act_lbl, COL_TEXT_PRI);
        } else {
            if (is_passive) {
                LBL_SET(s_dash_health_st, s_txt_dash_health_st, "PASSIVE WARN");
                label_color(s_dash_health_st, COL_WARN_AMBER);
            } else {
                LBL_SET(s_dash_health_st, s_txt_dash_health_st, "NORMAL (OK)");
                label_color(s_dash_health_st, COL_OK_GREEN);
            }
            LBL_SET(s_dash_act_lbl, s_txt_dash_act, s_frozen ? "RESUME" : "PAUSE");
            obj_bg_color(s_dash_act_btn, s_frozen ? COL_BG_CARD_ACT : COL_BG_CARD, 0);
            label_color(s_dash_act_lbl, s_frozen ? COL_ACCENT_CYAN : COL_TEXT_PRI);
        }

        LBL_FMT(s_dash_health_tec, s_txt_dash_health_tec, "TEC: %lu | REC: %lu",
                (unsigned long)st->tec, (unsigned long)st->rec);
        LBL_FMT(s_dash_health_err, s_txt_dash_health_err, "Errors: %lu",
                (unsigned long)st->bus_errors);
    }

    /* 3. View 1: DIAG */
    if (s_current_view == VIEW_DIAG) {
        LBL_FMT(s_diag_tec_val, s_txt_diag_tec, "%lu", (unsigned long)st->tec);
        bar_update(s_diag_tec_bar, st->tec > 255u ? 255 : (int32_t)st->tec,
                   st->tec > 127u ? COL_ERR_RED : st->tec > 96u ? COL_WARN_AMBER : COL_OK_GREEN);

        LBL_FMT(s_diag_rec_val, s_txt_diag_rec, "%lu", (unsigned long)st->rec);
        bar_update(s_diag_rec_bar, st->rec > 255u ? 255 : (int32_t)st->rec,
                   st->rec > 127u ? COL_ERR_RED : st->rec > 96u ? COL_WARN_AMBER : COL_OK_GREEN);

        LBL_FMT(s_diag_err_val,  s_txt_diag_err,  "%lu", (unsigned long)st->bus_errors);
        LBL_FMT(s_diag_boff_val, s_txt_diag_boff, "%lu", (unsigned long)st->bus_off);
        LBL_FMT(s_diag_ovf_val,  s_txt_diag_ovf,  "%lu", (unsigned long)st->rx_missed);

        if (is_bus_off) {
            LBL_SET(s_diag_act_lbl, s_txt_diag_act, "RECOVER BUS");
            obj_bg_color(s_diag_act_btn, COL_ERR_RED, 0);
        } else {
            LBL_SET(s_diag_act_lbl, s_txt_diag_act, "CLEAR STATS");
            obj_bg_color(s_diag_act_btn, COL_BG_CARD_ACT, 0);
        }

        if (sd.recording) {
            LBL_FMT(s_diag_sd_st_val, s_txt_diag_sd_st, "REC #%02lu", (unsigned long)sd.files);
            label_color(s_diag_sd_st_val, COL_ERR_RED);
        } else if (sd.mounted) {
            LBL_SET(s_diag_sd_st_val, s_txt_diag_sd_st, "IDLE (OK)");
            label_color(s_diag_sd_st_val, COL_OK_GREEN);
        } else {
            LBL_SET(s_diag_sd_st_val, s_txt_diag_sd_st, "NO SD");
            label_color(s_diag_sd_st_val, COL_TEXT_MUTED);
        }

        if (sd.mounted) {
            if (sd.free_kb > 1024u * 1024u) {
                LBL_FMT(s_diag_sd_free_val, s_txt_diag_sd_free, "%.1f GB",
                        (double)sd.free_kb / (1024.0 * 1024.0));
            } else {
                LBL_FMT(s_diag_sd_free_val, s_txt_diag_sd_free, "%lu MB",
                        (unsigned long)(sd.free_kb / 1024u));
            }
        } else {
            LBL_SET(s_diag_sd_free_val, s_txt_diag_sd_free, "--");
        }

        LBL_FMT(s_diag_sd_drop_val, s_txt_diag_sd_drop, "%lu", (unsigned long)sd.dropped);
    }

    /* 4. View 2: SET */
    if (s_current_view == VIEW_SET) {
        const can_bitrate_t cur_br = twai_current_bitrate();
        const int cur_bl = bsp_backlight_get();
        for (int i = 0; i < 4; i++) {
            const bool act = (BITRATES[i] == cur_br);
            obj_bg_color(s_set_br_btn[i], act ? COL_ACCENT_CYAN : COL_BG_CARD, 0);
            label_color(lv_obj_get_child(s_set_br_btn[i], 0), act ? COL_BG_ROOT : COL_TEXT_PRI);
        }
        for (int i = 0; i < 3; i++) {
            const bool act = (MODES[i] == cur_mode);
            obj_bg_color(s_set_mode_btn[i], act ? COL_ACCENT_CYAN : COL_BG_CARD, 0);
            label_color(lv_obj_get_child(s_set_mode_btn[i], 0), act ? COL_BG_ROOT : COL_TEXT_PRI);
        }
        for (int i = 0; i < 4; i++) {
            const bool act = (BRIGHTNESS_LEVELS[i] == cur_bl);
            obj_bg_color(s_set_bl_btn[i], act ? COL_ACCENT_CYAN : COL_BG_CARD, 0);
            label_color(lv_obj_get_child(s_set_bl_btn[i], 0), act ? COL_BG_ROOT : COL_TEXT_PRI);
        }

        if (conn == UI_CONN_ONLINE) {
            LBL_FMT(s_set_host_link_lbl, s_txt_set_host_link,
                    "Status: ONLINE  (RTT: %lu ms)", (unsigned long)rtt_ms);
            label_color(s_set_host_link_lbl, COL_OK_GREEN);
        } else if (conn == UI_CONN_HOST) {
            LBL_SET(s_set_host_link_lbl, s_txt_set_host_link,
                    "Status: CONNECTING TO HOST...");
            label_color(s_set_host_link_lbl, COL_ACCENT_CYAN);
        } else if (conn == UI_CONN_RECONNECT) {
            LBL_SET(s_set_host_link_lbl, s_txt_set_host_link,
                    "Status: RECONNECTING...");
            label_color(s_set_host_link_lbl, COL_WARN_AMBER);
        } else {
            LBL_SET(s_set_host_link_lbl, s_txt_set_host_link,
                    "Status: JOINING WI-FI...");
            label_color(s_set_host_link_lbl, COL_WARN_AMBER);
        }
    }
}

/* ---------------------------------------------------------------- Public API */
void linsight_ui_init(void)
{
    lv_obj_t *root = lv_scr_act();
    lv_obj_set_style_bg_color(root, COL_BG_ROOT, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Top bar */
    build_top_bar(root);

    /* 2. Main Views */
    for (int i = 0; i < VIEW_COUNT; i++) {
        s_views[i] = card_new(root, 0, VIEWPORT_Y, UI_W, VIEWPORT_H);
        lv_obj_set_style_bg_color(s_views[i], COL_BG_ROOT, 0);
        lv_obj_set_style_bg_opa(s_views[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_views[i], 0, 0);
        lv_obj_set_style_radius(s_views[i], 0, 0);
    }

    build_dash_view(s_views[VIEW_DASH]);
    build_diag_view(s_views[VIEW_DIAG]);
    build_set_view(s_views[VIEW_SET]);

    /* 3. Bottom dock */
    build_bottom_dock(root);

    /* 4. Toast overlay */
    s_toast = card_new(root, 50, 96, 220, 36);
    lv_obj_set_style_bg_color(s_toast, COL_BG_CARD_ACT, 0);
    lv_obj_set_style_border_color(s_toast, COL_ACCENT_CYAN, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    s_toast_lbl = lbl_new(s_toast, "", 0, 0, &lv_font_montserrat_14, COL_TEXT_PRI);
    lv_obj_center(s_toast_lbl);
    object_show(s_toast, false);

    /* Initial state */
    view_show(VIEW_DASH);

    /* 20 Hz tick timer */
    lv_timer_create(ui_tick_cb, 50, NULL);

    /* Boot splash animation */
    splash_create();
}

void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms)
{
    const uint32_t clamped = rtt_ms > 0xFFFFFFu ? 0xFFFFFFu : rtt_ms;
    const uint32_t packed = ((uint32_t)state << 24) | clamped;
    __atomic_store_n(&s_conn_mailbox, packed, __ATOMIC_RELAXED);
}

void linsight_ui_handle_key(bool long_press)
{
    lvgl_port_lock(0);
    if (long_press) {
        /* Context-sensitive tactical long press */
        if (s_current_view == VIEW_DASH) {
            dash_action_cb(NULL);
        } else if (s_current_view == VIEW_DIAG) {
            diag_action_cb(NULL);
        } else {
            view_show(VIEW_DASH);
        }
    } else {
        view_show((view_id_t)((s_current_view + 1) % VIEW_COUNT));
    }
    lvgl_port_unlock();
}