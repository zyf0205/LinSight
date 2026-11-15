/*
 * LinSight CAN analyzer UI (320x240, LVGL 8.3) — Cyber Instrument HIG.
 * Zero dynamic heap allocation on 20 Hz tick.
 */

#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "device_info.h"
#include "esp_lvgl_port.h"
#include "frame.h"
#include "lvgl.h"
#include "sd_store.h"
#include "twai.h"

#define UI_W 320
#define UI_H 240
#define NAV_H 26
#define CONTENT_Y 26
#define CONTENT_H 186
#define BOT_Y 212
#define BOT_H 28

#define TOP_N 5        /* Top 5 active IDs on DASH */
#define ROW_N 7        /* 7 rolling frames on TRACE */
#define SCAN_N 512     /* records examined per 50 ms tick */
#define CNT_SLOTS 256  /* per-ID counter hash table */
#define RATE_WINDOW_MS 1000u

/* Palette (Calm Industrial Dark) */
#define COL_BG          lv_color_hex(0x13151A)
#define COL_CARD        lv_color_hex(0x1A1D24)
#define COL_CARD_BORDER lv_color_hex(0x282D38)
#define COL_NAV_BAR     lv_color_hex(0x161920)
#define COL_ACTIVE_TAB  lv_color_hex(0x282E3D)
#define COL_ROW_EVEN    lv_color_hex(0x15181F)
#define COL_ROW_ODD     lv_color_hex(0x1A1D25)
#define COL_ACTIVE_BTN  lv_color_hex(0x252B38)
#define COL_TEXT        lv_color_hex(0xE2E8F0)
#define COL_MUTED       lv_color_hex(0x94A3B8)
#define COL_DIM         lv_color_hex(0x5E6676)
#define COL_CYAN        lv_color_hex(0x38BDF8)
#define COL_PURPLE      lv_color_hex(0xA78BFA)
#define COL_GREEN       lv_color_hex(0x22C55E)
#define COL_AMBER       lv_color_hex(0xEAB308)
#define COL_RED         lv_color_hex(0xEF4444)

typedef enum { VIEW_DASH, VIEW_TRACE, VIEW_DIAG, VIEW_SETTINGS, VIEW_COUNT } view_id_t;

typedef struct {
    uint32_t id;
    uint32_t count;
    uint32_t win;
    uint16_t rate;
    uint8_t ext, dlc, used;
    uint8_t data[8];
} id_count_t;

typedef struct {
    lv_obj_t *id, *rate, *per, *cnt, *bar_track, *bar, *share;
    char txt_id[12], txt_rate[10], txt_per[8], txt_cnt[12], txt_share[6];
} dash_row_t;

typedef struct {
    lv_obj_t *dt, *id, *dlc, *data;
    char txt_dt[10], txt_id[12], txt_dlc[4], txt_data[28];
} trace_row_t;

/* -------------------------------------------------------------- statics */

static lv_obj_t *s_views[VIEW_COUNT];
static view_id_t s_current_view = VIEW_DASH, s_prev_view = VIEW_DASH;

/* Top Navigation Bar */
static lv_obj_t *s_nav_br_pill, *s_nav_br_label, *s_nav_back_btn, *s_nav_tab_group;
static lv_obj_t *s_nav_tab_btn[3], *s_nav_tab_label[3], *s_nav_settings_title;
static lv_obj_t *s_nav_host_dot, *s_nav_host_label, *s_nav_sd_dot, *s_nav_sd_label;

/* DASH View */
static lv_obj_t *s_dash_fps_num, *s_dash_fps_unit, *s_dash_load_text, *s_dash_load_bar;
static lv_obj_t *s_dash_health_pill, *s_dash_health_label, *s_dash_empty;
static dash_row_t s_dash_row[TOP_N];

/* TRACE View */
static lv_obj_t *s_trace_empty;
static trace_row_t s_trace_row[ROW_N];
static linsight_can_record_t s_trace_rec[ROW_N];
static bool s_trace_rec_valid[ROW_N];

/* DIAG View */
static lv_obj_t *s_diag_val[6];

/* SETTINGS View */
static lv_obj_t *s_br_pill[4], *s_mode_pill, *s_mode_label, *s_bl_pill[4];
static lv_obj_t *s_set_pause_btn, *s_set_pause_label;

/* Bottom Telemetry Bar */
static lv_obj_t *s_bot_rx_label, *s_bot_load_label, *s_bot_sd_label;
static lv_obj_t *s_bot_state_pill, *s_bot_state_label;

/* Frame Detail Inspector Modal */
static lv_obj_t *s_modal_mask, *s_modal_id_label, *s_modal_sub_label, *s_modal_sub2_label;
static lv_obj_t *s_modal_byte_val[8], *s_modal_ascii_label;

/* Toast Feedback */
static lv_obj_t *s_toast;
static uint32_t s_toast_tick;

/* Static text buffers */
static char s_txt_nav_br[16], s_txt_nav_sd[12], s_txt_nav_host[16];
static char s_txt_dash_fps[12], s_txt_dash_load[16], s_txt_dash_health[20];
static char s_txt_diag_val[6][16], s_txt_mode[20], s_txt_set_pause[20];
static char s_txt_bot_rx[16], s_txt_bot_load[16], s_txt_bot_sd[16], s_txt_bot_state[12];
static char s_txt_modal_id[32], s_txt_modal_sub[48], s_txt_modal_sub2[48];
static char s_txt_modal_byte[8][4], s_txt_modal_ascii[32];
static char s_txt_toast[40];

/* Runtime UI states */
static linsight_conn_t s_conn = UI_CONN_WIFI;
static uint32_t s_rtt_ms = 0;
static bool s_frozen = false, s_floor_valid = false, s_total_valid = false;
static uint16_t s_floor = 0;
static uint32_t s_prev_total = 0;

/* Mailbox from net_task */
static volatile linsight_conn_t s_conn_req = UI_CONN_WIFI;
static volatile uint32_t s_rtt_req = 0;

/* Capture history scanning & ranking */
static linsight_can_record_t s_scan_buf[SCAN_N];
static uint32_t s_scan_n = 0;
static uint16_t s_scan_seq = 0;
static bool s_scan_valid = false;
static id_count_t s_counts[CNT_SLOTS];

static const id_count_t *s_top[TOP_N];
static int s_top_n = 0;
static uint32_t s_rate_tick = 0;
static bool s_rate_valid = false;

static const can_bitrate_t BITRATES[4] = {CAN_BITRATE_125K, CAN_BITRATE_250K, CAN_BITRATE_500K, CAN_BITRATE_1M};
static const char *BITRATE_LABELS[4] = {"125K", "250K", "500K", "1M"};
static const int BRIGHTNESS_LEVELS[4] = {20, 50, 80, 100};

/* ------------------------------------------------------------ primitives */

#define LBL_SET(lbl, buf, text) label_set_str((lbl), (buf), sizeof(buf), (text))
#define LBL_SETF(lbl, buf, fmt, ...) label_setf((lbl), (buf), sizeof(buf), (fmt), __VA_ARGS__)

static void label_color(lv_obj_t *label, lv_color_t color)
{
    if (lv_obj_get_style_text_color(label, 0).full != color.full) {
        lv_obj_set_style_text_color(label, color, 0);
    }
}

static void label_set_str(lv_obj_t *label, char *buf, size_t cap, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0) return;
    size_t len = 0;
    while (text[len] != '\0' && len + 1 < cap) len++;
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

static void object_show(lv_obj_t *obj, bool show)
{
    if (!obj || show == !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (show) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void fmt_id(char *buf, size_t cap, uint32_t id, bool ext)
{
    snprintf(buf, cap, ext ? "%08X" : "%03X", (unsigned)id);
}

static void fmt_count(char *buf, size_t cap, uint32_t n)
{
    if (n < 10000u) snprintf(buf, cap, "%u", (unsigned)n);
    else if (n < 1000000u) snprintf(buf, cap, "%u.%uk", (unsigned)(n / 1000u), (unsigned)((n % 1000u) / 100u));
    else snprintf(buf, cap, "%u.%02uM", (unsigned)(n / 1000000u), (unsigned)((n / 10000u) % 100u));
}

static lv_obj_t *obj_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
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
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label_new(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text_static(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *label_new_centered(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = label_new(parent, text, font, color);
    lv_obj_center(l);
    return l;
}

static lv_obj_t *label_cell(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, const lv_font_t *font, lv_color_t color, bool right_align)
{
    lv_obj_t *l = label_new(parent, text, font, color);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    if (right_align) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    return l;
}

static lv_obj_t *btn_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                         lv_coord_t h, lv_coord_t radius, lv_color_t bg,
                         lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = obj_new(parent, x, y, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, radius, 0);
    if (cb) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

static lv_obj_t *card_new(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = btn_new(parent, x, y, w, h, 6, COL_CARD, NULL, NULL);
    lv_obj_set_style_border_color(card, COL_CARD_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    return card;
}

static lv_obj_t *bar_new(lv_obj_t *screen, lv_coord_t y, lv_coord_t h)
{
    lv_obj_t *bar = btn_new(screen, 0, y, UI_W, h, 0, COL_NAV_BAR, NULL, NULL);
    lv_obj_set_style_border_color(bar, COL_CARD_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    return bar;
}

static lv_obj_t *empty_hint_create(lv_obj_t *page, lv_coord_t y, lv_coord_t h, const char *msg)
{
    lv_obj_t *box = card_new(page, 16, y + 8, 288, h - 16);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x181B22), 0);
    lv_obj_set_style_border_color(box, COL_CARD_BORDER, 0);
    lv_obj_set_style_radius(box, 6, 0);

    lv_obj_t *icon = label_new(box, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, COL_CYAN);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *l = label_new(box, msg, &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_width(l, 260);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -14);
    return box;
}

static void toast_showf(const char *fmt, ...)
{
    if (!s_toast) return;
    char tmp[sizeof(s_txt_toast)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    label_set_str((lv_obj_t *)lv_obj_get_child(s_toast, 0), s_txt_toast, sizeof(s_txt_toast), tmp);
    s_toast_tick = lv_tick_get();
    object_show(s_toast, true);
}

/* -------------------------------------------------------- trace counters */

static id_count_t *count_slot(uint32_t id, bool ext)
{
    const uint32_t h = (id * 2654435761u + (ext ? 0x9E3779B9u : 0u)) & (CNT_SLOTS - 1);
    for (uint32_t i = 0; i < CNT_SLOTS; i++) {
        id_count_t *slot = &s_counts[(h + CNT_SLOTS - i) & (CNT_SLOTS - 1)];
        if (!slot->used) {
            memset(slot, 0, sizeof(*slot));
            slot->used = 1;
            slot->id = id;
            slot->ext = ext ? 1 : 0;
            return slot;
        }
        if (slot->id == id && slot->ext == (ext ? 1 : 0)) return slot;
    }
    return NULL;
}

static uint32_t count_get(uint32_t id, bool ext)
{
    const uint32_t h = (id * 2654435761u + (ext ? 0x9E3779B9u : 0u)) & (CNT_SLOTS - 1);
    for (uint32_t i = 0; i < CNT_SLOTS; i++) {
        const id_count_t *slot = &s_counts[(h + CNT_SLOTS - i) & (CNT_SLOTS - 1)];
        if (!slot->used) break;
        if (slot->id == id && slot->ext == (ext ? 1 : 0)) return slot->count;
    }
    return 0;
}

/* ------------------------------------------------------- ranking engine */

static void rate_sample(void)
{
    const uint32_t elapsed = lv_tick_elaps(s_rate_tick);
    if (elapsed < RATE_WINDOW_MS) return;
    for (int i = 0; i < CNT_SLOTS; i++) {
        id_count_t *slot = &s_counts[i];
        if (slot->used) {
            const uint32_t rate = slot->win * 1000u / elapsed;
            slot->rate = rate > 0xFFFFu ? 0xFFFFu : (uint16_t)rate;
            slot->win = 0;
        }
    }
    s_rate_valid = true;
    s_rate_tick = lv_tick_get();
}

static bool rank_before(const id_count_t *a, const id_count_t *b)
{
    if (a->rate != b->rate) return a->rate > b->rate;
    if (a->count != b->count) return a->count > b->count;
    if (a->id != b->id) return a->id < b->id;
    return a->ext < b->ext;
}

static void rank_put(const id_count_t *slot)
{
    int pos;
    if (s_top_n < TOP_N) pos = s_top_n++;
    else if (rank_before(slot, s_top[TOP_N - 1])) pos = TOP_N - 1;
    else return;

    while (pos > 0 && rank_before(slot, s_top[pos - 1])) {
        s_top[pos] = s_top[pos - 1];
        pos--;
    }
    s_top[pos] = slot;
}

static void top_build(void)
{
    s_top_n = 0;
    for (int i = 0; i < CNT_SLOTS; i++) {
        const id_count_t *slot = &s_counts[i];
        if (slot->used && (!s_rate_valid || slot->rate > 0)) rank_put(slot);
    }
}

static void counts_clear(void)
{
    memset(s_counts, 0, sizeof(s_counts));
    s_rate_valid = false;
    s_rate_tick = lv_tick_get();
    s_top_n = 0;
}

static void dash_clear_row(int i)
{
    dash_row_t *r = &s_dash_row[i];
    LBL_SET(r->id, r->txt_id, "");
    LBL_SET(r->rate, r->txt_rate, "");
    LBL_SET(r->per, r->txt_per, "");
    LBL_SET(r->cnt, r->txt_cnt, "");
    LBL_SET(r->share, r->txt_share, "");
    object_show(r->bar_track, false);
    object_show(r->bar, false);
}

static void trace_clear_row(int i)
{
    s_trace_rec_valid[i] = false;
    trace_row_t *r = &s_trace_row[i];
    LBL_SET(r->dt, r->txt_dt, "");
    LBL_SET(r->id, r->txt_id, "");
    LBL_SET(r->dlc, r->txt_dlc, "");
    LBL_SET(r->data, r->txt_data, "");
}

static void view_reset(void)
{
    counts_clear();
    linsight_can_record_t newest;
    if (twai_ui_tail(&newest, 1) == 1) {
        s_scan_seq = newest.seq;
        s_floor = newest.seq;
        s_scan_valid = s_floor_valid = true;
    } else {
        s_scan_valid = s_floor_valid = false;
    }
    for (int i = 0; i < TOP_N; i++) dash_clear_row(i);
    for (int i = 0; i < ROW_N; i++) trace_clear_row(i);
    object_show(s_dash_empty, true);
    object_show(s_trace_empty, true);
    s_total_valid = false;
}

/* ----------------------------------------------------- detail inspector */

static void modal_hide(void) { object_show(s_modal_mask, false); }
static void modal_close_click_cb(lv_event_t *e) { (void)e; modal_hide(); }

static void modal_update_bytes(const uint8_t *data, uint8_t dlc)
{
    char ascii[10];
    size_t len = dlc > 8 ? 8 : dlc;
    for (size_t i = 0; i < 8; i++) {
        if (i < len) {
            snprintf(s_txt_modal_byte[i], sizeof(s_txt_modal_byte[i]), "%02X", (unsigned)data[i]);
            label_color(s_modal_byte_val[i], COL_TEXT);
            ascii[i] = (data[i] >= 32 && data[i] <= 126) ? (char)data[i] : '.';
        } else {
            strcpy(s_txt_modal_byte[i], "--");
            label_color(s_modal_byte_val[i], COL_DIM);
            ascii[i] = ' ';
        }
        lv_label_set_text_static(s_modal_byte_val[i], s_txt_modal_byte[i]);
    }
    ascii[len] = '\0';
    LBL_SETF(s_modal_ascii_label, s_txt_modal_ascii, "ASCII: \"%s\"", ascii);
    object_show(s_modal_mask, true);
}

static void modal_show_slot(const id_count_t *slot)
{
    if (!slot || !slot->used) return;
    char idbuf[12], per[10];
    fmt_id(idbuf, sizeof(idbuf), slot->id, slot->ext != 0);
    LBL_SETF(s_modal_id_label, s_txt_modal_id, "ID: 0x%s (%s)", idbuf, slot->ext ? "EXT" : "STD");
    label_color(s_modal_id_label, slot->ext ? COL_PURPLE : COL_CYAN);

    if (slot->rate > 0) snprintf(per, sizeof(per), "%u ms", (unsigned)(1000u / slot->rate));
    else snprintf(per, sizeof(per), "---");

    LBL_SETF(s_modal_sub_label, s_txt_modal_sub, "Rate: %u Hz  |  Period: %s",
             (unsigned)slot->rate, per);
    LBL_SETF(s_modal_sub2_label, s_txt_modal_sub2, "Count: %u hits  |  DLC: %u Bytes",
             (unsigned)slot->count, (unsigned)slot->dlc);
    modal_update_bytes(slot->data, slot->dlc);
}

static void modal_show_record(const linsight_can_record_t *r)
{
    if (!r) return;
    const bool ext = (r->flags & LINSIGHT_REC_FLAG_EXT) != 0;
    if (r->flags & LINSIGHT_REC_FLAG_ERR) {
        LBL_SET(s_modal_id_label, s_txt_modal_id, "ERR-FRM (Error Frame)");
        label_color(s_modal_id_label, COL_RED);
        LBL_SET(s_modal_sub_label, s_txt_modal_sub, "Bus disturbance / Protocol violation");
        LBL_SET(s_modal_sub2_label, s_txt_modal_sub2, "Check bus wiring & CAN termination");
        for (size_t i = 0; i < 8; i++) {
            LBL_SET(s_modal_byte_val[i], s_txt_modal_byte[i], "--");
            label_color(s_modal_byte_val[i], COL_DIM);
        }
        LBL_SET(s_modal_ascii_label, s_txt_modal_ascii, "ASCII: N/A");
        object_show(s_modal_mask, true);
    } else {
        char idbuf[12];
        fmt_id(idbuf, sizeof(idbuf), r->id, ext);
        LBL_SETF(s_modal_id_label, s_txt_modal_id, "ID: 0x%s (%s)", idbuf, ext ? "EXT" : "STD");
        label_color(s_modal_id_label, ext ? COL_PURPLE : COL_CYAN);
        LBL_SETF(s_modal_sub_label, s_txt_modal_sub, "DLC: %u Bytes  |  Total: %u hits",
                 (unsigned)r->dlc, (unsigned)count_get(r->id, ext));
        LBL_SETF(s_modal_sub2_label, s_txt_modal_sub2, "Seq: #%u  |  Timestamp: %u ms",
                 (unsigned)r->seq, (unsigned)r->ts_ms);
        modal_update_bytes(r->data, r->dlc);
    }
}

static void dash_row_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_top_n && s_top[idx]) modal_show_slot(s_top[idx]);
}

static void trace_row_click_cb(lv_event_t *e)
{
    const int row_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (row_idx >= 0 && row_idx < ROW_N && s_trace_rec_valid[row_idx]) {
        modal_show_record(&s_trace_rec[row_idx]);
    }
}

/* ------------------------------------------------------------ navigation */

static void nav_tab_update(void)
{
    for (int i = 0; i < 3; i++) {
        const bool active = (s_current_view == (view_id_t)i);
        lv_obj_set_style_bg_color(s_nav_tab_btn[i], active ? COL_ACTIVE_TAB : COL_NAV_BAR, 0);
        label_color(s_nav_tab_label[i], active ? COL_CYAN : COL_MUTED);
    }
}

static void view_show(view_id_t view)
{
    if (view >= VIEW_COUNT) view = VIEW_DASH;
    if (s_current_view != VIEW_SETTINGS && view == VIEW_SETTINGS) s_prev_view = s_current_view;
    s_current_view = view;

    for (int i = 0; i < VIEW_COUNT; i++) object_show(s_views[i], i == (int)view);

    const bool in_settings = (view == VIEW_SETTINGS);
    object_show(s_nav_br_pill, !in_settings);
    object_show(s_nav_back_btn, in_settings);
    object_show(s_nav_tab_group, !in_settings);
    object_show(s_nav_settings_title, in_settings);
    if (!in_settings) nav_tab_update();
}

static void tab_click_cb(lv_event_t *e) { view_show((view_id_t)(intptr_t)lv_event_get_user_data(e)); }
static void settings_click_cb(lv_event_t *e) { (void)e; view_show(VIEW_SETTINGS); }
static void back_click_cb(lv_event_t *e) { (void)e; view_show(s_prev_view); }

/* --------------------------------------------------------------- actions */

static void bitrate_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (BITRATES[idx] == twai_current_bitrate()) return;
    toast_showf(twai_capture_restart(BITRATES[idx], twai_current_mode()) == ESP_OK
                ? "BITRATE %s" : "BITRATE FAILED", BITRATE_LABELS[idx]);
}

static void mode_click_cb(lv_event_t *e)
{
    (void)e;
    const can_mode_t next = twai_current_mode() == CAN_MODE_NORMAL ? CAN_MODE_LISTEN_ONLY : CAN_MODE_NORMAL;
    toast_showf(twai_capture_restart(twai_current_bitrate(), next) == ESP_OK
                ? (next == CAN_MODE_NORMAL ? "MODE: NORMAL (ACK)" : "MODE: LISTEN-ONLY")
                : "MODE CHANGE FAILED");
}

static void brightness_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bsp_backlight_set(BRIGHTNESS_LEVELS[idx]);
    toast_showf("BRIGHTNESS %d%%", BRIGHTNESS_LEVELS[idx]);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_bl_pill[i], i == idx ? COL_GREEN : COL_ACTIVE_BTN, 0);
        label_color((lv_obj_t *)lv_obj_get_child(s_bl_pill[i], 0), i == idx ? COL_BG : COL_TEXT);
    }
}

static void toggle_pause(void)
{
    s_frozen = !s_frozen;
    toast_showf(s_frozen ? "PAUSED" : "RESUMED");
}

static void pause_click_cb(lv_event_t *e) { (void)e; toggle_pause(); }
static void mark_click_cb(lv_event_t *e) { (void)e; sd_store_mark("MANUAL", 0, 0); toast_showf("SD EVENT MARKED"); }
static void recover_click_cb(lv_event_t *e) { (void)e; toast_showf(twai_capture_recover() == ESP_OK ? "BUS RECOVER OK" : "RECOVER FAILED"); }
static void clear_click_cb(lv_event_t *e) { (void)e; twai_stats_clear(); view_reset(); toast_showf("STATS CLEARED"); }

/* ---------------------------------------------------------- record scan */

static void scan_records(void)
{
    s_scan_n = twai_ui_tail(s_scan_buf, SCAN_N);
    if (s_floor_valid && s_scan_n > 0 && (int16_t)(s_scan_buf[0].seq - s_floor) > 0) {
        s_floor_valid = false;
    }

    for (uint32_t i = 0; i < s_scan_n; i++) {
        const linsight_can_record_t *r = &s_scan_buf[i];
        if (s_scan_valid && (int16_t)(r->seq - s_scan_seq) <= 0) continue;
        s_scan_seq = r->seq;
        s_scan_valid = true;

        if (!(r->flags & LINSIGHT_REC_FLAG_ERR)) {
            id_count_t *slot = count_slot(r->id, (r->flags & LINSIGHT_REC_FLAG_EXT) != 0);
            if (slot) {
                slot->count++;
                slot->win++;
                slot->dlc = r->dlc;
                memcpy(slot->data, r->data, 8);
            }
        }
    }
}

/* ------------------------------------------------------- render views */

static void render_dash(const can_stats_t *st)
{
    LBL_SETF(s_dash_fps_num, s_txt_dash_fps, "%u", (unsigned)st->fps);
    lv_obj_update_layout(s_dash_fps_num);
    lv_obj_align_to(s_dash_fps_unit, s_dash_fps_num, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -2);

    const uint32_t load_pct = st->load_permille / 10u;
    LBL_SETF(s_dash_load_text, s_txt_dash_load, "LOAD %u%%", (unsigned)load_pct);
    lv_bar_set_value(s_dash_load_bar, (int32_t)load_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_dash_load_bar,
                              (load_pct >= 80) ? COL_RED : (load_pct >= 50) ? COL_AMBER : COL_GREEN,
                              LV_PART_INDICATOR);

    const bool is_bus_off = (strcmp(st->state, "BUS_OFF") == 0);
    const bool is_passive = (strcmp(st->state, "RECOVERING") == 0 || st->tec > 127u || st->rec > 127u);
    const bool has_errors = (st->bus_errors > 0 || st->tec > 0 || st->rec > 0);

    lv_color_t h_bg = lv_color_hex(0x064E3B), h_txt = COL_GREEN;
    if (is_bus_off) {
        LBL_SET(s_dash_health_label, s_txt_dash_health, "BUS-OFF");
        h_bg = COL_RED; h_txt = COL_TEXT;
    } else if (is_passive) {
        LBL_SET(s_dash_health_label, s_txt_dash_health, "PASSIVE");
        h_bg = COL_AMBER; h_txt = COL_BG;
    } else if (has_errors) {
        LBL_SETF(s_dash_health_label, s_txt_dash_health, "BERR %u", (unsigned)st->bus_errors);
        h_bg = COL_AMBER; h_txt = COL_BG;
    } else {
        LBL_SET(s_dash_health_label, s_txt_dash_health, LV_SYMBOL_OK " NORMAL");
    }
    lv_obj_set_style_bg_color(s_dash_health_pill, h_bg, 0);
    label_color(s_dash_health_label, h_txt);

    top_build();
    const uint32_t top_max = (s_top_n > 0 && s_top[0]->rate > 0) ? s_top[0]->rate : 1;

    for (int i = 0; i < s_top_n; i++) {
        dash_row_t *r = &s_dash_row[i];
        const id_count_t *slot = s_top[i];
        char idbuf[12];
        fmt_id(idbuf, sizeof(idbuf), slot->id, slot->ext != 0);
        LBL_SET(r->id, r->txt_id, idbuf);
        label_color(r->id, slot->ext ? COL_PURPLE : COL_CYAN);

        if (!s_rate_valid) {
            LBL_SET(r->rate, r->txt_rate, "---");
            LBL_SET(r->per, r->txt_per, "---");
        } else {
            if (slot->rate >= 10000u) LBL_SETF(r->rate, r->txt_rate, "%uk", (unsigned)(slot->rate / 1000u));
            else LBL_SETF(r->rate, r->txt_rate, "%u", (unsigned)slot->rate);

            if (slot->rate == 0) LBL_SET(r->per, r->txt_per, "---");
            else if (slot->rate > 1000u) LBL_SET(r->per, r->txt_per, "<1ms");
            else LBL_SETF(r->per, r->txt_per, "%ums", (unsigned)(1000u / slot->rate));
        }

        char cnt[12];
        fmt_count(cnt, sizeof(cnt), slot->count);
        LBL_SET(r->cnt, r->txt_cnt, cnt);

        const uint32_t bar_w = (slot->rate * 40u) / top_max;
        lv_obj_set_width(r->bar, (lv_coord_t)(bar_w < 2 ? 2 : bar_w));
        lv_obj_set_style_bg_color(r->bar, slot->ext ? COL_PURPLE : COL_CYAN, 0);
        object_show(r->bar_track, true);
        object_show(r->bar, true);

        const uint32_t pct = (st->fps > 0) ? (slot->rate * 100u / st->fps) : 0;
        LBL_SETF(r->share, r->txt_share, "%u%%", (unsigned)pct);
    }

    for (int i = s_top_n; i < TOP_N; i++) dash_clear_row(i);
    object_show(s_dash_empty, s_top_n == 0);
}

static void render_trace(void)
{
    uint16_t pick[ROW_N];
    int rows = 0;
    for (int i = (int)s_scan_n - 1; i >= 0 && rows < ROW_N; i--) {
        if (s_floor_valid && (int16_t)(s_scan_buf[i].seq - s_floor) <= 0) continue;
        pick[rows++] = (uint16_t)i;
    }

    for (int r_idx = 0; r_idx < rows; r_idx++) {
        trace_row_t *r = &s_trace_row[r_idx];
        const int scan_idx = pick[rows - 1 - r_idx];
        const linsight_can_record_t *rec = &s_scan_buf[scan_idx];
        s_trace_rec[r_idx] = *rec;
        s_trace_rec_valid[r_idx] = true;
        const bool ext = (rec->flags & LINSIGHT_REC_FLAG_EXT) != 0;
        const bool err = (rec->flags & LINSIGHT_REC_FLAG_ERR) != 0;
        const bool rtr = (rec->flags & LINSIGHT_REC_FLAG_RTR) != 0;

        if (r_idx > 0) {
            const int prev_idx = pick[rows - r_idx];
            const uint32_t dt = rec->ts_ms >= s_scan_buf[prev_idx].ts_ms ? rec->ts_ms - s_scan_buf[prev_idx].ts_ms : 0;
            LBL_SETF(r->dt, r->txt_dt, "+%u", (unsigned)dt);
        } else {
            LBL_SET(r->dt, r->txt_dt, "0");
        }

        if (err) {
            LBL_SET(r->id, r->txt_id, "ERR-FRM");
            label_color(r->id, COL_RED);
            LBL_SET(r->dlc, r->txt_dlc, "-");
            label_color(r->dlc, COL_DIM);
            LBL_SET(r->data, r->txt_data, "** BUS ERROR **");
            label_color(r->data, COL_RED);
        } else {
            char idbuf[12];
            fmt_id(idbuf, sizeof(idbuf), rec->id, ext);
            LBL_SET(r->id, r->txt_id, idbuf);
            label_color(r->id, ext ? COL_PURPLE : COL_CYAN);

            if (rtr) {
                LBL_SET(r->dlc, r->txt_dlc, "R");
                label_color(r->dlc, COL_AMBER);
                LBL_SET(r->data, r->txt_data, "REMOTE FRAME");
                label_color(r->data, COL_AMBER);
            } else {
                LBL_SETF(r->dlc, r->txt_dlc, "%u", (unsigned)rec->dlc);
                label_color(r->dlc, COL_DIM);
                char hex[28];
                size_t used = 0;
                const uint8_t dlc = rec->dlc > 8 ? 8 : rec->dlc;
                for (uint8_t b = 0; b < dlc; b++) {
                    used += snprintf(hex + used, sizeof(hex) - used, "%02X%s",
                                     (unsigned)rec->data[b], b + 1 < dlc ? " " : "");
                }
                LBL_SET(r->data, r->txt_data, hex);
                label_color(r->data, COL_TEXT);
            }
        }
    }

    for (int r_idx = rows; r_idx < ROW_N; r_idx++) trace_clear_row(r_idx);
    object_show(s_trace_empty, rows == 0);
}

static void diag_set_val(int idx, uint32_t val, lv_color_t alert_col)
{
    LBL_SETF(s_diag_val[idx], s_txt_diag_val[idx], "%u", (unsigned)val);
    label_color(s_diag_val[idx], val > 0 ? alert_col : COL_GREEN);
}

static void render_diag(const can_stats_t *st)
{
    LBL_SETF(s_diag_val[0], s_txt_diag_val[0], "%u / %u", (unsigned)st->tec, (unsigned)st->rec);
    label_color(s_diag_val[0], (st->tec >= 128u || st->rec >= 128u) ? COL_RED
                               : (st->tec || st->rec) ? COL_AMBER : COL_GREEN);
    diag_set_val(1, st->bus_errors, COL_RED);
    diag_set_val(2, st->bus_off, COL_RED);
    diag_set_val(3, st->rx_missed, COL_AMBER);
    diag_set_val(4, st->rx_queue_full, COL_AMBER);

    sd_status_t sd;
    sd_store_get_status(&sd);
    if (sd.recording) {
        LBL_SETF(s_diag_val[5], s_txt_diag_val[5], "REC %uF", (unsigned)sd.files);
    } else {
        LBL_SET(s_diag_val[5], s_txt_diag_val[5], sd.mounted ? "READY" : "NO SD");
    }
    label_color(s_diag_val[5], sd.mounted ? COL_GREEN : COL_DIM);
}

static void render_settings(void)
{
    const can_bitrate_t cur_br = twai_current_bitrate();
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_br_pill[i], BITRATES[i] == cur_br ? COL_CYAN : COL_ACTIVE_BTN, 0);
        label_color((lv_obj_t *)lv_obj_get_child(s_br_pill[i], 0),
                    BITRATES[i] == cur_br ? COL_BG : COL_TEXT);
    }

    const can_mode_t mode = twai_current_mode();
    LBL_SET(s_mode_label, s_txt_mode,
            mode == CAN_MODE_NORMAL ? "Normal (ACK)"
            : mode == CAN_MODE_SELFTEST ? "Self-test"
                                        : "Listen-Only");
    lv_obj_set_style_bg_color(s_mode_pill, mode == CAN_MODE_NORMAL ? COL_AMBER : COL_GREEN, 0);
    label_color(s_mode_label, COL_BG);

    const bool paused = s_frozen;
    LBL_SETF(s_set_pause_label, s_txt_set_pause, "%s %s",
             paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, paused ? "Resume" : "Pause");
    lv_obj_set_style_bg_color(s_set_pause_btn, paused ? COL_AMBER : COL_ACTIVE_BTN, 0);
    label_color(s_set_pause_label, paused ? COL_BG : COL_TEXT);
}

static void render_bars(const can_stats_t *st)
{
    /* Top Nav: Bitrate & mode pill */
    LBL_SETF(s_nav_br_label, s_txt_nav_br, "%s %s",
             can_bitrate_name(twai_current_bitrate()),
             twai_current_mode() == CAN_MODE_LISTEN_ONLY ? "LO" : "NRM");

    /* Top Nav: SD & Host badges */
    sd_status_t sd;
    sd_store_get_status(&sd);
    object_show(s_nav_sd_dot, sd.mounted || sd.recording);
    if (sd.recording) {
        lv_obj_set_style_bg_color(s_nav_sd_dot, COL_RED, 0);
        LBL_SET(s_nav_sd_label, s_txt_nav_sd, "REC");
        label_color(s_nav_sd_label, COL_RED);
    } else if (sd.mounted) {
        lv_obj_set_style_bg_color(s_nav_sd_dot, COL_GREEN, 0);
        LBL_SET(s_nav_sd_label, s_txt_nav_sd, "SD");
        label_color(s_nav_sd_label, COL_MUTED);
    } else {
        LBL_SET(s_nav_sd_label, s_txt_nav_sd, "");
    }

    const bool host_on = (s_conn == UI_CONN_ONLINE);
    const bool wifi_on = (s_conn == UI_CONN_WIFI);
    object_show(s_nav_host_dot, host_on || wifi_on);
    if (host_on || wifi_on) {
        lv_obj_set_style_bg_color(s_nav_host_dot, host_on ? COL_GREEN : COL_AMBER, 0);
        if (host_on && s_rtt_ms > 0) {
            LBL_SETF(s_nav_host_label, s_txt_nav_host, "%ums", (unsigned)s_rtt_ms);
        } else {
            LBL_SET(s_nav_host_label, s_txt_nav_host, host_on ? "HOST" : "WIFI");
        }
        label_color(s_nav_host_label, host_on ? COL_TEXT : COL_MUTED);
    } else {
        LBL_SET(s_nav_host_label, s_txt_nav_host, "");
    }

    /* Bottom Bar */
    char rx_buf[12];
    fmt_count(rx_buf, sizeof(rx_buf), st->rx_total);
    LBL_SETF(s_bot_rx_label, s_txt_bot_rx, "RX: %s", rx_buf);
    LBL_SETF(s_bot_load_label, s_txt_bot_load, "FPS: %u", (unsigned)st->fps);

    if (sd.recording) {
        LBL_SETF(s_bot_sd_label, s_txt_bot_sd, "REC %uF", (unsigned)sd.files);
    } else {
        LBL_SET(s_bot_sd_label, s_txt_bot_sd, sd.mounted ? "SD READY" : "NO SD");
    }
    label_color(s_bot_sd_label, sd.recording ? COL_GREEN : sd.mounted ? COL_MUTED : COL_DIM);

    const bool paused = s_frozen;
    const bool running = !paused && strcmp(st->state, "RUNNING") == 0;
    LBL_SET(s_bot_state_label, s_txt_bot_state, paused ? "PAUSED" : running ? "RUN" : "STOP");
    lv_obj_set_style_bg_color(s_bot_state_pill, paused ? COL_AMBER : running ? COL_GREEN : COL_DIM, 0);
    label_color(s_bot_state_label, paused || running ? COL_BG : COL_TEXT);
}

/* ---------------------------------------------------------------- timer */

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_conn != s_conn_req || (s_conn == UI_CONN_ONLINE && s_rtt_ms != s_rtt_req)) {
        s_conn = s_conn_req;
        s_rtt_ms = s_rtt_req;
    }

    can_stats_t st;
    twai_stats_snapshot(&st);

    if (s_total_valid && st.rx_total < s_prev_total) view_reset();
    s_prev_total = st.rx_total;
    s_total_valid = true;

    scan_records();

    rate_sample();

    render_bars(&st);

    if (s_current_view == VIEW_DASH) {
        if (!s_frozen) render_dash(&st);
    } else if (s_current_view == VIEW_TRACE) {
        if (!s_frozen) render_trace();
    } else if (s_current_view == VIEW_DIAG) {
        render_diag(&st);
    } else if (s_current_view == VIEW_SETTINGS) {
        render_settings();
    }

    if (s_toast && !lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN) && lv_tick_elaps(s_toast_tick) >= 1400u) {
        object_show(s_toast, false);
    }
}

/* -------------------------------------------------------------- builders */

static void top_nav_create(lv_obj_t *screen)
{
    lv_obj_t *bar = bar_new(screen, 0, NAV_H);

    /* Left: Bitrate & Mode pill / Back button */
    s_nav_br_pill = btn_new(bar, 5, 3, 60, 20, 4, COL_ACTIVE_BTN, settings_click_cb, NULL);
    s_nav_br_label = label_new_centered(s_nav_br_pill, "500K", &lv_font_montserrat_12, COL_CYAN);

    s_nav_back_btn = btn_new(bar, 5, 3, 60, 20, 4, COL_ACTIVE_BTN, back_click_cb, NULL);
    lv_obj_add_flag(s_nav_back_btn, LV_OBJ_FLAG_HIDDEN);
    label_new_centered(s_nav_back_btn, LV_SYMBOL_LEFT " Back", &lv_font_montserrat_12, COL_TEXT);

    /* Center: Segmented Tab Bar [DASH | TRACE | DIAG] (X = 74, W = 138) */
    s_nav_tab_group = btn_new(bar, 74, 2, 138, 22, 5, lv_color_hex(0x1B1E28), NULL, NULL);
    static const char *tab_names[3] = {"DASH", "TRACE", "DIAG"};
    for (int i = 0; i < 3; i++) {
        s_nav_tab_btn[i] = btn_new(s_nav_tab_group, i * 46, 0, 46, 22, 4, COL_NAV_BAR,
                                   tab_click_cb, (void *)(intptr_t)i);
        s_nav_tab_label[i] = label_new_centered(s_nav_tab_btn[i], tab_names[i], &lv_font_montserrat_12, COL_MUTED);
    }
    nav_tab_update();

    s_nav_settings_title = label_cell(bar, "SETTINGS", 74, 5, 138, &lv_font_montserrat_12, COL_TEXT, false);
    lv_obj_set_style_text_align(s_nav_settings_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_nav_settings_title, LV_OBJ_FLAG_HIDDEN);

    /* Right: Host Dot + SD Dot & Labels (cleanly partitioned from X=218 to 316) */
    s_nav_host_dot = btn_new(bar, 220, 10, 6, 6, LV_RADIUS_CIRCLE, COL_GREEN, NULL, NULL);
    lv_obj_add_flag(s_nav_host_dot, LV_OBJ_FLAG_HIDDEN);
    s_nav_host_label = label_cell(bar, "", 228, 5, 0, &lv_font_montserrat_12, COL_MUTED, false);

    s_nav_sd_dot = btn_new(bar, 270, 10, 6, 6, LV_RADIUS_CIRCLE, COL_RED, NULL, NULL);
    lv_obj_add_flag(s_nav_sd_dot, LV_OBJ_FLAG_HIDDEN);
    s_nav_sd_label = label_cell(bar, "", 278, 5, 0, &lv_font_montserrat_12, COL_MUTED, false);
}

static void bottom_bar_create(lv_obj_t *screen)
{
    lv_obj_t *bar = bar_new(screen, BOT_Y, BOT_H);
    s_bot_rx_label = label_cell(bar, "RX: 0", 8, 7, 0, &lv_font_montserrat_12, COL_TEXT, false);
    s_bot_load_label = label_cell(bar, "FPS: 0", 96, 7, 0, &lv_font_montserrat_12, COL_MUTED, false);
    s_bot_sd_label = label_cell(bar, "SD READY", 174, 7, 0, &lv_font_montserrat_12, COL_MUTED, false);
    s_bot_state_pill = btn_new(bar, 258, 4, 54, 20, LV_RADIUS_CIRCLE, COL_GREEN, pause_click_cb, NULL);
    s_bot_state_label = label_new_centered(s_bot_state_pill, "RUN", &lv_font_montserrat_12, COL_BG);
}

static void dash_view_create(lv_obj_t *parent)
{
    lv_obj_t *page = obj_new(parent, 0, CONTENT_Y, UI_W, CONTENT_H);
    s_views[VIEW_DASH] = page;

    lv_obj_t *banner = card_new(page, 4, 3, 312, 38);
    s_dash_fps_num = label_cell(banner, "0", 8, 8, 0, &lv_font_montserrat_20, COL_TEXT, false);
    s_dash_fps_unit = label_cell(banner, "fps", 24, 14, 0, &lv_font_montserrat_12, COL_MUTED, false);
    lv_obj_align_to(s_dash_fps_unit, s_dash_fps_num, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -2);

    s_dash_load_text = label_cell(banner, "LOAD 0%", 106, 6, 0, &lv_font_montserrat_12, COL_TEXT, false);

    s_dash_load_bar = lv_bar_create(banner);
    lv_obj_set_pos(s_dash_load_bar, 106, 22);
    lv_obj_set_size(s_dash_load_bar, 96, 7);
    lv_obj_set_style_radius(s_dash_load_bar, 3, 0);
    lv_obj_set_style_bg_color(s_dash_load_bar, lv_color_hex(0x1F2432), 0);
    lv_obj_set_style_bg_opa(s_dash_load_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dash_load_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_dash_load_bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_dash_load_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(s_dash_load_bar, 0, 100);
    lv_bar_set_value(s_dash_load_bar, 0, LV_ANIM_OFF);

    s_dash_health_pill = btn_new(banner, 214, 7, 90, 24, LV_RADIUS_CIRCLE, lv_color_hex(0x064E3B), NULL, NULL);
    s_dash_health_label = label_new_centered(s_dash_health_pill, LV_SYMBOL_OK " NORMAL",
                                             &lv_font_montserrat_12, COL_GREEN);

    static const struct { const char *t; lv_coord_t x, w; } cols[] = {
        {"ID", 10, 0}, {"RATE/s", 92, 42}, {"PER", 138, 36}, {"COUNT", 178, 46}, {"SHARE", 230, 80}
    };
    for (int i = 0; i < 5; i++) {
        label_cell(page, cols[i].t, cols[i].x, 43, cols[i].w, &lv_font_montserrat_12, COL_DIM, cols[i].w > 0);
    }

    for (int i = 0; i < TOP_N; i++) {
        lv_obj_t *row = btn_new(page, 4, 58 + i * 24, 312, 23, 4,
                                (i & 1) ? COL_ROW_ODD : COL_ROW_EVEN,
                                dash_row_click_cb, (void *)(intptr_t)i);
        dash_row_t *r = &s_dash_row[i];
        r->id = label_cell(row, "", 6, 4, 76, &lv_font_montserrat_14, COL_CYAN, false);
        r->rate = label_cell(row, "", 88, 4, 42, &lv_font_montserrat_14, COL_TEXT, true);
        r->per = label_cell(row, "", 134, 5, 36, &lv_font_montserrat_12, COL_MUTED, true);
        r->cnt = label_cell(row, "", 174, 5, 46, &lv_font_montserrat_12, COL_MUTED, true);
        r->bar_track = btn_new(row, 226, 8, 40, 7, 3, lv_color_hex(0x1F2432), NULL, NULL);
        r->bar = btn_new(r->bar_track, 0, 0, 0, 7, 3, COL_CYAN, NULL, NULL);
        object_show(r->bar_track, false);
        object_show(r->bar, false);
        r->share = label_cell(row, "", 268, 5, 38, &lv_font_montserrat_12, COL_MUTED, true);
    }

    s_dash_empty = empty_hint_create(page, 52, 130, "Waiting for CAN frames...\nCheck wiring & bitrate");
}

static void trace_view_create(lv_obj_t *parent)
{
    lv_obj_t *page = obj_new(parent, 0, CONTENT_Y, UI_W, CONTENT_H);
    s_views[VIEW_TRACE] = page;

    static const struct { const char *t; lv_coord_t x, w; bool right; } trace_cols[] = {
        {"dt (ms)", 10, 44, true}, {"ID", 60, 0, false}, {"LEN", 130, 16, false}, {"DATA (HEX)", 152, 0, false}
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl = label_cell(page, trace_cols[i].t, trace_cols[i].x, 4, trace_cols[i].w, &lv_font_montserrat_12, COL_DIM, trace_cols[i].right);
        if (i == 2) lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    for (int i = 0; i < ROW_N; i++) {
        lv_obj_t *row = btn_new(page, 4, 21 + i * 23, 312, 22, 3,
                                (i & 1) ? COL_ROW_ODD : COL_ROW_EVEN,
                                trace_row_click_cb, (void *)(intptr_t)i);
        trace_row_t *r = &s_trace_row[i];
        r->dt = label_cell(row, "", 6, 4, 44, &lv_font_montserrat_12, COL_MUTED, true);
        r->id = label_cell(row, "", 56, 4, 66, &lv_font_montserrat_12, COL_CYAN, false);
        r->dlc = label_cell(row, "", 126, 4, 16, &lv_font_montserrat_12, COL_DIM, false);
        lv_obj_set_style_text_align(r->dlc, LV_TEXT_ALIGN_CENTER, 0);
        r->data = label_cell(row, "", 148, 4, 158, &lv_font_montserrat_12, COL_TEXT, false);
    }

    s_trace_empty = empty_hint_create(page, 20, 162, "Waiting for CAN traffic...\nFrames will stream here");
}

static void diag_view_create(lv_obj_t *parent)
{
    lv_obj_t *page = obj_new(parent, 0, CONTENT_Y, UI_W, CONTENT_H);
    s_views[VIEW_DIAG] = page;

    static const char *titles[6] = {
        "TEC / REC", "BUS ERRORS", "BUS OFF",
        "FIFO MISS", "QUEUE FULL", "BLACKBOX"
    };
    static const char *subtitles[6] = {
        "Tx/Rx Errors", "Bit/CRC Alert", "Node Isolated",
        "HW Overrun", "Ring Overrun", "SD Storage"
    };

    for (int i = 0; i < 6; i++) {
        const lv_coord_t cx = 6 + (i % 3) * 105;
        const lv_coord_t cy = 6 + (i / 3) * 66;
        lv_obj_t *card = card_new(page, cx, cy, 98, 60);
        label_cell(card, titles[i], 8, 6, 0, &lv_font_montserrat_12, COL_MUTED, false);
        s_diag_val[i] = label_cell(card, "0", 8, 22, 0, &lv_font_montserrat_16, COL_GREEN, false);
        label_cell(card, subtitles[i], 8, 42, 82, &lv_font_montserrat_12, COL_DIM, false);
    }

    lv_obj_t *rec_btn = btn_new(page, 6, 140, 148, 36, 6, COL_ACTIVE_BTN, recover_click_cb, NULL);
    label_new_centered(rec_btn, LV_SYMBOL_REFRESH " Bus Recover", &lv_font_montserrat_14, COL_TEXT);

    lv_obj_t *clr_btn = btn_new(page, 166, 140, 148, 36, 6, COL_ACTIVE_BTN, clear_click_cb, NULL);
    label_new_centered(clr_btn, LV_SYMBOL_TRASH " Clear Stats", &lv_font_montserrat_14, COL_TEXT);
}

static void settings_view_create(lv_obj_t *parent)
{
    lv_obj_t *page = obj_new(parent, 0, CONTENT_Y, UI_W, CONTENT_H);
    s_views[VIEW_SETTINGS] = page;

    /* Card 1: CAN Bitrate & Mode */
    lv_obj_t *card1 = card_new(page, 4, 6, 312, 46);
    for (int i = 0; i < 4; i++) {
        s_br_pill[i] = btn_new(card1, 8 + i * 45, 8, 40, 30, 4, COL_ACTIVE_BTN,
                               bitrate_click_cb, (void *)(intptr_t)i);
        label_new_centered(s_br_pill[i], BITRATE_LABELS[i], &lv_font_montserrat_12, COL_TEXT);
    }
    s_mode_pill = btn_new(card1, 192, 8, 112, 30, 4, COL_GREEN, mode_click_cb, NULL);
    s_mode_label = label_new_centered(s_mode_pill, "Listen-Only", &lv_font_montserrat_12, COL_BG);

    /* Card 2: Backlight & Actions */
    lv_obj_t *card2 = card_new(page, 4, 58, 312, 48);
    label_cell(card2, "BL:", 8, 16, 0, &lv_font_montserrat_12, COL_MUTED, false);

    static const char *bl_labels[4] = {"20%", "50%", "80%", "100%"};
    for (int i = 0; i < 4; i++) {
        s_bl_pill[i] = btn_new(card2, 32 + i * 34, 9, 30, 30, 4,
                               i == 2 ? COL_GREEN : COL_ACTIVE_BTN,
                               brightness_click_cb, (void *)(intptr_t)i);
        label_new_centered(s_bl_pill[i], bl_labels[i], &lv_font_montserrat_12,
                           i == 2 ? COL_BG : COL_TEXT);
    }

    s_set_pause_btn = btn_new(card2, 172, 9, 66, 30, 4, COL_ACTIVE_BTN, pause_click_cb, NULL);
    s_set_pause_label = label_new_centered(s_set_pause_btn, LV_SYMBOL_PAUSE " Pause",
                                           &lv_font_montserrat_12, COL_TEXT);

    lv_obj_t *mark_btn = btn_new(card2, 244, 9, 60, 30, 4, COL_ACTIVE_BTN, mark_click_cb, NULL);
    label_new_centered(mark_btn, LV_SYMBOL_SAVE " Mark", &lv_font_montserrat_12, COL_TEXT);

    /* Card 3: Device Info */
    lv_obj_t *card3 = card_new(page, 4, 112, 312, 58);
    label_cell(card3, "LinSight CAN Analyzer", 10, 8, 0, &lv_font_montserrat_12, COL_TEXT, false);

    lv_obj_t *fw_pill = btn_new(card3, 238, 6, 64, 20, 4, lv_color_hex(0x232938), NULL, NULL);
    label_new_centered(fw_pill, "v" LINSIGHT_FW_VERSION, &lv_font_montserrat_12, COL_CYAN);

    char dev_info[48];
    snprintf(dev_info, sizeof(dev_info), "ID: %s  |  Reset: %s", linsight_device_id(), linsight_reset_reason());
    label_cell(card3, dev_info, 10, 34, 0, &lv_font_montserrat_12, COL_MUTED, false);
}

static void modal_inspector_create(void)
{
    s_modal_mask = obj_new(lv_layer_top(), 0, 0, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_modal_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal_mask, LV_OPA_70, 0);
    lv_obj_add_flag(s_modal_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal_mask, modal_close_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = card_new(s_modal_mask, 16, 14, 288, 212);
    lv_obj_set_style_border_color(card, COL_CYAN, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    s_modal_id_label = label_cell(card, "CAN FRAME DETAIL", 12, 10, 0, &lv_font_montserrat_16, COL_CYAN, false);
    lv_obj_t *close_btn = btn_new(card, 248, 6, 28, 24, 4, COL_ACTIVE_BTN, modal_close_click_cb, NULL);
    label_new_centered(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_12, COL_TEXT);

    s_modal_sub_label = label_cell(card, "", 12, 32, 0, &lv_font_montserrat_12, COL_TEXT, false);
    s_modal_sub2_label = label_cell(card, "", 12, 48, 0, &lv_font_montserrat_12, COL_MUTED, false);

    lv_obj_t *div = obj_new(card, 12, 66, 264, 1);
    lv_obj_set_style_bg_color(div, COL_CARD_BORDER, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    label_cell(card, "PAYLOAD (HEX / ASCII):", 12, 73, 0, &lv_font_montserrat_12, COL_DIM, false);

    static const char *d_names[8] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"};
    for (int i = 0; i < 8; i++) {
        lv_obj_t *box = card_new(card, 12 + i * 33, 90, 31, 38);
        lv_obj_set_style_radius(box, 4, 0);

        s_modal_byte_val[i] = label_new(box, "--", &lv_font_montserrat_14, COL_TEXT);
        lv_obj_align(s_modal_byte_val[i], LV_ALIGN_TOP_MID, 0, 4);

        lv_obj_t *d_lbl = label_new(box, d_names[i], &lv_font_montserrat_12, COL_DIM);
        lv_obj_align(d_lbl, LV_ALIGN_BOTTOM_MID, 0, -3);
    }

    s_modal_ascii_label = label_cell(card, "ASCII: \"........\"", 12, 136, 0, &lv_font_montserrat_12, COL_CYAN, false);

    lv_obj_t *hint = label_new(card, "[ Tap anywhere or X to close ]", &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    object_show(s_modal_mask, false);
}

/* ---------------------------------------------------------------- splash */

#define SPLASH_CONTENT_W 240
#define SPLASH_BAR_W 60
#define SPLASH_BAR_MS 900u

static void splash_bar_cb(void *var, int32_t value)
{
    lv_obj_set_width((lv_obj_t *)var, (lv_coord_t)value);
    lv_obj_set_x((lv_obj_t *)var, (SPLASH_CONTENT_W - value) / 2);
}

static void splash_ready_cb(lv_anim_t *a)
{
    lv_obj_del((lv_obj_t *)lv_anim_get_user_data(a));
}

static void splash_create(void)
{
    lv_obj_t *splash = btn_new(lv_layer_top(), 0, 0, UI_W, UI_H, 0, COL_BG, NULL, NULL);
    lv_obj_add_flag(splash, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *content = obj_new(splash, (UI_W - SPLASH_CONTENT_W) / 2, 84, SPLASH_CONTENT_W, 88);

    lv_obj_t *brand = label_new(content, "LinSight", &lv_font_montserrat_24, COL_TEXT);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *bar = btn_new(content, SPLASH_CONTENT_W / 2, 34, 0, 3, 2, COL_CYAN, NULL, NULL);

    lv_obj_t *sub = label_new(content, "CYBER CAN ANALYZER", &lv_font_montserrat_12, COL_MUTED);
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
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    top_nav_create(screen);
    dash_view_create(screen);
    trace_view_create(screen);
    diag_view_create(screen);
    settings_view_create(screen);
    bottom_bar_create(screen);
    modal_inspector_create();

    view_show(VIEW_DASH);
    s_rate_tick = lv_tick_get();

    s_toast = card_new(lv_layer_top(), 48, 178, 224, 24);
    lv_obj_set_style_border_color(s_toast, COL_CYAN, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x222734), 0);
    lv_obj_t *toast_lbl = label_new_centered(s_toast, "", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_width(toast_lbl, 210);
    lv_obj_set_style_text_align(toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    object_show(s_toast, false);

    lv_timer_create(tick_cb, 50, NULL);
    splash_create();
}

void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms)
{
    s_conn_req = state;
    s_rtt_req = rtt_ms;
}

void linsight_ui_handle_key(bool long_press)
{
    lvgl_port_lock(0);
    if (s_current_view == VIEW_SETTINGS) {
        view_show(s_prev_view);
    } else if (long_press) {
        view_show(VIEW_SETTINGS);
    } else {
        view_show((s_current_view + 1) % 3);
    }
    lvgl_port_unlock();
}
