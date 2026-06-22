/**
 * @file quaddle_ble_hid_central.c
 * @brief BLE HID central for BM769/Q34B gamepads.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_cli.h"
#include "tal_event.h"
#include "tal_kv.h"
#include "netconn_wifi.h"
#include "netmgr.h"
#include "tuya_iot.h"
#include "tkl_bluetooth.h"
#include "tkl_gpio.h"

#include "quaddle_ble_hid_central.h"
#include "quaddle_robot_bridge.h"

#if defined(ENABLE_QUADDLE_BLE_HID_CENTRAL) && ENABLE_QUADDLE_BLE_HID_CENTRAL

#ifndef QUADDLE_BLE_GAMEPAD_NAME_FILTER
#define QUADDLE_BLE_GAMEPAD_NAME_FILTER "bm769,q34b,gamepadspace"
#endif

#ifndef QUADDLE_BLE_HID_VERBOSE_LOG
#define QUADDLE_BLE_HID_VERBOSE_LOG 0
#endif

#if defined(QUADDLE_BLE_HID_VERBOSE_LOG) && (QUADDLE_BLE_HID_VERBOSE_LOG == 1)
#define QHID_LOG_DETAIL(...) PR_DEBUG(__VA_ARGS__)
#define QHID_LOG_NOTICE_DETAIL(...) PR_NOTICE(__VA_ARGS__)
#else
#define QHID_LOG_DETAIL(...) do { } while (0)
#define QHID_LOG_NOTICE_DETAIL(...) do { } while (0)
#endif

#define HID_SERVICE_UUID              0x1812
#define HID_PROTOCOL_MODE_CHAR_UUID   0x2A4E
#define HID_REPORT_CHAR_UUID          0x2A4D
#define HID_CONTROL_POINT_CHAR_UUID   0x2A4C
#define VENDOR_INPUT_CHAR_UUID        0x0003
#define QUADDLE_BLE_INVALID_HANDLE    0xFFFF
#define QUADDLE_BLE_CANCEL_NOT_ACTIVE 2 /* NimBLE BLE_HS_EALREADY */
#define QUADDLE_BLE_SCAN_INTERVAL     0x0040
#define QUADDLE_BLE_SCAN_WINDOW       0x0040
#define QUADDLE_BLE_SAVED_SCAN_INTERVAL 0x0100
#define QUADDLE_BLE_SAVED_SCAN_WINDOW 0x0020
#define QUADDLE_BLE_RESCAN_MS         1000
#define QUADDLE_BLE_CANCEL_SETTLE_MS  1500
#define QUADDLE_BLE_CONNECT_TIMEOUT_MS 9000
#define QUADDLE_BLE_SECURITY_WAIT_MS   650
#define QUADDLE_BLE_SUBSCRIBE_NEXT_MS 120
#define QUADDLE_BLE_HID_WAKE_MS       180
#define QUADDLE_BLE_POLL_READ_MS      100
#define QUADDLE_BLE_BOOT_POLL_MS      50
#define QUADDLE_BLE_BOOT_SHORT_MIN_MS 80
#define QUADDLE_BLE_BOOT_HOLD_MS      2000
#define QUADDLE_BLE_BOOT_NETCFG_DELAY_MS 500
#define QUADDLE_BLE_WIFI_RESUME_SCAN_DELAY_MS 4000
#define QUADDLE_BLE_CHAT_IDLE_SCAN_DELAY_MS 1000
#define QUADDLE_BLE_WIFI_BUSY_MAX_MS  45000
#define QUADDLE_BLE_CONN_NORMAL_MIN   0x0018
#define QUADDLE_BLE_CONN_NORMAL_MAX   0x0030
#define QUADDLE_BLE_CONN_BUSY_MIN     0x0080
#define QUADDLE_BLE_CONN_BUSY_MAX     0x00A0
#define QUADDLE_BLE_CONN_TIMEOUT      0x0320
#define QUADDLE_BLE_MAX_SERVICES      8
#define QUADDLE_BLE_MAX_INPUT_CHARS   16
#define QUADDLE_BLE_STICK_THRESH      3
#define QUADDLE_BLE_KV_PEER_ADDR      "qgp_addr"
#define QUADDLE_BLE_KV_PEER_NAME      "qgp_name"
#define QUADDLE_BLE_EVENT_WIFI_NETCFG "netcfg.wifi"
#define QUADDLE_BLE_BOOT_BUTTON_PIN   TUYA_GPIO_NUM_0

typedef struct {
    uint8_t valid;
    uint8_t type;
    uint8_t addr[6];
} SAVED_PEER_ADDR_T;

typedef enum {
    GP_PROFILE_UNKNOWN = 0,
    GP_PROFILE_BM769,
    GP_PROFILE_Q34B,
} GAMEPAD_PROFILE_E;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    uint16_t uuid16;
} SVC_RANGE_T;

typedef struct {
    uint16_t handle;
    uint16_t end_handle;
    uint16_t uuid16;
    uint8_t property;
} INPUT_CHAR_T;

typedef struct {
    bool connected;
    bool connecting;
    bool connect_cancel_pending;
    bool scanning;
    bool initialized;
    uint16_t conn_handle;
    TKL_BLE_GAP_ADDR_T peer_addr;
    char peer_name[32];
    GAMEPAD_PROFILE_E profile;
    bool saved_peer_valid;
    TKL_BLE_GAP_ADDR_T saved_peer_addr;
    char saved_peer_name[32];

    SVC_RANGE_T services[QUADDLE_BLE_MAX_SERVICES];
    uint8_t service_count;
    uint8_t service_index;
    uint16_t hid_protocol_mode_handle;
    uint16_t hid_control_point_handle;

    INPUT_CHAR_T input_chars[QUADDLE_BLE_MAX_INPUT_CHARS];
    uint8_t input_char_count;
    uint8_t input_char_index;

    uint8_t prev_lx;
    uint8_t prev_ly;
    uint8_t prev_rx;
    uint8_t prev_ry;
    uint16_t prev_buttons;
    uint8_t prev_hat;
    bool prev_valid;
    uint8_t prev_q34b_f4;
    uint32_t prev_q34b_btn_word;
    bool prev_q34b_raw_valid;
    TIMER_ID rescan_timer;
    TIMER_ID connect_timer;
    TIMER_ID security_timer;
    TIMER_ID subscribe_timer;
    TIMER_ID hid_wake_timer;
    TIMER_ID poll_timer;
    TIMER_ID boot_timer;
    TIMER_ID wifi_resume_timer;
    TIMER_ID boot_netcfg_timer;
    uint8_t poll_read_index;
    bool poll_read_pending;
    uint16_t poll_read_handle;
    uint8_t poll_snap[QUADDLE_BLE_MAX_INPUT_CHARS][64];
    uint8_t poll_snap_len[QUADDLE_BLE_MAX_INPUT_CHARS];
    uint8_t report_trace_count;
    uint16_t scan_report_count;
    uint8_t scan_report_log_count;
    bool boot_down;
    bool boot_fired_this_hold;
    bool wifi_busy;
    bool chat_busy;
    uint8_t conn_param_mode;
    uint32_t boot_press_start_ms;
} QUADDLE_BLE_HID_CTX_T;

static QUADDLE_BLE_HID_CTX_T s_hid;

static void schedule_rescan(void);
static void schedule_rescan_delay(uint32_t delay_ms);
static void set_wifi_busy_internal(bool busy);

static bool ble_activity_blocked(void)
{
    return s_hid.wifi_busy || s_hid.chat_busy;
}

static uint16_t uuid_to_u16(const TKL_BLE_UUID_T *uuid)
{
    if (!uuid) {
        return 0xFFFF;
    }
    if (uuid->uuid_type == TKL_BLE_UUID_TYPE_16) {
        return uuid->uuid.uuid16;
    }
    if (uuid->uuid_type == TKL_BLE_UUID_TYPE_128) {
        return ((uint16_t)uuid->uuid.uuid128[13] << 8) | uuid->uuid.uuid128[12];
    }
    return 0xFFFF;
}

static bool uuid128_has_vendor_input(const TKL_BLE_UUID_T *uuid)
{
    if (!uuid || uuid->uuid_type != TKL_BLE_UUID_TYPE_128) {
        return false;
    }
    for (uint8_t i = 0; i + 1 < sizeof(uuid->uuid.uuid128); i++) {
        if (uuid->uuid.uuid128[i] == 0x03 && uuid->uuid.uuid128[i + 1] == 0x00) {
            return true;
        }
    }
    return false;
}

static int stick_x(uint8_t raw)
{
    return (int)raw - 128;
}

static int stick_y(uint8_t raw)
{
    return 128 - (int)raw;
}

static const char *hat_name(uint8_t hat)
{
    switch (hat & 0x0F) {
    case 0x01:
        return "U";
    case 0x02:
        return "D";
    case 0x04:
        return "L1";
    case 0x08:
        return "R1";
    case 0x09:
        return "UR";
    case 0x0A:
        return "DR";
    case 0x06:
        return "DL";
    case 0x05:
        return "UL";
    default:
        return "C";
    }
}

static int abs_i(int v)
{
    return v < 0 ? -v : v;
}

static void format_addr(const TKL_BLE_GAP_ADDR_T *addr, char *out, size_t out_len)
{
    if (!addr || !out || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x/%s", addr->addr[5], addr->addr[4], addr->addr[3],
             addr->addr[2], addr->addr[1], addr->addr[0],
             addr->type == TKL_BLE_GAP_ADDR_TYPE_RANDOM ? "random" : "public");
    out[out_len - 1] = '\0';
}

static bool addr_equal(const TKL_BLE_GAP_ADDR_T *a, const TKL_BLE_GAP_ADDR_T *b)
{
    return a && b && a->type == b->type && memcmp(a->addr, b->addr, sizeof(a->addr)) == 0;
}

static bool saved_addr_matches(const TKL_BLE_GAP_ADDR_T *addr)
{
    if (!s_hid.saved_peer_valid || !addr) {
        return false;
    }
    return addr_equal(addr, &s_hid.saved_peer_addr);
}

static void log_saved_peer(const char *prefix, const TKL_BLE_GAP_ADDR_T *addr, const char *name)
{
    char addr_text[32] = {0};

    format_addr(addr, addr_text, sizeof(addr_text));
    PR_NOTICE("quaddle ble hid: %s saved peer addr=%s name=%s", prefix, addr_text,
              (name && name[0]) ? name : "(unknown)");
}

static void load_saved_peer(void)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    SAVED_PEER_ADDR_T saved = {0};

    s_hid.saved_peer_valid = false;
    memset(&s_hid.saved_peer_addr, 0, sizeof(s_hid.saved_peer_addr));
    memset(s_hid.saved_peer_name, 0, sizeof(s_hid.saved_peer_name));

    if (tal_kv_get(QUADDLE_BLE_KV_PEER_ADDR, &buf, &len) != OPRT_OK || !buf) {
        return;
    }
    if (len == sizeof(saved)) {
        memcpy(&saved, buf, sizeof(saved));
        if (saved.valid) {
            s_hid.saved_peer_addr.type = saved.type;
            memcpy(s_hid.saved_peer_addr.addr, saved.addr, sizeof(s_hid.saved_peer_addr.addr));
            s_hid.saved_peer_valid = true;
        }
    }
    tal_kv_free(buf);

    buf = NULL;
    len = 0;
    if (tal_kv_get(QUADDLE_BLE_KV_PEER_NAME, &buf, &len) == OPRT_OK && buf && len > 0) {
        size_t copy_len = len;
        if (copy_len >= sizeof(s_hid.saved_peer_name)) {
            copy_len = sizeof(s_hid.saved_peer_name) - 1;
        }
        memcpy(s_hid.saved_peer_name, buf, copy_len);
        s_hid.saved_peer_name[copy_len] = '\0';
        tal_kv_free(buf);
    }

    if (s_hid.saved_peer_valid) {
        log_saved_peer("loaded", &s_hid.saved_peer_addr, s_hid.saved_peer_name);
    }
}

static void save_current_peer(void)
{
    SAVED_PEER_ADDR_T saved = {0};

    if (s_hid.saved_peer_valid && addr_equal(&s_hid.saved_peer_addr, &s_hid.peer_addr)) {
        return;
    }

    saved.valid = 1;
    saved.type = s_hid.peer_addr.type;
    memcpy(saved.addr, s_hid.peer_addr.addr, sizeof(saved.addr));
    if (tal_kv_set(QUADDLE_BLE_KV_PEER_ADDR, (const uint8_t *)&saved, sizeof(saved)) != OPRT_OK) {
        PR_WARN("quaddle ble hid: save peer addr failed");
        return;
    }
    if (s_hid.peer_name[0] != '\0') {
        (void)tal_kv_set(QUADDLE_BLE_KV_PEER_NAME, (const uint8_t *)s_hid.peer_name, strlen(s_hid.peer_name) + 1);
    }

    s_hid.saved_peer_valid = true;
    s_hid.saved_peer_addr = s_hid.peer_addr;
    memset(s_hid.saved_peer_name, 0, sizeof(s_hid.saved_peer_name));
    strncpy(s_hid.saved_peer_name, s_hid.peer_name, sizeof(s_hid.saved_peer_name) - 1);
    log_saved_peer("stored", &s_hid.saved_peer_addr, s_hid.saved_peer_name);
}

static bool peer_allowed_by_saved_filter(const TKL_BLE_GAP_ADDR_T *addr)
{
    char addr_text[32] = {0};

    if (!s_hid.saved_peer_valid) {
        return true;
    }
    if (saved_addr_matches(addr)) {
        return true;
    }
    format_addr(addr, addr_text, sizeof(addr_text));
    QHID_LOG_DETAIL("quaddle ble hid: ignore non-saved gamepad addr=%s", addr_text);
    return false;
}

static void emit_line(const char *line)
{
    if (!line || line[0] == '\0') {
        return;
    }
    QHID_LOG_DETAIL("quaddle ble hid: %s", line);
    (void)quaddle_robot_bridge_handle_line(line);
}

static void emitf(const char *fmt, ...)
{
    char line[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    emit_line(line);
}

static uint16_t bm769_button_mask(const uint8_t *d, uint16_t len)
{
    uint16_t f = 0;

    if (!d || len < 12) {
        return 0;
    }
    if (d[8] & (1U << 0)) {
        f |= 1u << 0;
    }
    if (d[8] & (1U << 1)) {
        f |= 1u << 1;
    }
    if (d[7] & (1U << 4)) {
        f |= 1u << 2;
    }
    if (d[7] & (1U << 5)) {
        f |= 1u << 3;
    }
    if (d[7] & (1U << 6)) {
        f |= 1u << 4;
    }
    if (d[7] & (1U << 7)) {
        f |= 1u << 5;
    }
    {
        const bool zl_down = len > 10 && d[10] != 0;
        if ((d[8] & (1U << 4)) != 0 && !zl_down) {
            f |= 1u << 6;
        }
    }
    if (d[8] & (1U << 5)) {
        f |= 1u << 7;
    }
    if (d[8] & (1U << 6)) {
        f |= 1u << 8;
    }
    if (len > 10 && d[10] != 0) {
        f |= 1u << 9;
    }
    if (len > 11 && d[11] != 0) {
        f |= 1u << 10;
    }
    return f;
}

static const uint8_t *q34b_frame_ptr(const uint8_t *d, uint16_t len)
{
    if (!d) {
        return NULL;
    }
    for (uint16_t off = 0; off <= 3; off++) {
        if (len < off + 10) {
            break;
        }
        if (d[off + 9] == 0x8F) {
            return d + off;
        }
    }
    return NULL;
}

static bool looks_like_bm769(const uint8_t *d, uint16_t len)
{
    return d && len >= 13 && d[0] == 0x20 && d[1] == 0x0F && d[2] == 0x00;
}

static uint8_t q34b_hat(uint8_t f4)
{
    switch (f4) {
    case 0xFF:
        return 0x00;
    case 0x00:
        return 0x01;
    case 0x01:
        return 0x09;
    case 0x02:
        return 0x08;
    case 0x03:
        return 0x0A;
    case 0x04:
        return 0x02;
    case 0x05:
        return 0x06;
    case 0x06:
        return 0x04;
    case 0x07:
        return 0x05;
    default:
        return 0x00;
    }
}

static uint16_t q34b_button_mask(uint32_t w)
{
    uint16_t f = 0;
    const bool lt_down = (uint8_t)((w >> 24) & 0xFFu) >= 24u;

    if (w & (1u << 0)) {
        f |= 1u << 3;
    }
    if (w & (1u << 1)) {
        f |= 1u << 2;
    }
    if (w & (1u << 3)) {
        f |= 1u << 5;
    }
    if (w & (1u << 4)) {
        f |= 1u << 4;
    }
    if (w & (1u << 7)) {
        f |= 1u << 1;
    }
    if (w & (1u << 6)) {
        f |= 1u << 0;
    }
    if ((w & (1u << 8)) != 0 && !lt_down) {
        f |= 1u << 6;
    }
    if (w & (1u << 9)) {
        f |= 1u << 7;
    }
    if ((w & (1u << 10)) != 0 && !lt_down) {
        f |= 1u << 6;
    }
    if (w & (1u << 11)) {
        f |= 1u << 7;
    }
    if (lt_down) {
        f |= 1u << 9;
    }
    if ((uint8_t)((w >> 16) & 0xFFu) >= 24u) {
        f |= 1u << 10;
    }
    return f;
}

static void emit_dpad_change(uint8_t now, uint8_t prev, const char *tag)
{
    if (now == prev) {
        return;
    }
    if (now == 0x00 && prev != 0x00) {
        emitf("%s DPAD_%s-", tag, hat_name(prev));
    } else if (now != 0x00 && prev == 0x00) {
        emitf("%s DPAD_%s+", tag, hat_name(now));
    } else {
        emitf("%s DPAD_%s-", tag, hat_name(prev));
        emitf("%s DPAD_%s+", tag, hat_name(now));
    }
}

static void feed_canonical(const char *tag, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint16_t buttons,
                           uint8_t hat)
{
    static const struct {
        uint16_t mask;
        const char *name;
    } btn_map[] = {
        {1u << 0, "L1"},   {1u << 1, "R1"},   {1u << 2, "B"},    {1u << 3, "A"},
        {1u << 4, "Y"},    {1u << 5, "X"},    {1u << 6, "MINUS"}, {1u << 7, "PLUS"},
        {1u << 8, "HOME"}, {1u << 9, "ZL"}, {1u << 10, "R2"},
    };

    if (!s_hid.prev_valid) {
        s_hid.prev_lx = lx;
        s_hid.prev_ly = ly;
        s_hid.prev_rx = rx;
        s_hid.prev_ry = ry;
        s_hid.prev_buttons = buttons;
        s_hid.prev_hat = hat;
        s_hid.prev_valid = true;
        return;
    }

    uint16_t changed = s_hid.prev_buttons ^ buttons;
    for (uint8_t i = 0; i < sizeof(btn_map) / sizeof(btn_map[0]); i++) {
        if (changed & btn_map[i].mask) {
            emitf("%s %s%s", tag, btn_map[i].name, (buttons & btn_map[i].mask) ? "+" : "-");
        }
    }

    if (abs_i((int)lx - (int)s_hid.prev_lx) >= QUADDLE_BLE_STICK_THRESH ||
        abs_i((int)ly - (int)s_hid.prev_ly) >= QUADDLE_BLE_STICK_THRESH) {
        emitf("%s LSTICK %d,%d", tag, stick_x(lx), stick_y(ly));
    }
    if (rx != s_hid.prev_rx || ry != s_hid.prev_ry) {
        int head = stick_x(rx);
        if (head > 100) {
            head = 100;
        } else if (head < -100) {
            head = -100;
        }
        emitf("%s RSTICK %d,%d", tag, head, stick_y(ry));
    }
    emit_dpad_change(hat, s_hid.prev_hat, tag);

    s_hid.prev_lx = lx;
    s_hid.prev_ly = ly;
    s_hid.prev_rx = rx;
    s_hid.prev_ry = ry;
    s_hid.prev_buttons = buttons;
    s_hid.prev_hat = hat;
}

static void feed_report_q34b(const uint8_t *f)
{
    const uint32_t btn_word = (uint32_t)f[5] | ((uint32_t)f[6] << 8) | ((uint32_t)f[7] << 16) |
                              ((uint32_t)f[8] << 24);
    const uint16_t btn_mask = q34b_button_mask(btn_word);
    const uint8_t  hat_nibble = q34b_hat(f[4]);

    if (s_hid.prev_q34b_raw_valid) {
        const uint32_t w_ch = btn_word ^ s_hid.prev_q34b_btn_word;
        if (f[4] == 0xFF && s_hid.prev_q34b_f4 == 0xFF) {
            const uint32_t k_ot_mask = (1u << 2) | (1u << 5);
            const uint32_t ot_ch = w_ch & k_ot_mask;
            if (ot_ch & (1u << 2)) {
                emitf("[Q34B] O%s", (btn_word & (1u << 2)) != 0 ? "+" : "-");
            }
            if (ot_ch & (1u << 5)) {
                emitf("[Q34B] T%s", (btn_word & (1u << 5)) != 0 ? "+" : "-");
            }
        }
    }
    s_hid.prev_q34b_f4 = f[4];
    s_hid.prev_q34b_btn_word = btn_word;
    s_hid.prev_q34b_raw_valid = true;
    feed_canonical("[Q34B]", f[0], f[1], f[2], f[3], btn_mask, hat_nibble);
}

static void feed_report(const uint8_t *d, uint16_t len)
{
    const uint8_t *q34b = q34b_frame_ptr(d, len);

    if (q34b) {
        s_hid.profile = GP_PROFILE_Q34B;
        feed_report_q34b(q34b);
        return;
    }

    if (s_hid.profile == GP_PROFILE_Q34B && len >= 10 && !looks_like_bm769(d, len)) {
        s_hid.profile = GP_PROFILE_Q34B;
        feed_report_q34b(d);
        return;
    }

    if (!looks_like_bm769(d, len)) {
        return;
    }
    s_hid.profile = GP_PROFILE_BM769;
    feed_canonical("[BM769]", d[3], d[4], d[5], d[6], bm769_button_mask(d, len), d[7] & 0x0F);
}

static void trace_report_hex(const char *source, uint16_t handle, const uint8_t *data, uint16_t len)
{
    char hex[96];
    size_t pos = 0;
    uint16_t show_len = len > 16 ? 16 : len;

    if (!data || s_hid.report_trace_count >= 20) {
        return;
    }
    for (uint16_t i = 0; i < show_len && pos + 4 < sizeof(hex); i++) {
        pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%s%02X", i ? " " : "", data[i]);
    }
    if (len > show_len && pos + 5 < sizeof(hex)) {
        (void)snprintf(hex + pos, sizeof(hex) - pos, " ...");
    }
    QHID_LOG_NOTICE_DETAIL("quaddle ble hid: %s handle=%u len=%u data=%s", source ? source : "report", handle, len,
                           hex);
    s_hid.report_trace_count++;
}

static bool name_filter_match(const char *name)
{
    const char *filters = QUADDLE_BLE_GAMEPAD_NAME_FILTER;
    size_t name_len;

    if (!name || name[0] == '\0') {
        return false;
    }
    name_len = strlen(name);
    while (*filters) {
        char token[24];
        size_t n = 0;
        while (*filters == ',' || *filters == ' ') {
            filters++;
        }
        while (*filters && *filters != ',' && n + 1 < sizeof(token)) {
            token[n++] = (char)tolower((unsigned char)*filters++);
        }
        token[n] = '\0';
        if (n > 0) {
            for (size_t i = 0; i + n <= name_len; i++) {
                size_t j = 0;
                while (j < n && tolower((unsigned char)name[i + j]) == token[j]) {
                    j++;
                }
                if (j == n) {
                    return true;
                }
            }
        }
        while (*filters && *filters != ',') {
            filters++;
        }
    }
    return false;
}

static void adv_extract_name(const uint8_t *data, uint16_t len, char *out, size_t out_len)
{
    uint16_t pos = 0;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    while (data && pos + 1 < len) {
        uint8_t field_len = data[pos++];
        uint8_t type;
        uint8_t copy_len;

        if (field_len == 0 || pos + field_len > len) {
            break;
        }
        type = data[pos++];
        if (type == 0x08 || type == 0x09) {
            copy_len = field_len - 1;
            if (copy_len >= out_len) {
                copy_len = (uint8_t)out_len - 1;
            }
            memcpy(out, &data[pos], copy_len);
            out[copy_len] = '\0';
            return;
        }
        pos = (uint16_t)(pos + field_len - 1);
    }
}

static void reset_decoder(void)
{
    s_hid.prev_valid = false;
    s_hid.prev_lx = 0;
    s_hid.prev_ly = 0;
    s_hid.prev_rx = 0;
    s_hid.prev_ry = 0;
    s_hid.prev_buttons = 0;
    s_hid.prev_hat = 0;
    s_hid.prev_q34b_f4 = 0;
    s_hid.prev_q34b_btn_word = 0;
    s_hid.prev_q34b_raw_valid = false;
}

static OPERATE_RET apply_conn_params(bool coexist_busy)
{
    TKL_BLE_GAP_CONN_PARAMS_T conn = {0};
    uint8_t target_mode = coexist_busy ? 2 : 1;
    OPERATE_RET rt;

    if (!s_hid.connected || s_hid.conn_handle == QUADDLE_BLE_INVALID_HANDLE || s_hid.conn_param_mode == target_mode) {
        return OPRT_OK;
    }

    conn.conn_interval_min = coexist_busy ? QUADDLE_BLE_CONN_BUSY_MIN : QUADDLE_BLE_CONN_NORMAL_MIN;
    conn.conn_interval_max = coexist_busy ? QUADDLE_BLE_CONN_BUSY_MAX : QUADDLE_BLE_CONN_NORMAL_MAX;
    conn.conn_latency = 0;
    conn.conn_sup_timeout = QUADDLE_BLE_CONN_TIMEOUT;
    rt = tkl_ble_gap_conn_param_update(s_hid.conn_handle, &conn);
    if (rt == OPRT_OK) {
        s_hid.conn_param_mode = target_mode;
        PR_NOTICE("quaddle ble hid: %s BLE conn interval %u-%u",
                  coexist_busy ? "coexist busy" : "normal", conn.conn_interval_min, conn.conn_interval_max);
    } else {
        PR_WARN("quaddle ble hid: conn param update failed rt=%d busy=%d", rt, coexist_busy);
    }
    return rt;
}

static void stop_hid_activity_timers(void)
{
    if (s_hid.rescan_timer) {
        (void)tal_sw_timer_stop(s_hid.rescan_timer);
    }
    if (s_hid.connect_timer) {
        (void)tal_sw_timer_stop(s_hid.connect_timer);
    }
    if (s_hid.security_timer) {
        (void)tal_sw_timer_stop(s_hid.security_timer);
    }
    if (s_hid.subscribe_timer) {
        (void)tal_sw_timer_stop(s_hid.subscribe_timer);
    }
    if (s_hid.hid_wake_timer) {
        (void)tal_sw_timer_stop(s_hid.hid_wake_timer);
    }
    if (s_hid.poll_timer) {
        (void)tal_sw_timer_stop(s_hid.poll_timer);
    }
    s_hid.poll_read_pending = false;
    s_hid.poll_read_handle = QUADDLE_BLE_INVALID_HANDLE;
}

static void wifi_resume_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

    if (!s_hid.wifi_busy) {
        return;
    }
    PR_WARN("quaddle ble hid: wifi busy timeout, resume gamepad BLE");
    set_wifi_busy_internal(false);
}

static void set_wifi_busy_internal(bool busy)
{
    bool was_busy = s_hid.wifi_busy;

    if (s_hid.wifi_busy == busy) {
        if (busy && s_hid.wifi_resume_timer) {
            (void)tal_sw_timer_start(s_hid.wifi_resume_timer, QUADDLE_BLE_WIFI_BUSY_MAX_MS, TAL_TIMER_ONCE);
        } else {
            (void)apply_conn_params(ble_activity_blocked());
        }
        return;
    }

    s_hid.wifi_busy = busy;
    PR_NOTICE("quaddle ble hid: wifi coexist %s", busy ? "busy" : "normal");

    if (busy) {
        if (s_hid.wifi_resume_timer) {
            (void)tal_sw_timer_start(s_hid.wifi_resume_timer, QUADDLE_BLE_WIFI_BUSY_MAX_MS, TAL_TIMER_ONCE);
        }
        stop_hid_activity_timers();
        if (s_hid.scanning) {
            (void)tkl_ble_gap_scan_stop();
            s_hid.scanning = false;
        }
        if (s_hid.connecting) {
            (void)tkl_ble_gap_connect_cancel();
            s_hid.connecting = false;
        }
        if (s_hid.connected && s_hid.conn_handle != QUADDLE_BLE_INVALID_HANDLE) {
            PR_NOTICE("quaddle ble hid: pause gamepad BLE during WiFi provisioning");
            (void)tkl_ble_gap_disconnect(s_hid.conn_handle, TKL_BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        }
        return;
    }

    if (s_hid.wifi_resume_timer) {
        (void)tal_sw_timer_stop(s_hid.wifi_resume_timer);
    }
    (void)apply_conn_params(ble_activity_blocked());

    if (s_hid.connected && s_hid.hid_wake_timer) {
        if (!ble_activity_blocked()) {
            (void)tal_sw_timer_start(s_hid.hid_wake_timer, QUADDLE_BLE_HID_WAKE_MS, TAL_TIMER_ONCE);
        }
    } else if (!ble_activity_blocked() && !s_hid.connected && !s_hid.connecting) {
        schedule_rescan_delay(was_busy ? QUADDLE_BLE_WIFI_RESUME_SCAN_DELAY_MS : QUADDLE_BLE_RESCAN_MS);
    }
}

static void boot_netcfg_start_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    OPERATE_RET rt = OPRT_OK;

    PR_NOTICE("quaddle ble hid: start Tuya BLE provisioning");
    TUYA_CALL_ERR_LOG(netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG, &(netcfg_args_t){.type = NETCFG_TUYA_BLE}));
#else
    PR_WARN("quaddle ble hid: Tuya BLE provisioning ignored, WiFi is disabled");
#endif
}

static void schedule_boot_netcfg_start(void)
{
    if (tuya_iot_is_connected()) {
        PR_NOTICE("quaddle ble hid: BOOT short press ignored, device already online");
        return;
    }
    PR_NOTICE("quaddle ble hid: BOOT short press: pause gamepad and enter Tuya BLE provisioning");
    set_wifi_busy_internal(true);
    if (s_hid.boot_netcfg_timer) {
        (void)tal_sw_timer_start(s_hid.boot_netcfg_timer, QUADDLE_BLE_BOOT_NETCFG_DELAY_MS, TAL_TIMER_ONCE);
    } else {
        boot_netcfg_start_timer_cb(NULL, NULL);
    }
}

static OPERATE_RET start_scan(void)
{
    TKL_BLE_GAP_SCAN_PARAMS_T scan = {0};
    OPERATE_RET rt = OPRT_OK;

    if (!s_hid.initialized || s_hid.connected || s_hid.connecting || s_hid.connect_cancel_pending ||
        s_hid.scanning || ble_activity_blocked()) {
        return OPRT_OK;
    }
    scan.extended = 0;
    scan.active = 1;
    scan.scan_phys = TKL_BLE_GAP_PHY_1MBPS;
    scan.interval = s_hid.saved_peer_valid ? QUADDLE_BLE_SAVED_SCAN_INTERVAL : QUADDLE_BLE_SCAN_INTERVAL;
    scan.window = s_hid.saved_peer_valid ? QUADDLE_BLE_SAVED_SCAN_WINDOW : QUADDLE_BLE_SCAN_WINDOW;
    scan.timeout = 0;
    scan.scan_channel_map = 0x01 | 0x02 | 0x04;
    rt = tkl_ble_gap_scan_start(&scan);
    if (rt != OPRT_OK) {
        PR_WARN("quaddle ble hid: scan start failed rt=%d, retry in %ums", rt, QUADDLE_BLE_RESCAN_MS);
        schedule_rescan();
        return rt;
    }
    s_hid.scanning = true;
    s_hid.scan_report_count = 0;
    s_hid.scan_report_log_count = 0;
    if (s_hid.saved_peer_valid) {
        char addr_text[32] = {0};
        format_addr(&s_hid.saved_peer_addr, addr_text, sizeof(addr_text));
        PR_NOTICE("quaddle ble hid: low-power scan saved peer addr=%s name=%s interval=%u window=%u", addr_text,
                  s_hid.saved_peer_name[0] ? s_hid.saved_peer_name : "(unknown)", scan.interval, scan.window);
    } else {
        PR_NOTICE("quaddle ble hid: scanning for %s", QUADDLE_BLE_GAMEPAD_NAME_FILTER);
    }
    return OPRT_OK;
}

static void rescan_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;
    if (ble_activity_blocked()) {
        return;
    }
    /* A connect-cancel completion event is normally delivered immediately.
     * This is the fallback for controllers which do not report it. */
    s_hid.connect_cancel_pending = false;
    (void)start_scan();
}

static void schedule_rescan(void)
{
    schedule_rescan_delay(QUADDLE_BLE_RESCAN_MS);
}

static void schedule_rescan_delay(uint32_t delay_ms)
{
    if (ble_activity_blocked()) {
        return;
    }
    if (s_hid.rescan_timer) {
        (void)tal_sw_timer_start(s_hid.rescan_timer, delay_ms, TAL_TIMER_ONCE);
    }
}

static OPERATE_RET connect_peer(void);

static void connect_timer_cb(TIMER_ID timer_id, void *arg)
{
    OPERATE_RET rt;

    (void)timer_id;
    (void)arg;

    if (!s_hid.connecting || s_hid.connected) {
        return;
    }

    PR_WARN("quaddle ble hid: connect timeout name=%s, cancel GAP connection", s_hid.peer_name);
    s_hid.connecting = false;
    rt = tkl_ble_gap_connect_cancel();
    if (rt == OPRT_OK) {
        s_hid.connect_cancel_pending = true;
    } else if (rt == QUADDLE_BLE_CANCEL_NOT_ACTIVE) {
        PR_DEBUG("quaddle ble hid: connection procedure already ended before cancel");
    } else {
        PR_WARN("quaddle ble hid: connect cancel returned rt=%d", rt);
    }
    schedule_rescan_delay(QUADDLE_BLE_CANCEL_SETTLE_MS);
}

static void security_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

    if (!s_hid.connected || s_hid.conn_handle == QUADDLE_BLE_INVALID_HANDLE) {
        return;
    }

    QHID_LOG_NOTICE_DETAIL("quaddle ble hid: security wait done, start GATT discovery");
    (void)tkl_ble_gattc_exchange_mtu_request(s_hid.conn_handle, 517);
    (void)tkl_ble_gattc_all_service_discovery(s_hid.conn_handle);
}

static bool input_char_readable(const INPUT_CHAR_T *ch)
{
    return ch && ((ch->property & TKL_BLE_GATT_CHAR_PROP_READ) == TKL_BLE_GATT_CHAR_PROP_READ);
}

static uint8_t readable_input_char_count(void)
{
    uint8_t count = 0;

    for (uint8_t i = 0; i < s_hid.input_char_count; i++) {
        if (input_char_readable(&s_hid.input_chars[i])) {
            count++;
        }
    }

    return count;
}

static bool input_char_notifiable(const INPUT_CHAR_T *ch)
{
    return ch && ((ch->property & TKL_BLE_GATT_CHAR_PROP_NOTIFY) == TKL_BLE_GATT_CHAR_PROP_NOTIFY ||
                  (ch->property & TKL_BLE_GATT_CHAR_PROP_INDICATE) == TKL_BLE_GATT_CHAR_PROP_INDICATE);
}

static uint8_t notifiable_input_char_count(void)
{
    uint8_t count = 0;

    for (uint8_t i = 0; i < s_hid.input_char_count; i++) {
        if (input_char_notifiable(&s_hid.input_chars[i])) {
            count++;
        }
    }

    return count;
}

static bool add_input_char(uint16_t handle, uint16_t end_handle, uint16_t uuid16, uint8_t property)
{
    if (s_hid.input_char_count >= QUADDLE_BLE_MAX_INPUT_CHARS) {
        return false;
    }
    for (uint8_t i = 0; i < s_hid.input_char_count; i++) {
        if (s_hid.input_chars[i].handle == handle) {
            return true;
        }
    }
    s_hid.input_chars[s_hid.input_char_count].handle = handle;
    s_hid.input_chars[s_hid.input_char_count].end_handle = end_handle;
    s_hid.input_chars[s_hid.input_char_count].uuid16 = uuid16;
    s_hid.input_chars[s_hid.input_char_count].property = property;
    s_hid.input_char_count++;
    return true;
}

static OPERATE_RET discover_next_service_chars(void)
{
    while (s_hid.service_index < s_hid.service_count) {
        SVC_RANGE_T *svc = &s_hid.services[s_hid.service_index++];
        OPERATE_RET rt = tkl_ble_gattc_all_char_discovery(s_hid.conn_handle, svc->start_handle, svc->end_handle);
        if (rt == OPRT_OK) {
            QHID_LOG_DETAIL("quaddle ble hid: discovering chars svc=0x%04x range=%u-%u", svc->uuid16,
                            svc->start_handle, svc->end_handle);
            return OPRT_OK;
        }
    }
    s_hid.input_char_index = 0;
    if (s_hid.input_char_count == 0) {
        PR_ERR("quaddle ble hid: no HID/vendor input characteristic found");
        (void)tkl_ble_gap_disconnect(s_hid.conn_handle, TKL_BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        return OPRT_NOT_FOUND;
    }
    return tkl_ble_gattc_char_desc_discovery(s_hid.conn_handle, s_hid.input_chars[0].handle,
                                             s_hid.input_chars[0].end_handle);
}

static OPERATE_RET discover_next_cccd(void)
{
    s_hid.input_char_index++;
    if (s_hid.input_char_index >= s_hid.input_char_count) {
        PR_NOTICE("quaddle ble hid: ready, subscribed %u input report(s), readable %u", s_hid.input_char_count,
                  readable_input_char_count());
        save_current_peer();
        if (s_hid.hid_wake_timer) {
            (void)tal_sw_timer_start(s_hid.hid_wake_timer, QUADDLE_BLE_HID_WAKE_MS, TAL_TIMER_ONCE);
        }
        return OPRT_OK;
    }
    return tkl_ble_gattc_char_desc_discovery(s_hid.conn_handle, s_hid.input_chars[s_hid.input_char_index].handle,
                                             s_hid.input_chars[s_hid.input_char_index].end_handle);
}

static void subscribe_next_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;
    if (s_hid.connected) {
        (void)discover_next_cccd();
    }
}

static void hid_wake_timer_cb(TIMER_ID timer_id, void *arg)
{
    uint8_t report_mode = 0x01;
    uint8_t exit_suspend = 0x01;
    uint8_t readable_count;
    uint8_t notify_count;

    (void)timer_id;
    (void)arg;

    if (!s_hid.connected) {
        return;
    }
    if (s_hid.hid_protocol_mode_handle != QUADDLE_BLE_INVALID_HANDLE) {
        (void)tkl_ble_gattc_write_without_rsp(s_hid.conn_handle, s_hid.hid_protocol_mode_handle, &report_mode,
                                              sizeof(report_mode));
        QHID_LOG_NOTICE_DETAIL("quaddle ble hid: set report protocol mode handle=%u", s_hid.hid_protocol_mode_handle);
    }
    if (s_hid.hid_control_point_handle != QUADDLE_BLE_INVALID_HANDLE) {
        (void)tkl_ble_gattc_write_without_rsp(s_hid.conn_handle, s_hid.hid_control_point_handle, &exit_suspend,
                                              sizeof(exit_suspend));
        QHID_LOG_NOTICE_DETAIL("quaddle ble hid: exit suspend handle=%u", s_hid.hid_control_point_handle);
    }
    readable_count = readable_input_char_count();
    notify_count = notifiable_input_char_count();
    if (!ble_activity_blocked() && s_hid.poll_timer && readable_count > 0 && notify_count == 0) {
        s_hid.poll_read_index = 0;
        s_hid.poll_read_pending = false;
        s_hid.poll_read_handle = QUADDLE_BLE_INVALID_HANDLE;
        memset(s_hid.poll_snap, 0, sizeof(s_hid.poll_snap));
        memset(s_hid.poll_snap_len, 0, sizeof(s_hid.poll_snap_len));
        (void)tal_sw_timer_start(s_hid.poll_timer, QUADDLE_BLE_POLL_READ_MS, TAL_TIMER_CYCLE);
        QHID_LOG_NOTICE_DETAIL("quaddle ble hid: read polling enabled every %ums (%u readable input report(s))",
                               QUADDLE_BLE_POLL_READ_MS, readable_count);
    } else {
        QHID_LOG_NOTICE_DETAIL("quaddle ble hid: read polling skipped, readable=%u notify=%u", readable_count,
                               notify_count);
    }
}

static void poll_read_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

    if (ble_activity_blocked() || !s_hid.connected || s_hid.input_char_count == 0 || s_hid.poll_read_pending) {
        return;
    }
    for (uint8_t i = 0; i < s_hid.input_char_count; i++) {
        OPERATE_RET rt;
        uint8_t index;

        if (s_hid.poll_read_index >= s_hid.input_char_count) {
            s_hid.poll_read_index = 0;
        }
        index = s_hid.poll_read_index++;
        if (!input_char_readable(&s_hid.input_chars[index])) {
            continue;
        }
        s_hid.poll_read_handle = s_hid.input_chars[index].handle;
        rt = tkl_ble_gattc_read(s_hid.conn_handle, s_hid.input_chars[index].handle);
        if (rt == OPRT_OK) {
            s_hid.poll_read_pending = true;
        } else {
            s_hid.poll_read_handle = QUADDLE_BLE_INVALID_HANDLE;
        }
        break;
    }
}

static int input_char_index_by_handle(uint16_t handle)
{
    for (uint8_t i = 0; i < s_hid.input_char_count; i++) {
        if (s_hid.input_chars[i].handle == handle) {
            return (int)i;
        }
    }
    return -1;
}

static bool report_all_zero(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) {
        return false;
    }
    for (uint16_t i = 0; i < len; i++) {
        if (data[i] != 0) {
            return false;
        }
    }
    return true;
}

static OPERATE_RET connect_peer(void)
{
    TKL_BLE_GAP_ADDR_T conn_addr = s_hid.peer_addr;
    TKL_BLE_GAP_CONN_PARAMS_T conn = {0};
    TKL_BLE_GAP_SCAN_PARAMS_T scan = {0};
    char addr_text[32] = {0};
    OPERATE_RET rt;

    if (!s_hid.initialized || s_hid.connected || s_hid.connecting || ble_activity_blocked()) {
        return OPRT_OK;
    }
    conn.conn_interval_min = ble_activity_blocked() ? QUADDLE_BLE_CONN_BUSY_MIN : QUADDLE_BLE_CONN_NORMAL_MIN;
    conn.conn_interval_max = ble_activity_blocked() ? QUADDLE_BLE_CONN_BUSY_MAX : QUADDLE_BLE_CONN_NORMAL_MAX;
    conn.conn_latency = 0;
    conn.conn_sup_timeout = QUADDLE_BLE_CONN_TIMEOUT;
    conn.connection_timeout = 8000;
    scan.interval = 0x0040;
    scan.window = 0x0040;
    scan.scan_phys = TKL_BLE_GAP_PHY_1MBPS;
    scan.scan_channel_map = 0x01 | 0x02 | 0x04;

    s_hid.connecting = true;
    s_hid.connect_cancel_pending = false;
    format_addr(&conn_addr, addr_text, sizeof(addr_text));
    PR_NOTICE("quaddle ble hid: connecting name=%s addr=%s", s_hid.peer_name, addr_text);
    rt = tkl_ble_gap_connect(&conn_addr, &scan, &conn);
    if (rt != OPRT_OK) {
        PR_ERR("quaddle ble hid: connect start failed rt=%d", rt);
        s_hid.connecting = false;
        schedule_rescan();
        return rt;
    }
    if (s_hid.connect_timer) {
        (void)tal_sw_timer_start(s_hid.connect_timer, QUADDLE_BLE_CONNECT_TIMEOUT_MS, TAL_TIMER_ONCE);
    }
    return OPRT_OK;
}

static void gap_cb(TKL_BLE_GAP_PARAMS_EVT_T *event)
{
    if (!event) {
        return;
    }

    switch (event->type) {
    case TKL_BLE_EVT_STACK_INIT:
        QHID_LOG_NOTICE_DETAIL("quaddle ble hid: stack init result=%d", event->result);
        if (!ble_activity_blocked()) {
            (void)start_scan();
        }
        break;

    case TKL_BLE_GAP_EVT_ADV_REPORT: {
        char name[32];
        char addr_text[32] = {0};
        bool saved_addr_match;
        if (ble_activity_blocked()) {
            break;
        }
        adv_extract_name(event->gap_event.adv_report.data.p_data, event->gap_event.adv_report.data.length, name,
                         sizeof(name));
        saved_addr_match = saved_addr_matches(&event->gap_event.adv_report.peer_addr);
        s_hid.scan_report_count++;
        if (s_hid.scan_report_log_count < 12 &&
            (s_hid.scan_report_count <= 3 || name[0] != '\0' || saved_addr_match)) {
            format_addr(&event->gap_event.adv_report.peer_addr, addr_text, sizeof(addr_text));
            PR_NOTICE("quaddle ble hid: scan report #%u type=%u addr=%s rssi=%d name=%s",
                      s_hid.scan_report_count, event->gap_event.adv_report.adv_type, addr_text,
                      event->gap_event.adv_report.rssi, name[0] ? name : "(none)");
            s_hid.scan_report_log_count++;
        }
        if (name[0] == '\0') {
            if (!s_hid.saved_peer_valid || event->gap_event.adv_report.adv_type != TKL_BLE_ADV_DATA) {
                break;
            }
            PR_NOTICE("quaddle ble hid: anonymous adv seen; try exact saved peer address");
            s_hid.scanning = false;
            s_hid.peer_addr = s_hid.saved_peer_addr;
            memset(s_hid.peer_name, 0, sizeof(s_hid.peer_name));
            strncpy(s_hid.peer_name, s_hid.saved_peer_name, sizeof(s_hid.peer_name) - 1);
            (void)tkl_ble_gap_scan_stop();
            (void)connect_peer();
            break;
        }
        if (!name_filter_match(name)) {
            break;
        }
        if (!peer_allowed_by_saved_filter(&event->gap_event.adv_report.peer_addr)) {
            break;
        }
        PR_NOTICE("quaddle ble hid: target adv name=%s rssi=%d", name, event->gap_event.adv_report.rssi);
        s_hid.scanning = false;
        memset(s_hid.peer_name, 0, sizeof(s_hid.peer_name));
        strncpy(s_hid.peer_name, name, sizeof(s_hid.peer_name) - 1);
        memcpy(&s_hid.peer_addr, &event->gap_event.adv_report.peer_addr, sizeof(s_hid.peer_addr));
        (void)tkl_ble_gap_scan_stop();
        (void)connect_peer();
    } break;

    case TKL_BLE_GAP_EVT_CONNECT:
        if (event->gap_event.connect.role != TKL_BLE_ROLE_CLIENT) {
            break;
        }
        if (s_hid.connect_timer) {
            (void)tal_sw_timer_stop(s_hid.connect_timer);
        }
        if (s_hid.security_timer) {
            (void)tal_sw_timer_stop(s_hid.security_timer);
        }
        s_hid.connecting = false;
        s_hid.connect_cancel_pending = false;
        if (event->result != OPRT_OK || event->conn_handle == QUADDLE_BLE_INVALID_HANDLE) {
            PR_WARN("quaddle ble hid: connect failed result=%d", event->result);
            if (ble_activity_blocked()) {
                break;
            }
            schedule_rescan();
            break;
        }
        s_hid.connected = true;
        s_hid.conn_handle = event->conn_handle;
        s_hid.service_count = 0;
        s_hid.service_index = 0;
        s_hid.hid_protocol_mode_handle = QUADDLE_BLE_INVALID_HANDLE;
        s_hid.hid_control_point_handle = QUADDLE_BLE_INVALID_HANDLE;
        s_hid.input_char_count = 0;
        s_hid.input_char_index = 0;
        s_hid.poll_read_index = 0;
        s_hid.poll_read_pending = false;
        s_hid.poll_read_handle = QUADDLE_BLE_INVALID_HANDLE;
        memset(s_hid.poll_snap, 0, sizeof(s_hid.poll_snap));
        memset(s_hid.poll_snap_len, 0, sizeof(s_hid.poll_snap_len));
        s_hid.report_trace_count = 0;
        s_hid.profile = GP_PROFILE_UNKNOWN;
        if (strstr(s_hid.peer_name, "Q34B") || strstr(s_hid.peer_name, "q34b")) {
            s_hid.profile = GP_PROFILE_Q34B;
        } else if (strstr(s_hid.peer_name, "BM769") || strstr(s_hid.peer_name, "bm769")) {
            s_hid.profile = GP_PROFILE_BM769;
        }
        reset_decoder();
        PR_NOTICE("quaddle ble hid: connected handle=%u name=%s", s_hid.conn_handle, s_hid.peer_name);
        (void)apply_conn_params(ble_activity_blocked());
        OPERATE_RET sec_rt = tkl_ble_gap_security_request(s_hid.conn_handle);
        if (sec_rt == OPRT_OK) {
            QHID_LOG_NOTICE_DETAIL("quaddle ble hid: security requested, wait %ums before GATT discovery",
                                   QUADDLE_BLE_SECURITY_WAIT_MS);
        } else {
            PR_WARN("quaddle ble hid: security request failed rt=%d, continue GATT discovery", sec_rt);
        }
        if (s_hid.security_timer) {
            (void)tal_sw_timer_start(s_hid.security_timer, QUADDLE_BLE_SECURITY_WAIT_MS, TAL_TIMER_ONCE);
        } else {
            (void)tkl_ble_gattc_exchange_mtu_request(s_hid.conn_handle, 247);
            (void)tkl_ble_gattc_all_service_discovery(s_hid.conn_handle);
        }
        break;

    case TKL_BLE_GAP_EVT_CONN_PARAM_UPDATE:
        if (s_hid.connected && event->conn_handle == s_hid.conn_handle) {
            if (ble_activity_blocked() &&
                event->gap_event.conn_param.conn_interval_max < QUADDLE_BLE_CONN_BUSY_MIN) {
                s_hid.conn_param_mode = 0;
                (void)apply_conn_params(true);
            } else {
                s_hid.conn_param_mode = ble_activity_blocked() ? 2 : 1;
            }
        }
        break;

    case TKL_BLE_GAP_EVT_DISCONNECT:
        if (s_hid.connect_timer) {
            (void)tal_sw_timer_stop(s_hid.connect_timer);
        }
        if (s_hid.security_timer) {
            (void)tal_sw_timer_stop(s_hid.security_timer);
        }
        if (s_hid.poll_timer) {
            (void)tal_sw_timer_stop(s_hid.poll_timer);
        }
        PR_NOTICE("quaddle ble hid: disconnected reason=%d", event->gap_event.disconnect.reason);
        s_hid.connected = false;
        s_hid.connecting = false;
        s_hid.connect_cancel_pending = false;
        s_hid.scanning = false;
        s_hid.conn_handle = QUADDLE_BLE_INVALID_HANDLE;
        s_hid.poll_read_index = 0;
        s_hid.poll_read_pending = false;
        s_hid.poll_read_handle = QUADDLE_BLE_INVALID_HANDLE;
        s_hid.conn_param_mode = 0;
        memset(s_hid.poll_snap, 0, sizeof(s_hid.poll_snap));
        memset(s_hid.poll_snap_len, 0, sizeof(s_hid.poll_snap_len));
        reset_decoder();
        quaddle_robot_bridge_reset();
        if (!ble_activity_blocked()) {
            schedule_rescan();
        }
        break;

    default:
        break;
    }
}

static void gatt_cb(TKL_BLE_GATT_PARAMS_EVT_T *event)
{
    if (!event || event->conn_handle != s_hid.conn_handle) {
        return;
    }

    switch (event->type) {
    case TKL_BLE_GATT_EVT_PRIM_SEV_DISCOVERY:
        if (event->result != OPRT_OK) {
            PR_ERR("quaddle ble hid: service discovery failed %d", event->result);
            if (s_hid.connected && s_hid.conn_handle != QUADDLE_BLE_INVALID_HANDLE) {
                (void)tkl_ble_gap_disconnect(s_hid.conn_handle, TKL_BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            }
            break;
        }
        s_hid.service_count = 0;
        for (uint8_t i = 0; i < event->gatt_event.svc_disc.svc_num && s_hid.service_count < QUADDLE_BLE_MAX_SERVICES;
             i++) {
            TKL_BLE_GATT_SVC_HANDLE_T *svc = &event->gatt_event.svc_disc.services[i];
            uint16_t uuid16 = uuid_to_u16(&svc->uuid);
            s_hid.services[s_hid.service_count].start_handle = svc->start_handle;
            s_hid.services[s_hid.service_count].end_handle = svc->end_handle;
            s_hid.services[s_hid.service_count].uuid16 = uuid16;
            s_hid.service_count++;
            QHID_LOG_DETAIL("quaddle ble hid: svc uuid=0x%04x range=%u-%u", uuid16, svc->start_handle,
                            svc->end_handle);
        }
        s_hid.service_index = 0;
        (void)discover_next_service_chars();
        break;

    case TKL_BLE_GATT_EVT_CHAR_DISCOVERY:
        if (event->result == OPRT_OK) {
            TKL_BLE_GATT_CHAR_DISC_TYPE_T *disc = &event->gatt_event.char_disc;
            uint16_t service_end = 0xFFFF;
            if (s_hid.service_index > 0 && s_hid.service_index - 1 < s_hid.service_count) {
                service_end = s_hid.services[s_hid.service_index - 1].end_handle;
            }
            for (uint8_t i = 0; i < disc->char_num; i++) {
                uint16_t uuid16 = uuid_to_u16(&disc->characteristics[i].uuid);
                uint16_t end_handle = service_end;
                uint16_t service_uuid = 0xFFFF;
                uint8_t property = disc->characteristics[i].property;
                if (s_hid.service_index > 0 && s_hid.service_index - 1 < s_hid.service_count) {
                    service_uuid = s_hid.services[s_hid.service_index - 1].uuid16;
                }
                if (i + 1 < disc->char_num && disc->characteristics[i + 1].handle > 0) {
                    end_handle = disc->characteristics[i + 1].handle - 1;
                }
                if (service_uuid == HID_SERVICE_UUID && uuid16 == HID_PROTOCOL_MODE_CHAR_UUID) {
                    s_hid.hid_protocol_mode_handle = disc->characteristics[i].handle;
                    QHID_LOG_NOTICE_DETAIL("quaddle ble hid: protocol mode handle=%u",
                                           s_hid.hid_protocol_mode_handle);
                } else if (service_uuid == HID_SERVICE_UUID && uuid16 == HID_CONTROL_POINT_CHAR_UUID) {
                    s_hid.hid_control_point_handle = disc->characteristics[i].handle;
                    QHID_LOG_NOTICE_DETAIL("quaddle ble hid: control point handle=%u",
                                           s_hid.hid_control_point_handle);
                }
                if (uuid16 == HID_REPORT_CHAR_UUID || uuid16 == VENDOR_INPUT_CHAR_UUID ||
                    uuid128_has_vendor_input(&disc->characteristics[i].uuid)) {
                    (void)add_input_char(disc->characteristics[i].handle, end_handle, uuid16, property);
                    QHID_LOG_NOTICE_DETAIL("quaddle ble hid: input char uuid=0x%04x handle=%u end=%u prop=0x%02x",
                                           uuid16, disc->characteristics[i].handle, end_handle, property);
                }
            }
        }
        (void)discover_next_service_chars();
        break;

    case TKL_BLE_GATT_EVT_CHAR_DESC_DISCOVERY:
        if (event->result == OPRT_OK) {
            uint8_t enable_notify[2] = {0x01, 0x00};
            uint16_t cccd = event->gatt_event.desc_disc.cccd_handle;
            if (cccd != QUADDLE_BLE_INVALID_HANDLE) {
                (void)tkl_ble_gattc_write(event->conn_handle, cccd, enable_notify, sizeof(enable_notify));
                QHID_LOG_NOTICE_DETAIL("quaddle ble hid: subscribed char=%u cccd=%u",
                                       s_hid.input_chars[s_hid.input_char_index].handle, cccd);
            }
        } else {
            QHID_LOG_DETAIL("quaddle ble hid: desc discovery failed char=%u result=%d",
                            s_hid.input_chars[s_hid.input_char_index].handle, event->result);
        }
        if (s_hid.subscribe_timer) {
            (void)tal_sw_timer_start(s_hid.subscribe_timer, QUADDLE_BLE_SUBSCRIBE_NEXT_MS, TAL_TIMER_ONCE);
        }
        break;

    case TKL_BLE_GATT_EVT_NOTIFY_INDICATE_RX:
        trace_report_hex("notify", event->gatt_event.data_report.char_handle,
                         event->gatt_event.data_report.report.p_data, event->gatt_event.data_report.report.length);
        feed_report(event->gatt_event.data_report.report.p_data, event->gatt_event.data_report.report.length);
        break;

    case TKL_BLE_GATT_EVT_READ_RX:
        s_hid.poll_read_pending = false;
        s_hid.poll_read_handle = QUADDLE_BLE_INVALID_HANDLE;
        if (event->result == OPRT_OK && event->gatt_event.data_read.report.p_data &&
            event->gatt_event.data_read.report.length > 0) {
            const uint8_t *data = event->gatt_event.data_read.report.p_data;
            uint16_t len = event->gatt_event.data_read.report.length;
            uint16_t snap_len = len > sizeof(s_hid.poll_snap[0]) ? sizeof(s_hid.poll_snap[0]) : len;
            int index = input_char_index_by_handle(event->gatt_event.data_read.char_handle);

            if (report_all_zero(data, len)) {
                QHID_LOG_DETAIL("quaddle ble hid: ignore all-zero read handle=%u len=%u",
                                event->gatt_event.data_read.char_handle, len);
                break;
            }
            if (index >= 0 && s_hid.poll_snap_len[index] == snap_len &&
                memcmp(s_hid.poll_snap[index], data, snap_len) == 0) {
                break;
            }

            trace_report_hex("read", event->gatt_event.data_read.char_handle, data, len);
            feed_report(data, len);
            if (index >= 0) {
                memcpy(s_hid.poll_snap[index], data, snap_len);
                s_hid.poll_snap_len[index] = (uint8_t)snap_len;
            }
        }
        break;

    default:
        break;
    }
}

OPERATE_RET quaddle_ble_hid_central_clear_saved(void)
{
    PR_NOTICE("quaddle ble hid: clearing pairing (bonds + saved peer)");

    if (s_hid.rescan_timer) {
        (void)tal_sw_timer_stop(s_hid.rescan_timer);
    }
    if (s_hid.connect_timer) {
        (void)tal_sw_timer_stop(s_hid.connect_timer);
    }
    if (s_hid.security_timer) {
        (void)tal_sw_timer_stop(s_hid.security_timer);
    }
    if (s_hid.subscribe_timer) {
        (void)tal_sw_timer_stop(s_hid.subscribe_timer);
    }
    if (s_hid.hid_wake_timer) {
        (void)tal_sw_timer_stop(s_hid.hid_wake_timer);
    }
    if (s_hid.poll_timer) {
        (void)tal_sw_timer_stop(s_hid.poll_timer);
    }
    if (s_hid.wifi_resume_timer) {
        (void)tal_sw_timer_stop(s_hid.wifi_resume_timer);
    }

    if (s_hid.scanning) {
        (void)tkl_ble_gap_scan_stop();
    }
    if (s_hid.connecting) {
        (void)tkl_ble_gap_connect_cancel();
    }
    if (s_hid.connected && s_hid.conn_handle != QUADDLE_BLE_INVALID_HANDLE) {
        (void)tkl_ble_gap_disconnect(s_hid.conn_handle, TKL_BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    }

    (void)tkl_ble_gap_unpair_all();
    (void)tal_kv_del(QUADDLE_BLE_KV_PEER_ADDR);
    (void)tal_kv_del(QUADDLE_BLE_KV_PEER_NAME);

    s_hid.connected = false;
    s_hid.connecting = false;
    s_hid.connect_cancel_pending = false;
    s_hid.scanning = false;
    s_hid.conn_handle = QUADDLE_BLE_INVALID_HANDLE;
    s_hid.service_count = 0;
    s_hid.service_index = 0;
    s_hid.input_char_count = 0;
    s_hid.input_char_index = 0;
    s_hid.poll_read_index = 0;
    s_hid.poll_read_pending = false;
    s_hid.profile = GP_PROFILE_UNKNOWN;
    s_hid.peer_name[0] = '\0';
    s_hid.saved_peer_valid = false;
    memset(&s_hid.saved_peer_addr, 0, sizeof(s_hid.saved_peer_addr));
    memset(s_hid.saved_peer_name, 0, sizeof(s_hid.saved_peer_name));
    reset_decoder();
    quaddle_robot_bridge_reset();

    PR_NOTICE("quaddle ble hid: pairing cleared; scanning any supported gamepad");
    schedule_rescan();
    return OPRT_OK;
}

void quaddle_ble_hid_central_set_wifi_busy(bool busy)
{
    if (!s_hid.initialized) {
        s_hid.wifi_busy = busy;
        return;
    }
    set_wifi_busy_internal(busy);
}

void quaddle_ble_hid_central_set_chat_busy(bool busy)
{
    if (!s_hid.initialized) {
        s_hid.chat_busy = busy;
        return;
    }
    if (s_hid.chat_busy == busy) {
        return;
    }

    s_hid.chat_busy = busy;
    PR_NOTICE("quaddle ble hid: AI chat %s", busy ? "busy, pause discovery" : "idle, resume discovery");

    if (busy) {
        if (s_hid.rescan_timer) {
            (void)tal_sw_timer_stop(s_hid.rescan_timer);
        }
        if (s_hid.connect_timer) {
            (void)tal_sw_timer_stop(s_hid.connect_timer);
        }
        if (s_hid.scanning) {
            (void)tkl_ble_gap_scan_stop();
            s_hid.scanning = false;
        }
        if (s_hid.connecting) {
            if (tkl_ble_gap_connect_cancel() == OPRT_OK) {
                s_hid.connect_cancel_pending = true;
            }
            s_hid.connecting = false;
        }
        if (s_hid.poll_timer) {
            (void)tal_sw_timer_stop(s_hid.poll_timer);
        }
        s_hid.poll_read_pending = false;
        s_hid.poll_read_handle = QUADDLE_BLE_INVALID_HANDLE;
        (void)apply_conn_params(true);
        return;
    }

    (void)apply_conn_params(ble_activity_blocked());
    if (s_hid.wifi_busy) {
        return;
    }
    if (s_hid.connected && s_hid.hid_wake_timer) {
        (void)tal_sw_timer_start(s_hid.hid_wake_timer, QUADDLE_BLE_HID_WAKE_MS, TAL_TIMER_ONCE);
    } else if (!s_hid.connecting) {
        schedule_rescan_delay(QUADDLE_BLE_CHAT_IDLE_SCAN_DELAY_MS);
    }
}

static int netcfg_wifi_event_cb(void *data)
{
    (void)data;
    quaddle_ble_hid_central_set_wifi_busy(true);
    return OPRT_OK;
}

static void boot_timer_cb(TIMER_ID timer_id, void *arg)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    bool down;
    uint32_t now_ms;

    (void)timer_id;
    (void)arg;

    if (tkl_gpio_read(QUADDLE_BLE_BOOT_BUTTON_PIN, &level) != OPRT_OK) {
        return;
    }
    down = (level == TUYA_GPIO_LEVEL_LOW);
    if (!down) {
        if (s_hid.boot_down && !s_hid.boot_fired_this_hold) {
            now_ms = (uint32_t)tal_system_get_millisecond();
            if (now_ms - s_hid.boot_press_start_ms >= QUADDLE_BLE_BOOT_SHORT_MIN_MS) {
                s_hid.boot_fired_this_hold = true;
                schedule_boot_netcfg_start();
            }
        }
        s_hid.boot_down = false;
        s_hid.boot_fired_this_hold = false;
        return;
    }
    now_ms = (uint32_t)tal_system_get_millisecond();
    if (!s_hid.boot_down) {
        s_hid.boot_down = true;
        s_hid.boot_press_start_ms = now_ms;
        return;
    }
    if (s_hid.boot_fired_this_hold || now_ms - s_hid.boot_press_start_ms < QUADDLE_BLE_BOOT_HOLD_MS) {
        return;
    }

    s_hid.boot_fired_this_hold = true;
    PR_NOTICE("quaddle ble hid: BOOT long press: clear pairing (same as r)");
    (void)quaddle_ble_hid_central_clear_saved();
}

static OPERATE_RET init_boot_button(void)
{
    OPERATE_RET rt;
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };

    rt = tkl_gpio_init(QUADDLE_BLE_BOOT_BUTTON_PIN, &gpio_cfg);
    if (rt != OPRT_OK) {
        PR_WARN("quaddle ble hid: BOOT GPIO0 init failed %d", rt);
    }
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(boot_timer_cb, NULL, &s_hid.boot_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_start(s_hid.boot_timer, QUADDLE_BLE_BOOT_POLL_MS, TAL_TIMER_CYCLE));
    PR_NOTICE("quaddle ble hid: BOOT GPIO0 short press starts netcfg, hold %ums clears saved gamepad",
              QUADDLE_BLE_BOOT_HOLD_MS);
    return OPRT_OK;
}

static void cli_clear_saved_cmd(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    (void)quaddle_ble_hid_central_clear_saved();
    tal_cli_echo("quaddle ble hid: pairing cleared (same as Arduino r)\r\n");
}

static const cli_cmd_t s_quaddle_ble_cli[] = {
    {"r", "r  clear saved gamepad pairing/bonds", cli_clear_saved_cmd},
    {"quaddle_ble_clear", "quaddle_ble_clear  clear saved gamepad pairing/bonds", cli_clear_saved_cmd},
};

OPERATE_RET quaddle_ble_hid_central_init(void)
{
    OPERATE_RET rt;

    if (s_hid.initialized) {
        return OPRT_OK;
    }
    memset(&s_hid, 0, sizeof(s_hid));
    /* Before Wi-Fi provisioning the AI mode remains INIT.  Start with gamepad
     * discovery allowed; real AI session events will pause it when needed. */
    s_hid.chat_busy = false;
    s_hid.conn_handle = QUADDLE_BLE_INVALID_HANDLE;
    s_hid.hid_protocol_mode_handle = QUADDLE_BLE_INVALID_HANDLE;
    s_hid.hid_control_point_handle = QUADDLE_BLE_INVALID_HANDLE;
    load_saved_peer();
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(rescan_timer_cb, NULL, &s_hid.rescan_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(connect_timer_cb, NULL, &s_hid.connect_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(security_timer_cb, NULL, &s_hid.security_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(subscribe_next_timer_cb, NULL, &s_hid.subscribe_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(hid_wake_timer_cb, NULL, &s_hid.hid_wake_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(poll_read_timer_cb, NULL, &s_hid.poll_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(wifi_resume_timer_cb, NULL, &s_hid.wifi_resume_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(boot_netcfg_start_timer_cb, NULL, &s_hid.boot_netcfg_timer));
    TUYA_CALL_ERR_RETURN(init_boot_button());
    TUYA_CALL_ERR_LOG(tal_cli_cmd_register(s_quaddle_ble_cli, sizeof(s_quaddle_ble_cli) / sizeof(s_quaddle_ble_cli[0])));
    TUYA_CALL_ERR_LOG(tal_event_subscribe(QUADDLE_BLE_EVENT_WIFI_NETCFG, "quaddle_ble", netcfg_wifi_event_cb,
                                          SUBSCRIBE_TYPE_NORMAL));
    TUYA_CALL_ERR_RETURN(tkl_ble_gap_callback_register(gap_cb));
    TUYA_CALL_ERR_RETURN(tkl_ble_gatt_callback_register(gatt_cb));
    s_hid.initialized = true;
    rt = tkl_ble_stack_init(TKL_BLE_ROLE_CLIENT);
    if (rt != OPRT_OK) {
        s_hid.initialized = false;
        PR_ERR("quaddle ble hid: stack init failed %d", rt);
        return rt;
    }
    /* Tuya BLE may have initialized the shared stack before this module
     * registered its callback, so TKL_BLE_EVT_STACK_INIT may not be emitted
     * again. Start scanning explicitly; start_scan() is idempotent if the
     * callback already started it. */
    rt = start_scan();
    if (rt != OPRT_OK) {
        PR_WARN("quaddle ble hid: initial scan deferred rt=%d", rt);
    } else {
        PR_NOTICE("quaddle ble hid: pre-network gamepad discovery enabled");
    }
    QHID_LOG_NOTICE_DETAIL("quaddle ble hid: central init fix=20260604-poll-filter-v8");
    return OPRT_OK;
}

#endif /* ENABLE_QUADDLE_BLE_HID_CENTRAL */
