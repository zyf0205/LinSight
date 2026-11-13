/*
 * LinSight CAN analyzer UI (320x240, LVGL 8.3) — Cyber Instrument HIG.
 *
 * Architecture:
 *   Top Navigation & Status Bar (26 px):
 *     - Bitrate & Mode pill (tap -> jump to Setup)
 *     - Segmented Tab Bar: [ DASH | TRACE | DIAG ] for direct touch switching
 *     - Status cluster: SD recording badge, Host link/RTT, Setup gear icon
 *
 *   Four Coordinated Views (186 px content height, Y: 26..212):
 *     1. DASH     — Bus telemetry banner (Large FPS, dynamic Bus Load Gauge Bar,
 *                   adaptive Health Badge), 5 x 24 px Top-ID ranked cards with
 *                   bandwidth share bars. Tap any ID row to open Frame Inspector!
 *     2. TRACE    — Real-time packet sniffer with Δt (delta time ms), ID, DLC,
 *                   and spaced Hex bytes. Tap any frame row to open Frame Inspector!
 *     3. DIAG     — Dedicated 2x3 engineering diagnostics grid (TEC/REC, Bus Errors,
 *                   Bus-Off, HW FIFO Overrun, SW Queue Full, SD Blackbox state)
 *                   plus instant Bus Recovery and Clear Stats actions.
 *     4. SETTINGS — Interactive control center: Bitrate selector (125k..1M),
 *                   Listen/Normal mode toggle, Backlight brightness presets
 *                   (20%, 50%, 80%, 100%), Manual SD event marker, Device info.
 *
 *   Interactive CAN Detail Inspector Modal:
 *     - High-density popover card displaying full ID (Std/Ext), rate (Hz),
 *       period (ms), cumulative hits, byte-by-byte visual boxes (D0..D7),
 *       and ASCII preview string.
 *
 *   Bottom Telemetry Bar (28 px, Y: 212..240):
 *     - Global RX frame tally, Bus Load %, SD storage state, and Run/Pause badge.
 *
 * Performance & Memory:
 *   - Zero dynamic allocations on the 20 Hz tick. All strings reside in static
 *     buffers and use lv_label_set_text_static.
 *   - Lock-free mailbox drains net_task connection events safely.
 *   - Hardware touch targets >= 32 px with gesture bubble protection.
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

/* ------------------------------------------------------ palette (Calm Industrial Dark) */

#define COL_BG          lv_color_hex(0x13151A) /* Soothing Dark Charcoal */
#define COL_CARD        lv_color_hex(0x1A1D24) /* Elevated Soft Slate */
#define COL_CARD_BORDER lv_color_hex(0x282D38) /* Subtle Tech Border */
#define COL_NAV_BAR     lv_color_hex(0x161920) /* Top Nav Bar */
#define COL_BOT_BAR     lv_color_hex(0x161920) /* Bottom Telemetry Bar */
#define COL_ACTIVE_TAB  lv_color_hex(0x282E3D) /* Active Tab Highlight */
#define COL_ROW_EVEN    lv_color_hex(0x15181F)
#define COL_ROW_ODD     lv_color_hex(0x1A1D25)
#define COL_ACTIVE_BTN  lv_color_hex(0x252B38)
#define COL_TEXT        lv_color_hex(0xE2E8F0) /* Soft Crisp White (no glare) */
#define COL_MUTED       lv_color_hex(0x94A3B8) /* Slate Gray */
#define COL_DIM         lv_color_hex(0x5E6676) /* Muted Steel */
#define COL_CYAN        lv_color_hex(0x38BDF8) /* Calming Sky Blue for Std ID */
#define COL_PURPLE      lv_color_hex(0xA78BFA) /* Soft Lilac for Ext ID */
#define COL_GREEN       lv_color_hex(0x22C55E) /* Calming Emerald Green */
#define COL_AMBER       lv_color_hex(0xEAB308) /* Warm Amber for Warning */
#define COL_RED         lv_color_hex(0xEF4444) /* Soft Crimson Red for Error */

typedef enum {
    VIEW_DASH = 0,
    VIEW_TRACE = 1,
    VIEW_DIAG = 2,
    VIEW_SETTINGS = 3,
    VIEW_COUNT = 4,
} view_id_t;

typedef struct {
    uint32_t id;
    uint32_t count;
    uint32_t win;
    uint16_t rate;
    uint8_t ext;
    uint8_t dlc;
    uint8_t data[8];
    uint8_t rtr;
    uint8_t used;
    uint32_t last_ts_ms;
} id_count_t;

/* -------------------------------------------------------------- statics */

static lv_obj_t *s_views[VIEW_COUNT];
static view_id_t s_current_view = VIEW_DASH;
static view_id_t s_prev_view = VIEW_DASH;

/* Top Navigation Bar */
static lv_obj_t *s_nav_br_pill;
static lv_obj_t *s_nav_br_label;
static lv_obj_t *s_nav_back_btn;
static lv_obj_t *s_nav_back_label;
static lv_obj_t *s_nav_tab_group;
static lv_obj_t *s_nav_tab_btn[3];
static lv_obj_t *s_nav_tab_label[3];
static lv_obj_t *s_nav_settings_title;
static lv_obj_t *s_nav_host_dot;
static lv_obj_t *s_nav_host_label;
static lv_obj_t *s_nav_sd_dot;
static lv_obj_t *s_nav_sd_label;

/* DASH View */
static lv_obj_t *s_dash_fps_num;
static lv_obj_t *s_dash_load_text;
static lv_obj_t *s_dash_load_bar;
static lv_obj_t *s_dash_health_pill;
static lv_obj_t *s_dash_health_label;
static lv_obj_t *s_dash_row[TOP_N];
static lv_obj_t *s_dash_id[TOP_N];
static lv_obj_t *s_dash_rate[TOP_N];
static lv_obj_t *s_dash_per[TOP_N];
static lv_obj_t *s_dash_cnt[TOP_N];
static lv_obj_t *s_dash_bar[TOP_N];
static lv_obj_t *s_dash_share[TOP_N];
static lv_obj_t *s_dash_empty;

/* TRACE View */
static lv_obj_t *s_trace_row[ROW_N];
static lv_obj_t *s_trace_dt[ROW_N];
static lv_obj_t *s_trace_id[ROW_N];
static lv_obj_t *s_trace_dlc[ROW_N];
static lv_obj_t *s_trace_data[ROW_N];
static lv_obj_t *s_trace_empty;
static linsight_can_record_t s_trace_rec[ROW_N];
static bool s_trace_rec_valid[ROW_N];

/* DIAG View */
static lv_obj_t *s_diag_val[6];

/* SETTINGS View */
static lv_obj_t *s_br_pill[4];
static lv_obj_t *s_mode_pill;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_bl_pill[4];
static lv_obj_t *s_set_pause_btn;
static lv_obj_t *s_set_pause_label;

/* Bottom Telemetry Bar */
static lv_obj_t *s_bot_rx_label;
static lv_obj_t *s_bot_load_label;
static lv_obj_t *s_bot_sd_label;
static lv_obj_t *s_bot_state_pill;
static lv_obj_t *s_bot_state_label;

/* Frame Detail Inspector Modal */
static lv_obj_t *s_modal_mask;
static lv_obj_t *s_modal_card;
static lv_obj_t *s_modal_id_label;
static lv_obj_t *s_modal_sub_label;
static lv_obj_t *s_modal_byte_box[8];
static lv_obj_t *s_modal_byte_val[8];
static lv_obj_t *s_modal_ascii_label;

/* Toast Feedback */
static lv_obj_t *s_toast;
static uint32_t s_toast_tick;

/* Dynamic text buffers (zero-allocation) */
static char s_txt_nav_br[12];
static char s_txt_nav_sd[12];
static char s_txt_nav_host[16];

static char s_txt_dash_fps[10];
static char s_txt_dash_load[12];
static char s_txt_dash_health[16];
static char s_txt_dash_id[TOP_N][12];
static char s_txt_dash_rate[TOP_N][10];
static char s_txt_dash_per[TOP_N][8];
static char s_txt_dash_cnt[TOP_N][10];
static char s_txt_dash_share[TOP_N][6];

static char s_txt_trace_dt[ROW_N][10];
static char s_txt_trace_id[ROW_N][12];
static char s_txt_trace_dlc[ROW_N][4];
static char s_txt_trace_data[ROW_N][28];

static char s_txt_diag_val[6][16];

static char s_txt_mode[16];
static char s_txt_set_pause[16];

static char s_txt_bot_rx[16];
static char s_txt_bot_load[16];
static char s_txt_bot_sd[16];
static char s_txt_bot_state[12];

static char s_txt_modal_id[28];
static char s_txt_modal_sub[48];
static char s_txt_modal_byte[8][4];
static char s_txt_modal_ascii[32];
static char s_txt_toast[40];

/* Runtime UI states */
static linsight_conn_t s_conn = UI_CONN_WIFI;
static uint32_t s_rtt_ms = 0;
static bool s_frozen = false;
static uint32_t s_remote_tick = 0;
static bool s_remote_on = false;
static bool s_floor_valid = false;
static uint16_t s_floor = 0;
static bool s_total_valid = false;
static uint32_t s_prev_total = 0;

/* Mailbox from net_task */
static volatile linsight_conn_t s_conn_req = UI_CONN_WIFI;
static volatile uint32_t s_rtt_req = 0;
static volatile bool s_remote_req = false;

/* Capture history scanning & ranking */
static linsight_can_record_t s_scan_buf[SCAN_N];
static uint32_t s_scan_n = 0;
static uint16_t s_scan_seq = 0;
static bool s_scan_valid = false;
static id_count_t s_counts[CNT_SLOTS];

static const id_count_t *s_top[TOP_N];
static int s_top_n = 0;
static bool s_top_any = false;
static uint32_t s_rate_tick = 0;
static bool s_rate_valid = false;

static const can_bitrate_t BITRATES[4] = {CAN_BITRATE_125K, CAN_BITRATE_250K,
                                          CAN_BITRATE_500K, CAN_BITRATE_1M};
static const char *BITRATE_LABELS[4] = {"125K", "250K", "500K", "1M"};
static const int BRIGHTNESS_LEVELS[4] = {20, 50, 80, 100};

/* Forward declarations */
static void view_show(view_id_t view);
static void modal_show_slot(const id_count_t *slot);
static void modal_show_record(const linsight_can_record_t *r);
static void modal_hide(void);
static void toast_showf(const char *fmt, ...);
static void view_reset(void);
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

static void object_show(lv_obj_t *obj, bool show)
{
    if (!obj) {
        return;
    }
    const bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (show == !hidden) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fmt_id(char *buf, size_t cap, uint32_t id, bool ext)
{
    if (ext) {
        snprintf(buf, cap, "%08X", (unsigned)id);
    } else {
        snprintf(buf, cap, "%03X", (unsigned)id);
    }
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
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, COL_CARD_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 6, 0);
    return card;
}

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

/* -------------------------------------------------------- trace counters */

static id_count_t *count_slot(uint32_t id, bool ext)
{
    const uint32_t h = (id * 2654435761u + (ext ? 0x9E3779B9u : 0u)) & (CNT_SLOTS - 1);
    for (uint32_t i = 0; i < CNT_SLOTS; i++) {
        id_count_t *slot = &s_counts[(h + CNT_SLOTS - i) & (CNT_SLOTS - 1)];
        if (!slot->used) {
            slot->used = 1;
            slot->id = id;
            slot->ext = ext ? 1 : 0;
            slot->count = 0;
            slot->win = 0;
            slot->rate = 0;
            slot->dlc = 0;
            slot->rtr = 0;
            slot->last_ts_ms = 0;
            memset(slot->data, 0, sizeof(slot->data));
            return slot;
        }
        if (slot->id == id && slot->ext == (ext ? 1 : 0)) {
            return slot;
        }
    }
    return NULL;
}

static uint32_t count_get(uint32_t id, bool ext)
{
    const uint32_t h = (id * 2654435761u + (ext ? 0x9E3779B9u : 0u)) & (CNT_SLOTS - 1);
    for (uint32_t i = 0; i < CNT_SLOTS; i++) {
        const id_count_t *slot = &s_counts[(h + CNT_SLOTS - i) & (CNT_SLOTS - 1)];
        if (!slot->used) {
            break;
        }
        if (slot->id == id && slot->ext == (ext ? 1 : 0)) {
            return slot->count;
        }
    }
    return 0;
}

static bool seq_after(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) > 0;
}

/* ------------------------------------------------------- ranking engine */

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
            continue;
        }
        rank_put(slot);
    }
}

static void counts_clear(void)
{
    memset(s_counts, 0, sizeof(s_counts));
    s_rate_valid = false;
    s_rate_tick = lv_tick_get();
    s_top_n = 0;
    s_top_any = false;
}

static void view_reset(void)
{
    counts_clear();
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

    for (int i = 0; i < TOP_N; i++) {
        label_set_str(s_dash_id[i], s_txt_dash_id[i], sizeof(s_txt_dash_id[i]), "");
        label_set_str(s_dash_rate[i], s_txt_dash_rate[i], sizeof(s_txt_dash_rate[i]), "");
        label_set_str(s_dash_per[i], s_txt_dash_per[i], sizeof(s_txt_dash_per[i]), "");
        label_set_str(s_dash_cnt[i], s_txt_dash_cnt[i], sizeof(s_txt_dash_cnt[i]), "");
        label_set_str(s_dash_share[i], s_txt_dash_share[i], sizeof(s_txt_dash_share[i]), "");
        object_show(s_dash_bar[i], false);
    }
    for (int i = 0; i < ROW_N; i++) {
        label_set_str(s_trace_dt[i], s_txt_trace_dt[i], sizeof(s_txt_trace_dt[i]), "");
        label_set_str(s_trace_id[i], s_txt_trace_id[i], sizeof(s_txt_trace_id[i]), "");
        label_set_str(s_trace_dlc[i], s_txt_trace_dlc[i], sizeof(s_txt_trace_dlc[i]), "");
        label_set_str(s_trace_data[i], s_txt_trace_data[i], sizeof(s_txt_trace_data[i]), "");
    }
    object_show(s_dash_empty, true);
    object_show(s_trace_empty, true);
    s_total_valid = false;
}

/* ----------------------------------------------------- detail inspector */

static void modal_close_click_cb(lv_event_t *e)
{
    (void)e;
    modal_hide();
}

static void modal_hide(void)
{
    object_show(s_modal_mask, false);
}

static void modal_show_slot(const id_count_t *slot)
{
    if (!slot || !slot->used) {
        return;
    }
    char idbuf[12];
    fmt_id(idbuf, sizeof(idbuf), slot->id, slot->ext != 0);
    label_setf(s_modal_id_label, s_txt_modal_id, sizeof(s_txt_modal_id), "ID: 0x%s (%s)",
               idbuf, slot->ext ? "EXT" : "STD");
    label_color(s_modal_id_label, slot->ext ? COL_PURPLE : COL_CYAN);

    char per[10];
    if (slot->rate > 0) {
        snprintf(per, sizeof(per), "%u ms", (unsigned)(1000u / slot->rate));
    } else {
        snprintf(per, sizeof(per), "---");
    }

    label_setf(s_modal_sub_label, s_txt_modal_sub, sizeof(s_txt_modal_sub),
               "Rate: %u Hz | Per: %s | Hits: %u | DLC: %u", (unsigned)slot->rate, per,
               (unsigned)slot->count, (unsigned)slot->dlc);

    /* Byte boxes */
    char ascii[16];
    size_t dlc = slot->dlc > 8 ? 8 : slot->dlc;
    for (size_t i = 0; i < 8; i++) {
        if (i < dlc) {
            snprintf(s_txt_modal_byte[i], sizeof(s_txt_modal_byte[i]), "%02X",
                     (unsigned)slot->data[i]);
            lv_label_set_text_static(s_modal_byte_val[i], s_txt_modal_byte[i]);
            label_color(s_modal_byte_val[i], COL_TEXT);
            const uint8_t c = slot->data[i];
            ascii[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        } else {
            snprintf(s_txt_modal_byte[i], sizeof(s_txt_modal_byte[i]), "--");
            lv_label_set_text_static(s_modal_byte_val[i], s_txt_modal_byte[i]);
            label_color(s_modal_byte_val[i], COL_DIM);
            ascii[i] = ' ';
        }
    }
    ascii[dlc] = '\0';
    label_setf(s_modal_ascii_label, s_txt_modal_ascii, sizeof(s_txt_modal_ascii),
               "ASCII: \"%s\"", ascii);

    object_show(s_modal_mask, true);
}

static void modal_show_record(const linsight_can_record_t *r)
{
    if (!r) {
        return;
    }
    const bool ext = (r->flags & LINSIGHT_REC_FLAG_EXT) != 0;
    const bool err = (r->flags & LINSIGHT_REC_FLAG_ERR) != 0;

    if (err) {
        label_set_str(s_modal_id_label, s_txt_modal_id, sizeof(s_txt_modal_id),
                      "ERR-FRM (Error Frame)");
        label_color(s_modal_id_label, COL_RED);
        label_set_str(s_modal_sub_label, s_txt_modal_sub, sizeof(s_txt_modal_sub),
                      "Bus physical/protocol disturbance");
        for (size_t i = 0; i < 8; i++) {
            label_set_str(s_modal_byte_val[i], s_txt_modal_byte[i],
                          sizeof(s_txt_modal_byte[i]), "--");
            label_color(s_modal_byte_val[i], COL_DIM);
        }
        label_set_str(s_modal_ascii_label, s_txt_modal_ascii,
                      sizeof(s_txt_modal_ascii), "ASCII: N/A");
    } else {
        char idbuf[12];
        fmt_id(idbuf, sizeof(idbuf), r->id, ext);
        label_setf(s_modal_id_label, s_txt_modal_id, sizeof(s_txt_modal_id), "ID: 0x%s (%s)",
                   idbuf, ext ? "EXT" : "STD");
        label_color(s_modal_id_label, ext ? COL_PURPLE : COL_CYAN);

        const uint32_t cnt = count_get(r->id, ext);
        label_setf(s_modal_sub_label, s_txt_modal_sub, sizeof(s_txt_modal_sub),
                   "DLC: %u Bytes | Hits: %u | Seq: %u", (unsigned)r->dlc,
                   (unsigned)cnt, (unsigned)r->seq);

        char ascii[16];
        size_t dlc = r->dlc > 8 ? 8 : r->dlc;
        for (size_t i = 0; i < 8; i++) {
            if (i < dlc) {
                snprintf(s_txt_modal_byte[i], sizeof(s_txt_modal_byte[i]), "%02X",
                         (unsigned)r->data[i]);
                lv_label_set_text_static(s_modal_byte_val[i], s_txt_modal_byte[i]);
                label_color(s_modal_byte_val[i], COL_TEXT);
                const uint8_t c = r->data[i];
                ascii[i] = (c >= 32 && c <= 126) ? (char)c : '.';
            } else {
                snprintf(s_txt_modal_byte[i], sizeof(s_txt_modal_byte[i]), "--");
                lv_label_set_text_static(s_modal_byte_val[i], s_txt_modal_byte[i]);
                label_color(s_modal_byte_val[i], COL_DIM);
                ascii[i] = ' ';
            }
        }
        ascii[dlc] = '\0';
        label_setf(s_modal_ascii_label, s_txt_modal_ascii, sizeof(s_txt_modal_ascii),
                   "ASCII: \"%s\"", ascii);
    }
    object_show(s_modal_mask, true);
}

static void dash_row_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_top_n && s_top[idx]) {
        modal_show_slot(s_top[idx]);
    }
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
        label_color(s_nav_tab_label[i], active ? COL_TEXT : COL_MUTED);
    }
}

static void view_show(view_id_t view)
{
    if (view >= VIEW_COUNT) {
        view = VIEW_DASH;
    }
    if (s_current_view != VIEW_SETTINGS && view == VIEW_SETTINGS) {
        s_prev_view = s_current_view;
    }
    s_current_view = view;

    for (int i = 0; i < VIEW_COUNT; i++) {
        object_show(s_views[i], i == (int)view);
    }

    const bool in_settings = (view == VIEW_SETTINGS);
    object_show(s_nav_br_pill, !in_settings);
    object_show(s_nav_back_btn, in_settings);
    object_show(s_nav_tab_group, !in_settings);
    object_show(s_nav_settings_title, in_settings);

    if (!in_settings) {
        nav_tab_update();
    }
}

static void tab_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    view_show((view_id_t)idx);
}

static void settings_click_cb(lv_event_t *e)
{
    (void)e;
    view_show(VIEW_SETTINGS);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    view_show(s_prev_view);
}

/* --------------------------------------------------------------- actions */

static void bitrate_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (BITRATES[idx] == twai_current_bitrate()) {
        return;
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
        toast_showf("MODE: %s", next == CAN_MODE_NORMAL ? "NORMAL (ACK)" : "LISTEN-ONLY");
    } else {
        toast_showf("MODE CHANGE FAILED");
    }
}

static void brightness_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bsp_backlight_set(BRIGHTNESS_LEVELS[idx]);
    toast_showf("BRIGHTNESS %d%%", BRIGHTNESS_LEVELS[idx]);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_bl_pill[i], i == idx ? COL_GREEN : COL_ACTIVE_BTN, 0);
    }
}

static void toggle_pause(void)
{
    if (s_frozen) {
        s_frozen = false;
        toast_showf("RESUMED");
        return;
    }
    can_stats_t st;
    twai_stats_snapshot(&st);
    if (st.paused) {
        twai_capture_set_paused(false);
        toast_showf("CAPTURE RESUMED");
        return;
    }
    s_frozen = true;
    toast_showf("PAUSED");
}

static void pause_click_cb(lv_event_t *e)
{
    (void)e;
    toggle_pause();
}

static void mark_click_cb(lv_event_t *e)
{
    (void)e;
    sd_store_mark("MANUAL", 0, 0);
    toast_showf("SD EVENT MARKED");
}

static void recover_click_cb(lv_event_t *e)
{
    (void)e;
    toast_showf(twai_capture_recover() == ESP_OK ? "BUS RECOVER OK" : "RECOVER FAILED");
}

static void clear_click_cb(lv_event_t *e)
{
    (void)e;
    twai_stats_clear();
    view_reset();
    toast_showf("STATS CLEARED");
}

/* ---------------------------------------------------------- record scan */

static void scan_records(void)
{
    s_scan_n = twai_ui_tail(s_scan_buf, SCAN_N);
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
            if (slot) {
                slot->count++;
                slot->win++;
                slot->dlc = r->dlc;
                slot->rtr = (r->flags & LINSIGHT_REC_FLAG_RTR) != 0 ? 1 : 0;
                slot->last_ts_ms = r->ts_ms;
                memcpy(slot->data, r->data, 8);
            }
        }
    }
}

/* ------------------------------------------------------- render views */

static void render_dash(const can_stats_t *st)
{
    /* FPS Big Label */
    label_setf(s_dash_fps_num, s_txt_dash_fps, sizeof(s_txt_dash_fps), "%u",
               (unsigned)st->fps);

    /* Bus Load Bar & Text */
    const uint32_t load_pct = st->load_permille / 10u;
    label_setf(s_dash_load_text, s_txt_dash_load, sizeof(s_txt_dash_load), "LOAD %u%%",
               (unsigned)load_pct);
    lv_bar_set_value(s_dash_load_bar, (int32_t)load_pct, LV_ANIM_OFF);
    const lv_color_t bar_color = (load_pct >= 80) ? COL_RED
                                 : (load_pct >= 50) ? COL_AMBER
                                                    : COL_GREEN;
    lv_obj_set_style_bg_color(s_dash_load_bar, bar_color, LV_PART_INDICATOR);

    /* Adaptive Health Pill: Normal when 0 errors; Alert only when degraded */
    const bool is_bus_off = (strcmp(st->state, "BUS_OFF") == 0);
    const bool is_passive = (strcmp(st->state, "RECOVERING") == 0 || st->tec > 127u || st->rec > 127u);
    const bool has_errors = (st->bus_errors > 0 || st->tec > 0 || st->rec > 0);

    if (is_bus_off) {
        label_set_str(s_dash_health_label, s_txt_dash_health, sizeof(s_txt_dash_health),
                      "BUS-OFF");
        lv_obj_set_style_bg_color(s_dash_health_pill, COL_RED, 0);
        label_color(s_dash_health_label, COL_TEXT);
    } else if (is_passive) {
        label_set_str(s_dash_health_label, s_txt_dash_health, sizeof(s_txt_dash_health),
                      "PASSIVE");
        lv_obj_set_style_bg_color(s_dash_health_pill, COL_AMBER, 0);
        label_color(s_dash_health_label, COL_TEXT);
    } else if (has_errors) {
        label_setf(s_dash_health_label, s_txt_dash_health, sizeof(s_txt_dash_health),
                   "BERR %u", (unsigned)st->bus_errors);
        lv_obj_set_style_bg_color(s_dash_health_pill, COL_AMBER, 0);
        label_color(s_dash_health_label, COL_TEXT);
    } else {
        label_set_str(s_dash_health_label, s_txt_dash_health, sizeof(s_txt_dash_health),
                      LV_SYMBOL_OK " NORMAL");
        lv_obj_set_style_bg_color(s_dash_health_pill, lv_color_hex(0x064E3B), 0);
        label_color(s_dash_health_label, COL_GREEN);
    }

    /* Top Active IDs Table */
    top_build();
    const uint32_t top_max = (s_top_n > 0 && s_top[0]->rate > 0) ? s_top[0]->rate : 1;

    for (int i = 0; i < s_top_n; i++) {
        const id_count_t *slot = s_top[i];
        char idbuf[12];
        fmt_id(idbuf, sizeof(idbuf), slot->id, slot->ext != 0);
        label_set_str(s_dash_id[i], s_txt_dash_id[i], sizeof(s_txt_dash_id[i]), idbuf);
        label_color(s_dash_id[i], slot->ext ? COL_PURPLE : COL_CYAN);

        char rate[10];
        if (!s_rate_valid) {
            snprintf(rate, sizeof(rate), "---");
        } else if (slot->rate >= 10000u) {
            snprintf(rate, sizeof(rate), "%uk", (unsigned)(slot->rate / 1000u));
        } else {
            snprintf(rate, sizeof(rate), "%u", (unsigned)slot->rate);
        }
        label_set_str(s_dash_rate[i], s_txt_dash_rate[i], sizeof(s_txt_dash_rate[i]), rate);

        char per[8];
        if (!s_rate_valid || slot->rate == 0) {
            snprintf(per, sizeof(per), "---");
        } else if (slot->rate > 1000u) {
            snprintf(per, sizeof(per), "<1ms");
        } else {
            snprintf(per, sizeof(per), "%ums", (unsigned)(1000u / slot->rate));
        }
        label_set_str(s_dash_per[i], s_txt_dash_per[i], sizeof(s_txt_dash_per[i]), per);

        char cnt[10];
        fmt_count(cnt, sizeof(cnt), slot->count);
        label_set_str(s_dash_cnt[i], s_txt_dash_cnt[i], sizeof(s_txt_dash_cnt[i]), cnt);

        /* Proportional Bandwidth Bar */
        const uint32_t bar_w = (slot->rate * 40u) / top_max;
        lv_obj_set_width(s_dash_bar[i], (lv_coord_t)(bar_w < 3 ? 3 : bar_w));
        lv_obj_set_style_bg_color(s_dash_bar[i], slot->ext ? COL_PURPLE : COL_CYAN, 0);
        object_show(s_dash_bar[i], true);

        const uint32_t pct = (st->fps > 0) ? (slot->rate * 100u / st->fps) : 0;
        label_setf(s_dash_share[i], s_txt_dash_share[i], sizeof(s_txt_dash_share[i]),
                   "%u%%", (unsigned)pct);
    }

    for (int i = s_top_n; i < TOP_N; i++) {
        label_set_str(s_dash_id[i], s_txt_dash_id[i], sizeof(s_txt_dash_id[i]), "");
        label_set_str(s_dash_rate[i], s_txt_dash_rate[i], sizeof(s_txt_dash_rate[i]), "");
        label_set_str(s_dash_per[i], s_txt_dash_per[i], sizeof(s_txt_dash_per[i]), "");
        label_set_str(s_dash_cnt[i], s_txt_dash_cnt[i], sizeof(s_txt_dash_cnt[i]), "");
        label_set_str(s_dash_share[i], s_txt_dash_share[i], sizeof(s_txt_dash_share[i]), "");
        object_show(s_dash_bar[i], false);
    }
    object_show(s_dash_empty, s_top_n == 0);
}

static void render_trace(void)
{
    uint16_t pick[ROW_N];
    int rows = 0;
    for (int i = (int)s_scan_n - 1; i >= 0 && rows < ROW_N; i--) {
        if (s_floor_valid && !seq_after(s_scan_buf[i].seq, s_floor)) {
            continue;
        }
        pick[rows++] = (uint16_t)i;
    }

    for (int r_idx = 0; r_idx < rows; r_idx++) {
        const int scan_idx = pick[rows - 1 - r_idx];
        const linsight_can_record_t *rec = &s_scan_buf[scan_idx];
        s_trace_rec[r_idx] = *rec;
        s_trace_rec_valid[r_idx] = true;
        const bool ext = (rec->flags & LINSIGHT_REC_FLAG_EXT) != 0;
        const bool err = (rec->flags & LINSIGHT_REC_FLAG_ERR) != 0;
        const bool rtr = (rec->flags & LINSIGHT_REC_FLAG_RTR) != 0;

        /* Delta time from previous frame */
        if (r_idx > 0) {
            const int prev_scan_idx = pick[rows - r_idx];
            const uint32_t dt = rec->ts_ms >= s_scan_buf[prev_scan_idx].ts_ms
                                    ? rec->ts_ms - s_scan_buf[prev_scan_idx].ts_ms
                                    : 0;
            label_setf(s_trace_dt[r_idx], s_txt_trace_dt[r_idx],
                       sizeof(s_txt_trace_dt[r_idx]), "+%u", (unsigned)dt);
        } else {
            label_set_str(s_trace_dt[r_idx], s_txt_trace_dt[r_idx],
                          sizeof(s_txt_trace_dt[r_idx]), "0");
        }

        if (err) {
            label_set_str(s_trace_id[r_idx], s_txt_trace_id[r_idx],
                          sizeof(s_txt_trace_id[r_idx]), "ERR-FRM");
            label_color(s_trace_id[r_idx], COL_RED);
        } else {
            char idbuf[12];
            fmt_id(idbuf, sizeof(idbuf), rec->id, ext);
            label_set_str(s_trace_id[r_idx], s_txt_trace_id[r_idx],
                          sizeof(s_txt_trace_id[r_idx]), idbuf);
            label_color(s_trace_id[r_idx], ext ? COL_PURPLE : COL_CYAN);
        }

        if (rtr) {
            label_set_str(s_trace_dlc[r_idx], s_txt_trace_dlc[r_idx],
                          sizeof(s_txt_trace_dlc[r_idx]), "R");
            label_set_str(s_trace_data[r_idx], s_txt_trace_data[r_idx],
                          sizeof(s_txt_trace_data[r_idx]), "REMOTE REQUEST");
            label_color(s_trace_data[r_idx], COL_AMBER);
        } else {
            label_setf(s_trace_dlc[r_idx], s_txt_trace_dlc[r_idx],
                       sizeof(s_txt_trace_dlc[r_idx]), "%u", (unsigned)rec->dlc);
            char hex[28];
            size_t used = 0;
            const uint8_t dlc = rec->dlc > 8 ? 8 : rec->dlc;
            for (uint8_t b = 0; b < dlc; b++) {
                used += snprintf(hex + used, sizeof(hex) - used, "%02X%s",
                                 (unsigned)rec->data[b], b + 1 < dlc ? " " : "");
            }
            label_set_str(s_trace_data[r_idx], s_txt_trace_data[r_idx],
                          sizeof(s_txt_trace_data[r_idx]), hex);
            label_color(s_trace_data[r_idx], COL_TEXT);
        }
    }

    for (int r_idx = rows; r_idx < ROW_N; r_idx++) {
        s_trace_rec_valid[r_idx] = false;
        label_set_str(s_trace_dt[r_idx], s_txt_trace_dt[r_idx],
                      sizeof(s_txt_trace_dt[r_idx]), "");
        label_set_str(s_trace_id[r_idx], s_txt_trace_id[r_idx],
                      sizeof(s_txt_trace_id[r_idx]), "");
        label_set_str(s_trace_dlc[r_idx], s_txt_trace_dlc[r_idx],
                      sizeof(s_txt_trace_dlc[r_idx]), "");
        label_set_str(s_trace_data[r_idx], s_txt_trace_data[r_idx],
                      sizeof(s_txt_trace_data[r_idx]), "");
    }
    object_show(s_trace_empty, rows == 0);
}

static void render_diag(const can_stats_t *st)
{
    /* Card 0: TEC / REC */
    label_setf(s_diag_val[0], s_txt_diag_val[0], sizeof(s_txt_diag_val[0]),
               "%u / %u", (unsigned)st->tec, (unsigned)st->rec);
    label_color(s_diag_val[0], (st->tec >= 128u || st->rec >= 128u) ? COL_RED
                               : (st->tec || st->rec) ? COL_AMBER
                                                      : COL_GREEN);

    /* Card 1: Bus Errors */
    label_setf(s_diag_val[1], s_txt_diag_val[1], sizeof(s_txt_diag_val[1]),
               "%u", (unsigned)st->bus_errors);
    label_color(s_diag_val[1], st->bus_errors > 0 ? COL_RED : COL_GREEN);

    /* Card 2: Bus Off */
    label_setf(s_diag_val[2], s_txt_diag_val[2], sizeof(s_txt_diag_val[2]),
               "%u", (unsigned)st->bus_off);
    label_color(s_diag_val[2], st->bus_off > 0 ? COL_RED : COL_GREEN);

    /* Card 3: HW FIFO Miss */
    label_setf(s_diag_val[3], s_txt_diag_val[3], sizeof(s_txt_diag_val[3]),
               "%u", (unsigned)st->rx_missed);
    label_color(s_diag_val[3], st->rx_missed > 0 ? COL_AMBER : COL_GREEN);

    /* Card 4: SW Queue Full */
    label_setf(s_diag_val[4], s_txt_diag_val[4], sizeof(s_txt_diag_val[4]),
               "%u", (unsigned)st->rx_queue_full);
    label_color(s_diag_val[4], st->rx_queue_full > 0 ? COL_AMBER : COL_GREEN);

    /* Card 5: Storage */
    sd_status_t sd;
    sd_store_get_status(&sd);
    if (sd.recording) {
        label_setf(s_diag_val[5], s_txt_diag_val[5], sizeof(s_txt_diag_val[5]),
                   "REC %uF", (unsigned)sd.files);
        label_color(s_diag_val[5], COL_GREEN);
    } else if (sd.mounted) {
        label_set_str(s_diag_val[5], s_txt_diag_val[5], sizeof(s_txt_diag_val[5]), "READY");
        label_color(s_diag_val[5], COL_GREEN);
    } else {
        label_set_str(s_diag_val[5], s_txt_diag_val[5], sizeof(s_txt_diag_val[5]), "NO SD");
        label_color(s_diag_val[5], COL_DIM);
    }
}

static void render_settings(const can_stats_t *st)
{
    const can_bitrate_t cur_br = twai_current_bitrate();
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_br_pill[i], BITRATES[i] == cur_br ? COL_CYAN : COL_ACTIVE_BTN, 0);
    }

    const can_mode_t mode = twai_current_mode();
    label_set_str(s_mode_label, s_txt_mode, sizeof(s_txt_mode),
                  mode == CAN_MODE_NORMAL ? "Normal (ACK)" : "Listen-Only");
    lv_obj_set_style_bg_color(s_mode_pill, mode == CAN_MODE_NORMAL ? COL_AMBER : COL_GREEN, 0);

    const bool paused = s_frozen || st->paused;
    label_setf(s_set_pause_label, s_txt_set_pause, sizeof(s_txt_set_pause), "%s %s",
               paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, paused ? "Resume" : "Pause");
    lv_obj_set_style_bg_color(s_set_pause_btn, paused ? COL_AMBER : COL_ACTIVE_BTN, 0);
}

static void render_bars(const can_stats_t *st)
{
    /* Top Nav: Bitrate & mode pill */
    const can_mode_t mode = twai_current_mode();
    const char *br_name = can_bitrate_name(twai_current_bitrate());
    label_setf(s_nav_br_label, s_txt_nav_br, sizeof(s_txt_nav_br), "%s %s",
               br_name, mode == CAN_MODE_LISTEN_ONLY ? "LO" : "NRM");

    /* Top Nav: SD & Host badges */
    sd_status_t sd;
    sd_store_get_status(&sd);
    if (sd.recording) {
        object_show(s_nav_sd_dot, true);
        lv_obj_set_style_bg_color(s_nav_sd_dot, COL_RED, 0);
        label_set_str(s_nav_sd_label, s_txt_nav_sd, sizeof(s_txt_nav_sd), "REC");
        label_color(s_nav_sd_label, COL_RED);
    } else if (sd.mounted) {
        object_show(s_nav_sd_dot, false);
        label_set_str(s_nav_sd_label, s_txt_nav_sd, sizeof(s_txt_nav_sd), "SD");
        label_color(s_nav_sd_label, COL_MUTED);
    } else {
        object_show(s_nav_sd_dot, false);
        label_set_str(s_nav_sd_label, s_txt_nav_sd, sizeof(s_txt_nav_sd), "");
    }

    if (s_conn == UI_CONN_ONLINE) {
        object_show(s_nav_host_dot, true);
        lv_obj_set_style_bg_color(s_nav_host_dot, COL_GREEN, 0);
        if (s_rtt_ms > 0) {
            label_setf(s_nav_host_label, s_txt_nav_host, sizeof(s_txt_nav_host), "%ums",
                       (unsigned)s_rtt_ms);
        } else {
            label_set_str(s_nav_host_label, s_txt_nav_host, sizeof(s_txt_nav_host), "HOST");
        }
        label_color(s_nav_host_label, COL_TEXT);
    } else if (s_conn == UI_CONN_WIFI) {
        object_show(s_nav_host_dot, true);
        lv_obj_set_style_bg_color(s_nav_host_dot, COL_AMBER, 0);
        label_set_str(s_nav_host_label, s_txt_nav_host, sizeof(s_txt_nav_host), "WIFI");
        label_color(s_nav_host_label, COL_MUTED);
    } else {
        object_show(s_nav_host_dot, false);
        label_set_str(s_nav_host_label, s_txt_nav_host, sizeof(s_txt_nav_host), "");
    }

    /* Bottom Bar: RX, Load, SD, State */
    char rx_buf[10];
    fmt_count(rx_buf, sizeof(rx_buf), st->rx_total);
    label_setf(s_bot_rx_label, s_txt_bot_rx, sizeof(s_txt_bot_rx), "RX: %s", rx_buf);

    label_setf(s_bot_load_label, s_txt_bot_load, sizeof(s_txt_bot_load), "FPS: %u",
               (unsigned)st->fps);

    if (sd.recording) {
        label_setf(s_bot_sd_label, s_txt_bot_sd, sizeof(s_txt_bot_sd), "REC %uF",
                   (unsigned)sd.files);
        label_color(s_bot_sd_label, COL_GREEN);
    } else if (sd.mounted) {
        label_set_str(s_bot_sd_label, s_txt_bot_sd, sizeof(s_txt_bot_sd), "SD READY");
        label_color(s_bot_sd_label, COL_MUTED);
    } else {
        label_set_str(s_bot_sd_label, s_txt_bot_sd, sizeof(s_txt_bot_sd), "NO SD");
        label_color(s_bot_sd_label, COL_DIM);
    }

    const bool paused = s_frozen || st->paused;
    if (paused) {
        label_set_str(s_bot_state_label, s_txt_bot_state, sizeof(s_txt_bot_state), "PAUSED");
        lv_obj_set_style_bg_color(s_bot_state_pill, COL_AMBER, 0);
    } else if (strcmp(st->state, "RUNNING") == 0) {
        label_set_str(s_bot_state_label, s_txt_bot_state, sizeof(s_txt_bot_state), "RUN");
        lv_obj_set_style_bg_color(s_bot_state_pill, COL_GREEN, 0);
    } else {
        label_set_str(s_bot_state_label, s_txt_bot_state, sizeof(s_txt_bot_state), "STOP");
        lv_obj_set_style_bg_color(s_bot_state_pill, COL_DIM, 0);
    }
}

/* ---------------------------------------------------------------- timer */

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_conn != s_conn_req) {
        s_conn = s_conn_req;
        s_rtt_ms = s_rtt_req;
    } else if (s_conn == UI_CONN_ONLINE && s_rtt_ms != s_rtt_req) {
        s_rtt_ms = s_rtt_req;
    }
    if (s_remote_req) {
        s_remote_req = false;
        s_remote_tick = lv_tick_get();
        s_remote_on = true;
    }

    can_stats_t st;
    twai_stats_snapshot(&st);

    if (s_total_valid && st.rx_total < s_prev_total) {
        view_reset();
    }
    s_prev_total = st.rx_total;
    s_total_valid = true;

    scan_records();

    if (st.paused) {
        for (int i = 0; i < CNT_SLOTS; i++) {
            s_counts[i].win = 0;
        }
        s_rate_tick = lv_tick_get();
    } else {
        rate_sample();
    }

    render_bars(&st);

    switch (s_current_view) {
    case VIEW_DASH:
        if (!s_frozen && !st.paused) {
            render_dash(&st);
        }
        break;
    case VIEW_TRACE:
        if (!s_frozen && !st.paused) {
            render_trace();
        }
        break;
    case VIEW_DIAG:
        render_diag(&st);
        break;
    case VIEW_SETTINGS:
        render_settings(&st);
        break;
    default:
        break;
    }

    if (s_toast && !lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(s_toast_tick) >= 1400u) {
        object_show(s_toast, false);
    }
}

/* -------------------------------------------------------------- builders */

static void top_nav_create(lv_obj_t *screen)
{
    lv_obj_t *bar = obj_new(screen, 0, 0, UI_W, NAV_H);
    lv_obj_set_style_bg_color(bar, COL_NAV_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, COL_CARD_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);

    /* Left: Bitrate & Mode pill (visible in operational views) */
    s_nav_br_pill = obj_new(bar, 6, 3, 68, 20);
    lv_obj_set_style_bg_color(s_nav_br_pill, COL_ACTIVE_BTN, 0);
    lv_obj_set_style_bg_opa(s_nav_br_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_nav_br_pill, 4, 0);
    lv_obj_add_flag(s_nav_br_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_nav_br_pill, settings_click_cb, LV_EVENT_CLICKED, NULL);

    s_nav_br_label = label_new(s_nav_br_pill, "500K", &lv_font_montserrat_12, COL_CYAN);
    lv_obj_center(s_nav_br_label);

    /* Left: Back button (visible only in SETTINGS view) */
    s_nav_back_btn = obj_new(bar, 6, 3, 68, 20);
    lv_obj_set_style_bg_color(s_nav_back_btn, COL_ACTIVE_BTN, 0);
    lv_obj_set_style_bg_opa(s_nav_back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_nav_back_btn, 4, 0);
    lv_obj_add_flag(s_nav_back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_nav_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_nav_back_btn, LV_OBJ_FLAG_HIDDEN);

    s_nav_back_label = label_new(s_nav_back_btn, LV_SYMBOL_LEFT " Back", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_center(s_nav_back_label);

    /* Center: Segmented Tab Bar [ DASH | TRACE | DIAG ] (X = (320 - 156) / 2 = 82) */
    s_nav_tab_group = obj_new(bar, 82, 2, 156, 22);
    lv_obj_set_style_bg_color(s_nav_tab_group, lv_color_hex(0x1B1E28), 0);
    lv_obj_set_style_bg_opa(s_nav_tab_group, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_nav_tab_group, 6, 0);

    static const char *tab_names[3] = {"DASH", "TRACE", "DIAG"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *tab = obj_new(s_nav_tab_group, i * 52, 0, 52, 22);
        lv_obj_set_style_radius(tab, 5, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tab, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_nav_tab_btn[i] = tab;

        s_nav_tab_label[i] = label_new(tab, tab_names[i], &lv_font_montserrat_12, COL_MUTED);
        lv_obj_center(s_nav_tab_label[i]);
    }
    nav_tab_update();

    /* Center: Settings title (visible only in SETTINGS view) */
    s_nav_settings_title = label_new(bar, "SETTINGS", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_pos(s_nav_settings_title, 82, 5);
    lv_obj_set_width(s_nav_settings_title, 156);
    lv_obj_set_style_text_align(s_nav_settings_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_nav_settings_title, LV_OBJ_FLAG_HIDDEN);

    /* Right: Host Link & SD Status in X: 240..316 */
    /* Host Dot + Label */
    s_nav_host_dot = obj_new(bar, 242, 11, 5, 5);
    lv_obj_set_style_bg_color(s_nav_host_dot, COL_GREEN, 0);
    lv_obj_set_style_bg_opa(s_nav_host_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_nav_host_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(s_nav_host_dot, LV_OBJ_FLAG_HIDDEN);

    s_nav_host_label = label_new(bar, "", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(s_nav_host_label, 250, 5);

    /* SD Dot + Label */
    s_nav_sd_dot = obj_new(bar, 280, 11, 5, 5);
    lv_obj_set_style_bg_color(s_nav_sd_dot, COL_RED, 0);
    lv_obj_set_style_bg_opa(s_nav_sd_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_nav_sd_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(s_nav_sd_dot, LV_OBJ_FLAG_HIDDEN);

    s_nav_sd_label = label_new(bar, "", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(s_nav_sd_label, 288, 5);
}

static void bottom_bar_create(lv_obj_t *screen)
{
    lv_obj_t *bar = obj_new(screen, 0, BOT_Y, UI_W, BOT_H);
    lv_obj_set_style_bg_color(bar, COL_BOT_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, COL_CARD_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);

    s_bot_rx_label = label_new(bar, "RX: 0", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_pos(s_bot_rx_label, 8, 7);

    s_bot_load_label = label_new(bar, "FPS: 0", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(s_bot_load_label, 106, 7);

    s_bot_sd_label = label_new(bar, "SD READY", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(s_bot_sd_label, 184, 7);

    s_bot_state_pill = obj_new(bar, 266, 4, 48, 20);
    lv_obj_set_style_bg_color(s_bot_state_pill, COL_GREEN, 0);
    lv_obj_set_style_bg_opa(s_bot_state_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bot_state_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(s_bot_state_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_bot_state_pill, pause_click_cb, LV_EVENT_CLICKED, NULL);

    s_bot_state_label = label_new(s_bot_state_pill, "RUN", &lv_font_montserrat_12, COL_BG);
    lv_obj_center(s_bot_state_label);
}

static void dash_view_create(lv_obj_t *parent)
{
    lv_obj_t *page = obj_new(parent, 0, CONTENT_Y, UI_W, CONTENT_H);
    s_views[VIEW_DASH] = page;

    /* Metric Banner Card */
    lv_obj_t *banner = card_new(page, 4, 3, 312, 38);

    s_dash_fps_num = label_new(banner, "0", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_pos(s_dash_fps_num, 8, 8);
    lv_obj_t *fps_tag = label_new(banner, "fps", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(fps_tag, 62, 14);

    /* Bus Load Gauge Bar */
    s_dash_load_text = label_new(banner, "LOAD 0%", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_pos(s_dash_load_text, 100, 6);

    s_dash_load_bar = lv_bar_create(banner);
    lv_obj_set_pos(s_dash_load_bar, 100, 22);
    lv_obj_set_size(s_dash_load_bar, 104, 7);
    lv_obj_set_style_radius(s_dash_load_bar, 3, 0);
    lv_obj_set_style_bg_color(s_dash_load_bar, lv_color_hex(0x1F2432), 0);
    lv_obj_set_style_bg_opa(s_dash_load_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dash_load_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_dash_load_bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_dash_load_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(s_dash_load_bar, 0, 100);
    lv_bar_set_value(s_dash_load_bar, 0, LV_ANIM_OFF);

    /* Adaptive Health Badge */
    s_dash_health_pill = obj_new(banner, 218, 7, 86, 24);
    lv_obj_set_style_bg_color(s_dash_health_pill, lv_color_hex(0x064E3B), 0);
    lv_obj_set_style_bg_opa(s_dash_health_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dash_health_pill, LV_RADIUS_CIRCLE, 0);

    s_dash_health_label = label_new(s_dash_health_pill, LV_SYMBOL_OK " NORMAL",
                                    &lv_font_montserrat_12, COL_GREEN);
    lv_obj_center(s_dash_health_label);

    /* Table Header */
    lv_obj_t *hdr = label_new(page, "ID", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 8, 44);
    hdr = label_new(page, "RATE/s", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 82, 44);
    lv_obj_set_width(hdr, 46);
    lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_RIGHT, 0);
    hdr = label_new(page, "PER", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 130, 44);
    lv_obj_set_width(hdr, 38);
    lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_RIGHT, 0);
    hdr = label_new(page, "COUNT", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 170, 44);
    lv_obj_set_width(hdr, 50);
    lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_RIGHT, 0);
    hdr = label_new(page, "SHARE", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 226, 44);
    lv_obj_set_width(hdr, 82);
    lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_RIGHT, 0);

    /* 5 Top-ID Rows (Card Rows, Clickable) */
    for (int i = 0; i < TOP_N; i++) {
        lv_obj_t *row = obj_new(page, 4, 60 + i * 24, 312, 23);
        lv_obj_set_style_bg_color(row, (i & 1) ? COL_ROW_ODD : COL_ROW_EVEN, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, dash_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_dash_row[i] = row;

        s_dash_id[i] = label_new(row, "", &lv_font_montserrat_16, COL_CYAN);
        lv_obj_set_pos(s_dash_id[i], 6, 2);
        lv_obj_set_width(s_dash_id[i], 74);

        s_dash_rate[i] = label_new(row, "", &lv_font_montserrat_14, COL_TEXT);
        lv_obj_set_pos(s_dash_rate[i], 82, 3);
        lv_obj_set_width(s_dash_rate[i], 46);
        lv_obj_set_style_text_align(s_dash_rate[i], LV_TEXT_ALIGN_RIGHT, 0);

        s_dash_per[i] = label_new(row, "", &lv_font_montserrat_12, COL_MUTED);
        lv_obj_set_pos(s_dash_per[i], 130, 4);
        lv_obj_set_width(s_dash_per[i], 38);
        lv_obj_set_style_text_align(s_dash_per[i], LV_TEXT_ALIGN_RIGHT, 0);

        s_dash_cnt[i] = label_new(row, "", &lv_font_montserrat_12, COL_MUTED);
        lv_obj_set_pos(s_dash_cnt[i], 170, 4);
        lv_obj_set_width(s_dash_cnt[i], 50);
        lv_obj_set_style_text_align(s_dash_cnt[i], LV_TEXT_ALIGN_RIGHT, 0);

        s_dash_bar[i] = obj_new(row, 226, 9, 40, 6);
        lv_obj_set_style_bg_color(s_dash_bar[i], COL_CYAN, 0);
        lv_obj_set_style_bg_opa(s_dash_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_dash_bar[i], 3, 0);
        object_show(s_dash_bar[i], false);

        s_dash_share[i] = label_new(row, "", &lv_font_montserrat_12, COL_MUTED);
        lv_obj_set_pos(s_dash_share[i], 270, 4);
        lv_obj_set_width(s_dash_share[i], 38);
        lv_obj_set_style_text_align(s_dash_share[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    s_dash_empty = obj_new(page, 4, 60, 312, 120);
    lv_obj_t *empty_label = label_new(s_dash_empty, "Waiting for CAN frames...\nCheck wiring & bitrate",
                                      &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(empty_label, 280);
    lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(empty_label);
}

static void trace_view_create(lv_obj_t *parent)
{
    lv_obj_t *page = obj_new(parent, 0, CONTENT_Y, UI_W, CONTENT_H);
    s_views[VIEW_TRACE] = page;

    /* Table Header */
    lv_obj_t *hdr = label_new(page, "dt (ms)", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 6, 4);
    hdr = label_new(page, "ID", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 60, 4);
    hdr = label_new(page, "L", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 134, 4);
    hdr = label_new(page, "DATA (HEX)", &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(hdr, 150, 4);

    /* 7 Rolling Frame Rows */
    for (int i = 0; i < ROW_N; i++) {
        lv_obj_t *row = obj_new(page, 4, 21 + i * 23, 312, 22);
        lv_obj_set_style_bg_color(row, (i & 1) ? COL_ROW_ODD : COL_ROW_EVEN, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 3, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, trace_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_trace_row[i] = row;

        s_trace_dt[i] = label_new(row, "", &lv_font_montserrat_12, COL_MUTED);
        lv_obj_set_pos(s_trace_dt[i], 4, 3);
        lv_obj_set_width(s_trace_dt[i], 50);

        s_trace_id[i] = label_new(row, "", &lv_font_montserrat_12, COL_CYAN);
        lv_obj_set_pos(s_trace_id[i], 56, 3);
        lv_obj_set_width(s_trace_id[i], 72);

        s_trace_dlc[i] = label_new(row, "", &lv_font_montserrat_12, COL_DIM);
        lv_obj_set_pos(s_trace_dlc[i], 130, 3);

        s_trace_data[i] = label_new(row, "", &lv_font_montserrat_12, COL_TEXT);
        lv_obj_set_pos(s_trace_data[i], 146, 3);
        lv_obj_set_width(s_trace_data[i], 160);
    }

    s_trace_empty = obj_new(page, 4, 21, 312, 160);
    lv_obj_t *empty_label = label_new(s_trace_empty, "Waiting for CAN traffic...\nFrames will stream here",
                                      &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(empty_label, 280);
    lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(empty_label);
}

static void diag_view_create(lv_obj_t *parent)
{
    lv_obj_t *page = obj_new(parent, 0, CONTENT_Y, UI_W, CONTENT_H);
    s_views[VIEW_DIAG] = page;

    static const char *card_titles[6] = {
        "TEC / REC", "BUS ERRORS", "BUS OFF",
        "FIFO MISS", "QUEUE FULL", "BLACKBOX"
    };
    static const char *card_subtitles[6] = {
        "Tx/Rx Err Count", "Protocol Alerts", "Controller Off",
        "HW Overrun", "SW Ring Overrun", "SD Log Status"
    };

    /* 2x3 Grid of Diagnostic Cards */
    for (int i = 0; i < 6; i++) {
        const lv_coord_t x = 6 + (i % 3) * 104;
        const lv_coord_t y = 6 + (i / 3) * 66;
        lv_obj_t *card = card_new(page, x, y, 100, 60);

        lv_obj_t *title = label_new(card, card_titles[i], &lv_font_montserrat_12, COL_MUTED);
        lv_obj_set_pos(title, 6, 4);

        s_diag_val[i] = label_new(card, "0", &lv_font_montserrat_16, COL_GREEN);
        lv_obj_set_pos(s_diag_val[i], 6, 20);

        lv_obj_t *sub = label_new(card, card_subtitles[i], &lv_font_montserrat_12, COL_DIM);
        lv_obj_set_pos(sub, 6, 40);
        lv_obj_set_width(sub, 90);
    }

    /* Actions: Bus Recover & Clear */
    lv_obj_t *rec_btn = obj_new(page, 6, 142, 150, 36);
    lv_obj_set_style_bg_color(rec_btn, COL_ACTIVE_BTN, 0);
    lv_obj_set_style_bg_opa(rec_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rec_btn, 6, 0);
    lv_obj_add_flag(rec_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rec_btn, recover_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = label_new(rec_btn, LV_SYMBOL_REFRESH " Bus Recover",
                              &lv_font_montserrat_12, COL_TEXT);
    lv_obj_center(lbl);

    lv_obj_t *clr_btn = obj_new(page, 164, 142, 150, 36);
    lv_obj_set_style_bg_color(clr_btn, COL_ACTIVE_BTN, 0);
    lv_obj_set_style_bg_opa(clr_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(clr_btn, 6, 0);
    lv_obj_add_flag(clr_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clr_btn, clear_click_cb, LV_EVENT_CLICKED, NULL);
    lbl = label_new(clr_btn, LV_SYMBOL_TRASH " Clear Stats",
                    &lv_font_montserrat_12, COL_TEXT);
    lv_obj_center(lbl);
}

static void settings_view_create(lv_obj_t *parent)
{
    lv_obj_t *page = obj_new(parent, 0, CONTENT_Y, UI_W, CONTENT_H);
    s_views[VIEW_SETTINGS] = page;

    /* Card 1: CAN Bitrate & Mode */
    lv_obj_t *card1 = card_new(page, 4, 6, 312, 46);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *pill = obj_new(card1, 8 + i * 46, 8, 40, 30);
        lv_obj_set_style_bg_color(pill, COL_ACTIVE_BTN, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, 4, 0);
        lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pill, bitrate_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_br_pill[i] = pill;

        lv_obj_t *l = label_new(pill, BITRATE_LABELS[i], &lv_font_montserrat_12, COL_TEXT);
        lv_obj_center(l);
    }

    s_mode_pill = obj_new(card1, 196, 8, 108, 30);
    lv_obj_set_style_bg_color(s_mode_pill, COL_GREEN, 0);
    lv_obj_set_style_bg_opa(s_mode_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mode_pill, 4, 0);
    lv_obj_add_flag(s_mode_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mode_pill, mode_click_cb, LV_EVENT_CLICKED, NULL);

    s_mode_label = label_new(s_mode_pill, "Listen-Only", &lv_font_montserrat_12, COL_BG);
    lv_obj_center(s_mode_label);

    /* Card 2: Backlight & Actions */
    lv_obj_t *card2 = card_new(page, 4, 58, 312, 48);
    lv_obj_t *bl_tag = label_new(card2, "BL:", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(bl_tag, 6, 16);

    static const char *bl_labels[4] = {"20%", "50%", "80%", "100%"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *pill = obj_new(card2, 32 + i * 36, 9, 32, 28);
        lv_obj_set_style_bg_color(pill, i == 2 ? COL_GREEN : COL_ACTIVE_BTN, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, 4, 0);
        lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pill, brightness_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_bl_pill[i] = pill;

        lv_obj_t *l = label_new(pill, bl_labels[i], &lv_font_montserrat_12, COL_TEXT);
        lv_obj_center(l);
    }

    s_set_pause_btn = obj_new(card2, 180, 9, 60, 28);
    lv_obj_set_style_bg_color(s_set_pause_btn, COL_ACTIVE_BTN, 0);
    lv_obj_set_style_bg_opa(s_set_pause_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_set_pause_btn, 4, 0);
    lv_obj_add_flag(s_set_pause_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_set_pause_btn, pause_click_cb, LV_EVENT_CLICKED, NULL);

    s_set_pause_label = label_new(s_set_pause_btn, LV_SYMBOL_PAUSE " Pause",
                                  &lv_font_montserrat_12, COL_TEXT);
    lv_obj_center(s_set_pause_label);

    lv_obj_t *mark_btn = obj_new(card2, 246, 9, 58, 28);
    lv_obj_set_style_bg_color(mark_btn, COL_ACTIVE_BTN, 0);
    lv_obj_set_style_bg_opa(mark_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mark_btn, 4, 0);
    lv_obj_add_flag(mark_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mark_btn, mark_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mark_lbl = label_new(mark_btn, LV_SYMBOL_SAVE " Mark",
                                   &lv_font_montserrat_12, COL_TEXT);
    lv_obj_center(mark_lbl);

    /* Card 3: Device Info */
    lv_obj_t *card3 = card_new(page, 4, 112, 312, 54);
    lv_obj_t *info1 = label_new(card3, "LinSight CAN v" LINSIGHT_FW_VERSION " | ESP32-S3 (8MB PSRAM)",
                                &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_pos(info1, 8, 8);

    char dev_info[48];
    snprintf(dev_info, sizeof(dev_info), "ID: %s | Reset: %s", linsight_device_id(),
             linsight_reset_reason());
    lv_obj_t *info2 = label_new(card3, dev_info, &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(info2, 8, 28);
}

static void modal_inspector_create(void)
{
    /* Fullscreen dark semi-transparent backdrop */
    s_modal_mask = obj_new(lv_layer_top(), 0, 0, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_modal_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal_mask, LV_OPA_70, 0);
    lv_obj_add_flag(s_modal_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal_mask, modal_close_click_cb, LV_EVENT_CLICKED, NULL);

    /* Elevated Floating Card */
    s_modal_card = obj_new(s_modal_mask, 16, 16, 288, 208);
    lv_obj_set_style_bg_color(s_modal_card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(s_modal_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_modal_card, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_modal_card, 1, 0);
    lv_obj_set_style_radius(s_modal_card, 8, 0);
    lv_obj_add_flag(s_modal_card, LV_OBJ_FLAG_CLICKABLE);

    /* Header: ID + Close button */
    s_modal_id_label = label_new(s_modal_card, "CAN FRAME DETAIL",
                                 &lv_font_montserrat_16, COL_CYAN);
    lv_obj_set_pos(s_modal_id_label, 12, 10);

    lv_obj_t *close_btn = obj_new(s_modal_card, 252, 6, 28, 24);
    lv_obj_set_style_bg_color(close_btn, COL_ACTIVE_BTN, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(close_btn, 4, 0);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, modal_close_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_lbl = label_new(close_btn, LV_SYMBOL_CLOSE,
                                    &lv_font_montserrat_12, COL_TEXT);
    lv_obj_center(close_lbl);

    s_modal_sub_label = label_new(s_modal_card, "", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(s_modal_sub_label, 12, 34);

    lv_obj_t *div = obj_new(s_modal_card, 12, 54, 264, 1);
    lv_obj_set_style_bg_color(div, COL_CARD_BORDER, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    lv_obj_t *bytes_title = label_new(s_modal_card, "DATA BYTES (HEX):",
                                      &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_pos(bytes_title, 12, 62);

    /* 8 Byte Boxes */
    for (int i = 0; i < 8; i++) {
        lv_obj_t *box = obj_new(s_modal_card, 12 + i * 33, 80, 31, 38);
        lv_obj_set_style_bg_color(box, COL_ROW_EVEN, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, COL_CARD_BORDER, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_radius(box, 4, 0);
        s_modal_byte_box[i] = box;

        s_modal_byte_val[i] = label_new(box, "--", &lv_font_montserrat_14, COL_TEXT);
        lv_obj_align(s_modal_byte_val[i], LV_ALIGN_TOP_MID, 0, 4);

        char d_str[4];
        snprintf(d_str, sizeof(d_str), "D%d", i);
        lv_obj_t *d_lbl = label_new(box, d_str, &lv_font_montserrat_12, COL_DIM);
        lv_obj_align(d_lbl, LV_ALIGN_BOTTOM_MID, 0, -3);
    }

    s_modal_ascii_label = label_new(s_modal_card, "ASCII: \"........\"",
                                    &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_pos(s_modal_ascii_label, 12, 130);

    lv_obj_t *hint = label_new(s_modal_card, "[ Tap anywhere or X to close ]",
                               &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    object_show(s_modal_mask, false);
}

/* ---------------------------------------------------------------- splash */

#define SPLASH_CONTENT_W 240
#define SPLASH_BAR_W 40
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
    lv_obj_t *splash = obj_new(lv_layer_top(), 0, 0, UI_W, UI_H);
    lv_obj_set_style_bg_color(splash, COL_BG, 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_add_flag(splash, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *content = obj_new(splash, (UI_W - SPLASH_CONTENT_W) / 2, 84,
                                SPLASH_CONTENT_W, 88);

    lv_obj_t *brand = label_new(content, "LinSight", &lv_font_montserrat_24, COL_TEXT);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *bar = obj_new(content, SPLASH_CONTENT_W / 2, 34, 0, 3);
    lv_obj_set_style_bg_color(bar, COL_CYAN, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);

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

    /* Screen-level toast */
    s_toast = obj_new(screen, 40, 214, 240, 22);
    lv_obj_set_style_bg_color(s_toast, COL_CARD, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_toast, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);

    lv_obj_t *toast_lbl = label_new(s_toast, "", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_width(toast_lbl, 230);
    lv_obj_set_style_text_align(toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(toast_lbl);
    object_show(s_toast, false);

    lv_timer_create(tick_cb, 50, NULL); /* 20 Hz tick */
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
    if (long_press) {
        /* Long press: toggle Settings view */
        if (s_current_view == VIEW_SETTINGS) {
            view_show(s_prev_view);
        } else {
            view_show(VIEW_SETTINGS);
        }
    } else {
        /* Short press: cycle operational views DASH -> TRACE -> DIAG -> DASH */
        if (s_current_view == VIEW_SETTINGS) {
            view_show(s_prev_view);
        } else {
            view_id_t next = (s_current_view + 1) % 3;
            view_show(next);
        }
    }
    lvgl_port_unlock();
}

void linsight_ui_sync_control(void)
{
    s_remote_req = true;
}
