/*
 * LinSight CAN analyzer UI (320x240, LVGL 8.3)
 * Precision Dark Telemetry architecture (refined with breathing room):
 *   - DASH: dual hero gauges (12-segment tri-color load bar + FPS/total/drops),
 *           4-cell diagnostic strip (TEC / REC / ERR+MISS / PSRAM RING),
 *           clean single-line SD black-box bar + MARK button.
 *   - SETUP: 4-way bitrate matrix, LISTEN / NORMAL (amber ACK warning) selector,
 *            dedicated touch [CLEAR] button, and single-line host link bar.
 *
 * Hard constraints:
 *   - Zero heap allocation on the 20 Hz tick (all strings use static buffers
 *     with strcmp dirty-checking before lv_label_set_text_static).
 *   - No SD, I2C, NVS, or TWAI restart inside the LVGL lock.
 */

#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "can_ring.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sd_store.h"
#include "twai.h"

/* Font macros with graceful fallback if local sdkconfig has not yet regenerated
 * from sdkconfig.defaults (where MONTSERRAT_16 and MONTSERRAT_20 are enabled). */
#define FONT_12 (&lv_font_montserrat_12)
#define FONT_14 (&lv_font_montserrat_14)
#if defined(LV_FONT_MONTSERRAT_16) && LV_FONT_MONTSERRAT_16
#define FONT_16 (&lv_font_montserrat_16)
#else
#define FONT_16 (&lv_font_montserrat_14)
#endif
#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
#define FONT_20 (&lv_font_montserrat_20)
#else
#define FONT_20 (&lv_font_montserrat_24)
#endif
#define FONT_24 (&lv_font_montserrat_24)

/* -------------------------------------------------------------- geometry */
#define UI_W                320
#define UI_H                240
#define TOP_H               24
#define VIEWPORT_Y          24
#define VIEWPORT_H          182
#define BOT_Y               206
#define BOT_H               34

#define PAD_X               6
#define CARD_FULL_W         (UI_W - (PAD_X * 2)) /* 308 */
#define LOAD_SEGMENTS       12

/* ---------------------------------------------------------------- colors */
#define COL_BG_ROOT         lv_color_hex(0x070A0F)
#define COL_BG_SURFACE      lv_color_hex(0x101620)
#define COL_BG_ELEVATED     lv_color_hex(0x182232)
#define COL_BG_PRESSED      lv_color_hex(0x26354D)
#define COL_BG_AMBER_DIM    lv_color_hex(0x271B09)

#define COL_BORDER_SUBTLE   lv_color_hex(0x1E293B)
#define COL_BORDER_FOCUS    lv_color_hex(0x334155)

#define COL_TEXT_PRI        lv_color_hex(0xF8FAFC)
#define COL_TEXT_SEC        lv_color_hex(0x94A3B8)
#define COL_TEXT_DIM        lv_color_hex(0x475569)

#define COL_CYAN            lv_color_hex(0x06B6D4)
#define COL_GREEN           lv_color_hex(0x10B981)
#define COL_AMBER           lv_color_hex(0xF59E0B)
#define COL_RED             lv_color_hex(0xEF4444)

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

/* Top status bar */
static lv_obj_t *s_top_bar;
static lv_obj_t *s_top_state_pill, *s_top_state_lbl;
static lv_obj_t *s_top_cfg_lbl, *s_top_link_lbl;

/* Bottom navigation dock */
static lv_obj_t *s_dock;
static lv_obj_t *s_dock_btn[VIEW_COUNT];
static lv_obj_t *s_dock_line[VIEW_COUNT];
static lv_obj_t *s_dock_lbl[VIEW_COUNT];

/* DASH: Hero Left (Bus Load) */
static lv_obj_t *s_dash_load_val_lbl;
static lv_obj_t *s_dash_load_seg[LOAD_SEGMENTS];

/* DASH: Hero Right (FPS & Totals) */
static lv_obj_t *s_dash_drop_lbl;
static lv_obj_t *s_dash_fps_val_lbl;
static lv_obj_t *s_dash_total_lbl;

/* DASH: 4-Cell Diagnostic Strip (TEC / REC / ERR+MISS / PSRAM RING) */
static lv_obj_t *s_cell_card[4];
static lv_obj_t *s_cell_bar[4];
static lv_obj_t *s_cell_val[4];

/* DASH: SD Black-Box Bar & MARK Shutter */
static lv_obj_t *s_sd_badge, *s_sd_badge_lbl;
static lv_obj_t *s_sd_main_lbl;

/* SETUP: Bitrate Matrix, Mode & Clear Row, Host Link Card */
static lv_obj_t *s_set_br_btn[4];
static lv_obj_t *s_set_br_line[4];
static lv_obj_t *s_set_br_lbl[4];

static lv_obj_t *s_set_mode_btn[2];
static lv_obj_t *s_set_mode_line[2];
static lv_obj_t *s_set_mode_lbl[2];

static lv_obj_t *s_set_host_ep_lbl;
static lv_obj_t *s_set_host_pill, *s_set_host_pill_lbl;

/* Toast overlay */
static lv_obj_t *s_toast, *s_toast_bar, *s_toast_lbl;
static uint32_t s_toast_tick;

/* Static string buffers (zero heap allocation on the 20 Hz tick) */
static char s_txt_top_state[12];
static char s_txt_top_cfg[24];
static char s_txt_top_link[20];

static char s_txt_load_val[12];
static char s_txt_drop[16];
static char s_txt_fps_val[12];
static char s_txt_total[24];

static char s_txt_cell[4][12];

static char s_txt_sd_badge[8];
static char s_txt_sd_main[36];

static char s_txt_host_ep[32];
static char s_txt_host_pill[20];
static char s_txt_toast[32];

/* Lock-free mailbox for connection state & RTT */
static uint32_t s_conn_mailbox = (uint32_t)UI_CONN_WIFI << 24;

/* Host endpoint published by net_task; protected by a short critical section */
static portMUX_TYPE s_ep_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_ep_ip;
static uint16_t s_ep_port;
static bool s_ep_valid;

/* Deferred CAN reconfiguration (runs outside the LVGL lock) */
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

static void obj_border_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t selector)
{
    if (lv_obj_get_style_border_color(obj, selector).full != color.full) {
        lv_obj_set_style_border_color(obj, color, selector);
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

#define LBL_SET(lbl, buf, text)     label_set_str((lbl), (buf), sizeof(buf), (text))
#define LBL_FMT(lbl, buf, fmt, ...) label_printf((lbl), (buf), sizeof(buf), (fmt), __VA_ARGS__)

static void object_show(lv_obj_t *obj, bool show)
{
    if (!obj) return;
    if (show) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* --------------------------------------------------------- Widget Builders */
static lv_obj_t *card_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_BG_SURFACE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, COL_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 5, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static lv_obj_t *rect_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h, lv_color_t bg, lv_coord_t radius)
{
    lv_obj_t *r = card_new(parent, x, y, w, h);
    lv_obj_set_style_bg_color(r, bg, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, radius, 0);
    return r;
}

static lv_obj_t *btn_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                         lv_coord_t h, lv_color_t bg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = card_new(parent, x, y, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_border_color(b, COL_BORDER_FOCUS, 0);
    lv_obj_set_style_bg_color(b, COL_BG_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, COL_CYAN, LV_STATE_PRESSED);
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

/* ------------------------------------------------------------ Toast / View */
static void toast_show(const char *msg, lv_color_t accent)
{
    if (!s_toast) return;
    LBL_SET(s_toast_lbl, s_txt_toast, msg);
    obj_border_color(s_toast, accent, 0);
    obj_bg_color(s_toast_bar, accent, 0);
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
        obj_bg_color(s_dock_btn[i], active ? COL_BG_ELEVATED : COL_BG_ROOT, 0);
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
    if (sd_store_mark("MANUAL", 0, 0)) {
        toast_show("MARK SAVED", COL_AMBER);
    } else {
        toast_show("NO SD CARD", COL_RED);
    }
}

static void clear_stats_cb(lv_event_t *e)
{
    (void)e;
    twai_stats_clear();
    toast_show("STATS CLEARED", COL_CYAN);
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
    __atomic_store_n(&s_cfg_busy, false, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

static void cfg_request(cfg_kind_t kind, int idx)
{
    if (__atomic_load_n(&s_cfg_busy, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_cfg_result, __ATOMIC_ACQUIRE) != 0) {
        toast_show("APPLYING...", COL_CYAN);
        return;
    }
    if ((kind == CFG_BITRATE && BITRATES[idx] == twai_current_bitrate()) ||
        (kind == CFG_MODE && MODES[idx] == twai_current_mode())) {
        return;
    }

    __atomic_store_n(&s_cfg_req, ((uint32_t)kind << 8) | (uint32_t)idx, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_result, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cfg_busy, true, __ATOMIC_RELEASE);
    if (xTaskCreate(cfg_apply_task, "cfg_apply", 4096, NULL, 3, NULL) != pdPASS) {
        __atomic_store_n(&s_cfg_busy, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_cfg_result, -1, __ATOMIC_RELEASE);
    } else {
        toast_show("APPLYING...", COL_CYAN);
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

    s_top_state_pill = rect_new(s_top_bar, PAD_X, 3, 76, 18, COL_GREEN, 9);
    snprintf(s_txt_top_state, sizeof(s_txt_top_state), "RUNNING");
    s_top_state_lbl = lbl_new(s_top_state_pill, s_txt_top_state, 0, 0,
                              FONT_12, COL_BG_ROOT);
    lv_obj_center(s_top_state_lbl);

    snprintf(s_txt_top_cfg, sizeof(s_txt_top_cfg), "500K • LISTEN");
    s_top_cfg_lbl = lbl_new(s_top_bar, s_txt_top_cfg, 90, 5, FONT_12,
                            COL_TEXT_PRI);

    snprintf(s_txt_top_link, sizeof(s_txt_top_link), "WIFI ...");
    s_top_link_lbl = lbl_new(s_top_bar, s_txt_top_link, 200, 5, FONT_12,
                             COL_TEXT_DIM);
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
        lv_obj_set_style_radius(s_dock_btn[i], 0, 0);
        lv_obj_set_style_border_width(s_dock_btn[i], 0, 0);

        s_dock_line[i] = rect_new(s_dock_btn[i], 32, 0, w - 64, 3, COL_CYAN, 1);

        s_dock_lbl[i] = lbl_new(s_dock_btn[i], names[i], 0, 1, FONT_14,
                                i == 0 ? COL_TEXT_PRI : COL_TEXT_SEC);
        lv_obj_center(s_dock_lbl[i]);
    }
}

static void build_dash_view(lv_obj_t *parent)
{
    /* ---- Row 1: Dual Hero Cards (y=5, h=76) ---- */
    lv_obj_t *c_load = card_new(parent, PAD_X, 5, 151, 76);
    lbl_new(c_load, "BUS LOAD", 12, 8, FONT_12, COL_TEXT_SEC);

    snprintf(s_txt_load_val, sizeof(s_txt_load_val), "0.0%%");
    s_dash_load_val_lbl = lbl_new(c_load, s_txt_load_val, 12, 24,
                                  FONT_24, COL_TEXT_PRI);

    /* 12-segment tri-color LED load bar inside a dark recessed trough */
    lv_obj_t *seg_well = rect_new(c_load, 10, 56, 131, 10, COL_BG_ROOT, 3);
    for (int i = 0; i < LOAD_SEGMENTS; i++) {
        s_dash_load_seg[i] = rect_new(seg_well, 2 + i * 11, 2, 8, 6, COL_BG_ELEVATED, 1);
    }

    /* Right Hero Card: Frame Rate & Totals */
    lv_obj_t *c_fps = card_new(parent, 163, 5, 151, 76);
    lbl_new(c_fps, "FPS", 12, 8, FONT_12, COL_TEXT_SEC);

    s_txt_drop[0] = '\0';
    s_dash_drop_lbl = lbl_new(c_fps, s_txt_drop, 76, 8, FONT_12, COL_RED);
    lv_obj_set_width(s_dash_drop_lbl, 63);
    lv_obj_set_style_text_align(s_dash_drop_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    object_show(s_dash_drop_lbl, false);

    snprintf(s_txt_fps_val, sizeof(s_txt_fps_val), "0");
    s_dash_fps_val_lbl = lbl_new(c_fps, s_txt_fps_val, 12, 24,
                                 FONT_24, COL_TEXT_PRI);

    snprintf(s_txt_total, sizeof(s_txt_total), "TOTAL 0");
    s_dash_total_lbl = lbl_new(c_fps, s_txt_total, 12, 54, FONT_12,
                               COL_TEXT_SEC);

    /* ---- Row 2: 4-Cell Diagnostic Strip (y=87, h=46) ---- */
    static const char *CELL_TITLES[4] = {"TEC", "REC", "ERR", "RING"};
    for (int i = 0; i < 4; i++) {
        const lv_coord_t cx = PAD_X + i * 78;
        s_cell_card[i] = card_new(parent, cx, 87, 74, 46);
        s_cell_bar[i] = rect_new(s_cell_card[i], 0, 0, 74, 2, COL_GREEN, 0);

        lbl_new(s_cell_card[i], CELL_TITLES[i], 8, 6, FONT_12,
                COL_TEXT_SEC);

        snprintf(s_txt_cell[i], sizeof(s_txt_cell[i]), i == 3 ? "0%%" : "0");
        s_cell_val[i] = lbl_new(s_cell_card[i], s_txt_cell[i], 8, 21,
                                FONT_20, COL_TEXT_PRI);
    }

    /* ---- Row 3: Clean Single-Line SD Bar + MARK Button (y=139, h=38) ---- */
    lv_obj_t *c_sd = card_new(parent, PAD_X, 139, 218, 38);
    s_sd_badge = rect_new(c_sd, 8, 8, 46, 22, COL_BG_ELEVATED, 4);
    snprintf(s_txt_sd_badge, sizeof(s_txt_sd_badge), "NO SD");
    s_sd_badge_lbl = lbl_new(s_sd_badge, s_txt_sd_badge, 0, 0, FONT_12,
                             COL_TEXT_DIM);
    lv_obj_center(s_sd_badge_lbl);

    snprintf(s_txt_sd_main, sizeof(s_txt_sd_main), "NO CARD");
    s_sd_main_lbl = lbl_new(c_sd, s_txt_sd_main, 62, 11, FONT_14,
                            COL_TEXT_DIM);
    lv_obj_set_width(s_sd_main_lbl, 148);
    lv_label_set_long_mode(s_sd_main_lbl, LV_LABEL_LONG_DOT);

    /* Clean Single-Label Amber MARK Shutter Button */
    lv_obj_t *mark_btn = btn_new(parent, 230, 139, 84, 38, COL_BG_AMBER_DIM,
                                 mark_event_cb, NULL);
    lv_obj_set_style_border_color(mark_btn, COL_AMBER, 0);
    lv_obj_t *m_lbl = lbl_new(mark_btn, "MARK", 0, 0, FONT_16, COL_AMBER);
    lv_obj_center(m_lbl);
}

static void build_set_view(lv_obj_t *parent)
{
    /* ---- Section 1: 4-Way Bitrate Matrix ---- */
    lbl_new(parent, "BITRATE", PAD_X + 2, 6, FONT_12, COL_TEXT_SEC);

    for (int i = 0; i < 4; i++) {
        const lv_coord_t bx = PAD_X + i * 78;
        s_set_br_btn[i] = btn_new(parent, bx, 22, 74, 38, COL_BG_SURFACE,
                                  bitrate_click_cb, (void *)(intptr_t)i);
        s_set_br_line[i] = rect_new(s_set_br_btn[i], 0, 0, 74, 2, COL_CYAN, 0);
        s_set_br_lbl[i] = lbl_new(s_set_br_btn[i], BITRATE_LABELS[i], 0, 0,
                                  FONT_16, COL_TEXT_PRI);
        lv_obj_center(s_set_br_lbl[i]);
    }

    /* ---- Section 2: Controller Mode & Touch CLEAR Button ---- */
    lbl_new(parent, "MODE", PAD_X + 2, 68, FONT_12, COL_TEXT_SEC);

    for (int i = 0; i < 2; i++) {
        const lv_coord_t mx = PAD_X + i * 106;
        s_set_mode_btn[i] = btn_new(parent, mx, 84, 102, 38, COL_BG_SURFACE,
                                    mode_click_cb, (void *)(intptr_t)i);
        s_set_mode_line[i] = rect_new(s_set_mode_btn[i], 0, 0, 102, 2,
                                      i == 0 ? COL_CYAN : COL_AMBER, 0);
        s_set_mode_lbl[i] = lbl_new(s_set_mode_btn[i], MODE_LABELS[i], 0, 0,
                                    FONT_16, COL_TEXT_PRI);
        lv_obj_center(s_set_mode_lbl[i]);
    }

    /* Dedicated Touch CLEAR Button (also triggered via long-press GPIO0) */
    lv_obj_t *clr_btn = btn_new(parent, 220, 84, 94, 38, COL_BG_ELEVATED,
                                clear_stats_cb, NULL);
    lv_obj_set_style_border_color(clr_btn, COL_CYAN, 0);
    lv_obj_t *clr_lbl = lbl_new(clr_btn, "CLEAR", 0, 0, FONT_16, COL_CYAN);
    lv_obj_center(clr_lbl);

    /* ---- Section 3: Clean Single-Line Host Endpoint Card ---- */
    lv_obj_t *c_host = card_new(parent, PAD_X, 134, CARD_FULL_W, 40);
    rect_new(c_host, 0, 0, 3, 40, COL_CYAN, 0);

    snprintf(s_txt_host_ep, sizeof(s_txt_host_ep), "NO HOST LINK");
    s_set_host_ep_lbl = lbl_new(c_host, s_txt_host_ep, 14, 12,
                                FONT_14, COL_TEXT_SEC);
    lv_obj_set_width(s_set_host_ep_lbl, 194);
    lv_label_set_long_mode(s_set_host_ep_lbl, LV_LABEL_LONG_DOT);

    s_set_host_pill = rect_new(c_host, 214, 9, 86, 22, COL_BG_ELEVATED, 11);
    snprintf(s_txt_host_pill, sizeof(s_txt_host_pill), "WIFI ...");
    s_set_host_pill_lbl = lbl_new(s_set_host_pill, s_txt_host_pill, 0, 0,
                                  FONT_12, COL_TEXT_DIM);
    lv_obj_center(s_set_host_pill_lbl);
}

/* --------------------------------------------------------------- 20Hz Tick */
static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    const uint32_t conn_packed = __atomic_load_n(&s_conn_mailbox, __ATOMIC_RELAXED);
    const linsight_conn_t conn = (linsight_conn_t)(conn_packed >> 24);
    const uint32_t rtt_ms = conn_packed & 0xFFFFFFu;

    if (s_toast && !lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(s_toast_tick) > 1400u) {
        object_show(s_toast, false);
    }

    const int32_t cfg_result = __atomic_load_n(&s_cfg_result, __ATOMIC_ACQUIRE);
    if (cfg_result != 0) {
        if (cfg_result > 0) {
            toast_show("CONFIG APPLIED", COL_GREEN);
        } else {
            toast_show("CONFIG FAILED", COL_RED);
        }
        __atomic_store_n(&s_cfg_result, 0, __ATOMIC_RELEASE);
    }

    can_stats_t st;
    twai_stats_snapshot(&st);
    sd_status_t sd = {0};
    sd_store_get_status(&sd);
    can_ring_info_t ring = {0};
    can_ring_get_info(&ring);

    const can_bitrate_t bitrate = twai_current_bitrate();
    const can_mode_t mode = twai_current_mode();
    const bool bus_off = (st.state == CAN_STATE_BUS_OFF);
    const bool recovering = (st.state == CAN_STATE_RECOVERING);
    const bool passive = (st.tec > 127u || st.rec > 127u);

    /* ---- Top Status Bar ---- */
    if (bus_off) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "BUS-OFF");
        obj_bg_color(s_top_state_pill, COL_RED, 0);
        label_color(s_top_state_lbl, COL_TEXT_PRI);
    } else if (recovering) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "RECOVER");
        obj_bg_color(s_top_state_pill, COL_AMBER, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    } else if (passive) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "PASSIVE");
        obj_bg_color(s_top_state_pill, COL_AMBER, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    } else if (st.state == CAN_STATE_RUNNING) {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "RUNNING");
        obj_bg_color(s_top_state_pill, COL_GREEN, 0);
        label_color(s_top_state_lbl, COL_BG_ROOT);
    } else {
        LBL_SET(s_top_state_lbl, s_txt_top_state, "STOPPED");
        obj_bg_color(s_top_state_pill, COL_BG_ELEVATED, 0);
        label_color(s_top_state_lbl, COL_TEXT_SEC);
    }

    LBL_FMT(s_top_cfg_lbl, s_txt_top_cfg, "%s • %s", can_bitrate_name(bitrate),
            can_mode_name(mode));
    label_color(s_top_cfg_lbl, mode == CAN_MODE_NORMAL ? COL_AMBER : COL_TEXT_PRI);

    if (conn == UI_CONN_ONLINE) {
        LBL_FMT(s_top_link_lbl, s_txt_top_link, "HOST %lums", (unsigned long)rtt_ms);
        label_color(s_top_link_lbl, COL_GREEN);
    } else if (conn == UI_CONN_HOST) {
        LBL_SET(s_top_link_lbl, s_txt_top_link, "HOST ...");
        label_color(s_top_link_lbl, COL_CYAN);
    } else if (conn == UI_CONN_RECONNECT) {
        LBL_SET(s_top_link_lbl, s_txt_top_link, "RETRY");
        label_color(s_top_link_lbl, COL_AMBER);
    } else {
        LBL_SET(s_top_link_lbl, s_txt_top_link, "WIFI ...");
        label_color(s_top_link_lbl, COL_TEXT_DIM);
    }

    /* ---- Page 1: DASH Telemetry ---- */
    if (s_current_view == VIEW_DASH) {
        const uint32_t permille = st.load_permille > 1000u ? 1000u : st.load_permille;
        LBL_FMT(s_dash_load_val_lbl, s_txt_load_val, "%lu.%lu%%",
                (unsigned long)(permille / 10u), (unsigned long)(permille % 10u));

        /* 12-segment tri-color LED bar: 0..5 green, 6..8 amber, 9..11 red */
        const int lit = (permille == 0u)
                            ? 0
                            : (int)((permille * LOAD_SEGMENTS + 999u) / 1000u);
        for (int i = 0; i < LOAD_SEGMENTS; i++) {
            lv_color_t seg_col = COL_BG_ELEVATED;
            if (i < lit) {
                seg_col = (i < 6) ? COL_GREEN : (i < 9) ? COL_AMBER : COL_RED;
            }
            obj_bg_color(s_dash_load_seg[i], seg_col, 0);
        }

        /* FPS, Ring Drops (only shown when > 0), and Total Frames */
        LBL_FMT(s_dash_fps_val_lbl, s_txt_fps_val, "%lu", (unsigned long)st.fps);
        if (ring.dropped == 0u) {
            object_show(s_dash_drop_lbl, false);
        } else {
            object_show(s_dash_drop_lbl, true);
            if (ring.dropped < 1000u) {
                LBL_FMT(s_dash_drop_lbl, s_txt_drop, "DROP %lu",
                        (unsigned long)ring.dropped);
            } else {
                LBL_FMT(s_dash_drop_lbl, s_txt_drop, "DROP %luk",
                        (unsigned long)(ring.dropped / 1000u));
            }
        }

        if (st.rx_total < 100000u) {
            LBL_FMT(s_dash_total_lbl, s_txt_total, "TOTAL %lu",
                    (unsigned long)st.rx_total);
        } else if (st.rx_total < 1000000u) {
            LBL_FMT(s_dash_total_lbl, s_txt_total, "TOTAL %.1fk",
                    (double)st.rx_total / 1000.0);
        } else {
            LBL_FMT(s_dash_total_lbl, s_txt_total, "TOTAL %.2fM",
                    (double)st.rx_total / 1000000.0);
        }

        /* 4-Cell Diagnostic Strip: TEC, REC, ERR+MISS, PSRAM RING % */
        LBL_FMT(s_cell_val[0], s_txt_cell[0], "%lu", (unsigned long)st.tec);
        const lv_color_t tec_col = st.tec > 127u ? COL_RED
                                   : st.tec > 96u ? COL_AMBER
                                                  : COL_GREEN;
        obj_bg_color(s_cell_bar[0], tec_col, 0);
        label_color(s_cell_val[0], st.tec > 96u ? tec_col : COL_TEXT_PRI);

        LBL_FMT(s_cell_val[1], s_txt_cell[1], "%lu", (unsigned long)st.rec);
        const lv_color_t rec_col = st.rec > 127u ? COL_RED
                                   : st.rec > 96u ? COL_AMBER
                                                  : COL_GREEN;
        obj_bg_color(s_cell_bar[1], rec_col, 0);
        label_color(s_cell_val[1], st.rec > 96u ? rec_col : COL_TEXT_PRI);

        const uint32_t err_sum = st.bus_errors + st.rx_missed;
        if (err_sum < 10000u) {
            LBL_FMT(s_cell_val[2], s_txt_cell[2], "%lu", (unsigned long)err_sum);
        } else {
            LBL_FMT(s_cell_val[2], s_txt_cell[2], "%luk",
                    (unsigned long)(err_sum / 1000u));
        }
        const lv_color_t err_col = (bus_off || err_sum > 0u) ? COL_RED : COL_GREEN;
        obj_bg_color(s_cell_bar[2], err_col, 0);
        label_color(s_cell_val[2], err_sum > 0u ? COL_RED : COL_TEXT_PRI);

        const uint32_t ring_pct =
            ring.cap > 0u ? (uint32_t)(((uint64_t)ring.used * 100ull) / ring.cap) : 0u;
        LBL_FMT(s_cell_val[3], s_txt_cell[3], "%lu%%", (unsigned long)ring_pct);
        const lv_color_t ring_col = ring_pct >= 80u ? COL_RED
                                    : ring_pct >= 40u ? COL_AMBER
                                                      : COL_CYAN;
        obj_bg_color(s_cell_bar[3], ring_col, 0);
        label_color(s_cell_val[3], ring_pct >= 40u ? ring_col : COL_TEXT_PRI);

        /* Clean Single-Line SD Black-Box Bar */
        if (!sd.mounted) {
            LBL_SET(s_sd_badge_lbl, s_txt_sd_badge, "NO SD");
            obj_bg_color(s_sd_badge, COL_BG_ELEVATED, 0);
            label_color(s_sd_badge_lbl, COL_TEXT_DIM);

            LBL_SET(s_sd_main_lbl, s_txt_sd_main, "NO CARD");
            label_color(s_sd_main_lbl, COL_TEXT_DIM);
        } else {
            char size_str[16];
            if (sd.free_kb >= 1024u * 1024u) {
                snprintf(size_str, sizeof(size_str), "%.1f GB",
                         (double)sd.free_kb / (1024.0 * 1024.0));
            } else {
                snprintf(size_str, sizeof(size_str), "%lu MB",
                         (unsigned long)(sd.free_kb / 1024u));
            }

            if (sd.recording) {
                LBL_SET(s_sd_badge_lbl, s_txt_sd_badge, "REC");
                obj_bg_color(s_sd_badge, COL_GREEN, 0);
                label_color(s_sd_badge_lbl, COL_BG_ROOT);

                if (sd.dropped > 0u) {
                    LBL_FMT(s_sd_main_lbl, s_txt_sd_main, "#%02lu • %s • DROP %lu",
                            (unsigned long)sd.files, size_str, (unsigned long)sd.dropped);
                    label_color(s_sd_main_lbl, COL_AMBER);
                } else {
                    LBL_FMT(s_sd_main_lbl, s_txt_sd_main, "#%02lu • %s",
                            (unsigned long)sd.files, size_str);
                    label_color(s_sd_main_lbl, COL_TEXT_PRI);
                }
            } else {
                LBL_SET(s_sd_badge_lbl, s_txt_sd_badge, "IDLE");
                obj_bg_color(s_sd_badge, COL_BG_ELEVATED, 0);
                label_color(s_sd_badge_lbl, COL_CYAN);

                LBL_FMT(s_sd_main_lbl, s_txt_sd_main, "READY • %s", size_str);
                label_color(s_sd_main_lbl, COL_TEXT_SEC);
            }
        }
    }

    /* ---- Page 2: SETUP Console ---- */
    if (s_current_view == VIEW_SET) {
        for (int i = 0; i < 4; i++) {
            const bool active = (BITRATES[i] == bitrate);
            obj_bg_color(s_set_br_btn[i], active ? COL_CYAN : COL_BG_SURFACE, 0);
            obj_border_color(s_set_br_btn[i], active ? COL_CYAN : COL_BORDER_FOCUS, 0);
            object_show(s_set_br_line[i], !active);
            label_color(s_set_br_lbl[i], active ? COL_BG_ROOT : COL_TEXT_PRI);
        }

        for (int i = 0; i < 2; i++) {
            const bool active = (MODES[i] == mode);
            const lv_color_t active_col = (i == 0) ? COL_CYAN : COL_AMBER;
            obj_bg_color(s_set_mode_btn[i], active ? active_col : COL_BG_SURFACE, 0);
            obj_border_color(s_set_mode_btn[i],
                             active ? active_col : COL_BORDER_FOCUS, 0);
            object_show(s_set_mode_line[i], !active);
            label_color(s_set_mode_lbl[i], active ? COL_BG_ROOT : COL_TEXT_PRI);
        }

        portENTER_CRITICAL(&s_ep_lock);
        const uint32_t ip = s_ep_ip;
        const uint16_t port = s_ep_port;
        const bool valid = s_ep_valid;
        portEXIT_CRITICAL(&s_ep_lock);

        if (valid) {
            const uint8_t *o = (const uint8_t *)&ip;
            LBL_FMT(s_set_host_ep_lbl, s_txt_host_ep, "HOST %u.%u.%u.%u:%u",
                    (unsigned)o[0], (unsigned)o[1], (unsigned)o[2], (unsigned)o[3],
                    (unsigned)port);
            label_color(s_set_host_ep_lbl, COL_TEXT_PRI);
        } else {
            LBL_SET(s_set_host_ep_lbl, s_txt_host_ep, "NO HOST LINK");
            label_color(s_set_host_ep_lbl, COL_TEXT_DIM);
        }

        if (conn == UI_CONN_ONLINE) {
            LBL_FMT(s_set_host_pill_lbl, s_txt_host_pill, "%lu ms",
                    (unsigned long)rtt_ms);
            obj_bg_color(s_set_host_pill, COL_GREEN, 0);
            label_color(s_set_host_pill_lbl, COL_BG_ROOT);
        } else if (conn == UI_CONN_HOST) {
            LBL_SET(s_set_host_pill_lbl, s_txt_host_pill, "HOST ...");
            obj_bg_color(s_set_host_pill, COL_CYAN, 0);
            label_color(s_set_host_pill_lbl, COL_BG_ROOT);
        } else if (conn == UI_CONN_RECONNECT) {
            LBL_SET(s_set_host_pill_lbl, s_txt_host_pill, "RETRY");
            obj_bg_color(s_set_host_pill, COL_AMBER, 0);
            label_color(s_set_host_pill_lbl, COL_BG_ROOT);
        } else {
            LBL_SET(s_set_host_pill_lbl, s_txt_host_pill, "WIFI ...");
            obj_bg_color(s_set_host_pill, COL_BG_ELEVATED, 0);
            label_color(s_set_host_pill_lbl, COL_TEXT_DIM);
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

    s_toast = card_new(root, 0, 0, 224, 38);
    lv_obj_center(s_toast);
    lv_obj_set_style_bg_color(s_toast, COL_BG_ELEVATED, 0);
    lv_obj_set_style_border_color(s_toast, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);
    s_toast_bar = rect_new(s_toast, 0, 0, 220, 3, COL_CYAN, 0);
    s_toast_lbl = lbl_new(s_toast, "", 0, 1, FONT_16, COL_TEXT_PRI);
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
        clear_stats_cb(NULL);
    }
    lvgl_port_unlock();
}
