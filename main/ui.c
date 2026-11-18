/*
 * LinSight CAN analyzer UI (320x240, LVGL 8.3)
 * Two pages: DASH (live metrics) and SETUP (bitrate / mode / host link).
 * The 20 Hz tick only reads bounded snapshots; no SD, I2C, NVS or network I/O
 * runs inside the LVGL lock.
 */

#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
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
#define CARD_FULL_W     (UI_W - (PAD_X * 2)) /* 308 */
#define MARK_BTN_X      216
#define MARK_BTN_W      78

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
    VIEW_DASH = 0,
    VIEW_SET = 1,
    VIEW_COUNT = 2
} view_id_t;

typedef enum {
    CFG_BITRATE = 1,
    CFG_MODE = 2,
} cfg_kind_t;

/* --------------------------------------------------------------- statics */
static lv_obj_t *s_views[VIEW_COUNT];
static view_id_t s_current_view = VIEW_DASH;

/* Top bar */
static lv_obj_t *s_top_bar;
static lv_obj_t *s_top_state_pill, *s_top_state_lbl;
static lv_obj_t *s_top_cfg_lbl, *s_top_link_lbl;

/* Bottom dock */
static lv_obj_t *s_dock;
static lv_obj_t *s_dock_btn[VIEW_COUNT];
static lv_obj_t *s_dock_line[VIEW_COUNT];
static lv_obj_t *s_dock_lbl[VIEW_COUNT];

/* DASH */
static lv_obj_t *s_dash_load_val, *s_dash_load_bar;
static lv_obj_t *s_dash_fps_val, *s_dash_total_lbl;
static lv_obj_t *s_dash_health_lbl, *s_dash_sd_lbl;

/* SETUP */
static lv_obj_t *s_set_br_btn[4];
static lv_obj_t *s_set_mode_btn[2];
static lv_obj_t *s_set_host_lbl;

/* Toast */
static lv_obj_t *s_toast, *s_toast_lbl;
static uint32_t s_toast_tick;

/* Static string buffers (no heap allocation on the 20 Hz tick) */
static char s_txt_top_state[12], s_txt_top_cfg[24], s_txt_top_link[20];
static char s_txt_dash_load[12], s_txt_dash_fps[12], s_txt_dash_total[20];
static char s_txt_dash_health[48], s_txt_dash_sd[48];
static char s_txt_set_host[56];
static char s_txt_toast[40];

/* Runtime state */
static uint32_t s_conn_mailbox = (uint32_t)UI_CONN_WIFI << 24;

/* Host endpoint published by net_task; a short critical section is enough. */
static portMUX_TYPE s_ep_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_ep_ip;
static uint16_t s_ep_port;
static bool s_ep_valid;

/* Deferred CAN reconfiguration (never run from the LVGL task). */
static volatile bool s_cfg_busy;
static volatile uint32_t s_cfg_req;
static volatile int32_t s_cfg_result;

static const can_bitrate_t BITRATES[4] = {
    CAN_BITRATE_125K, CAN_BITRATE_250K, CAN_BITRATE_500K, CAN_BITRATE_1M};
static const char *BITRATE_LABELS[4] = {"125K", "250K", "500K", "1M"};
static const can_mode_t MODES[2] = {CAN_MODE_LISTEN_ONLY, CAN_MODE_NORMAL};
static const char *MODE_LABELS[2] = {"LISTEN", "NORMAL"};

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
static lv_obj_t *card_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h)
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

static lv_obj_t *btn_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                         lv_coord_t h, lv_color_t bg, lv_event_cb_t cb, void *user_data)
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

static lv_obj_t *bar_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                         lv_coord_t h, int32_t max, lv_color_t ind_col)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, max);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, COL_BG_ROOT, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, ind_col, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 3, LV_PART_INDICATOR);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/* ------------------------------------------------------------ Toast / View */
static void toast_show(const char *msg)
{
    if (!s_toast) return;
    LBL_SET(s_toast_lbl, s_txt_toast, msg);
    s_toast_tick = lv_tick_get();
    object_show(s_toast, true);
    lv_obj_move_foreground(s_toast);
}

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

static void mark_event_cb(lv_event_t *e)
{
    (void)e;
    toast_show(sd_store_mark("MANUAL", 0, 0) ? "MARK SAVED" : "NO RECORDING");
}

static void cfg_apply_task(void *arg)
{
    (void)arg;
    const uint32_t req = __atomic_load_n(&s_cfg_req, __ATOMIC_RELAXED);
    const cfg_kind_t kind = (cfg_kind_t)(req >> 8);
    const uint8_t idx = (uint8_t)req;

    can_bitrate_t bitrate = twai_current_bitrate();
    can_mode_t mode = twai_current_mode();
    if (kind == CFG_BITRATE) {
        bitrate = BITRATES[idx];
    } else {
        mode = MODES[idx];
    }

    const esp_err_t err = twai_capture_restart(bitrate, mode);
    __atomic_store_n(&s_cfg_result, err == ESP_OK ? 1 : -1, __ATOMIC_RELEASE);
    s_cfg_busy = false;
    vTaskDelete(NULL);
}

static void cfg_request(cfg_kind_t kind, int idx)
{
    if (s_cfg_busy || s_cfg_result != 0) {
        toast_show("APPLYING...");
        return;
    }
    if ((kind == CFG_BITRATE && BITRATES[idx] == twai_current_bitrate()) ||
        (kind == CFG_MODE && MODES[idx] == twai_current_mode())) {
        return;
    }

    __atomic_store_n(&s_cfg_req, ((uint32_t)kind << 8) | (uint32_t)idx, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_result, 0, __ATOMIC_RELEASE);
    s_cfg_busy = true;
    if (xTaskCreate(cfg_apply_task, "cfg_apply", 4096, NULL, 3, NULL) != pdPASS) {
        s_cfg_busy = false;
        __atomic_store_n(&s_cfg_result, -1, __ATOMIC_RELEASE);
    } else {
        toast_show("APPLYING...");
    }
}

static void bitrate_click_cb(lv_event_t *e)
{
    cfg_request(CFG_BITRATE, (int)(intptr_t)lv_event_get_user_data(e));
}

static void mode_click_cb(lv_event_t *e)
{
    cfg_request(CFG_MODE, (int)(intptr_t)lv_event_get_user_data(e));
}

/* ------------------------------------------------------------- View Builders */
static void build_top_bar(lv_obj_t *parent)
{
    s_top_bar = card_new(parent, 0, 0, UI_W, TOP_H);
    lv_obj_set_style_bg_color(s_top_bar, COL_BG_ROOT, 0);
    lv_obj_set_style_radius(s_top_bar, 0, 0);
    lv_obj_set_style_border_side(s_top_bar, LV_BORDER_SIDE_BOTTOM, 0);

    s_top_state_pill = card_new(s_top_bar, PAD_X, 3, 76, 18);
    lv_obj_set_style_radius(s_top_state_pill, 9, 0);
    lv_obj_set_style_border_width(s_top_state_pill, 0, 0);
    lv_obj_set_style_bg_color(s_top_state_pill, COL_OK_GREEN, 0);

    snprintf(s_txt_top_state, sizeof(s_txt_top_state), "RUNNING");
    s_top_state_lbl = lbl_new(s_top_state_pill, s_txt_top_state, 0, 0,
                              &lv_font_montserrat_12, COL_BG_ROOT);
    lv_obj_center(s_top_state_lbl);

    snprintf(s_txt_top_cfg, sizeof(s_txt_top_cfg), "500K • LISTEN");
    s_top_cfg_lbl = lbl_new(s_top_bar, s_txt_top_cfg, 90, 5, &lv_font_montserrat_12,
                            COL_TEXT_PRI);

    snprintf(s_txt_top_link, sizeof(s_txt_top_link), "WIFI ...");
    s_top_link_lbl = lbl_new(s_top_bar, s_txt_top_link, 200, 5, &lv_font_montserrat_12,
                             COL_TEXT_MUTED);
    lv_obj_set_width(s_top_link_lbl, 114);
    lv_obj_set_style_text_align(s_top_link_lbl, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_bottom_dock(lv_obj_t *parent)
{
    s_dock = card_new(parent, 0, BOT_Y, UI_W, BOT_H);
    lv_obj_set_style_bg_color(s_dock, COL_BG_ROOT, 0);
    lv_obj_set_style_radius(s_dock, 0, 0);
    lv_obj_set_style_border_side(s_dock, LV_BORDER_SIDE_TOP, 0);

    const char *names[VIEW_COUNT] = {"DASH", "SETUP"};
    const lv_coord_t w = UI_W / VIEW_COUNT;

    for (int i = 0; i < VIEW_COUNT; i++) {
        s_dock_btn[i] = btn_new(s_dock, i * w, 0, w, BOT_H, COL_BG_ROOT, dock_click_cb,
                                (void *)(intptr_t)i);
        s_dock_line[i] = card_new(s_dock_btn[i], 0, 0, w, 2);
        lv_obj_set_style_bg_color(s_dock_line[i], COL_ACCENT_CYAN, 0);
        lv_obj_set_style_border_width(s_dock_line[i], 0, 0);

        s_dock_lbl[i] = lbl_new(s_dock_btn[i], names[i], 0, 0, &lv_font_montserrat_14,
                                i == 0 ? COL_TEXT_PRI : COL_TEXT_SEC);
        lv_obj_center(s_dock_lbl[i]);
    }
}

static void build_dash_view(lv_obj_t *parent)
{
    lv_obj_t *c1 = card_new(parent, PAD_X, PAD_Y, CARD_FULL_W, 92);
    lbl_new(c1, "BUS LOAD", 14, 12, &lv_font_montserrat_12, COL_TEXT_SEC);
    snprintf(s_txt_dash_load, sizeof(s_txt_dash_load), "0.0%%");
    s_dash_load_val = lbl_new(c1, s_txt_dash_load, 14, 28, &lv_font_montserrat_24,
                              COL_TEXT_PRI);
    s_dash_load_bar = bar_new(c1, 14, 73, 150, 6, 1000, COL_OK_GREEN);

    lv_obj_t *divider = card_new(c1, 176, 14, 1, 64);
    lv_obj_set_style_bg_color(divider, COL_BORDER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);

    lbl_new(c1, "FPS", 194, 12, &lv_font_montserrat_12, COL_TEXT_SEC);
    snprintf(s_txt_dash_fps, sizeof(s_txt_dash_fps), "0");
    s_dash_fps_val = lbl_new(c1, s_txt_dash_fps, 194, 28, &lv_font_montserrat_24,
                             COL_TEXT_PRI);
    snprintf(s_txt_dash_total, sizeof(s_txt_dash_total), "TOTAL 0");
    s_dash_total_lbl = lbl_new(c1, s_txt_dash_total, 194, 70, &lv_font_montserrat_12,
                               COL_TEXT_SEC);

    lv_obj_t *c2 = card_new(parent, PAD_X, 106, CARD_FULL_W, 68);
    snprintf(s_txt_dash_health, sizeof(s_txt_dash_health), "TEC 0 • REC 0 • ERR 0");
    s_dash_health_lbl = lbl_new(c2, s_txt_dash_health, 14, 14, &lv_font_montserrat_12,
                                COL_TEXT_SEC);
    lv_obj_set_width(s_dash_health_lbl, 194);
    lv_label_set_long_mode(s_dash_health_lbl, LV_LABEL_LONG_DOT);
    snprintf(s_txt_dash_sd, sizeof(s_txt_dash_sd), "SD NO CARD");
    s_dash_sd_lbl = lbl_new(c2, s_txt_dash_sd, 14, 40, &lv_font_montserrat_12,
                            COL_TEXT_MUTED);
    lv_obj_set_width(s_dash_sd_lbl, 194);
    lv_label_set_long_mode(s_dash_sd_lbl, LV_LABEL_LONG_DOT);

    lv_obj_t *mark = btn_new(c2, MARK_BTN_X, 19, MARK_BTN_W, 30, COL_BG_CARD, mark_event_cb,
                             NULL);
    lv_obj_set_style_border_color(mark, COL_WARN_AMBER, 0);
    lv_obj_t *mark_lbl = lbl_new(mark, "MARK", 0, 0, &lv_font_montserrat_12,
                                 COL_WARN_AMBER);
    lv_obj_center(mark_lbl);
}

static void build_set_view(lv_obj_t *parent)
{
    lv_obj_t *l = lbl_new(parent, "BITRATE", PAD_X, 12, &lv_font_montserrat_12,
                          COL_TEXT_SEC);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    for (int i = 0; i < 4; i++) {
        s_set_br_btn[i] = btn_new(parent, PAD_X + i * 79, 30, 71, 36, COL_BG_CARD,
                                  bitrate_click_cb, (void *)(intptr_t)i);
        lv_obj_t *bl = lbl_new(s_set_br_btn[i], BITRATE_LABELS[i], 0, 0,
                               &lv_font_montserrat_14, COL_TEXT_PRI);
        lv_obj_center(bl);
    }

    l = lbl_new(parent, "MODE", PAD_X, 80, &lv_font_montserrat_12, COL_TEXT_SEC);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    for (int i = 0; i < 2; i++) {
        s_set_mode_btn[i] = btn_new(parent, PAD_X + i * 158, 98, 150, 36, COL_BG_CARD,
                                    mode_click_cb, (void *)(intptr_t)i);
        lv_obj_t *bl = lbl_new(s_set_mode_btn[i], MODE_LABELS[i], 0, 0,
                               &lv_font_montserrat_14, COL_TEXT_PRI);
        lv_obj_center(bl);
    }

    lv_obj_t *footer = card_new(parent, PAD_X, 150, CARD_FULL_W, 24);
    snprintf(s_txt_set_host, sizeof(s_txt_set_host), "NO HOST LINK");
    s_set_host_lbl = lbl_new(footer, s_txt_set_host, 10, 5, &lv_font_montserrat_12,
                             COL_TEXT_MUTED);
    lv_obj_set_width(s_set_host_lbl, CARD_FULL_W - 20);
    lv_obj_set_style_text_align(s_set_host_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_set_host_lbl, LV_LABEL_LONG_DOT);
}

/* --------------------------------------------------------------- 20Hz Tick */
static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    const uint32_t conn_packed = __atomic_load_n(&s_conn_mailbox, __ATOMIC_RELAXED);
    const linsight_conn_t conn = (linsight_conn_t)(conn_packed >> 24);
    const uint32_t rtt_ms = conn_packed & 0xFFFFFFu;

    if (s_toast && !lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(s_toast_tick) > 1500u) {
        object_show(s_toast, false);
    }

    const int32_t cfg_result = __atomic_load_n(&s_cfg_result, __ATOMIC_ACQUIRE);
    if (cfg_result != 0) {
        toast_show(cfg_result > 0 ? "CONFIG APPLIED" : "CONFIG FAILED");
        __atomic_store_n(&s_cfg_result, 0, __ATOMIC_RELEASE);
    }

    can_stats_t st;
    twai_stats_snapshot(&st);
    sd_status_t sd = {0};
    sd_store_get_status(&sd);

    const can_bitrate_t bitrate = twai_current_bitrate();
    const can_mode_t mode = twai_current_mode();
    const bool bus_off = st.state == CAN_STATE_BUS_OFF;
    const bool recovering = st.state == CAN_STATE_RECOVERING;
    const bool passive = st.tec > 127u || st.rec > 127u;

    /* Top bar: run state, active configuration, host link. */
    if (bus_off) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "BUS-OFF");
        obj_bg_color(s_top_state_pill, COL_ERR_RED, 0);
        label_color(s_top_state_lbl, COL_TEXT_PRI);
    } else if (recovering) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "RECOVER");
        obj_bg_color(s_top_state_pill, COL_WARN_AMBER, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    } else if (passive) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "PASSIVE");
        obj_bg_color(s_top_state_pill, COL_WARN_AMBER, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    } else if (st.state == CAN_STATE_RUNNING) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "RUNNING");
        obj_bg_color(s_top_state_pill, COL_OK_GREEN, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    } else {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "STOPPED");
        obj_bg_color(s_top_state_pill, COL_BG_CARD_ACT, 0);
        label_color(s_top_state_lbl, COL_TEXT_SEC);
    }

    LBL_FMT(s_top_cfg_lbl, s_txt_top_cfg, "%s • %s", can_bitrate_name(bitrate),
            can_mode_name(mode));

    if (conn == UI_CONN_ONLINE) {
        LBL_FMT(s_top_link_lbl, s_txt_top_link, "HOST %lums", (unsigned long)rtt_ms);
        label_color(s_top_link_lbl, COL_OK_GREEN);
    } else if (conn == UI_CONN_HOST) {
        LBL_SET(s_top_link_lbl, s_txt_top_link, "HOST ...");
        label_color(s_top_link_lbl, COL_ACCENT_CYAN);
    } else if (conn == UI_CONN_RECONNECT) {
        LBL_SET(s_top_link_lbl, s_txt_top_link, "RETRY");
        label_color(s_top_link_lbl, COL_WARN_AMBER);
    } else {
        LBL_SET(s_top_link_lbl, s_txt_top_link, "WIFI ...");
        label_color(s_top_link_lbl, COL_TEXT_MUTED);
    }

    if (s_current_view == VIEW_DASH) {
        const uint32_t permille = st.load_permille > 1000u ? 1000u : st.load_permille;
        LBL_FMT(s_dash_load_val, s_txt_dash_load, "%lu.%lu%%",
                (unsigned long)(permille / 10u), (unsigned long)(permille % 10u));
        const lv_color_t load_col = permille < 500u ? COL_OK_GREEN
                                    : permille < 800u ? COL_WARN_AMBER
                                                      : COL_ERR_RED;
        bar_update(s_dash_load_bar, (int32_t)permille, load_col);

        LBL_FMT(s_dash_fps_val, s_txt_dash_fps, "%lu", (unsigned long)st.fps);
        if (st.rx_total < 10000u) {
            LBL_FMT(s_dash_total_lbl, s_txt_dash_total, "TOTAL %lu",
                    (unsigned long)st.rx_total);
        } else {
            LBL_FMT(s_dash_total_lbl, s_txt_dash_total, "TOTAL %.1fk",
                    (double)st.rx_total / 1000.0);
        }

        LBL_FMT(s_dash_health_lbl, s_txt_dash_health, "TEC %lu • REC %lu • ERR %lu",
                (unsigned long)st.tec, (unsigned long)st.rec,
                (unsigned long)st.bus_errors);
        label_color(s_dash_health_lbl,
                    bus_off ? COL_ERR_RED
                    : (passive || st.tec > 96u) ? COL_WARN_AMBER
                                                : COL_TEXT_SEC);

        if (!sd.mounted) {
            LBL_SET(s_dash_sd_lbl, s_txt_dash_sd, "SD NO CARD");
            label_color(s_dash_sd_lbl, COL_TEXT_MUTED);
        } else {
            char size[12];
            if (sd.free_kb > 1024u * 1024u) {
                snprintf(size, sizeof(size), "%.1f GB",
                         (double)sd.free_kb / (1024.0 * 1024.0));
            } else {
                snprintf(size, sizeof(size), "%lu MB",
                         (unsigned long)(sd.free_kb / 1024u));
            }
            if (sd.recording) {
                if (sd.dropped > 0) {
                    LBL_FMT(s_dash_sd_lbl, s_txt_dash_sd, "SD %02lu • %s • DROP %lu",
                            (unsigned long)sd.files, size, (unsigned long)sd.dropped);
                } else {
                    LBL_FMT(s_dash_sd_lbl, s_txt_dash_sd, "SD %02lu • %s",
                            (unsigned long)sd.files, size);
                }
            } else {
                LBL_FMT(s_dash_sd_lbl, s_txt_dash_sd, "SD IDLE • %s", size);
            }
            label_color(s_dash_sd_lbl, sd.recording ? COL_OK_GREEN : COL_TEXT_SEC);
        }
    }

    if (s_current_view == VIEW_SET) {
        for (int i = 0; i < 4; i++) {
            const bool active = (BITRATES[i] == bitrate);
            obj_bg_color(s_set_br_btn[i], active ? COL_ACCENT_CYAN : COL_BG_CARD, 0);
            label_color(lv_obj_get_child(s_set_br_btn[i], 0),
                        active ? COL_BG_ROOT : COL_TEXT_PRI);
        }
        for (int i = 0; i < 2; i++) {
            const bool active = (MODES[i] == mode);
            obj_bg_color(s_set_mode_btn[i], active ? COL_ACCENT_CYAN : COL_BG_CARD, 0);
            label_color(lv_obj_get_child(s_set_mode_btn[i], 0),
                        active ? COL_BG_ROOT : COL_TEXT_PRI);
        }

        portENTER_CRITICAL(&s_ep_lock);
        const uint32_t ip = s_ep_ip;
        const uint16_t port = s_ep_port;
        const bool valid = s_ep_valid;
        portEXIT_CRITICAL(&s_ep_lock);

        if (valid) {
            const uint8_t *o = (const uint8_t *)&ip;
            if (conn == UI_CONN_ONLINE) {
                LBL_FMT(s_set_host_lbl, s_txt_set_host,
                        "HOST %u.%u.%u.%u:%u • %lu ms", (unsigned)o[0], (unsigned)o[1],
                        (unsigned)o[2], (unsigned)o[3], (unsigned)port,
                        (unsigned long)rtt_ms);
                label_color(s_set_host_lbl, COL_OK_GREEN);
            } else {
                LBL_FMT(s_set_host_lbl, s_txt_set_host, "HOST %u.%u.%u.%u:%u • OFFLINE",
                        (unsigned)o[0], (unsigned)o[1], (unsigned)o[2], (unsigned)o[3],
                        (unsigned)port);
                label_color(s_set_host_lbl, COL_TEXT_SEC);
            }
        } else {
            LBL_SET(s_set_host_lbl, s_txt_set_host, "NO HOST LINK");
            label_color(s_set_host_lbl, COL_TEXT_MUTED);
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

    build_top_bar(root);

    for (int i = 0; i < VIEW_COUNT; i++) {
        s_views[i] = card_new(root, 0, VIEWPORT_Y, UI_W, VIEWPORT_H);
        lv_obj_set_style_bg_color(s_views[i], COL_BG_ROOT, 0);
        lv_obj_set_style_bg_opa(s_views[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_views[i], 0, 0);
        lv_obj_set_style_radius(s_views[i], 0, 0);
    }

    build_dash_view(s_views[VIEW_DASH]);
    build_set_view(s_views[VIEW_SET]);
    build_bottom_dock(root);

    s_toast = card_new(root, 0, 0, 220, 36);
    lv_obj_center(s_toast);
    lv_obj_set_style_bg_color(s_toast, COL_BG_CARD_ACT, 0);
    lv_obj_set_style_border_color(s_toast, COL_ACCENT_CYAN, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    s_toast_lbl = lbl_new(s_toast, "", 0, 0, &lv_font_montserrat_14, COL_TEXT_PRI);
    lv_obj_center(s_toast_lbl);
    object_show(s_toast, false);

    view_show(VIEW_DASH);
    lv_timer_create(ui_tick_cb, 50, NULL);
}

void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms)
{
    const uint32_t clamped = rtt_ms > 0xFFFFFFu ? 0xFFFFFFu : rtt_ms;
    const uint32_t packed = ((uint32_t)state << 24) | clamped;
    __atomic_store_n(&s_conn_mailbox, packed, __ATOMIC_RELAXED);
}

void linsight_ui_set_host_endpoint(uint32_t ip_be, uint16_t port)
{
    portENTER_CRITICAL(&s_ep_lock);
    s_ep_ip = ip_be;
    s_ep_port = port;
    s_ep_valid = true;
    portEXIT_CRITICAL(&s_ep_lock);
}

void linsight_ui_handle_key(bool long_press)
{
    lvgl_port_lock(0);
    if (!long_press) {
        view_show((view_id_t)((s_current_view + 1) % VIEW_COUNT));
    } else if (s_current_view == VIEW_DASH) {
        mark_event_cb(NULL);
    } else {
        twai_stats_clear();
        toast_show("STATS CLEARED");
    }
    lvgl_port_unlock();
}
