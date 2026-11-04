/*
 * LinSight CAN analyzer UI (320x240, LVGL 8.3) — iOS dark design language.
 *
 * Structure: hairline-separated top bar (segmented nav, PAUSED chip, link
 * status), Monitor page (status row, 8-row frame trace, 4-column hero stat
 * card) and Settings page (iOS grouped cards: bitrate / mode / actions).
 * All capture data is pulled by a 20 Hz LVGL timer; external code only pushes
 * conn state via ui.h setters. Row updates are dirty-checked to keep the SPI
 * bus quiet when nothing changed.
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "can_ring.h"
#include "esp_lvgl_port.h"
#include "frame.h"
#include "twai.h"
#include "lvgl.h"

#define UI_W 320
#define UI_H 240
#define TOPBAR_H 28

/* iOS dark system palette. */
#define COL_BG lv_color_hex(0x000000)
#define COL_CARD lv_color_hex(0x1C1C1E)
#define COL_TRACK lv_color_hex(0x2C2C2E)
#define COL_PRESSED lv_color_hex(0x3A3A3C)
#define COL_TEXT lv_color_hex(0xFFFFFF)
#define COL_DIM lv_color_hex(0x8E8E93)
#define COL_BLUE lv_color_hex(0x0A84FF)
#define COL_PURPLE lv_color_hex(0xBF5AF2)
#define COL_GREEN lv_color_hex(0x30D158)
#define COL_ORANGE lv_color_hex(0xFF9F0A)
#define COL_RED lv_color_hex(0xFF453A)

#define TRACE_ROWS 8
#define ROW_H 16
#define TRACE_Y0 74

typedef enum {
    PAGE_MONITOR = 0,
    PAGE_SETTINGS,
    PAGE_COUNT,
} page_id_t;

/* ------------------------------------------------------------- primitives */

static lv_obj_t *obj_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                         lv_coord_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label_new(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *card_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h)
{
    lv_obj_t *card = obj_new(parent, x, y, w, h);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    return card;
}

/* Grouped-card header: small uppercase dim caption. */
static void card_header(lv_obj_t *card, const char *text)
{
    lv_obj_t *label = label_new(card, text, &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(label, 12, 8);
}

/* Pill button with a bigger hot zone than its visual size. */
static lv_obj_t *pill_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h)
{
    lv_obj_t *pill = obj_new(parent, x, y, w, h);
    lv_obj_set_style_bg_color(pill, COL_TRACK, 0);
    lv_obj_set_style_bg_color(pill, COL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(pill, 6);
    return pill;
}

static void pill_select(lv_obj_t *pill, bool selected)
{
    lv_obj_set_style_bg_color(pill, selected ? COL_PRESSED : COL_TRACK, 0);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_child(pill, 0);
    lv_obj_set_style_text_color(label, selected ? COL_TEXT : COL_DIM, 0);
}

/* --------------------------------------------------------------- statics */

static lv_obj_t *s_screen;
static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_nav_seg[PAGE_COUNT];
static page_id_t s_page = PAGE_MONITOR;

/* topbar */
static lv_obj_t *s_conn_dot;
static lv_obj_t *s_conn_label;
static lv_obj_t *s_paused_chip;

/* monitor */
static lv_obj_t *s_bitrate_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_row_seq[TRACE_ROWS];
static lv_obj_t *s_row_id[TRACE_ROWS];
static lv_obj_t *s_row_flg[TRACE_ROWS];
static lv_obj_t *s_row_dlc[TRACE_ROWS];
static lv_obj_t *s_row_data[TRACE_ROWS];
static lv_obj_t *s_stat_val[4];
static const char *STAT_CAPS[4] = {"FPS", "LOAD", "ERR", "DROP"};

/* settings */
static lv_obj_t *s_br_pills[4];
static lv_obj_t *s_mode_pills[2];
static lv_obj_t *s_pause_label;

static const can_bitrate_t BITRATES[4] = {CAN_BITRATE_125K, CAN_BITRATE_250K,
                                          CAN_BITRATE_500K, CAN_BITRATE_1M};
static const char *BITRATE_NAMES[4] = {"125k", "250k", "500k", "1M"};

/* ---------------------------------------------------------------- pages */

static void opa_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void page_show(page_id_t page)
{
    s_page = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *seg = s_nav_seg[i];
        lv_obj_t *label = (lv_obj_t *)lv_obj_get_child(seg, 0);
        const bool sel = (i == (int)page);
        lv_obj_set_style_bg_color(seg, sel ? COL_PRESSED : COL_CARD, 0);
        lv_obj_set_style_text_color(label, sel ? COL_TEXT : COL_DIM, 0);
        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_pages[page], LV_OBJ_FLAG_HIDDEN);

    /* Subtle iOS-style fade-in on page entry. */
    lv_anim_del(s_pages[page], opa_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pages[page]);
    lv_anim_set_exec_cb(&a, opa_anim_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
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
        page_show(PAGE_SETTINGS);
    } else if (dir == LV_DIR_RIGHT) {
        page_show(PAGE_MONITOR);
    }
}

/* Tap anywhere on the segmented control picks the nearest segment. */
static void seg_click_cb(lv_event_t *e)
{
    lv_obj_t *track = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(track, &a);

    const int rel = p.x - a.x1;
    const int width = a.x2 - a.x1 + 1;
    int idx = (rel < 0) ? 0 : (rel * PAGE_COUNT) / width;
    if (idx >= PAGE_COUNT) {
        idx = PAGE_COUNT - 1;
    }
    page_show((page_id_t)idx);
}

static void topbar_create(void)
{
    lv_obj_t *topbar = obj_new(s_screen, 0, 0, UI_W, TOPBAR_H);
    lv_obj_set_style_bg_color(topbar, COL_BG, 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);

    /* iOS segmented control. */
    lv_obj_t *track = obj_new(topbar, 8, 3, 124, 22);
    lv_obj_set_style_bg_color(track, COL_CARD, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_add_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(track, 9);
    lv_obj_add_event_cb(track, seg_click_cb, LV_EVENT_CLICKED, NULL);

    static const char *names[PAGE_COUNT] = {"Monitor", "Settings"};
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *seg = obj_new(track, 2 + i * 61, 2, 59, 18);
        lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
        s_nav_seg[i] = seg;
        label_new(seg, names[i], &lv_font_montserrat_12, COL_DIM);
        lv_obj_center((lv_obj_t *)lv_obj_get_child(seg, 0));
    }

    /* Global PAUSED chip, shown whenever capture is paused. */
    s_paused_chip = obj_new(topbar, 140, 3, 64, 22);
    lv_obj_set_style_bg_color(s_paused_chip, COL_ORANGE, 0);
    lv_obj_set_style_bg_opa(s_paused_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_paused_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *chip_label = label_new(s_paused_chip, "PAUSED", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_center(chip_label);
    lv_obj_add_flag(s_paused_chip, LV_OBJ_FLAG_HIDDEN);

    /* Link status: colored dot + text, right aligned. */
    s_conn_dot = obj_new(topbar, 0, 0, 8, 8);
    lv_obj_align(s_conn_dot, LV_ALIGN_RIGHT_MID, -56, 0);
    lv_obj_set_style_bg_color(s_conn_dot, COL_DIM, 0);
    lv_obj_set_style_bg_opa(s_conn_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_conn_dot, LV_RADIUS_CIRCLE, 0);

    s_conn_label = label_new(topbar, "WiFi", &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(s_conn_label, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Hairline under the top bar. */
    lv_obj_t *hairline = obj_new(s_screen, 0, TOPBAR_H, UI_W, 1);
    lv_obj_set_style_bg_color(hairline, COL_CARD, 0);
    lv_obj_set_style_bg_opa(hairline, LV_OPA_COVER, 0);
}

/* --------------------------------------------------------------- monitor */

static void trace_row_create(lv_obj_t *page, int i)
{
    const lv_coord_t y = TRACE_Y0 + i * ROW_H;
    s_row_seq[i] = label_new(page, "", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(s_row_seq[i], 8, y);
    s_row_id[i] = label_new(page, "", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_pos(s_row_id[i], 46, y);
    s_row_flg[i] = label_new(page, "", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(s_row_flg[i], 112, y);
    s_row_dlc[i] = label_new(page, "", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(s_row_dlc[i], 126, y);
    s_row_data[i] = label_new(page, "", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(s_row_data[i], 148, y);
}

/* 4-column hero stat card: big value over a dim caption. */
static void stat_card_create(lv_obj_t *page)
{
    lv_obj_t *card = card_new(page, 8, 202, 304, 34);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *col = obj_new(card, i * 76, 0, 76, 34);
        s_stat_val[i] = label_new(col, "0", &lv_font_montserrat_14, COL_TEXT);
        lv_obj_align(s_stat_val[i], LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_t *cap = label_new(col, STAT_CAPS[i], &lv_font_montserrat_12, COL_DIM);
        lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 19);
    }
}

static void monitor_create(void)
{
    lv_obj_t *page = s_pages[PAGE_MONITOR];

    s_bitrate_label = label_new(page, "500k", &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_pos(s_bitrate_label, 8, 33);
    s_mode_label = label_new(page, "LISTEN", &lv_font_montserrat_14, COL_ORANGE);
    lv_obj_set_pos(s_mode_label, 62, 33);
    s_state_label = label_new(page, "STOPPED", &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_pos(s_state_label, 140, 33);

    static const char *headers[5] = {"SEQ", "ID", "F", "DLC", "DATA"};
    static const lv_coord_t hx[5] = {8, 46, 112, 126, 148};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *h = label_new(page, headers[i], &lv_font_montserrat_12, COL_DIM);
        lv_obj_set_pos(h, hx[i], 56);
    }

    for (int i = 0; i < TRACE_ROWS; i++) {
        trace_row_create(page, i);
    }

    stat_card_create(page);
}

/* -------------------------------------------------------------- settings */

static void bitrate_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    twai_capture_restart(BITRATES[idx], twai_current_mode());
}

static void mode_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    twai_capture_restart(twai_current_bitrate(),
                         idx == 0 ? CAN_MODE_LISTEN_ONLY : CAN_MODE_NORMAL);
}

static void pause_click_cb(lv_event_t *e)
{
    (void)e;
    can_stats_t st;
    twai_stats_snapshot(&st);
    twai_capture_set_paused(!st.paused);
}

static void clear_click_cb(lv_event_t *e)
{
    (void)e;
    twai_stats_clear();
}

static void settings_create(void)
{
    lv_obj_t *page = s_pages[PAGE_SETTINGS];

    lv_obj_t *br_card = card_new(page, 8, 32, 304, 84);
    card_header(br_card, "BITRATE");
    for (int i = 0; i < 4; i++) {
        lv_obj_t *pill = pill_new(br_card, 12 + i * 71, 34, 66, 32);
        label_new(pill, BITRATE_NAMES[i], &lv_font_montserrat_14, COL_DIM);
        lv_obj_center((lv_obj_t *)lv_obj_get_child(pill, 0));
        lv_obj_add_event_cb(pill, bitrate_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_br_pills[i] = pill;
    }

    lv_obj_t *mode_card = card_new(page, 8, 124, 304, 62);
    card_header(mode_card, "MODE");
    static const char *mode_names[2] = {"Listen-only", "Normal"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *pill = pill_new(mode_card, 12 + i * 142, 27, 138, 28);
        label_new(pill, mode_names[i], &lv_font_montserrat_14, COL_DIM);
        lv_obj_center((lv_obj_t *)lv_obj_get_child(pill, 0));
        lv_obj_add_event_cb(pill, mode_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_mode_pills[i] = pill;
    }

    lv_obj_t *act_card = card_new(page, 8, 194, 304, 40);
    lv_obj_t *pause = pill_new(act_card, 12, 6, 140, 28);
    s_pause_label = label_new(pause, "Pause", &lv_font_montserrat_14, COL_TEXT);
    lv_obj_center(s_pause_label);
    lv_obj_add_event_cb(pause, pause_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear = pill_new(act_card, 158, 6, 134, 28);
    label_new(clear, "Clear stats", &lv_font_montserrat_14, COL_TEXT);
    lv_obj_center((lv_obj_t *)lv_obj_get_child(clear, 0));
    lv_obj_add_event_cb(clear, clear_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------------------------------------------------------- refresh */

/* Returns true when the row content is unchanged (skip redraw). */
static bool trace_row_same(const linsight_can_record_t *a, const linsight_can_record_t *b)
{
    return a->seq == b->seq && a->ts_ms == b->ts_ms && a->id == b->id &&
           a->flags == b->flags && a->dlc == b->dlc &&
           memcmp(a->data, b->data, LINSIGHT_DLC_MAX) == 0;
}

static void refresh_trace(void)
{
    static linsight_can_record_t tail[TRACE_ROWS];
    static linsight_can_record_t prev[TRACE_ROWS];
    static uint32_t prev_n = 0xFFFFFFFFu;
    static bool prev_valid[TRACE_ROWS];
    static char buf_seq[8], buf_id[12], buf_flg[4], buf_dlc[4], buf_data[32];

    const uint32_t n = can_ring_tail(tail, TRACE_ROWS);

    for (uint32_t i = 0; i < TRACE_ROWS; i++) {
        if (i >= n) {
            if (prev_valid[i]) {
                lv_label_set_text(s_row_seq[i], "");
                lv_label_set_text(s_row_id[i], "");
                lv_label_set_text(s_row_flg[i], "");
                lv_label_set_text(s_row_dlc[i], "");
                lv_label_set_text(s_row_data[i], "");
                prev_valid[i] = false;
            }
            continue;
        }

        const linsight_can_record_t *r = &tail[i];
        if (prev_valid[i] && prev_n == n && trace_row_same(r, &prev[i])) {
            continue;
        }
        prev[i] = *r;
        prev_valid[i] = true;

        snprintf(buf_seq, sizeof(buf_seq), "%5u", (unsigned)r->seq);
        if (r->flags & LINSIGHT_REC_FLAG_EXT) {
            snprintf(buf_id, sizeof(buf_id), "%08X", (unsigned)r->id);
        } else {
            snprintf(buf_id, sizeof(buf_id), "    %03X", (unsigned)r->id);
        }
        snprintf(buf_flg, sizeof(buf_flg), "%c%s",
                 (r->flags & LINSIGHT_REC_FLAG_EXT) ? 'X' : 'S',
                 (r->flags & LINSIGHT_REC_FLAG_RTR) ? "R" : "");
        snprintf(buf_dlc, sizeof(buf_dlc), "%u", (unsigned)r->dlc);

        char *p = buf_data;
        for (uint8_t b = 0; b < r->dlc && b < LINSIGHT_DLC_MAX; b++) {
            p += snprintf(p, (size_t)(buf_data + sizeof(buf_data) - p), "%02X ",
                          (unsigned)r->data[b]);
        }
        *p = '\0';

        lv_label_set_text(s_row_seq[i], buf_seq);
        lv_label_set_text(s_row_id[i], buf_id);
        lv_obj_set_style_text_color(
            s_row_id[i], (r->flags & LINSIGHT_REC_FLAG_EXT) ? COL_PURPLE : COL_TEXT, 0);
        lv_label_set_text(s_row_flg[i], buf_flg);
        lv_label_set_text(s_row_dlc[i], buf_dlc);
        lv_label_set_text(s_row_data[i], buf_data);
    }
    prev_n = n;
}

/* One hero-stat column: value + conditional color. */
static void stat_col_set(int i, const char *value, lv_color_t color)
{
    lv_obj_t *label = s_stat_val[i];
    lv_label_set_text(label, value);
    lv_obj_set_style_text_color(label, color, 0);
}

static void refresh_monitor(const can_stats_t *st)
{
    lv_label_set_text(s_bitrate_label, can_bitrate_name(twai_current_bitrate()));

    lv_obj_set_style_text_color(s_mode_label,
                                twai_current_mode() == CAN_MODE_NORMAL ? COL_GREEN : COL_ORANGE,
                                0);
    lv_label_set_text(s_mode_label, can_mode_name(twai_current_mode()));

    const lv_color_t state_col =
        strcmp(st->state, "RUNNING") == 0 ? COL_GREEN
        : strcmp(st->state, "STOPPED") == 0 ? COL_DIM
                                            : COL_RED;
    lv_obj_set_style_text_color(s_state_label, state_col, 0);
    lv_label_set_text(s_state_label, st->state);

    char buf[12];
    snprintf(buf, sizeof(buf), "%u", (unsigned)st->fps);
    stat_col_set(0, buf, COL_TEXT);

    snprintf(buf, sizeof(buf), "%u%%", (unsigned)(st->load_permille / 10));
    stat_col_set(1, buf, st->load_permille > 850 ? COL_ORANGE : COL_TEXT);

    const uint32_t errs = st->bus_errors + st->rx_missed;
    snprintf(buf, sizeof(buf), "%u", (unsigned)errs);
    stat_col_set(2, buf, errs ? COL_RED : COL_TEXT);

    snprintf(buf, sizeof(buf), "%u", (unsigned)st->dropped);
    stat_col_set(3, buf, st->dropped ? COL_ORANGE : COL_TEXT);
}

static void refresh_settings(const can_stats_t *st)
{
    const can_bitrate_t br = twai_current_bitrate();
    for (int i = 0; i < 4; i++) {
        pill_select(s_br_pills[i], BITRATES[i] == br);
    }
    const can_mode_t mode = twai_current_mode();
    pill_select(s_mode_pills[0], mode == CAN_MODE_LISTEN_ONLY);
    pill_select(s_mode_pills[1], mode == CAN_MODE_NORMAL);
    lv_label_set_text(s_pause_label, st->paused ? "Resume" : "Pause");
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    can_stats_t st;
    twai_stats_snapshot(&st);

    /* PAUSED chip is global (both pages). */
    if (st.paused) {
        lv_obj_clear_flag(s_paused_chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_paused_chip, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_page == PAGE_MONITOR) {
        refresh_monitor(&st);
        refresh_trace();
    } else {
        refresh_settings(&st);
    }
}

/* ---------------------------------------------------------------- splash */

static void splash_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void splash_ready_cb(lv_anim_t *a)
{
    lv_obj_del((lv_obj_t *)a->var);
}

static void splash_create(void)
{
    lv_obj_t *splash = obj_new(lv_layer_top(), 0, 0, UI_W, UI_H);
    lv_obj_set_style_bg_color(splash, COL_BG, 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_add_flag(splash, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *content = obj_new(splash, 40, 84, 240, 88);
    lv_obj_set_style_opa(content, LV_OPA_TRANSP, 0);

    lv_obj_t *brand = label_new(content, "LinSight", &lv_font_montserrat_24, COL_TEXT);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 0);

    /* Blue accent bar, SF-symbol style. */
    lv_obj_t *bar = obj_new(content, 100, 34, 40, 3);
    lv_obj_set_style_bg_color(bar, COL_BLUE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);

    lv_obj_t *sub = label_new(content, "CAN ANALYZER", &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 48);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, content);
    lv_anim_set_exec_cb(&a, splash_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 450);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, splash);
    lv_anim_set_exec_cb(&a, splash_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 1200);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, splash_ready_cb);
    lv_anim_start(&a);
}

/* ---------------------------------------------------------------- public */

void linsight_ui_init(void)
{
    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    for (int i = 0; i < PAGE_COUNT; i++) {
        s_pages[i] = obj_new(s_screen, 0, TOPBAR_H, UI_W, UI_H - TOPBAR_H);
        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_pages[i], gesture_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

    topbar_create();
    monitor_create();
    settings_create();
    page_show(PAGE_MONITOR);
    splash_create();

    lv_timer_create(refresh_timer_cb, 50, NULL); /* 20 Hz */
}

void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms)
{
    lvgl_port_lock(0);

    lv_color_t color;
    const char *text;
    switch (state) {
    case UI_CONN_HOST:
        color = COL_ORANGE;
        text = "Host...";
        break;
    case UI_CONN_ONLINE:
        color = COL_GREEN;
        text = "online";
        break;
    case UI_CONN_RECONNECT:
        color = COL_RED;
        text = "Host...";
        break;
    case UI_CONN_WIFI:
    default:
        color = COL_DIM;
        text = "WiFi...";
        break;
    }
    lv_obj_set_style_bg_color(s_conn_dot, color, 0);

    char buf[24];
    if (state == UI_CONN_ONLINE) {
        snprintf(buf, sizeof(buf), "%u ms", (unsigned)rtt_ms);
    } else {
        snprintf(buf, sizeof(buf), "%s", text);
    }
    lv_label_set_text(s_conn_label, buf);

    lvgl_port_unlock();
}

void linsight_ui_toggle_settings(void)
{
    lvgl_port_lock(0);
    page_show(s_page == PAGE_MONITOR ? PAGE_SETTINGS : PAGE_MONITOR);
    lvgl_port_unlock();
}
