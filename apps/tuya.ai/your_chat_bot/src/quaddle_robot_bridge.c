/**
 * @file quaddle_robot_bridge.c
 * @brief C port of QuaddleRobotBridge UART command mapping.
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_cli.h"
#include "tal_system.h"

#include "quaddle_robot_bridge.h"
#include "second_uart.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

#define LSTICK_DEAD_ABS_MAX        3
#define LSTICK_EDGE_ABS            120
#define LSTICK_MODERATE_ENTER_ABS  10
#define LSTICK_MODERATE_MAX_ABS    100
#define QUADDLE_CMD_MAX            48
#define ABXY_SOLO_DEFER_MS         200
#define STICK_DELAYED_K_MS         100
#define QUADDLE_POLL_INTERVAL_MS   20
#define GAMEPAD_PRIORITY_MS        500

typedef enum {
    QUADDLE_BTN_L1 = 1u << 0,
    QUADDLE_BTN_R1 = 1u << 1,
    QUADDLE_BTN_B  = 1u << 2,
    QUADDLE_BTN_A  = 1u << 3,
    QUADDLE_BTN_Y  = 1u << 4,
    QUADDLE_BTN_X  = 1u << 5,
    QUADDLE_BTN_ZL = 1u << 9,
    QUADDLE_BTN_ZR = 1u << 10,
} QUADDLE_BTN_E;

static const char *const s_stick_cmd_base[6][9] = {
    {"up", "wkF", "wkL", "wkR", "wkB", "wkF", "vtL", "vtR", "wkB"},
    {"up", "trF", "trL", "trR", "bdF", "trF", "trL", "trR", "slide"},
    {"up", "crF", "crL", "crR", "sadWkF", "crF", "crL", "crR", "sadWkF"},
    {"up", "ff", "pinWheel", "jumpSlide", "bf", "ff", "pinWheel", "jumpSlide", "bf"},
    {"up", "carryF", "sideL", "sideR", "smallWkF", "carryF", "sideL", "sideR", "smallTrF"},
    {"", "triCatF", "triCatL", "triCatR", "circle2L", "triCatF", "triCatL", "triCatR", "dance"},
};

static uint32_t s_buttons;
static char     s_last_k_cmd[QUADDLE_CMD_MAX];
static int16_t  s_last_jr_rx;
static int16_t  s_last_jr_ry;
static bool     s_jr_inited;
static char     s_last_raw_btn_cmd[QUADDLE_CMD_MAX];
static bool     s_r2_allow_next_g = true;

static bool     s_pending_solo_active;
static uint32_t s_pending_solo_deadline_ms;
static char     s_pending_solo_cmd[QUADDLE_CMD_MAX];
static char     s_pending_solo_btn;

static bool     s_pending_stick_active;
static uint32_t s_pending_stick_deadline_ms;
static char     s_pending_stick_cmd[QUADDLE_CMD_MAX];
static TIMER_ID s_poll_timer;
static bool     s_cli_registered;
static uint32_t s_last_gamepad_input_ms;

static int quaddle_abs(int v)
{
    return v < 0 ? -v : v;
}

static uint32_t now_ms(void)
{
    return (uint32_t)tal_system_get_millisecond();
}

static const char *payload_after_bracket(const char *line)
{
    const char *p;

    if (!line) {
        return NULL;
    }
    p = strchr(line, ']');
    if (!p) {
        return line;
    }
    ++p;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

static int classify_stick_zone(int lx, int ly)
{
    const int ax = quaddle_abs(lx);
    const int ay = quaddle_abs(ly);

    if (ax < LSTICK_DEAD_ABS_MAX && ay < LSTICK_DEAD_ABS_MAX) {
        return 0;
    }
    if (ly > LSTICK_EDGE_ABS) {
        return 5;
    }
    if (lx < -LSTICK_EDGE_ABS) {
        return 6;
    }
    if (lx > LSTICK_EDGE_ABS) {
        return 7;
    }
    if (ly < -LSTICK_EDGE_ABS) {
        return 8;
    }
    if (ly > LSTICK_MODERATE_ENTER_ABS && ly < LSTICK_MODERATE_MAX_ABS && ax < ay) {
        return 1;
    }
    if (lx < -LSTICK_MODERATE_ENTER_ABS && lx > -LSTICK_MODERATE_MAX_ABS && ax > ay) {
        return 2;
    }
    if (lx > LSTICK_MODERATE_ENTER_ABS && lx < LSTICK_MODERATE_MAX_ABS && ax > ay) {
        return 3;
    }
    if (ly < -LSTICK_MODERATE_ENTER_ABS && ly > -LSTICK_MODERATE_MAX_ABS && ay > ax) {
        return 4;
    }
    return -1;
}

static uint8_t stick_modifier_row(void)
{
    if (s_buttons & QUADDLE_BTN_A) {
        return 4;
    }
    if (s_buttons & QUADDLE_BTN_B) {
        return 3;
    }
    if (s_buttons & QUADDLE_BTN_Y) {
        return 1;
    }
    if (s_buttons & QUADDLE_BTN_X) {
        return 2;
    }
    if (s_buttons & QUADDLE_BTN_ZR) {
        return 5;
    }
    return 0;
}

static void format_k_cmd(const char *base, char *out, size_t n)
{
    if (!out || n == 0) {
        return;
    }
    out[0] = '\0';
    if (!base || base[0] == '\0') {
        return;
    }
    snprintf(out, n, "k%s", base);
}

static bool stick_full_cmd(int lx, int ly, char *out, size_t n)
{
    int z;

    if (!out || n == 0) {
        return false;
    }
    out[0] = '\0';
    z = classify_stick_zone(lx, ly);
    if (z < 0 || z > 8) {
        return false;
    }
    format_k_cmd(s_stick_cmd_base[stick_modifier_row()][z], out, n);
    return out[0] != '\0';
}

static void update_button_state(const char *label, bool pressed)
{
    uint32_t mask = 0;

    if (strcmp(label, "L1") == 0) {
        mask = QUADDLE_BTN_L1;
    } else if (strcmp(label, "R1") == 0) {
        mask = QUADDLE_BTN_R1;
    } else if (strcmp(label, "B") == 0) {
        mask = QUADDLE_BTN_B;
    } else if (strcmp(label, "A") == 0) {
        mask = QUADDLE_BTN_A;
    } else if (strcmp(label, "Y") == 0) {
        mask = QUADDLE_BTN_Y;
    } else if (strcmp(label, "X") == 0) {
        mask = QUADDLE_BTN_X;
    } else if (strcmp(label, "ZL") == 0 || strcmp(label, "L2") == 0) {
        mask = QUADDLE_BTN_ZL;
    } else if (strcmp(label, "ZR") == 0 || strcmp(label, "R2") == 0) {
        mask = QUADDLE_BTN_ZR;
    }

    if (mask == 0) {
        return;
    }
    if (pressed) {
        s_buttons |= mask;
    } else {
        s_buttons &= ~mask;
    }
}

static const char *cmd_for_pressed_label(const char *label)
{
    if (strcmp(label, "X") == 0) {
        return "kup";
    }
    if (strcmp(label, "B") == 0) {
        return "c";
    }
    if (strcmp(label, "A") == 0) {
        return "khds";
    }
    if (strcmp(label, "Y") == 0) {
        return "khi";
    }
    if (strcmp(label, "L1") == 0) {
        return "kstepHalf";
    }
    if (strcmp(label, "ZL") == 0 || strcmp(label, "L2") == 0) {
        return "kstep";
    }
    if (strcmp(label, "R1") == 0) {
        return "kpinWheel";
    }
    if (strcmp(label, "R2") == 0 || strcmp(label, "ZR") == 0) {
        return "g";
    }
    return NULL;
}

static void cancel_pending_solo(void)
{
    s_pending_solo_active = false;
    s_pending_solo_cmd[0] = '\0';
    s_pending_solo_btn    = '\0';
}

static void cancel_pending_stick(void)
{
    s_pending_stick_active = false;
    s_pending_stick_cmd[0] = '\0';
}

static OPERATE_RET send_k_cmd(const char *cmd)
{
    if (!cmd || cmd[0] != 'k') {
        return OPRT_INVALID_PARM;
    }
    cancel_pending_stick();
    if (strcmp(s_last_k_cmd, cmd) == 0) {
        return OPRT_OK;
    }
    OPERATE_RET rt = second_uart_send_string(cmd);
    if (rt == OPRT_OK) {
        strncpy(s_last_k_cmd, cmd, sizeof(s_last_k_cmd) - 1);
        s_last_k_cmd[sizeof(s_last_k_cmd) - 1] = '\0';
        s_r2_allow_next_g = true;
    }
    return rt;
}

static void mark_gamepad_input(void)
{
    s_last_gamepad_input_ms = now_ms();
}

static bool raw_btn_should_emit(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') {
        return false;
    }
    if ((cmd[0] == 'c' || cmd[0] == 'd') && cmd[1] == '\0') {
        return true;
    }
    if (cmd[0] == 'g' && cmd[1] == '\0') {
        return s_r2_allow_next_g || strcmp(s_last_raw_btn_cmd, cmd) != 0;
    }
    return strcmp(s_last_raw_btn_cmd, cmd) != 0;
}

static OPERATE_RET send_raw_btn_cmd(const char *cmd)
{
    OPERATE_RET rt;

    if (!raw_btn_should_emit(cmd)) {
        return OPRT_OK;
    }
    cancel_pending_stick();
    rt = second_uart_send_string_force(cmd);
    if (rt == OPRT_OK) {
        strncpy(s_last_raw_btn_cmd, cmd, sizeof(s_last_raw_btn_cmd) - 1);
        s_last_raw_btn_cmd[sizeof(s_last_raw_btn_cmd) - 1] = '\0';
        if (cmd[0] == 'g' && cmd[1] == '\0') {
            s_r2_allow_next_g = false;
        }
    }
    return rt;
}

static OPERATE_RET flush_pending_solo(void)
{
    char cmd[QUADDLE_CMD_MAX];

    if (!s_pending_solo_active || s_pending_solo_cmd[0] == '\0') {
        return OPRT_OK;
    }
    strncpy(cmd, s_pending_solo_cmd, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    cancel_pending_solo();
    return (cmd[0] == 'k') ? send_k_cmd(cmd) : send_raw_btn_cmd(cmd);
}

static OPERATE_RET flush_pending_stick(void)
{
    char cmd[QUADDLE_CMD_MAX];

    if (!s_pending_stick_active || s_pending_stick_cmd[0] == '\0') {
        return OPRT_OK;
    }
    strncpy(cmd, s_pending_stick_cmd, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    cancel_pending_stick();
    return send_k_cmd(cmd);
}

static bool zone_uses_delayed_k(int lx, int ly)
{
    int z = classify_stick_zone(lx, ly);
    return z == 0 || (z >= 1 && z <= 4);
}

static bool is_abxy(char c)
{
    return c == 'A' || c == 'B' || c == 'X' || c == 'Y';
}

static bool is_dpad_x_cmd(const char *cmd)
{
    if (!cmd || cmd[0] != 'X' || cmd[2] != '\0') {
        return false;
    }
    return cmd[1] == 'D' || cmd[1] == 'L' || cmd[1] == 'G' || cmd[1] == 'S';
}

static void poll_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;
    quaddle_robot_bridge_poll();
}

static void cli_quaddle_evt(int argc, char *argv[])
{
    char   line[96];
    size_t used = 0;
    int    i;

    if (argc < 2) {
        tal_cli_echo("quaddle_evt: usage: quaddle_evt A+ | quaddle_evt LSTICK 20,-3\r\n");
        return;
    }

    line[0] = '\0';
    for (i = 1; i < argc; i++) {
        int written = snprintf(line + used, sizeof(line) - used, "%s%s", (i == 1) ? "" : " ", argv[i]);
        if (written < 0 || (size_t)written >= sizeof(line) - used) {
            tal_cli_echo("quaddle_evt: line too long\r\n");
            return;
        }
        used += (size_t)written;
    }

    if (quaddle_robot_bridge_handle_line(line) == OPRT_OK) {
        tal_cli_echo("quaddle_evt: accepted\r\n");
    } else {
        tal_cli_echo("quaddle_evt: failed\r\n");
    }
}

static const cli_cmd_t s_quaddle_cli[] = {
    {"quaddle_evt", "quaddle_evt <Arduino-style gamepad event>", cli_quaddle_evt},
};

static bool build_robot_command(const char *line, char *out, size_t out_len)
{
    const char *p;
    size_t      len;

    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    p      = payload_after_bracket(line);
    if (!p || p[0] == '\0') {
        return false;
    }

    if (strncmp(p, "DPAD_", 5) == 0) {
        const char *d = p + 5;
        len           = strlen(d);
        if (len < 2 || d[len - 1] != '+') {
            return false;
        }
        if ((len == 3 && memcmp(d, "L1", 2) == 0) || (len == 2 && d[0] == 'L')) {
            strncpy(out, "XD", out_len - 1);
        } else if ((len == 3 && memcmp(d, "R1", 2) == 0) || (len == 2 && d[0] == 'R')) {
            strncpy(out, "XG", out_len - 1);
        } else if (len == 2 && d[0] == 'U') {
            strncpy(out, "XL", out_len - 1);
        } else if (len == 2 && d[0] == 'D') {
            strncpy(out, "XS", out_len - 1);
        }
        out[out_len - 1] = '\0';
        return out[0] != '\0';
    }

    if (strncmp(p, "LSTICK ", 7) == 0) {
        int lx = 0;
        int ly = 0;
        return sscanf(p + 7, "%d,%d", &lx, &ly) == 2 && stick_full_cmd(lx, ly, out, out_len);
    }

    if (strncmp(p, "RSTICK ", 7) == 0) {
        int rx = 0;
        int ry = 0;
        if (sscanf(p + 7, "%d,%d", &rx, &ry) != 2) {
            return false;
        }
        if (rx < -125) {
            rx = -125;
        } else if (rx > 125) {
            rx = 125;
        }
        if (ry < -125) {
            ry = -125;
        } else if (ry > 125) {
            ry = 125;
        }
        snprintf(out, out_len, "Jr %d %d", rx, ry);
        return true;
    }

    len = strlen(p);
    if (len < 2 || p[len - 1] != '+') {
        return false;
    }

    char label[20];
    if (len >= sizeof(label)) {
        return false;
    }
    memcpy(label, p, len - 1);
    label[len - 1] = '\0';
    const char *cmd = cmd_for_pressed_label(label);
    if (!cmd) {
        return false;
    }
    strncpy(out, cmd, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

OPERATE_RET quaddle_robot_bridge_init(void)
{
    OPERATE_RET rt;

    quaddle_robot_bridge_reset();
    rt = second_uart_init();
    if (rt != OPRT_OK) {
        return rt;
    }

    if (!s_poll_timer) {
        TUYA_CALL_ERR_LOG(tal_sw_timer_create(poll_timer_cb, NULL, &s_poll_timer));
    }
    if (s_poll_timer) {
        TUYA_CALL_ERR_LOG(tal_sw_timer_start(s_poll_timer, QUADDLE_POLL_INTERVAL_MS, TAL_TIMER_CYCLE));
    }

    if (!s_cli_registered) {
        tal_cli_cmd_register(s_quaddle_cli, 1);
        s_cli_registered = true;
    }

    return OPRT_OK;
}

void quaddle_robot_bridge_reset(void)
{
    s_buttons                 = 0;
    s_last_k_cmd[0]           = '\0';
    s_jr_inited               = false;
    s_last_raw_btn_cmd[0]     = '\0';
    s_r2_allow_next_g         = true;
    s_pending_solo_active     = false;
    s_pending_solo_cmd[0]     = '\0';
    s_pending_solo_btn        = '\0';
    s_pending_stick_active    = false;
    s_pending_stick_cmd[0]    = '\0';
    s_pending_solo_deadline_ms = 0;
    s_pending_stick_deadline_ms = 0;
    s_last_gamepad_input_ms = 0;
}

void quaddle_robot_bridge_poll(void)
{
    uint32_t now = now_ms();

    if (s_pending_solo_active && (int32_t)(now - s_pending_solo_deadline_ms) >= 0) {
        (void)flush_pending_solo();
    }
    if (s_pending_stick_active && (int32_t)(now - s_pending_stick_deadline_ms) >= 0) {
        (void)flush_pending_stick();
    }
}

OPERATE_RET quaddle_robot_bridge_handle_line(const char *line)
{
    const char *p;
    char        cmd[160];

    if (!line) {
        return OPRT_INVALID_PARM;
    }
    mark_gamepad_input();

    p = payload_after_bracket(line);
    if (p && p[0] != '\0') {
        size_t len = strlen(p);
        if (len >= 2 && (p[len - 1] == '+' || p[len - 1] == '-')) {
            char label[20];
            if (len < sizeof(label)) {
                memcpy(label, p, len - 1);
                label[len - 1] = '\0';
                update_button_state(label, p[len - 1] == '+');
            }
        }

        if (len == 2 && is_abxy(p[0]) && (p[1] == '+' || p[1] == '-')) {
            if (p[1] == '+') {
                const char label[2] = {p[0], '\0'};
                const char *solo = cmd_for_pressed_label(label);
                if (solo) {
                    s_pending_solo_btn = p[0];
                    strncpy(s_pending_solo_cmd, solo, sizeof(s_pending_solo_cmd) - 1);
                    s_pending_solo_cmd[sizeof(s_pending_solo_cmd) - 1] = '\0';
                    s_pending_solo_active = true;
                    s_pending_solo_deadline_ms = now_ms() + ABXY_SOLO_DEFER_MS;
                }
                return OPRT_OK;
            }
            if (s_pending_solo_active && p[0] == s_pending_solo_btn) {
                return flush_pending_solo();
            }
            return OPRT_OK;
        }

        if (strcmp(p, "R2-") == 0 || strcmp(p, "ZR-") == 0) {
            s_r2_allow_next_g = true;
        }
    }

    if (!build_robot_command(line, cmd, sizeof(cmd))) {
        return OPRT_OK;
    }

    if (p && strncmp(p, "LSTICK ", 7) == 0) {
        int lx = 0;
        int ly = 0;
        cancel_pending_solo();
        if (sscanf(p + 7, "%d,%d", &lx, &ly) == 2 && zone_uses_delayed_k(lx, ly)) {
            strncpy(s_pending_stick_cmd, cmd, sizeof(s_pending_stick_cmd) - 1);
            s_pending_stick_cmd[sizeof(s_pending_stick_cmd) - 1] = '\0';
            s_pending_stick_active = true;
            s_pending_stick_deadline_ms = now_ms() + STICK_DELAYED_K_MS;
            return OPRT_OK;
        }
    }

    if (cmd[0] == 'k') {
        return send_k_cmd(cmd);
    }
    if (cmd[0] == 'J' && cmd[1] == 'r' && (cmd[2] == ' ' || cmd[2] == '\t')) {
        int rx = 0;
        int ry = 0;
        if (sscanf(cmd, "Jr %d %d", &rx, &ry) != 2) {
            return OPRT_OK;
        }
        cancel_pending_stick();
        if (s_jr_inited && s_last_jr_rx == rx && s_last_jr_ry == ry) {
            return OPRT_OK;
        }
        const uint8_t pkt[5] = {'J', 'r', (uint8_t)(int8_t)rx, (uint8_t)(int8_t)ry, '~'};
        OPERATE_RET rt = second_uart_send_data_force(pkt, sizeof(pkt));
        if (rt == OPRT_OK) {
            s_last_jr_rx = (int16_t)rx;
            s_last_jr_ry = (int16_t)ry;
            s_jr_inited = true;
            s_r2_allow_next_g = true;
        }
        return rt;
    }
    if ((cmd[0] == 'g' || cmd[0] == 'c' || cmd[0] == 'd') && cmd[1] == '\0') {
        return send_raw_btn_cmd(cmd);
    }
    if (is_dpad_x_cmd(cmd)) {
        cancel_pending_stick();
        s_r2_allow_next_g = true;
        return second_uart_send_string_force(cmd);
    }
    return OPRT_OK;
}

BOOL_T quaddle_robot_bridge_gamepad_active(void)
{
    return quaddle_robot_bridge_gamepad_active_remaining_ms() > 0;
}

uint32_t quaddle_robot_bridge_gamepad_active_remaining_ms(void)
{
    uint32_t elapsed;

    if (s_last_gamepad_input_ms == 0) {
        return 0;
    }
    elapsed = now_ms() - s_last_gamepad_input_ms;
    if (elapsed >= GAMEPAD_PRIORITY_MS) {
        return 0;
    }
    return GAMEPAD_PRIORITY_MS - elapsed;
}

#endif /* ENABLE_CHAT_BOT_ROBOT_SECOND_UART */
