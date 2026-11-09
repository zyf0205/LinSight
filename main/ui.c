/*
 * LinSight CAN analyzer UI (320x240, LVGL 8.3) — implements docs/ui_design.md
 * v2.0 (Dark Instrument HIG).
 *
 * Two physical pages with three views:
 *   Monitor  — bus-health overview: 26 px status bar, 24 px health strip,
 *              6 x 24 px top-ID rows, 30 px telemetry bar. Tap a row to lock.
 *   Raw      — secondary 8 x 21 px frame trace in the same monitor container,
 *              sharing the status and telemetry bars. Tap a row to lock.
 *   Settings — 28 px nav bar, bitrate/mode card, filter & trigger card,
 *              quick actions (pause, clear, bus recover).
 * Page switching: left/right swipe, the gear/back buttons, or GPIO0 long press
 * (Overview -> Raw -> Settings -> Overview).
 *
 * Data path: a 50 ms LVGL timer (core 1, the LVGL task) pulls the capture-side
 * UI history and the stats snapshot; only labels whose text/color actually
 * changed are touched. The history is independent of net_task's streaming ring,
 * so an attached host draining records can never blank the trace.
 * Cross-task entry points (net_task, the GPIO button task) never touch LVGL
 * objects: they publish into small volatile mailboxes drained by the tick, so
 * they can never block on the LVGL mutex. All dynamic strings live in static
 * buffers and use lv_label_set_text_static, so the 20 Hz path performs no heap
 * allocation. Filter, per-ID counters and the one-shot trigger are local
 * display features; capture and streaming keep running while the local
 * display is frozen.
 */

#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "frame.h"
#include "lvgl.h"
#include "sd_store.h"
#include "twai.h"

#define UI_W 320
#define UI_H 240

#define ROW_N 8
#define ROW_H 21
#define SCAN_N 512      /* records examined per 50 ms tick (covers ~10 kfps) */
#define CNT_SLOTS 128   /* per-ID counter table */

#define TOP_N 6
#define RATE_WINDOW_MS 1000u
#define PAGE_CONTAINERS 2

/* Monitor overview: 26 px status bar + 24 health + 16 header + 6 x 24 rows. */
#define OV_Y 26
#define OV_H 184
#define OV_HEALTH_H 24
#define OV_HDR_H 16
#define OV_ROW_H 24
#define OV_HDR_Y OV_HEALTH_H
#define OV_ROWS_Y (OV_HEALTH_H + OV_HDR_H)

/* Raw trace in the same monitor container: 16 px header + 8 x 21 px rows. */
#define RAW_HDR_Y 0
#define RAW_ROWS_Y 16

/* ------------------------------------------------------ palette (design 2) */

#define COL_BG lv_color_hex(0x000000)
#define COL_BAR lv_color_hex(0x161618)
#define COL_ROW_EVEN lv_color_hex(0x0B0B0E)
#define COL_ROW_ODD lv_color_hex(0x141419)
#define COL_TILE lv_color_hex(0x1C1C1E)
#define COL_ACTIVE lv_color_hex(0x2C2C2E)
#define COL_TEXT lv_color_hex(0xF8FAFC)
#define COL_DIM lv_color_hex(0x8E8E93)
#define COL_BLUE lv_color_hex(0x0A84FF)
#define COL_PURPLE lv_color_hex(0xBF5AF2)
#define COL_GREEN lv_color_hex(0x30D158)
#define COL_ORANGE lv_color_hex(0xFF9F0A)
#define COL_RED lv_color_hex(0xFF453A)

typedef enum {
    PAGE_MONITOR = 0, /* bus-health overview, monitor container */
    PAGE_RAW,         /* raw trace, same monitor container */
    PAGE_SETTINGS,
    PAGE_COUNT,
} page_id_t;

typedef enum {
    FILTER_ALL = 0,
    FILTER_STD,
    FILTER_EXT,
    FILTER_LOCK,
    FILTER_COUNT,
} filter_t;

typedef enum {
    TRIG_OFF = 0,
    TRIG_ERR,
    TRIG_LOCK,
    TRIG_COUNT,
} trig_t;

typedef struct {
    uint32_t id;
    uint32_t count; /* cumulative since Clear (CNT column) */
    uint32_t win;   /* non-error frames in the current 1 s window */
    uint16_t rate;  /* frames/s from the last completed window */
    uint8_t ext;
    uint8_t used;
} id_count_t;

/* -------------------------------------------------------------- statics */

static lv_obj_t *s_screen;
static lv_obj_t *s_pages[PAGE_CONTAINERS];
static page_id_t s_page = PAGE_MONITOR;
static page_id_t s_back_page = PAGE_MONITOR;

/* Monitor: status bar */
static lv_obj_t *s_bus_pill;
static lv_obj_t *s_bus_label;
static lv_obj_t *s_br_label;
static lv_obj_t *s_run_label;
static lv_obj_t *s_host_dot;
static lv_obj_t *s_host_label;

/* Monitor: view groups inside the same container (overview / raw). */
static lv_obj_t *s_ov_group;
static lv_obj_t *s_raw_group;

/* Monitor overview: 4 health counters and 6 top-ID rows. */
static lv_obj_t *s_health[4];
static lv_obj_t *s_ov_row[TOP_N];
static lv_obj_t *s_ov_id[TOP_N];
static lv_obj_t *s_ov_rate[TOP_N];
static lv_obj_t *s_ov_per[TOP_N];
static lv_obj_t *s_ov_cnt[TOP_N];
static lv_obj_t *s_ov_bar[TOP_N];
static lv_obj_t *s_ov_empty;

/* Monitor raw: trace rows */
static lv_obj_t *s_row[ROW_N];
static lv_obj_t *s_row_id[ROW_N];
static lv_obj_t *s_row_dlc[ROW_N];
static lv_obj_t *s_row_data[ROW_N];
static lv_obj_t *s_row_cnt[ROW_N];
static lv_obj_t *s_empty; /* "waiting for traffic" hint over the blank grid */

/* Transient action feedback (bitrate change, clear, recover, lock). */
static lv_obj_t *s_toast;
static uint32_t s_toast_tick;

/* Monitor: bottom telemetry */
static lv_obj_t *s_bot[4];

/* Settings */
static lv_obj_t *s_br_pill[4];
static lv_obj_t *s_mode_pill;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_filter_pill[FILTER_COUNT];
static lv_obj_t *s_lock_label;
static lv_obj_t *s_trig_pill[TRIG_COUNT];
static lv_obj_t *s_pause_label;
static lv_obj_t *s_info_label;

/* Dynamic text storage (static text labels point here). */
static char s_txt_bus[10];
static char s_txt_br[12];
static char s_txt_run[10];
static char s_txt_host[16];
static char s_txt_health[4][20];
static char s_txt_ov_id[TOP_N][12];
static char s_txt_ov_rate[TOP_N][10];
static char s_txt_ov_per[TOP_N][6];
static char s_txt_ov_cnt[TOP_N][10];
static char s_txt_ov_empty[48];
static char s_txt_row_id[ROW_N][12];
static char s_txt_row_dlc[ROW_N][4];
static char s_txt_row_data[ROW_N][28];
static char s_txt_row_cnt[ROW_N][10];
static char s_txt_bot[4][16];
static char s_txt_lock[14];
static char s_txt_mode[12];
static char s_txt_pause[32];
static char s_txt_info[20];
static char s_txt_toast[40];

/* UI state */
static linsight_conn_t s_conn;
static uint32_t s_rtt_ms;
static bool s_frozen;      /* local display pause (capture continues) */
static bool s_triggered;   /* one-shot trigger froze the trace */
static bool s_trig_pending;
static bool s_limit_on;
static uint16_t s_trig_limit;
static filter_t s_filter;
static trig_t s_trig_mode;
static bool s_lock_valid;
static uint32_t s_lock_id;
static bool s_lock_ext;
static uint32_t s_prev_bus_off;
static uint32_t s_prev_bus_err;
static uint32_t s_remote_tick;
static bool s_remote_on;
static bool s_floor_valid; /* hide pre-Clear records until they leave the window */
static uint16_t s_floor;
static bool s_total_valid; /* detect a host CONTROL.clear_stats behind our back */
static uint32_t s_prev_total;

/* Cross-task mailbox: net_task publishes connection/remote state without
 * touching LVGL or taking the LVGL mutex -- lvgl_port_lock(0) waits forever,
 * and a stalled net_task stops PING/PONG until the host drops the session.
 * The 20 Hz tick applies these requests. Single writer (net_task), single
 * reader (LVGL task); u32/bool/enum volatile writes are atomic on Xtensa. */
static volatile linsight_conn_t s_conn_req = UI_CONN_WIFI;
static volatile uint32_t s_rtt_req;
static volatile bool s_remote_req;
static volatile uint32_t s_filter_req = 0xFFFFFFFFu;

/* Trace scan/count state */
static linsight_can_record_t s_scan_buf[SCAN_N];
static uint32_t s_scan_n;
static uint16_t s_scan_seq;
static bool s_scan_valid;
static id_count_t s_counts[CNT_SLOTS];

/* 1 s tumbling per-ID rate window and top-ID ranking (no heap). */
static const id_count_t *s_top[TOP_N];
static int s_top_n;
static bool s_top_any;
static uint32_t s_rate_tick;
static bool s_rate_valid;

/* Rendered row identity (row click locks this ID). */
static bool s_view_valid[ROW_N];
static uint32_t s_view_id[ROW_N];
static bool s_view_ext[ROW_N];

/* Rendered overview identity (row click locks this ID). */
static bool s_ov_valid[TOP_N];
static uint32_t s_ov_id_val[TOP_N];
static bool s_ov_ext_val[TOP_N];

static const can_bitrate_t BITRATES[4] = {CAN_BITRATE_125K, CAN_BITRATE_250K,
                                          CAN_BITRATE_500K, CAN_BITRATE_1M};
static const char *BITRATE_LABELS[4] = {"125K", "250K", "500K", "1M"};

static void refresh_filter_pills(void);
static void refresh_trigger_pills(void);
static void refresh_row_highlight(void);
static void refresh_settings(const can_stats_t *st);
static void refresh_health(const can_stats_t *st);
static void row_clear(int row);
static void ov_row_clear(int row);
static void ov_row_bg(int row, bool active);
static void render_overview(void);
static void page_show(page_id_t page);
static void toggle_pause(void);

/* ------------------------------------------------------------ primitives */

static bool color_same(lv_color_t a, lv_color_t b)
{
    return a.full == b.full;
}

static void label_color(lv_obj_t *label, lv_color_t color)
{
    if (!color_same(lv_obj_get_style_text_color(label, 0), color)) {
        lv_obj_set_style_text_color(label, color, 0);
    }
}

/* Point the label at a persistent buffer only when the text changes. */
static void label_set_str(lv_obj_t *label, char *buf, size_t cap, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0) {
        return;
    }

    size_t len = 0;
    while (text[len] != '\0' && len + 1 < cap) {
        len++;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    lv_label_set_text_static(label, buf);
}

static void label_setf(lv_obj_t *label, char *buf, size_t cap, const char *fmt, ...)
{
    char tmp[48];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    label_set_str(label, buf, cap, tmp);
}

/* Standard frames are 3 hex digits, extended frames 8 (docs/ui_design.md). */
static void fmt_id(char *buf, size_t cap, uint32_t id, bool ext)
{
    if (ext) {
        snprintf(buf, cap, "%08X", (unsigned)id);
    } else {
        snprintf(buf, cap, "%03X", (unsigned)id);
    }
}

static void object_show(lv_obj_t *obj, bool show)
{
    const bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (show == !hidden) {
        return; /* already in the requested state */
    }
    if (show) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Short-lived feedback over the telemetry bar ("BITRATE 500K", "CLEARED"...). */
static void toast_showf(const char *fmt, ...)
{
    char tmp[sizeof(s_txt_toast)];
    va_list ap;

    if (!s_toast) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    label_set_str((lv_obj_t *)lv_obj_get_child(s_toast, 0), s_txt_toast,
                  sizeof(s_txt_toast), tmp);
    s_toast_tick = lv_tick_get();
    object_show(s_toast, true);
}

static lv_obj_t *obj_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                         lv_coord_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label_new(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    /* Text is a literal or a static buffer owned by the caller, so it can be
     * referenced instead of copied. CLIP keeps the buffer read-only. */
    lv_label_set_text_static(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static lv_obj_t *card_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h)
{
    lv_obj_t *card = obj_new(parent, x, y, w, h);
    lv_obj_set_style_bg_color(card, COL_TILE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    return card;
}

static void clickable_set(lv_obj_t *obj, bool on)
{
    if (on == lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        return;
    }
    if (on) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

static lv_obj_t *pill_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h)
{
    lv_obj_t *pill = obj_new(parent, x, y, w, h);
    lv_obj_set_style_bg_color(pill, COL_ACTIVE, 0);
    lv_obj_set_style_bg_color(pill, COL_TILE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_ext_click_area(pill, 2);
    lv_obj_t *label = label_new(pill, "", &lv_font_montserrat_12, COL_DIM);
    lv_obj_center(label);
    return pill;
}

static void pill_style(lv_obj_t *pill, bool selected, lv_color_t sel_color)
{
    const lv_color_t bg = selected ? sel_color : COL_ACTIVE;
    if (!color_same(lv_obj_get_style_bg_color(pill, 0), bg)) {
        lv_obj_set_style_bg_color(pill, bg, 0);
    }
    label_color((lv_obj_t *)lv_obj_get_child(pill, 0), selected ? COL_TEXT : COL_DIM);
}

static lv_obj_t *action_btn_new(lv_obj_t *parent, lv_coord_t x, const char *text)
{
    lv_obj_t *btn = obj_new(parent, x, 5, 98, 50);
    lv_obj_set_style_bg_color(btn, COL_ACTIVE, 0);
    lv_obj_set_style_bg_color(btn, COL_TILE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *label = label_new(btn, text, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return btn;
}

/* ------------------------------------------------------------ navigation */

static int page_container(page_id_t page)
{
    return page == PAGE_SETTINGS ? 1 : 0;
}

static page_id_t page_next(page_id_t page)
{
    switch (page) {
    case PAGE_MONITOR:
        return PAGE_RAW;
    case PAGE_RAW:
        return PAGE_SETTINGS;
    default:
        return PAGE_MONITOR;
    }
}

static void page_show(page_id_t page)
{
    if (page < PAGE_MONITOR || page >= PAGE_COUNT) {
        page = PAGE_MONITOR;
    }
    if (page == PAGE_SETTINGS && s_page != PAGE_SETTINGS) {
        s_back_page = s_page;
    }
    s_page = page;

    const int active = page_container(page);
    for (int i = 0; i < PAGE_CONTAINERS; i++) {
        if (i == active) {
            lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    object_show(s_ov_group, page == PAGE_MONITOR);
    object_show(s_raw_group, page == PAGE_RAW);

    if (page == PAGE_SETTINGS) {
        can_stats_t st;
        twai_stats_snapshot(&st);
        refresh_settings(&st);
    }
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        if (s_page == PAGE_MONITOR) {
            page_show(PAGE_RAW);
        } else if (s_page == PAGE_RAW) {
            page_show(PAGE_SETTINGS);
        }
    } else if (dir == LV_DIR_RIGHT) {
        if (s_page == PAGE_RAW) {
            page_show(PAGE_MONITOR);
        } else if (s_page == PAGE_SETTINGS) {
            page_show(s_back_page);
        }
    }
}

static void settings_click_cb(lv_event_t *e)
{
    (void)e;
    page_show(PAGE_SETTINGS);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    page_show(s_back_page);
}

/* --------------------------------------------------------------- actions */

static void bitrate_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (BITRATES[idx] == twai_current_bitrate()) {
        return; /* no need to stop/start the driver for the active bitrate */
    }
    if (twai_capture_restart(BITRATES[idx], twai_current_mode()) == ESP_OK) {
        toast_showf("BITRATE %s", BITRATE_LABELS[idx]);
    } else {
        toast_showf("BITRATE FAILED");
    }
}

static void mode_click_cb(lv_event_t *e)
{
    (void)e;
    const can_mode_t mode = twai_current_mode();
    const can_mode_t next =
        mode == CAN_MODE_NORMAL ? CAN_MODE_LISTEN_ONLY : CAN_MODE_NORMAL;
    if (twai_capture_restart(twai_current_bitrate(), next) == ESP_OK) {
        toast_showf("MODE %s", next == CAN_MODE_NORMAL ? "NORMAL" : "LISTEN");
    } else {
        toast_showf("MODE CHANGE FAILED");
    }
}

static void toggle_pause(void)
{
    if (s_triggered) {
        /* Resuming from a trigger only unfreezes the display. */
        s_triggered = false;
        s_trig_pending = false;
        return;
    }
    if (s_frozen) {
        s_frozen = false;
        return;
    }

    can_stats_t st;
    twai_stats_snapshot(&st);
    if (st.paused) {
        twai_capture_set_paused(false); /* remote pause is real capture pause */
        return;
    }
    s_frozen = true;
}

static void pause_click_cb(lv_event_t *e)
{
    (void)e;
    toggle_pause();
}

static void counts_clear(void)
{
    memset(s_counts, 0, sizeof(s_counts));
    s_rate_valid = false;
    s_rate_tick = lv_tick_get();
    s_top_n = 0;
    s_top_any = false;
}

/* Drop the visible trace and counts without touching the capture path. */
static void view_reset(void)
{
    counts_clear();
    s_triggered = false;
    s_trig_pending = false;
    s_prev_bus_off = 0;
    s_prev_bus_err = 0;

    /* Counts restart from here: everything already in the history is pre-clear. */
    linsight_can_record_t newest;
    if (twai_ui_tail(&newest, 1) == 1) {
        s_scan_seq = newest.seq;
        s_scan_valid = true;
        s_floor = newest.seq;
        s_floor_valid = true;
    } else {
        s_scan_valid = false;
        s_floor_valid = false;
    }

    for (int i = 0; i < ROW_N; i++) {
        row_clear(i);
    }
    for (int i = 0; i < TOP_N; i++) {
        ov_row_clear(i);
    }
    object_show(s_empty, true);
    object_show(s_ov_empty, true);
    s_total_valid = false; /* re-baseline rx_total on the next tick */
}

static void clear_click_cb(lv_event_t *e)
{
    (void)e;
    twai_stats_clear();
    view_reset();
    toast_showf("VIEW CLEARED");
}

static void recover_click_cb(lv_event_t *e)
{
    (void)e;
    toast_showf(twai_capture_recover() == ESP_OK ? "BUS RECOVER" : "RECOVER FAILED");
}

static void filter_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == FILTER_LOCK && !s_lock_valid) {
        return;
    }
    s_filter = (filter_t)idx;
    refresh_filter_pills();
}

static void trig_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == TRIG_LOCK && !s_lock_valid) {
        return;
    }
    s_trig_mode = (trig_t)idx;
    s_triggered = false;
    s_trig_pending = false;
    refresh_trigger_pills();
}

static void toggle_lock_id(uint32_t id, bool ext)
{
    if (s_lock_valid && id == s_lock_id && ext == s_lock_ext) {
        s_lock_valid = false;
        if (s_filter == FILTER_LOCK) {
            s_filter = FILTER_ALL;
        }
        toast_showf("ID UNLOCKED");
    } else {
        s_lock_valid = true;
        s_lock_id = id;
        s_lock_ext = ext;
        toast_showf("ID 0x%X LOCKED", (unsigned)s_lock_id);
    }

    refresh_filter_pills();
    refresh_trigger_pills();
    refresh_row_highlight();
}

static void row_click_cb(lv_event_t *e)
{
    const int row = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_view_valid[row]) {
        return;
    }
    toggle_lock_id(s_view_id[row], s_view_ext[row]);
}

static void ov_row_click_cb(lv_event_t *e)
{
    const int row = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_ov_valid[row]) {
        return;
    }
    toggle_lock_id(s_ov_id_val[row], s_ov_ext_val[row]);
}

/* -------------------------------------------------------- trace counters */

static id_count_t *count_slot(uint32_t id, bool ext)
{
    const uint32_t h = (id * 2654435761u + (ext ? 0x9E3779B9u : 0u)) & (CNT_SLOTS - 1);

    for (uint32_t i = 0; i < CNT_SLOTS; i++) {
        id_count_t *slot = &s_counts[(h + i) & (CNT_SLOTS - 1)];
        if (!slot->used) {
            slot->used = 1;
            slot->id = id;
            slot->ext = ext ? 1 : 0;
            slot->count = 0;
            slot->win = 0;
            slot->rate = 0;
            return slot;
        }
        if (slot->id == id && slot->ext == (ext ? 1 : 0)) {
            return slot;
        }
    }

    /* Table full: reuse the home slot (oldest IDs lose their history). */
    id_count_t *slot = &s_counts[h];
    slot->id = id;
    slot->ext = ext ? 1 : 0;
    slot->count = 0;
    slot->win = 0;
    slot->rate = 0;
    return slot;
}

static uint32_t count_get(uint32_t id, bool ext)
{
    const uint32_t h = (id * 2654435761u + (ext ? 0x9E3779B9u : 0u)) & (CNT_SLOTS - 1);

    for (uint32_t i = 0; i < CNT_SLOTS; i++) {
        const id_count_t *slot = &s_counts[(h + i) & (CNT_SLOTS - 1)];
        if (!slot->used) {
            break;
        }
        if (slot->id == id && slot->ext == (ext ? 1 : 0)) {
            return slot->count;
        }
    }
    return 0; /* evicted by a hash collision */
}

static void fmt_count(char *buf, size_t cap, uint32_t n)
{
    if (n < 10000u) {
        snprintf(buf, cap, "%u", (unsigned)n);
    } else if (n < 1000000u) {
        snprintf(buf, cap, "%u.%uk", (unsigned)(n / 1000u),
                 (unsigned)((n % 1000u) / 100u));
    } else {
        snprintf(buf, cap, "%u.%02uM", (unsigned)(n / 1000000u),
                 (unsigned)((n / 10000u) % 100u));
    }
}

/* --------------------------------------------------------- trace render */

static bool seq_after(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) > 0;
}

static bool seq_after_eq(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) >= 0;
}

static bool lock_match(const linsight_can_record_t *r)
{
    return s_lock_valid && r->id == s_lock_id &&
           ((r->flags & LINSIGHT_REC_FLAG_EXT) != 0) == s_lock_ext;
}

static bool row_match(const linsight_can_record_t *r)
{
    if (r->flags & LINSIGHT_REC_FLAG_ERR) {
        return true; /* error frames are always visible */
    }
    switch (s_filter) {
    case FILTER_STD:
        return !(r->flags & LINSIGHT_REC_FLAG_EXT);
    case FILTER_EXT:
        return (r->flags & LINSIGHT_REC_FLAG_EXT) != 0;
    case FILTER_LOCK:
        return lock_match(r);
    default:
        return true;
    }
}

static void row_bg(int row, bool active)
{
    const lv_color_t color = active ? COL_ACTIVE : ((row & 1) ? COL_ROW_ODD : COL_ROW_EVEN);
    if (!color_same(lv_obj_get_style_bg_color(s_row[row], 0), color)) {
        lv_obj_set_style_bg_color(s_row[row], color, 0);
    }
    /* Blue accent on the left edge makes the locked ID obvious at a glance. */
    const lv_coord_t border = active ? 3 : 0;
    if (lv_obj_get_style_border_width(s_row[row], 0) != border) {
        lv_obj_set_style_border_width(s_row[row], border, 0);
    }
    if (active) {
        lv_obj_set_style_border_color(s_row[row], COL_BLUE, 0);
    }
}

static void fmt_data(char *buf, size_t cap, const linsight_can_record_t *r)
{
    size_t used = 0;
    uint8_t dlc = r->dlc;

    if (dlc > LINSIGHT_DLC_MAX) {
        dlc = LINSIGHT_DLC_MAX;
    }
    buf[0] = '\0';
    for (uint8_t i = 0; i < dlc; i++) {
        const int n = snprintf(buf + used, cap - used, "%02X%s", (unsigned)r->data[i],
                               i + 1 < dlc ? " " : "");
        if (n < 0 || (size_t)n >= cap - used) {
            break;
        }
        used += (size_t)n;
    }
}

static void row_set(int row, const linsight_can_record_t *r)
{
    const bool ext = (r->flags & LINSIGHT_REC_FLAG_EXT) != 0;
    const bool rtr = (r->flags & LINSIGHT_REC_FLAG_RTR) != 0;
    const bool err = (r->flags & LINSIGHT_REC_FLAG_ERR) != 0;

    if (err) {
        label_set_str(s_row_id[row], s_txt_row_id[row], sizeof(s_txt_row_id[row]),
                      "ERR-FRM");
        label_color(s_row_id[row], COL_RED);
    } else {
        char idbuf[12];
        fmt_id(idbuf, sizeof(idbuf), r->id, ext);
        label_set_str(s_row_id[row], s_txt_row_id[row], sizeof(s_txt_row_id[row]), idbuf);
        label_color(s_row_id[row], ext ? COL_PURPLE : COL_BLUE);
    }

    if (rtr) {
        label_set_str(s_row_dlc[row], s_txt_row_dlc[row], sizeof(s_txt_row_dlc[row]), "R");
        label_color(s_row_dlc[row], COL_ORANGE);
        label_set_str(s_row_data[row], s_txt_row_data[row], sizeof(s_txt_row_data[row]),
                      "REMOTE REQUEST");
        label_color(s_row_data[row], COL_ORANGE);
    } else {
        label_setf(s_row_dlc[row], s_txt_row_dlc[row], sizeof(s_txt_row_dlc[row]), "%u",
                   (unsigned)r->dlc);
        label_color(s_row_dlc[row], COL_DIM);
        char data[28];
        fmt_data(data, sizeof(data), r);
        label_set_str(s_row_data[row], s_txt_row_data[row], sizeof(s_txt_row_data[row]),
                      data);
        label_color(s_row_data[row], COL_TEXT);
    }

    if (err) {
        label_set_str(s_row_cnt[row], s_txt_row_cnt[row], sizeof(s_txt_row_cnt[row]), "");
    } else {
        char cnt[10];
        fmt_count(cnt, sizeof(cnt), count_get(r->id, ext));
        label_set_str(s_row_cnt[row], s_txt_row_cnt[row], sizeof(s_txt_row_cnt[row]), cnt);
    }
    label_color(s_row_cnt[row], COL_DIM);

    s_view_valid[row] = true;
    s_view_id[row] = r->id;
    s_view_ext[row] = ext;
    row_bg(row, lock_match(r));
}

static void row_clear(int row)
{
    label_set_str(s_row_id[row], s_txt_row_id[row], sizeof(s_txt_row_id[row]), "");
    label_set_str(s_row_dlc[row], s_txt_row_dlc[row], sizeof(s_txt_row_dlc[row]), "");
    label_set_str(s_row_data[row], s_txt_row_data[row], sizeof(s_txt_row_data[row]), "");
    label_set_str(s_row_cnt[row], s_txt_row_cnt[row], sizeof(s_txt_row_cnt[row]), "");
    s_view_valid[row] = false;
    row_bg(row, false);
}

static void refresh_row_highlight(void)
{
    for (int i = 0; i < ROW_N; i++) {
        const bool active =
            s_view_valid[i] && s_lock_valid && s_view_id[i] == s_lock_id &&
            s_view_ext[i] == s_lock_ext;
        row_bg(i, active);
    }
    for (int i = 0; i < TOP_N; i++) {
        const bool active =
            s_ov_valid[i] && s_lock_valid && s_ov_id_val[i] == s_lock_id &&
            s_ov_ext_val[i] == s_lock_ext;
        ov_row_bg(i, active);
    }
}

/* ------------------------------------------------------ overview ranking */

/* Tumbling 1 s window: publish win*1000/elapsed once per window, then restart.
 * The first second after boot/Clear has no rate yet (columns show "---"). */
static void rate_sample(void)
{
    const uint32_t elapsed = lv_tick_elaps(s_rate_tick);
    if (elapsed < RATE_WINDOW_MS) {
        return;
    }

    for (int i = 0; i < CNT_SLOTS; i++) {
        id_count_t *slot = &s_counts[i];
        if (!slot->used) {
            continue;
        }
        const uint32_t rate = slot->win * 1000u / elapsed;
        slot->rate = rate > 0xFFFFu ? 0xFFFFu : (uint16_t)rate;
        slot->win = 0;
    }
    s_rate_valid = true;
    s_rate_tick = lv_tick_get();
}

/* Live rows sort by rate; before the first window rate is unknown, so the
 * cumulative count decides. Ties: lower ID, standard before extended. */
static bool rank_before(const id_count_t *a, const id_count_t *b)
{
    if (a->rate != b->rate) {
        return a->rate > b->rate;
    }
    if (a->count != b->count) {
        return a->count > b->count;
    }
    if (a->id != b->id) {
        return a->id < b->id;
    }
    return a->ext < b->ext;
}

static void rank_put(const id_count_t *slot)
{
    if (s_top_n < TOP_N) {
        int pos = s_top_n++;
        while (pos > 0 && rank_before(slot, s_top[pos - 1])) {
            s_top[pos] = s_top[pos - 1];
            pos--;
        }
        s_top[pos] = slot;
    } else if (rank_before(slot, s_top[TOP_N - 1])) {
        int pos = TOP_N - 1;
        while (pos > 0 && rank_before(slot, s_top[pos - 1])) {
            s_top[pos] = s_top[pos - 1];
            pos--;
        }
        s_top[pos] = slot;
    }
}

static void top_build(void)
{
    s_top_n = 0;
    s_top_any = false;

    for (int i = 0; i < CNT_SLOTS; i++) {
        const id_count_t *slot = &s_counts[i];
        if (!slot->used) {
            continue;
        }
        s_top_any = true;
        if (s_rate_valid && slot->rate == 0) {
            continue; /* after the first window only live IDs are listed */
        }
        rank_put(slot);
    }
}

/* ----------------------------------------------------- overview render */

static void ov_row_bg(int row, bool active)
{
    const lv_color_t color = active ? COL_ACTIVE : ((row & 1) ? COL_ROW_ODD : COL_ROW_EVEN);
    if (!color_same(lv_obj_get_style_bg_color(s_ov_row[row], 0), color)) {
        lv_obj_set_style_bg_color(s_ov_row[row], color, 0);
    }
    /* Blue accent on the left edge makes the locked ID obvious at a glance. */
    const lv_coord_t border = active ? 3 : 0;
    if (lv_obj_get_style_border_width(s_ov_row[row], 0) != border) {
        lv_obj_set_style_border_width(s_ov_row[row], border, 0);
    }
    if (active) {
        lv_obj_set_style_border_color(s_ov_row[row], COL_BLUE, 0);
    }
}

static void ov_bar_set(int row, uint32_t rate, uint32_t ref, bool ext)
{
    uint32_t h = 2;
    if (ref > 0 && rate > 0) {
        h = 2u + rate * 18u / ref;
        if (h > 20u) {
            h = 20u;
        }
    }
    lv_obj_set_height(s_ov_bar[row], (lv_coord_t)h);
    lv_obj_set_y(s_ov_bar[row], OV_ROW_H - 2 - (lv_coord_t)h);
    lv_obj_set_style_bg_color(s_ov_bar[row], ext ? COL_PURPLE : COL_BLUE, 0);
}

static void ov_row_set(int row, const id_count_t *slot)
{
    char idbuf[12];
    char rate[10];
    fmt_id(idbuf, sizeof(idbuf), slot->id, slot->ext != 0);
    label_set_str(s_ov_id[row], s_txt_ov_id[row], sizeof(s_txt_ov_id[row]), idbuf);
    label_color(s_ov_id[row], slot->ext ? COL_PURPLE : COL_BLUE);

    if (!s_rate_valid) {
        snprintf(rate, sizeof(rate), "---");
    } else if (slot->rate >= 10000u) {
        snprintf(rate, sizeof(rate), "%uk", (unsigned)(slot->rate / 1000u));
    } else {
        snprintf(rate, sizeof(rate), "%u", (unsigned)slot->rate);
    }
    label_set_str(s_ov_rate[row], s_txt_ov_rate[row], sizeof(s_txt_ov_rate[row]), rate);

    char per[6];
    if (!s_rate_valid || slot->rate == 0) {
        snprintf(per, sizeof(per), "---");
    } else if (slot->rate > 1000u) {
        snprintf(per, sizeof(per), "<1");
    } else {
        snprintf(per, sizeof(per), "%u", (unsigned)(1000u / slot->rate));
    }
    label_set_str(s_ov_per[row], s_txt_ov_per[row], sizeof(s_txt_ov_per[row]), per);

    char cnt[10];
    fmt_count(cnt, sizeof(cnt), slot->count);
    label_set_str(s_ov_cnt[row], s_txt_ov_cnt[row], sizeof(s_txt_ov_cnt[row]), cnt);

    ov_bar_set(row, slot->rate, s_top_n > 0 ? s_top[0]->rate : 0, slot->ext != 0);
    object_show(s_ov_bar[row], true);

    s_ov_valid[row] = true;
    s_ov_id_val[row] = slot->id;
    s_ov_ext_val[row] = slot->ext != 0;
    ov_row_bg(row, s_lock_valid && slot->id == s_lock_id &&
                       (slot->ext != 0) == s_lock_ext);
}

static void ov_row_clear(int row)
{
    label_set_str(s_ov_id[row], s_txt_ov_id[row], sizeof(s_txt_ov_id[row]), "");
    label_set_str(s_ov_rate[row], s_txt_ov_rate[row], sizeof(s_txt_ov_rate[row]), "");
    label_set_str(s_ov_per[row], s_txt_ov_per[row], sizeof(s_txt_ov_per[row]), "");
    label_set_str(s_ov_cnt[row], s_txt_ov_cnt[row], sizeof(s_txt_ov_cnt[row]), "");
    s_ov_valid[row] = false;
    object_show(s_ov_bar[row], false);
    ov_row_bg(row, false);
}

static void render_overview(void)
{
    top_build();

    for (int i = 0; i < s_top_n; i++) {
        ov_row_set(i, s_top[i]);
    }
    for (int i = s_top_n; i < TOP_N; i++) {
        ov_row_clear(i);
    }

    lv_obj_t *hint = (lv_obj_t *)lv_obj_get_child(s_ov_empty, 0);
    if (!s_top_any) {
        label_set_str(hint, s_txt_ov_empty, sizeof(s_txt_ov_empty),
                      "Waiting for CAN frames\ncheck wiring / bitrate");
        object_show(s_ov_empty, true);
    } else if (s_top_n == 0) {
        label_set_str(hint, s_txt_ov_empty, sizeof(s_txt_ov_empty),
                      "No traffic in the last 1 s");
        object_show(s_ov_empty, true);
    } else {
        object_show(s_ov_empty, false);
    }
}

static void render_trace(bool use_limit, uint16_t limit)
{
    uint16_t pick[ROW_N];
    int rows = 0;
    int end = (int)s_scan_n - 1;

    if (use_limit) {
        while (end >= 0 && !seq_after_eq(limit, s_scan_buf[end].seq)) {
            end--;
        }
        if (end < 0) {
            end = (int)s_scan_n - 1; /* limit fell out of the window */
        }
    }

    for (int i = end; i >= 0 && rows < ROW_N; i--) {
        if (s_floor_valid && !seq_after(s_scan_buf[i].seq, s_floor)) {
            continue;
        }
        if (row_match(&s_scan_buf[i])) {
            pick[rows++] = (uint16_t)i;
        }
    }

    for (int row = 0; row < rows; row++) {
        /* pick[] is newest-first; rows fill oldest-first, newest at the bottom. */
        row_set(row, &s_scan_buf[pick[rows - 1 - row]]);
    }
    for (int row = rows; row < ROW_N; row++) {
        row_clear(row);
    }
    object_show(s_empty, rows == 0);
}

static void scan_records(bool capture_paused)
{
    s_scan_n = twai_ui_tail(s_scan_buf, SCAN_N);

    /* Once every record in the window is newer than the Clear point there is
     * nothing left to hide. */
    if (s_floor_valid && s_scan_n > 0 && seq_after(s_scan_buf[0].seq, s_floor)) {
        s_floor_valid = false;
    }

    for (uint32_t i = 0; i < s_scan_n; i++) {
        const linsight_can_record_t *r = &s_scan_buf[i];
        if (s_scan_valid && !seq_after(r->seq, s_scan_seq)) {
            continue;
        }
        s_scan_seq = r->seq;
        s_scan_valid = true;

        if (!(r->flags & LINSIGHT_REC_FLAG_ERR)) {
            id_count_t *slot =
                count_slot(r->id, (r->flags & LINSIGHT_REC_FLAG_EXT) != 0);
            slot->count++;
            slot->win++;
        }

        if (!s_triggered && !s_frozen && !capture_paused && s_trig_mode == TRIG_LOCK &&
            lock_match(r)) {
            s_triggered = true;
            s_trig_pending = true;
            s_limit_on = true;
            s_trig_limit = r->seq;
        }
    }
}

static void check_error_trigger(const can_stats_t *st)
{
    if (!s_triggered && !s_frozen && !st->paused && s_trig_mode == TRIG_ERR &&
        (st->bus_off > s_prev_bus_off || st->bus_errors > s_prev_bus_err)) {
        s_triggered = true;
        s_trig_pending = true;
        s_limit_on = false;
    }
    s_prev_bus_off = st->bus_off;
    s_prev_bus_err = st->bus_errors;
}

/* --------------------------------------------------------------- refresh */

static void fmt_bitrate(char *buf, size_t cap)
{
    const char *name = can_bitrate_name(twai_current_bitrate());
    char up[8];
    size_t i = 0;

    for (; name[i] && i < sizeof(up) - 1; i++) {
        const char c = name[i];
        up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    up[i] = '\0';

    if (twai_current_mode() == CAN_MODE_LISTEN_ONLY) {
        snprintf(buf, cap, "%s LO", up);
    } else {
        snprintf(buf, cap, "%s", up);
    }
}

static void refresh_host(void)
{
    const bool online = s_conn == UI_CONN_ONLINE;
    lv_color_t dot = COL_DIM;
    const char *text = "WIFI";

    switch (s_conn) {
    case UI_CONN_ONLINE:
        dot = COL_GREEN;
        break;
    case UI_CONN_HOST:
        dot = COL_ORANGE;
        text = "LINK";
        break;
    case UI_CONN_RECONNECT:
        dot = COL_ORANGE;
        text = "RETRY";
        break;
    default:
        break; /* Wi-Fi not associated yet */
    }

    if (!color_same(lv_obj_get_style_bg_color(s_host_dot, 0), dot)) {
        lv_obj_set_style_bg_color(s_host_dot, dot, 0);
    }
    if (online && s_rtt_ms > 0) {
        label_setf(s_host_label, s_txt_host, sizeof(s_txt_host), "HOST %ums",
                   (unsigned)s_rtt_ms);
    } else {
        label_setf(s_host_label, s_txt_host, sizeof(s_txt_host), "%s",
                   online ? "HOST" : text);
    }
    label_color(s_host_label, online ? COL_TEXT : COL_DIM);
}

static void refresh_bus(const can_stats_t *st)
{
    const char *name;
    lv_color_t color;

    if (strcmp(st->state, "BUS_OFF") == 0) {
        name = "BUS-OFF";
        color = COL_RED;
    } else if (strcmp(st->state, "RECOVERING") == 0 || st->tec > 127u || st->rec > 127u) {
        name = "PASSIVE";
        color = COL_ORANGE;
    } else if (strcmp(st->state, "RUNNING") == 0) {
        name = "ACTIVE";
        color = COL_GREEN;
    } else {
        name = "STOPPED";
        color = COL_DIM;
    }

    label_setf(s_bus_label, s_txt_bus, sizeof(s_txt_bus), "%s", name);
    if (!color_same(lv_obj_get_style_bg_color(s_bus_pill, 0), color)) {
        lv_obj_set_style_bg_color(s_bus_pill, color, 0);
    }
}

static void refresh_run(const can_stats_t *st)
{
    const char *text;
    lv_color_t color;

    if (s_triggered) {
        text = "TRIG'D";
        color = COL_BLUE;
    } else if (s_frozen || st->paused) {
        text = "PAUSED";
        color = COL_ORANGE;
    } else if (strcmp(st->state, "RUNNING") == 0) {
        text = "RUN";
        color = COL_GREEN;
    } else {
        text = "STOP";
        color = COL_DIM;
    }

    label_setf(s_run_label, s_txt_run, sizeof(s_txt_run), "%s", text);
    label_color(s_run_label, color);
}

static void refresh_bottom(const can_stats_t *st)
{
    label_setf(s_bot[0], s_txt_bot[0], sizeof(s_txt_bot[0]), "FPS %u",
               (unsigned)st->fps);
    label_color(s_bot[0], COL_TEXT);

    label_setf(s_bot[1], s_txt_bot[1], sizeof(s_txt_bot[1]), "LOAD %u%%",
               (unsigned)(st->load_permille / 10u));
    label_color(s_bot[1], st->load_permille >= 750u ? COL_ORANGE : COL_TEXT);

    label_setf(s_bot[2], s_txt_bot[2], sizeof(s_txt_bot[2]), "ERR %u/%u",
               (unsigned)st->tec, (unsigned)st->rec);
    label_color(s_bot[2], (st->tec >= 128u || st->rec >= 128u) ? COL_RED
                            : (st->tec || st->rec)                   ? COL_ORANGE
                                                                     : COL_DIM);

    sd_status_t sd;
    sd_store_get_status(&sd);

    /* The fourth tile doubles as SD/REC state while there is no overflow. */
    char ovf[10];
    if (st->dropped) {
        fmt_count(ovf, sizeof(ovf), st->dropped); /* keep long counts inside 80 px */
        label_setf(s_bot[3], s_txt_bot[3], sizeof(s_txt_bot[3]), "OVF %s", ovf);
        label_color(s_bot[3], COL_RED);
    } else if (sd.recording) {
        label_setf(s_bot[3], s_txt_bot[3], sizeof(s_txt_bot[3]), "REC %uF",
                   (unsigned)sd.files);
        label_color(s_bot[3], COL_GREEN);
    } else if (sd.mounted) {
        label_setf(s_bot[3], s_txt_bot[3], sizeof(s_txt_bot[3]), "SD READY");
        label_color(s_bot[3], COL_GREEN);
    } else if (sd.present) {
        label_setf(s_bot[3], s_txt_bot[3], sizeof(s_txt_bot[3]), "SD ERROR");
        label_color(s_bot[3], COL_ORANGE);
    } else {
        label_setf(s_bot[3], s_txt_bot[3], sizeof(s_txt_bot[3]), "NO SD");
        label_color(s_bot[3], COL_DIM);
    }
}

static void refresh_health(const can_stats_t *st)
{
    static const char *const names[4] = {"MISS", "QFULL", "BERR", "BOFF"};
    const uint32_t values[4] = {st->rx_missed, st->rx_queue_full, st->bus_errors,
                                st->bus_off};

    for (int i = 0; i < 4; i++) {
        char cnt[10];
        fmt_count(cnt, sizeof(cnt), values[i]);
        label_setf(s_health[i], s_txt_health[i], sizeof(s_txt_health[i]), "%s %s",
                   names[i], cnt);
        label_color(s_health[i], values[i] == 0 ? COL_DIM
                                    : i >= 2      ? COL_RED
                                                  : COL_ORANGE);
    }
}

static void refresh_monitor(const can_stats_t *st)
{
    char bitrate[12];
    fmt_bitrate(bitrate, sizeof(bitrate));
    label_setf(s_br_label, s_txt_br, sizeof(s_txt_br), "%s", bitrate);
    label_color(s_br_label, COL_TEXT);

    refresh_bus(st);
    refresh_run(st);
    refresh_host();
    refresh_bottom(st);
}

/* -------------------------------------------------------------- settings */

static void refresh_filter_pills(void)
{
    if (!s_lock_valid) {
        clickable_set(s_filter_pill[FILTER_LOCK], false);
        if (s_filter == FILTER_LOCK) {
            s_filter = FILTER_ALL;
        }
    } else {
        clickable_set(s_filter_pill[FILTER_LOCK], true);
    }

    for (int i = 0; i < FILTER_COUNT; i++) {
        const bool selected = (i == (int)s_filter) && (i != FILTER_LOCK || s_lock_valid);
        pill_style(s_filter_pill[i], selected, COL_BLUE);
    }

    if (s_lock_valid) {
        label_setf(s_lock_label, s_txt_lock, sizeof(s_txt_lock), "0x%X",
                   (unsigned)s_lock_id);
    } else {
        label_set_str(s_lock_label, s_txt_lock, sizeof(s_txt_lock), "0x---");
    }
}

static void refresh_trigger_pills(void)
{
    if (!s_lock_valid) {
        clickable_set(s_trig_pill[TRIG_LOCK], false);
        if (s_trig_mode == TRIG_LOCK) {
            s_trig_mode = TRIG_OFF;
            s_triggered = false;
            s_trig_pending = false;
        }
    } else {
        clickable_set(s_trig_pill[TRIG_LOCK], true);
    }

    for (int i = 0; i < TRIG_COUNT; i++) {
        pill_style(s_trig_pill[i], i == (int)s_trig_mode, COL_BLUE);
    }
}

static void refresh_settings(const can_stats_t *st)
{
    const can_bitrate_t br = twai_current_bitrate();
    for (int i = 0; i < 4; i++) {
        pill_style(s_br_pill[i], BITRATES[i] == br, COL_BLUE);
    }

    const can_mode_t mode = twai_current_mode();
    const char *mode_name = mode == CAN_MODE_NORMAL   ? "Normal"
                            : mode == CAN_MODE_SELFTEST ? "Selftest"
                                                        : "Listen";
    label_setf(s_mode_label, s_txt_mode, sizeof(s_txt_mode), "%s", mode_name);
    label_color(s_mode_label, mode == CAN_MODE_NORMAL ? COL_ORANGE : COL_GREEN);

    refresh_filter_pills();
    refresh_trigger_pills();

    const bool paused = s_triggered || s_frozen || st->paused;
    label_setf(s_pause_label, s_txt_pause, sizeof(s_txt_pause), "%s\n%s",
               paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, paused ? "Resume" : "Pause");
    label_color(s_pause_label, s_triggered ? COL_BLUE
                                : paused   ? COL_ORANGE
                                           : COL_TEXT);

    const bool remote = s_remote_on && lv_tick_elaps(s_remote_tick) < 1500u;
    sd_status_t sd;
    sd_store_get_status(&sd);
    if (remote) {
        label_set_str(s_info_label, s_txt_info, sizeof(s_txt_info), "REMOTE SYNC");
        label_color(s_info_label, COL_GREEN);
    } else if (sd.recording) {
        label_setf(s_info_label, s_txt_info, sizeof(s_txt_info), "REC %uF",
                   (unsigned)sd.files);
        label_color(s_info_label, COL_GREEN);
    } else if (sd.mounted) {
        label_set_str(s_info_label, s_txt_info, sizeof(s_txt_info), "SD READY");
        label_color(s_info_label, COL_DIM);
    } else if (sd.present) {
        label_set_str(s_info_label, s_txt_info, sizeof(s_txt_info), "SD ERROR");
        label_color(s_info_label, COL_ORANGE);
    } else {
        label_set_str(s_info_label, s_txt_info, sizeof(s_txt_info), "NO SD");
        label_color(s_info_label, COL_DIM);
    }
}

/* ---------------------------------------------------------------- timer */

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Drain the net_task mailbox (see the statics block): apply any pending
     * connection/remote-sync request here, on the LVGL task, with no locks. */
    if (s_conn != s_conn_req) {
        s_conn = s_conn_req;
        s_rtt_ms = s_rtt_req;
        refresh_host();
    } else if (s_conn == UI_CONN_ONLINE && s_rtt_ms != s_rtt_req) {
        s_rtt_ms = s_rtt_req;
        refresh_host();
    }
    if (s_remote_req) {
        s_remote_req = false;
        s_remote_tick = lv_tick_get();
        s_remote_on = true;

        if (s_filter_req != 0xFFFFFFFFu) {
            s_lock_valid = true;
            s_lock_id = s_filter_req;
            s_lock_ext = s_filter_req > 0x7FFu;
            s_filter = FILTER_LOCK;
            refresh_filter_pills();
            refresh_trigger_pills();
            refresh_row_highlight();
        }
    }

    can_stats_t st;
    twai_stats_snapshot(&st);

    /* linsight-cli clear / host CONTROL.clear_stats resets rx_total without
     * going through this UI: mirror the clear locally when it happens. */
    if (s_total_valid && st.rx_total < s_prev_total) {
        view_reset();
    }
    s_prev_total = st.rx_total;
    s_total_valid = true;

    scan_records(st.paused);

    /* Host pause freezes the rows but not the counters: restart the rate
     * window on resume so the pause duration never dilutes the first rate. */
    if (st.paused) {
        for (int i = 0; i < CNT_SLOTS; i++) {
            s_counts[i].win = 0;
        }
        s_rate_tick = lv_tick_get();
    } else {
        rate_sample();
    }

    check_error_trigger(&st);

    /* A trigger is a scene to look at: the locked-ID trigger belongs to the
     * raw trace, an error/BOff trigger to the health overview. Snapshot the
     * overview before leaving so the scene is what stays frozen. */
    if (s_trig_pending) {
        render_overview();
        page_show(s_trig_mode == TRIG_LOCK ? PAGE_RAW : PAGE_MONITOR);
    }

    if (s_page == PAGE_SETTINGS) {
        refresh_settings(&st);
    } else {
        refresh_monitor(&st);
        if (s_page == PAGE_RAW) {
            if (s_trig_pending) {
                render_trace(s_limit_on, s_trig_limit);
                s_trig_pending = false;
            } else if (!s_triggered && !s_frozen && !st.paused) {
                render_trace(false, 0);
            }
        } else {
            refresh_health(&st);
            if (s_trig_pending) {
                s_trig_pending = false; /* rendered as the pre-jump snapshot */
            } else if (!s_triggered && !s_frozen && !st.paused) {
                render_overview();
            }
        }
    }

    if (s_toast && !lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(s_toast_tick) >= 1400u) {
        object_show(s_toast, false);
    }
}

/* -------------------------------------------------------------- builders */

static void monitor_create(void)
{
    lv_obj_t *page = s_pages[PAGE_MONITOR];

    lv_obj_t *bar = obj_new(page, 0, 0, UI_W, 26);
    lv_obj_set_style_bg_color(bar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    s_bus_pill = obj_new(bar, 4, 4, 66, 18);
    lv_obj_set_style_bg_color(s_bus_pill, COL_DIM, 0);
    lv_obj_set_style_bg_opa(s_bus_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bus_pill, LV_RADIUS_CIRCLE, 0);
    s_bus_label = label_new(s_bus_pill, "STOPPED", &lv_font_montserrat_12, COL_BG);
    lv_obj_center(s_bus_label);

    s_br_label = label_new(bar, "", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_pos(s_br_label, 82, 5);

    s_run_label = label_new(bar, "", &lv_font_montserrat_12, COL_GREEN);
    lv_obj_set_pos(s_run_label, 136, 5);

    s_host_dot = obj_new(bar, 196, 10, 6, 6);
    lv_obj_set_style_bg_color(s_host_dot, COL_DIM, 0);
    lv_obj_set_style_bg_opa(s_host_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_host_dot, LV_RADIUS_CIRCLE, 0);
    s_host_label = label_new(bar, "", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(s_host_label, 205, 5);

    lv_obj_t *gear = obj_new(bar, 272, 1, 44, 24);
    lv_obj_set_style_bg_color(gear, COL_TILE, 0);
    lv_obj_set_style_bg_color(gear, COL_ACTIVE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(gear, 6, 0);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(gear, settings_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gear_label = label_new(gear, LV_SYMBOL_SETTINGS, &lv_font_montserrat_16,
                                     COL_TEXT);
    lv_obj_center(gear_label);

    /* Overview group: health strip, header and 6 large top-ID rows. */
    s_ov_group = obj_new(page, 0, OV_Y, UI_W, OV_H);
    lv_obj_add_flag(s_ov_group, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *health = obj_new(s_ov_group, 0, 0, UI_W, OV_HEALTH_H);
    lv_obj_set_style_bg_color(health, COL_BAR, 0);
    lv_obj_set_style_bg_opa(health, LV_OPA_COVER, 0);
    for (int i = 0; i < 4; i++) {
        s_health[i] = label_new(health, "", &lv_font_montserrat_12, COL_DIM);
        lv_obj_set_pos(s_health[i], i * 80, 4);
        lv_obj_set_width(s_health[i], 80);
        lv_obj_set_style_text_align(s_health[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *hdr = label_new(s_ov_group, "ID", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 4, OV_HDR_Y);
    hdr = label_new(s_ov_group, "RATE/s", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 140, OV_HDR_Y);
    lv_obj_set_width(hdr, 62);
    lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_RIGHT, 0);
    hdr = label_new(s_ov_group, "PER", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 206, OV_HDR_Y);
    lv_obj_set_width(hdr, 40);
    lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_RIGHT, 0);
    hdr = label_new(s_ov_group, "CNT", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 248, OV_HDR_Y);
    lv_obj_set_width(hdr, 60);
    lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < TOP_N; i++) {
        lv_obj_t *row = obj_new(s_ov_group, 0, OV_ROWS_Y + i * OV_ROW_H, UI_W, OV_ROW_H);
        lv_obj_set_style_bg_color(row, (i & 1) ? COL_ROW_ODD : COL_ROW_EVEN, 0);
        lv_obj_set_style_bg_color(row, COL_ACTIVE, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(row, ov_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_ov_row[i] = row;

        s_ov_id[i] = label_new(row, "", &lv_font_montserrat_20, COL_BLUE);
        lv_obj_set_pos(s_ov_id[i], 4, 1);
        lv_obj_set_width(s_ov_id[i], 132);

        s_ov_rate[i] = label_new(row, "", &lv_font_montserrat_20, COL_TEXT);
        lv_obj_set_pos(s_ov_rate[i], 140, 1);
        lv_obj_set_width(s_ov_rate[i], 62);
        lv_obj_set_style_text_align(s_ov_rate[i], LV_TEXT_ALIGN_RIGHT, 0);

        s_ov_per[i] = label_new(row, "", &lv_font_montserrat_12, COL_DIM);
        lv_obj_set_pos(s_ov_per[i], 206, 5);
        lv_obj_set_width(s_ov_per[i], 40);
        lv_obj_set_style_text_align(s_ov_per[i], LV_TEXT_ALIGN_RIGHT, 0);

        s_ov_cnt[i] = label_new(row, "", &lv_font_montserrat_12, COL_DIM);
        lv_obj_set_pos(s_ov_cnt[i], 248, 5);
        lv_obj_set_width(s_ov_cnt[i], 60);
        lv_obj_set_style_text_align(s_ov_cnt[i], LV_TEXT_ALIGN_RIGHT, 0);

        s_ov_bar[i] = obj_new(row, 311, 20, 6, 2);
        lv_obj_set_style_bg_color(s_ov_bar[i], COL_BLUE, 0);
        lv_obj_set_style_bg_opa(s_ov_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_ov_bar[i], 3, 0);
        object_show(s_ov_bar[i], false);
    }

    /* Shown while the ranking has no live IDs (topmost, but not clickable). */
    s_ov_empty = obj_new(s_ov_group, 0, OV_ROWS_Y, UI_W, TOP_N * OV_ROW_H);
    lv_obj_t *ov_hint =
        label_new(s_ov_empty, "Waiting for CAN frames\ncheck wiring / bitrate",
                  &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(ov_hint, UI_W - 24);
    lv_obj_set_style_text_align(ov_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(ov_hint, 4, 0);
    lv_obj_center(ov_hint);

    /* Raw group: the original 8 x 21 px frame trace, same container. */
    s_raw_group = obj_new(page, 0, OV_Y, UI_W, OV_H);
    lv_obj_add_flag(s_raw_group, LV_OBJ_FLAG_GESTURE_BUBBLE);

    static const char *headers[3] = {"ID", "L", "DATA (HEX)"};
    static const lv_coord_t hx[3] = {4, 70, 88};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *label = label_new(s_raw_group, headers[i], &lv_font_montserrat_12,
                                    COL_DIM);
        lv_obj_set_pos(label, hx[i], RAW_HDR_Y);
    }
    lv_obj_t *cnt_header = label_new(s_raw_group, "CNT", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(cnt_header, 268, RAW_HDR_Y);
    lv_obj_set_width(cnt_header, 48);
    lv_obj_set_style_text_align(cnt_header, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < ROW_N; i++) {
        lv_obj_t *row = obj_new(s_raw_group, 0, RAW_ROWS_Y + i * ROW_H, UI_W, ROW_H);
        lv_obj_set_style_bg_color(row, (i & 1) ? COL_ROW_ODD : COL_ROW_EVEN, 0);
        lv_obj_set_style_bg_color(row, COL_ACTIVE, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_row[i] = row;

        s_row_id[i] = label_new(row, "", &lv_font_montserrat_12, COL_BLUE);
        lv_obj_set_pos(s_row_id[i], 4, 3);
        s_row_dlc[i] = label_new(row, "", &lv_font_montserrat_12, COL_DIM);
        lv_obj_set_pos(s_row_dlc[i], 70, 3);
        s_row_data[i] = label_new(row, "", &lv_font_montserrat_12, COL_TEXT);
        lv_obj_set_pos(s_row_data[i], 88, 3);
        s_row_cnt[i] = label_new(row, "", &lv_font_montserrat_12, COL_DIM);
        lv_obj_set_pos(s_row_cnt[i], 268, 3);
        lv_obj_set_width(s_row_cnt[i], 48);
        lv_obj_set_style_text_align(s_row_cnt[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* Shown only while the grid has no matching frames (topmost, but not
     * clickable, so taps still reach the zebra rows underneath). */
    s_empty = obj_new(s_raw_group, 0, RAW_ROWS_Y, UI_W, ROW_N * ROW_H);
    lv_obj_t *empty_hint =
        label_new(s_empty, "Waiting for CAN frames\ncheck wiring / bitrate",
                  &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(empty_hint, UI_W - 24);
    lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(empty_hint, 4, 0);
    lv_obj_center(empty_hint);

    lv_obj_t *bottom = obj_new(page, 0, 210, UI_W, 30);
    lv_obj_set_style_bg_color(bottom, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, 0);
    for (int i = 0; i < 4; i++) {
        s_bot[i] = label_new(bottom, "", &lv_font_montserrat_12,
                             i == 3 ? COL_DIM : COL_TEXT);
        lv_obj_set_width(s_bot[i], 80);
        lv_obj_set_style_text_align(s_bot[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_bot[i], i * 80, (30 - 15) / 2);
    }
}

static void settings_create(void)
{
    lv_obj_t *page = s_pages[page_container(PAGE_SETTINGS)];

    lv_obj_t *bar = obj_new(page, 0, 0, UI_W, 28);
    lv_obj_set_style_bg_color(bar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    lv_obj_t *back = obj_new(bar, 0, 0, 104, 28);
    lv_obj_set_style_bg_color(back, COL_TILE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = label_new(back, LV_SYMBOL_LEFT " Back",
                                     &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_pos(back_label, 8, 6);

    s_info_label = label_new(bar, "", &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(s_info_label, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Card 1: bitrate + mode (design: Y 34..86). */
    lv_obj_t *card = card_new(page, 8, 34, 304, 52);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *pill = pill_new(card, 12 + i * 48, 10, 44, 32);
        lv_label_set_text_static((lv_obj_t *)lv_obj_get_child(pill, 0), BITRATE_LABELS[i]);
        lv_obj_add_event_cb(pill, bitrate_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_br_pill[i] = pill;
    }

    s_mode_pill = pill_new(card, 208, 10, 84, 32);
    lv_obj_add_event_cb(s_mode_pill, mode_click_cb, LV_EVENT_CLICKED, NULL);
    s_mode_label = (lv_obj_t *)lv_obj_get_child(s_mode_pill, 0);

    /* Card 2: filter + trigger (design: Y 92..168). */
    card = card_new(page, 8, 92, 304, 76);
    lv_obj_t *cap = label_new(card, "FILTER", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(cap, 12, 11);
    cap = label_new(card, "TRIG", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(cap, 12, 43);

    static const lv_coord_t fx[FILTER_COUNT] = {60, 118, 166, 214};
    static const lv_coord_t fw[FILTER_COUNT] = {54, 44, 44, 78};
    static const char *filter_names[FILTER_COUNT] = {"All IDs", "Std", "Ext", ""};
    for (int i = 0; i < FILTER_COUNT; i++) {
        lv_obj_t *pill = pill_new(card, fx[i], 10, fw[i], 24);
        lv_obj_t *label = (lv_obj_t *)lv_obj_get_child(pill, 0);
        if (i == FILTER_LOCK) {
            s_lock_label = label;
            lv_label_set_text_static(label, "0x---");
        } else {
            lv_label_set_text_static(label, filter_names[i]);
        }
        lv_obj_add_event_cb(pill, filter_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_filter_pill[i] = pill;
    }

    static const lv_coord_t tx[TRIG_COUNT] = {60, 118, 194};
    static const lv_coord_t tw[TRIG_COUNT] = {54, 72, 94};
    static const char *trig_names[TRIG_COUNT] = {"OFF", "Error/BOff", "Locked ID"};
    for (int i = 0; i < TRIG_COUNT; i++) {
        lv_obj_t *pill = pill_new(card, tx[i], 42, tw[i], 24);
        lv_label_set_text_static((lv_obj_t *)lv_obj_get_child(pill, 0), trig_names[i]);
        lv_obj_add_event_cb(pill, trig_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_trig_pill[i] = pill;
    }

    /* Card 3: quick actions (design: Y 174..234). */
    card = card_new(page, 8, 174, 304, 60);
    lv_obj_t *pause = action_btn_new(card, 2, LV_SYMBOL_PAUSE "\nPause");
    s_pause_label = (lv_obj_t *)lv_obj_get_child(pause, 0);
    lv_obj_add_event_cb(pause, pause_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear = action_btn_new(card, 103, LV_SYMBOL_TRASH "\nClear");
    lv_obj_add_event_cb(clear, clear_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *recover = action_btn_new(card, 204, LV_SYMBOL_REFRESH "\nRecover");
    lv_obj_add_event_cb(recover, recover_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------------------------------------------------------- splash */

#define SPLASH_CONTENT_W 240
#define SPLASH_BAR_W 40
#define SPLASH_BAR_MS 1000u

/* The underline grows from the title's center until it reaches its final
 * width; the ready callback then swaps straight to the Monitor page. */
static void splash_bar_cb(void *var, int32_t value)
{
    lv_obj_set_width((lv_obj_t *)var, (lv_coord_t)value);
    lv_obj_set_x((lv_obj_t *)var, (SPLASH_CONTENT_W - value) / 2);
}

static void splash_ready_cb(lv_anim_t *a)
{
    lv_obj_del((lv_obj_t *)lv_anim_get_user_data(a));
}

/* Boot splash on the top layer: brand and subtitle are static, a blue bar
 * grows underneath, then the whole overlay is deleted. It is clickable, so
 * stray touches are absorbed until gone. */
static void splash_create(void)
{
    lv_obj_t *splash = obj_new(lv_layer_top(), 0, 0, UI_W, UI_H);
    lv_obj_set_style_bg_color(splash, COL_BG, 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_add_flag(splash, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *content = obj_new(splash, (UI_W - SPLASH_CONTENT_W) / 2, 84,
                                SPLASH_CONTENT_W, 88);

    lv_obj_t *brand = label_new(content, "LinSight", &lv_font_montserrat_24, COL_TEXT);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 0);

    /* Blue accent bar, SF-symbol style; centered, starts at zero width. */
    lv_obj_t *bar = obj_new(content, SPLASH_CONTENT_W / 2, 34, 0, 3);
    lv_obj_set_style_bg_color(bar, COL_BLUE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);

    lv_obj_t *sub = label_new(content, "CAN ANALYZER", &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 48);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, splash_bar_cb);
    lv_anim_set_values(&a, 0, SPLASH_BAR_W);
    lv_anim_set_time(&a, SPLASH_BAR_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, splash);
    lv_anim_set_ready_cb(&a, splash_ready_cb);
    lv_anim_start(&a);
}

/* --------------------------------------------------------------- public */

void linsight_ui_init(void)
{
    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < PAGE_CONTAINERS; i++) {
        lv_obj_t *page = obj_new(s_screen, 0, 0, UI_W, UI_H);
        lv_obj_set_style_bg_color(page, COL_BG, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
        /* Must be clickable to become the gesture target on blank areas, and
         * must NOT bubble gestures (the handler lives on the page itself). */
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(page, gesture_cb, LV_EVENT_GESTURE, NULL);
        s_pages[i] = page;
    }

    monitor_create();
    settings_create();
    page_show(PAGE_MONITOR);

    s_rate_tick = lv_tick_get();
    refresh_filter_pills();
    refresh_trigger_pills();
    refresh_host();

    /* Screen-level toast, above both pages, over the telemetry bar. */
    s_toast = obj_new(s_screen, 40, 212, 240, 24);
    lv_obj_set_style_bg_color(s_toast, COL_TILE, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_t *toast_label = label_new(s_toast, "", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_width(toast_label, 232);
    lv_obj_set_style_text_align(toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(toast_label);
    object_show(s_toast, false);

    lv_timer_create(tick_cb, 50, NULL); /* 20 Hz */

    splash_create(); /* boot animation; deletes itself after ~1.45 s */
}

void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms)
{
    s_conn_req = state;
    s_rtt_req = rtt_ms;
}

void linsight_ui_handle_key(bool long_press)
{
    lvgl_port_lock(0);
    if (long_press) {
        page_show(page_next(s_page));
    } else {
        toggle_pause();
    }
    lvgl_port_unlock();
}

void linsight_ui_sync_control(uint8_t paused, uint16_t bitrate_kbps, uint32_t filter_id)
{
    /* paused / bitrate_kbps are informational here: the 20 Hz tick reads the
     * authoritative values from twai. The call itself proves the host link is
     * alive, which is what the REMOTE SYNC badge reports. */
    (void)paused;
    (void)bitrate_kbps;

    s_remote_req = true;
    s_filter_req = filter_id;
}
