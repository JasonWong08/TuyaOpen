/**
 * @file quaddle_robot_bridge.c
 * @brief C port of QuaddleRobotBridge UART command mapping.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_cli.h"
#include "tal_system.h"

#include "quaddle_robot_bridge.h"
#include "second_uart.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

#define LSTICK_DEAD_ABS_MAX        5
#define LSTICK_EDGE_ABS            120
#define LSTICK_MODERATE_ENTER_ABS  10
#define LSTICK_MODERATE_MAX_ABS    100
#define QUADDLE_CMD_MAX            48
#define ABXY_SOLO_DEFER_MS         200
#define STICK_DELAYED_K_MS         100
#define QUADDLE_POLL_INTERVAL_MS   20
#define GAMEPAD_PRIORITY_MS        500
#define AI_COMMAND_ARBITRATION_MS  120
#define AI_COMMAND_QUEUE_MAX       8
#define AI_COMMAND_TOKEN_TIMEOUT_MS 20000
#define AI_COMMAND_ASR_MCP_DEDUPE_MS 5000
#define PLUS_LONG_PRESS_MS         1000
#define ZL_LONG_PRESS_MS           1000
#define QUADDLE_MACRO_MAX_STEPS    48

typedef enum {
    QUADDLE_BTN_L1    = 1u << 0,
    QUADDLE_BTN_R1    = 1u << 1,
    QUADDLE_BTN_B     = 1u << 2,
    QUADDLE_BTN_A     = 1u << 3,
    QUADDLE_BTN_Y     = 1u << 4,
    QUADDLE_BTN_X     = 1u << 5,
    QUADDLE_BTN_MINUS = 1u << 6,
    QUADDLE_BTN_PLUS  = 1u << 7,
    QUADDLE_BTN_ZL    = 1u << 9,
    QUADDLE_BTN_ZR    = 1u << 10,
} QUADDLE_BTN_E;

enum {
    QUADDLE_MACRO_STEP_TEXT = 0,
    QUADDLE_MACRO_STEP_JR,
};

typedef struct {
    uint8_t  type;
    uint16_t delta_ms;
    char     text[QUADDLE_CMD_MAX];
    int8_t   jr_rx;
    int8_t   jr_ry;
} QUADDLE_MACRO_STEP_T;

typedef enum {
    QUADDLE_NON_GAIT_HOLD_NONE = 0,
    QUADDLE_NON_GAIT_HOLD_SOLO,
    QUADDLE_NON_GAIT_HOLD_R1_COMBO,
} QUADDLE_NON_GAIT_HOLD_KIND_E;

static const char *const s_stick_cmd_base[6][9] = {
    {"up", "wkF", "wkL", "wkR", "wkB", "wkF", "vtL", "vtR", "wkB"},
    {"up", "trF", "trL", "trR", "bdF", "trF", "trL", "trR", "slide"},
    {"up", "crF", "crL", "crR", "dragWkF", "crF", "crL", "crR", "dragWkF"},
    {"up", "ff", "flipRoll", "jumpSlide", "bf", "ff", "flipRoll", "jumpSlide", "bf"},
    {"up", "tiptoeF", "sideL", "sideR", "marchF", "tiptoeF", "vt2L", "vt2R", "paceF"},
    {"", "triCatF", "triCatL", "triCatR", "circle2L", "triCatF", "triCatL", "triCatR", "qDance"},
};

static const char s_minus_default_cmd[] = "T";

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
static char     s_last_combo_k_cmd[QUADDLE_CMD_MAX];
static bool     s_o_gu_upper_u;
static TIMER_ID s_poll_timer;
static bool     s_cli_registered;
static uint32_t s_last_gamepad_input_ms;
static char     s_solo_non_gait_hold_latch[QUADDLE_CMD_MAX];
static char     s_r1_combo_non_gait_hold_latch[QUADDLE_CMD_MAX];

static QUADDLE_MACRO_STEP_T s_macro_steps[QUADDLE_MACRO_MAX_STEPS];
static uint8_t  s_macro_step_count;
static bool     s_macro_recording;
static bool     s_macro_playing;
static bool     s_macro_has_first_step;
static uint32_t s_macro_last_record_ms;
static uint8_t  s_macro_play_index;
static uint32_t s_macro_play_next_ms;
static bool     s_macro_injecting;
static bool     s_plus_held;
static uint32_t s_plus_down_ms;
static bool     s_plus_long_fired;
static bool     s_zl_held;
static uint32_t s_zl_down_ms;
static bool     s_zl_long_fired;
static bool     s_pending_ai_active;
static uint32_t s_pending_ai_deadline_ms;
static char     s_pending_ai_cmd[QUADDLE_CMD_MAX];
static char     s_pending_ai_source[12];
static bool     s_ai_waiting_token;
static char     s_ai_expected_token;
static uint32_t s_ai_token_deadline_ms;
static char     s_ai_queue_cmd[AI_COMMAND_QUEUE_MAX][QUADDLE_CMD_MAX];
static char     s_ai_queue_source[AI_COMMAND_QUEUE_MAX][12];
static uint8_t  s_ai_queue_head;
static uint8_t  s_ai_queue_count;
static char     s_ai_token_line[64];
static uint8_t  s_ai_token_line_len;
static char     s_last_asr_cmd[QUADDLE_CMD_MAX];
static char     s_last_asr_cmd_head[16];
static uint32_t s_last_asr_cmd_ms;

static int quaddle_abs(int v)
{
    return v < 0 ? -v : v;
}

static uint32_t now_ms(void)
{
    return (uint32_t)tal_system_get_millisecond();
}

static char ai_command_token(const char *cmd)
{
    while (cmd && (*cmd == ' ' || *cmd == '\t')) {
        cmd++;
    }
    return (cmd && cmd[0] != '\0') ? cmd[0] : '\0';
}

static void ai_token_line_reset(void)
{
    s_ai_token_line_len = 0;
    s_ai_token_line[0]  = '\0';
}

static bool ai_token_line_matches_expected(void)
{
    char *start = s_ai_token_line;
    char *end   = s_ai_token_line + s_ai_token_line_len;

    while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }

    return (end - start) == 1 && start[0] == s_ai_expected_token;
}

static void ai_command_head(const char *cmd, char *out, size_t out_len)
{
    size_t n = 0;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    while (cmd && (*cmd == ' ' || *cmd == '\t')) {
        cmd++;
    }
    while (cmd && cmd[n] != '\0' && cmd[n] != ' ' && cmd[n] != '\t' && n + 1 < out_len) {
        out[n] = cmd[n];
        n++;
    }
    out[n] = '\0';
}

static bool ai_source_is(const char *source, const char *name)
{
    return source && name && strcmp(source, name) == 0;
}

static bool ai_command_same_or_similar(const char *cmd_a, const char *head_a, const char *cmd_b)
{
    char head_b[sizeof(s_last_asr_cmd_head)];

    if (!cmd_a || !cmd_b || cmd_a[0] == '\0' || cmd_b[0] == '\0') {
        return false;
    }
    if (strcmp(cmd_a, cmd_b) == 0) {
        return true;
    }

    ai_command_head(cmd_b, head_b, sizeof(head_b));
    if (!head_a || head_a[0] == '\0' || head_b[0] == '\0' || strcmp(head_a, head_b) != 0) {
        return false;
    }

    /* Same k-action with different parameter is still the same physical action
     * for ASR/MCP duplicate suppression, e.g. "kbkF 5" vs "kbkF 3". */
    return head_a[0] == 'k';
}

static void ai_record_asr_command(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') {
        return;
    }
    strncpy(s_last_asr_cmd, cmd, sizeof(s_last_asr_cmd) - 1);
    s_last_asr_cmd[sizeof(s_last_asr_cmd) - 1] = '\0';
    ai_command_head(cmd, s_last_asr_cmd_head, sizeof(s_last_asr_cmd_head));
    s_last_asr_cmd_ms = now_ms();
}

static bool ai_should_drop_mcp_duplicate(const char *cmd)
{
    uint32_t now;

    if (!cmd || s_last_asr_cmd[0] == '\0' || s_last_asr_cmd_ms == 0) {
        return false;
    }
    now = now_ms();
    if ((uint32_t)(now - s_last_asr_cmd_ms) > AI_COMMAND_ASR_MCP_DEDUPE_MS) {
        return false;
    }
    return ai_command_same_or_similar(s_last_asr_cmd, s_last_asr_cmd_head, cmd);
}

static void clear_ai_command_queue(void)
{
    s_pending_ai_active      = false;
    s_pending_ai_deadline_ms = 0;
    s_pending_ai_cmd[0]      = '\0';
    s_pending_ai_source[0]   = '\0';
    s_ai_waiting_token       = false;
    s_ai_expected_token      = '\0';
    s_ai_token_deadline_ms   = 0;
    ai_token_line_reset();
    s_ai_queue_head          = 0;
    s_ai_queue_count         = 0;
    s_last_asr_cmd[0]        = '\0';
    s_last_asr_cmd_head[0]   = '\0';
    s_last_asr_cmd_ms        = 0;
}

static const char *payload_after_bracket(const char *line)
{
    const char *p;

    if (!line) {
        return NULL;
    }
    p = strchr(line, ']');
    if (!p) {
        return NULL;
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

    if (ax <= LSTICK_DEAD_ABS_MAX && ay <= LSTICK_DEAD_ABS_MAX) {
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

static const char *lstick_no_mod_sector_base(int lx, int ly)
{
    static const float k_half = 0.3926990817f;
    float              a;

    if (quaddle_abs(lx) <= LSTICK_DEAD_ABS_MAX && quaddle_abs(ly) <= LSTICK_DEAD_ABS_MAX) {
        return "up";
    }
    a = atan2f((float)lx, (float)ly);
    if (a >= -k_half && a < k_half) {
        return "wkF";
    }
    if (a >= k_half && a < 3.0f * k_half) {
        return "wkR";
    }
    if (a >= 3.0f * k_half && a < 5.0f * k_half) {
        return "vtR";
    }
    if (a >= 5.0f * k_half && a < 7.0f * k_half) {
        return "bkR";
    }
    if (a >= 7.0f * k_half || a < -7.0f * k_half) {
        return "bkF";
    }
    if (a >= -7.0f * k_half && a < -5.0f * k_half) {
        return "bkL";
    }
    if (a >= -5.0f * k_half && a < -3.0f * k_half) {
        return "vtL";
    }
    return "wkL";
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
    int         z;
    const uint8_t row = stick_modifier_row();

    if (!out || n == 0) {
        return false;
    }
    out[0] = '\0';
    if (row == 0) {
        format_k_cmd(lstick_no_mod_sector_base(lx, ly), out, n);
        return out[0] != '\0';
    }
    z = classify_stick_zone(lx, ly);
    if (z < 0 || z > 8) {
        return false;
    }
    format_k_cmd(s_stick_cmd_base[row][z], out, n);
    return out[0] != '\0';
}

static bool lstick_in_dead_zone(int lx, int ly)
{
    return quaddle_abs(lx) <= LSTICK_DEAD_ABS_MAX && quaddle_abs(ly) <= LSTICK_DEAD_ABS_MAX;
}

static bool r1_lstick_override_cmd(int lx, int ly, char *out, size_t n)
{
    int z;
    const char *base = NULL;

    if (!out || n == 0) {
        return false;
    }
    out[0] = '\0';
    if ((s_buttons & QUADDLE_BTN_R1) == 0 || lstick_in_dead_zone(lx, ly)) {
        return false;
    }
    z = classify_stick_zone(lx, ly);
    switch (z) {
    case 1:
    case 5:
        base = "qBiped";
        break;
    case 4:
    case 8:
        base = "qScoot";
        break;
    case 2:
    case 6:
        base = "qFrontScoot";
        break;
    case 3:
    case 7:
        base = "glide";
        break;
    default:
        return false;
    }
    format_k_cmd(base, out, n);
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
    } else if (strcmp(label, "MINUS") == 0) {
        mask = QUADDLE_BTN_MINUS;
    } else if (strcmp(label, "PLUS") == 0) {
        mask = QUADDLE_BTN_PLUS;
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
        return "d";
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
        return "kqWheel";
    }
    if (strcmp(label, "R1") == 0) {
        return NULL;
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

static void cancel_deferred(void)
{
    cancel_pending_solo();
    cancel_pending_stick();
}

static void macro_stop_playback(void)
{
    s_macro_playing = false;
    s_macro_play_index = 0;
    s_macro_play_next_ms = 0;
}

static void macro_reset(void)
{
    memset(s_macro_steps, 0, sizeof(s_macro_steps));
    s_macro_step_count = 0;
    s_macro_recording = false;
    s_macro_playing = false;
    s_macro_has_first_step = false;
    s_macro_last_record_ms = 0;
    s_macro_play_index = 0;
    s_macro_play_next_ms = 0;
    s_macro_injecting = false;
    s_plus_held = false;
    s_plus_down_ms = 0;
    s_plus_long_fired = false;
    s_zl_held = false;
    s_zl_down_ms = 0;
    s_zl_long_fired = false;
}

static void macro_log_count(const char *verb)
{
    PR_NOTICE("quaddle macro: %s %u step(s)", verb ? verb : "state", s_macro_step_count);
}

static void macro_begin_record_clear(void)
{
    macro_stop_playback();
    cancel_deferred();
    memset(s_macro_steps, 0, sizeof(s_macro_steps));
    s_macro_step_count = 0;
    s_macro_has_first_step = false;
    s_macro_recording = true;
    s_macro_last_record_ms = now_ms();
    PR_NOTICE("quaddle macro: recording (hold PLUS 1s), tap PLUS to stop");
}

static void macro_on_plus_short_press(void)
{
    if (s_macro_playing) {
        macro_stop_playback();
        PR_NOTICE("quaddle macro: playback stopped");
        return;
    }
    if (s_macro_recording) {
        s_macro_recording = false;
        macro_log_count("saved");
        return;
    }
    if (s_macro_step_count > 0) {
        s_macro_playing = true;
        s_macro_play_index = 0;
        s_macro_play_next_ms = now_ms();
        macro_log_count("playing");
    }
}

static void macro_poll_plus_hold(bool plus_held)
{
    uint32_t now = now_ms();

    if (plus_held) {
        if (!s_plus_held) {
            s_plus_held = true;
            s_plus_down_ms = now;
            s_plus_long_fired = false;
        } else if (!s_plus_long_fired && (uint32_t)(now - s_plus_down_ms) >= PLUS_LONG_PRESS_MS) {
            s_plus_long_fired = true;
            macro_begin_record_clear();
        }
        return;
    }

    if (s_plus_held) {
        if (!s_plus_long_fired) {
            macro_on_plus_short_press();
        }
        s_plus_held = false;
        s_plus_long_fired = false;
    }
}

static void macro_before_robot_output(void)
{
    if (s_macro_playing && !s_macro_injecting) {
        macro_stop_playback();
        PR_NOTICE("quaddle macro: playback interrupted");
    }
}

static uint16_t macro_record_delta_ms(void)
{
    uint32_t now = now_ms();
    uint32_t delta;

    if (!s_macro_has_first_step) {
        s_macro_has_first_step = true;
        s_macro_last_record_ms = now;
        return 0;
    }
    delta = now - s_macro_last_record_ms;
    if (delta > 65535u) {
        delta = 65535u;
    }
    s_macro_last_record_ms = now;
    return (uint16_t)delta;
}

static void macro_record_text_line(const char *line)
{
    QUADDLE_MACRO_STEP_T *step;

    if (!s_macro_recording || !line || line[0] == '\0' || s_macro_step_count >= QUADDLE_MACRO_MAX_STEPS) {
        return;
    }
    step = &s_macro_steps[s_macro_step_count++];
    step->type = QUADDLE_MACRO_STEP_TEXT;
    step->delta_ms = macro_record_delta_ms();
    strncpy(step->text, line, sizeof(step->text) - 1);
    step->text[sizeof(step->text) - 1] = '\0';
}

static void macro_record_jr_packet(int rx, int ry)
{
    QUADDLE_MACRO_STEP_T *step;

    if (!s_macro_recording || s_macro_step_count >= QUADDLE_MACRO_MAX_STEPS) {
        return;
    }
    step = &s_macro_steps[s_macro_step_count++];
    step->type = QUADDLE_MACRO_STEP_JR;
    step->delta_ms = macro_record_delta_ms();
    step->jr_rx = (int8_t)rx;
    step->jr_ry = (int8_t)ry;
    step->text[0] = '\0';
}

static void macro_emit_step(const QUADDLE_MACRO_STEP_T *step)
{
    if (!step) {
        return;
    }

    s_macro_injecting = true;
    if (step->type == QUADDLE_MACRO_STEP_JR) {
        const uint8_t pkt[5] = {'J', 'r', (uint8_t)step->jr_rx, (uint8_t)step->jr_ry, '~'};
        (void)second_uart_send_data_force(pkt, sizeof(pkt));
    } else if (step->text[0] != '\0') {
        (void)second_uart_send_string_force(step->text);
    }
    s_macro_injecting = false;
}

static void macro_poll_playback(void)
{
    uint32_t now = now_ms();

    if (!s_macro_playing || s_macro_step_count == 0 || (int32_t)(now - s_macro_play_next_ms) < 0) {
        return;
    }

    macro_emit_step(&s_macro_steps[s_macro_play_index]);
    s_macro_play_index++;
    if (s_macro_play_index >= s_macro_step_count) {
        macro_stop_playback();
        PR_NOTICE("quaddle macro: playback done");
        return;
    }
    s_macro_play_next_ms = now + s_macro_steps[s_macro_play_index].delta_ms;
}

static void remember_combo_k_cmd(const char *cmd)
{
    if (!cmd || cmd[0] != 'k' || cmd[1] == '\0' || strcmp(cmd, "kup") == 0) {
        return;
    }
    if (stick_modifier_row() == 0) {
        return;
    }
    strncpy(s_last_combo_k_cmd, cmd, sizeof(s_last_combo_k_cmd) - 1);
    s_last_combo_k_cmd[sizeof(s_last_combo_k_cmd) - 1] = '\0';
}

static bool non_gait_solo_k_cmd(const char *cmd)
{
    return strcmp(cmd, "khds") == 0 || strcmp(cmd, "khi") == 0 || strcmp(cmd, "kqStepHalf") == 0 ||
           strcmp(cmd, "kqStep") == 0 || strcmp(cmd, "kqWheel") == 0;
}

static bool non_gait_r1_combo_k_cmd(const char *cmd)
{
    return strcmp(cmd, "kqBiped") == 0 || strcmp(cmd, "kqScoot") == 0 ||
           strcmp(cmd, "kqFrontScoot") == 0 || strcmp(cmd, "kglide") == 0;
}

static bool non_gait_k_cmd(const char *cmd)
{
    return cmd && cmd[0] == 'k' && (non_gait_solo_k_cmd(cmd) || non_gait_r1_combo_k_cmd(cmd));
}

static char *non_gait_hold_latch_for(QUADDLE_NON_GAIT_HOLD_KIND_E hold_kind)
{
    if (hold_kind == QUADDLE_NON_GAIT_HOLD_SOLO) {
        return s_solo_non_gait_hold_latch;
    }
    if (hold_kind == QUADDLE_NON_GAIT_HOLD_R1_COMBO) {
        return s_r1_combo_non_gait_hold_latch;
    }
    return NULL;
}

static bool non_gait_hold_should_emit(const char *cmd, QUADDLE_NON_GAIT_HOLD_KIND_E hold_kind)
{
    char *latch = non_gait_hold_latch_for(hold_kind);

    if (!latch) {
        return true;
    }
    if (hold_kind == QUADDLE_NON_GAIT_HOLD_SOLO && !non_gait_solo_k_cmd(cmd)) {
        return true;
    }
    if (hold_kind == QUADDLE_NON_GAIT_HOLD_R1_COMBO && !non_gait_r1_combo_k_cmd(cmd)) {
        return true;
    }
    return latch[0] == '\0' || strcmp(latch, cmd) != 0;
}

static void non_gait_hold_commit(const char *cmd, QUADDLE_NON_GAIT_HOLD_KIND_E hold_kind)
{
    char *latch = non_gait_hold_latch_for(hold_kind);

    if (!latch) {
        return;
    }
    if (hold_kind == QUADDLE_NON_GAIT_HOLD_SOLO && !non_gait_solo_k_cmd(cmd)) {
        return;
    }
    if (hold_kind == QUADDLE_NON_GAIT_HOLD_R1_COMBO && !non_gait_r1_combo_k_cmd(cmd)) {
        return;
    }
    strncpy(latch, cmd, QUADDLE_CMD_MAX - 1);
    latch[QUADDLE_CMD_MAX - 1] = '\0';
}

static void non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_KIND_E hold_kind)
{
    char *latch = non_gait_hold_latch_for(hold_kind);

    if (latch) {
        latch[0] = '\0';
    }
}

static QUADDLE_NON_GAIT_HOLD_KIND_E non_gait_hold_kind_for_payload(const char *p, const char *cmd)
{
    if (!non_gait_k_cmd(cmd) || !p) {
        return QUADDLE_NON_GAIT_HOLD_NONE;
    }
    if (strncmp(p, "LSTICK ", 7) == 0) {
        return QUADDLE_NON_GAIT_HOLD_R1_COMBO;
    }
    return QUADDLE_NON_GAIT_HOLD_SOLO;
}

static void maybe_release_non_gait_hold_on_edge(const char *p)
{
    if (!p) {
        return;
    }
    if (strcmp(p, "R1-") == 0) {
        non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_R1_COMBO);
        return;
    }
    if (strcmp(p, "L1-") == 0 || strcmp(p, "ZL-") == 0 || strcmp(p, "L2-") == 0) {
        non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_SOLO);
    }
}

static bool k_cmd_should_emit(const char *cmd, QUADDLE_NON_GAIT_HOLD_KIND_E hold_kind)
{
    if (!cmd || cmd[0] != 'k') {
        return true;
    }
    if (hold_kind != QUADDLE_NON_GAIT_HOLD_NONE && non_gait_k_cmd(cmd)) {
        return non_gait_hold_should_emit(cmd, hold_kind);
    }
    if (non_gait_k_cmd(cmd)) {
        return true;
    }
    return strcmp(s_last_k_cmd, cmd) != 0;
}

static OPERATE_RET send_combo_replay_k_cmd(const char *cmd)
{
    OPERATE_RET rt;

    if (!cmd || cmd[0] != 'k') {
        return OPRT_INVALID_PARM;
    }
    cancel_pending_stick();
    macro_before_robot_output();
    rt = second_uart_send_string_force(cmd);
    if (rt == OPRT_OK) {
        strncpy(s_last_k_cmd, cmd, sizeof(s_last_k_cmd) - 1);
        s_last_k_cmd[sizeof(s_last_k_cmd) - 1] = '\0';
        s_r2_allow_next_g = true;
        macro_record_text_line(cmd);
    }
    return rt;
}

static OPERATE_RET send_k_cmd_ex(const char *cmd, QUADDLE_NON_GAIT_HOLD_KIND_E hold_kind)
{
    OPERATE_RET rt;

    if (!cmd || cmd[0] != 'k') {
        return OPRT_INVALID_PARM;
    }
    cancel_pending_stick();
    if (!k_cmd_should_emit(cmd, hold_kind)) {
        return OPRT_OK;
    }
    remember_combo_k_cmd(cmd);
    macro_before_robot_output();
    rt = non_gait_k_cmd(cmd) ? second_uart_send_string_force(cmd) : second_uart_send_string(cmd);
    if (rt == OPRT_OK) {
        non_gait_hold_commit(cmd, hold_kind);
        strncpy(s_last_k_cmd, cmd, sizeof(s_last_k_cmd) - 1);
        s_last_k_cmd[sizeof(s_last_k_cmd) - 1] = '\0';
        s_r2_allow_next_g = true;
        macro_record_text_line(cmd);
    }
    return rt;
}

static OPERATE_RET send_k_cmd(const char *cmd)
{
    return send_k_cmd_ex(cmd, QUADDLE_NON_GAIT_HOLD_NONE);
}

static void mark_gamepad_input(void)
{
    s_last_gamepad_input_ms = now_ms();
    if (s_pending_ai_active || s_ai_waiting_token || s_ai_queue_count > 0) {
        PR_NOTICE("robot arbitration: gamepad superseded AI command queue");
        clear_ai_command_queue();
    }
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
    macro_before_robot_output();
    rt = second_uart_send_string_force(cmd);
    if (rt == OPRT_OK) {
        strncpy(s_last_raw_btn_cmd, cmd, sizeof(s_last_raw_btn_cmd) - 1);
        s_last_raw_btn_cmd[sizeof(s_last_raw_btn_cmd) - 1] = '\0';
        if (cmd[0] == 'g' && cmd[1] == '\0') {
            s_r2_allow_next_g = false;
        }
        macro_record_text_line(cmd);
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
    return (cmd[0] == 'k') ? send_k_cmd_ex(cmd, QUADDLE_NON_GAIT_HOLD_SOLO) : send_raw_btn_cmd(cmd);
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
    remember_combo_k_cmd(cmd);
    return send_k_cmd(cmd);
}

static bool zone_uses_delayed_k(int lx, int ly)
{
    const uint8_t row = stick_modifier_row();
    int           z;

    if (s_buttons & QUADDLE_BTN_R1) {
        return false;
    }
    if (row == 0) {
        return true;
    }
    z = classify_stick_zone(lx, ly);
    return z == 0 || (z >= 1 && z <= 4);
}

static const char *next_ot_gb_cmd(void)
{
    return s_o_gu_upper_u ? "gU" : "gu";
}

static OPERATE_RET send_ot_gb_toggle(void)
{
    const char *cmd = next_ot_gb_cmd();
    OPERATE_RET rt;

    s_o_gu_upper_u = !s_o_gu_upper_u;
    cancel_pending_stick();
    macro_before_robot_output();
    rt = second_uart_send_string_force(cmd);
    if (rt == OPRT_OK) {
        s_r2_allow_next_g = true;
        macro_record_text_line(cmd);
    }
    return rt;
}

static OPERATE_RET send_ot_key_t(void)
{
    OPERATE_RET rt;

    cancel_pending_stick();
    macro_before_robot_output();
    rt = second_uart_send_string_force("T");
    if (rt == OPRT_OK) {
        s_r2_allow_next_g = true;
        macro_record_text_line("T");
    }
    return rt;
}

static OPERATE_RET send_minus_default(void)
{
    OPERATE_RET rt;

    cancel_pending_stick();
    macro_before_robot_output();
    rt = second_uart_send_string_force(s_minus_default_cmd);
    if (rt == OPRT_OK) {
        s_r2_allow_next_g = true;
        macro_record_text_line(s_minus_default_cmd);
    }
    return rt;
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

static void poll_zl_hold(void)
{
    uint32_t now = now_ms();
    bool zl_held = (s_buttons & QUADDLE_BTN_ZL) != 0;

    if (zl_held) {
        if (!s_zl_held) {
            s_zl_held = true;
            s_zl_down_ms = now;
            s_zl_long_fired = false;
        } else if (!s_zl_long_fired && (uint32_t)(now - s_zl_down_ms) >= ZL_LONG_PRESS_MS) {
            s_zl_long_fired = true;
            (void)send_k_cmd_ex("kqStep", QUADDLE_NON_GAIT_HOLD_SOLO);
        }
        return;
    }
    if (s_zl_held) {
        if (!s_zl_long_fired) {
            (void)send_k_cmd_ex("kqStepHalf", QUADDLE_NON_GAIT_HOLD_SOLO);
        }
        non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_SOLO);
        s_zl_held = false;
        s_zl_long_fired = false;
    }
}

static void poll_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;
    quaddle_robot_bridge_poll();
}

#if defined(ENABLE_QUADDLE_GAMEPAD) && ENABLE_QUADDLE_GAMEPAD
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
#endif

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
    if (strncmp(p, "FIELD4 ", 7) == 0) {
        return false;
    }

    if (strncmp(p, "LSTICK ", 7) == 0) {
        int lx = 0;
        int ly = 0;
        if (sscanf(p + 7, "%d,%d", &lx, &ly) != 2) {
            return false;
        }
        if (r1_lstick_override_cmd(lx, ly, out, out_len)) {
            return true;
        }
        return stick_full_cmd(lx, ly, out, out_len);
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
    if (strcmp(label, "MINUS") == 0) {
        const char *minus_cmd = s_last_combo_k_cmd[0] != '\0' ? s_last_combo_k_cmd : s_minus_default_cmd;
        strncpy(out, minus_cmd, out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
    if (strcmp(label, "O") == 0) {
        strncpy(out, next_ot_gb_cmd(), out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
    if (strcmp(label, "T") == 0) {
        strncpy(out, "T", out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
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

#if defined(ENABLE_QUADDLE_GAMEPAD) && ENABLE_QUADDLE_GAMEPAD
    if (!s_cli_registered) {
        tal_cli_cmd_register(s_quaddle_cli, 1);
        s_cli_registered = true;
    }
#endif

    return OPRT_OK;
}

void quaddle_robot_bridge_reset(void)
{
    s_buttons                    = 0;
    s_last_k_cmd[0]              = '\0';
    s_jr_inited                  = false;
    s_last_raw_btn_cmd[0]        = '\0';
    s_r2_allow_next_g            = true;
    s_pending_solo_active        = false;
    s_pending_solo_cmd[0]        = '\0';
    s_pending_solo_btn           = '\0';
    s_pending_stick_active       = false;
    s_pending_stick_cmd[0]       = '\0';
    s_pending_solo_deadline_ms   = 0;
    s_pending_stick_deadline_ms = 0;
    s_last_combo_k_cmd[0]       = '\0';
    s_o_gu_upper_u              = false;
    s_last_gamepad_input_ms     = 0;
    s_solo_non_gait_hold_latch[0] = '\0';
    s_r1_combo_non_gait_hold_latch[0] = '\0';
    clear_ai_command_queue();
    macro_reset();
}

void quaddle_robot_bridge_poll(void)
{
    uint32_t now = now_ms();
    bool     schedule_next = false;

#if defined(ENABLE_QUADDLE_GAMEPAD) && ENABLE_QUADDLE_GAMEPAD
    if (s_pending_solo_active && (int32_t)(now - s_pending_solo_deadline_ms) >= 0) {
        (void)flush_pending_solo();
    }
    if (s_pending_stick_active && (int32_t)(now - s_pending_stick_deadline_ms) >= 0) {
        (void)flush_pending_stick();
    }
#endif
    if (s_ai_waiting_token && (int32_t)(now - s_ai_token_deadline_ms) >= 0) {
        PR_WARN("robot arbitration: token '%c' timeout after AI command \"%s\"", s_ai_expected_token,
                s_pending_ai_cmd);
        s_ai_waiting_token     = false;
        s_ai_expected_token    = '\0';
        s_ai_token_deadline_ms = 0;
        ai_token_line_reset();
        s_pending_ai_cmd[0]    = '\0';
        s_pending_ai_source[0] = '\0';
        s_ai_queue_head        = 0;
        s_ai_queue_count       = 0;
        PR_WARN("robot arbitration: AI command queue cleared after token timeout");
    }
    if (!s_pending_ai_active && !s_ai_waiting_token && s_ai_queue_count > 0) {
        schedule_next = true;
    }
    if (schedule_next && !s_pending_ai_active && !s_ai_waiting_token && s_ai_queue_count > 0) {
        strncpy(s_pending_ai_cmd, s_ai_queue_cmd[s_ai_queue_head], sizeof(s_pending_ai_cmd) - 1);
        s_pending_ai_cmd[sizeof(s_pending_ai_cmd) - 1] = '\0';
        strncpy(s_pending_ai_source, s_ai_queue_source[s_ai_queue_head], sizeof(s_pending_ai_source) - 1);
        s_pending_ai_source[sizeof(s_pending_ai_source) - 1] = '\0';
        s_ai_queue_head = (uint8_t)((s_ai_queue_head + 1) % AI_COMMAND_QUEUE_MAX);
        s_ai_queue_count--;
        s_pending_ai_deadline_ms = now + AI_COMMAND_ARBITRATION_MS;
        s_pending_ai_active      = true;
        PR_NOTICE("robot arbitration: AI command scheduled \"%s\" from %s for %ums", s_pending_ai_cmd,
                  s_pending_ai_source, AI_COMMAND_ARBITRATION_MS);
    }
    if (s_pending_ai_active && (int32_t)(now - s_pending_ai_deadline_ms) >= 0) {
        if (quaddle_robot_bridge_gamepad_active()) {
            PR_NOTICE("robot arbitration: skipped AI command \"%s\"; gamepad priority active", s_pending_ai_cmd);
            s_pending_ai_active = false;
            s_pending_ai_cmd[0] = '\0';
        } else {
            /* ASR/MCP dedupe is handled when queued; each accepted AI command must reach the robot. */
            OPERATE_RET rt = second_uart_send_string_force(s_pending_ai_cmd);
            if (rt == OPRT_OK) {
                s_ai_expected_token    = ai_command_token(s_pending_ai_cmd);
                s_ai_token_deadline_ms = now + AI_COMMAND_TOKEN_TIMEOUT_MS;
                s_ai_waiting_token     = (s_ai_expected_token != '\0');
                ai_token_line_reset();
                PR_NOTICE("robot arbitration: AI command sent \"%s\" from %s", s_pending_ai_cmd,
                          s_pending_ai_source);
                if (s_ai_waiting_token) {
                    PR_NOTICE("robot arbitration: waiting token '%c' for \"%s\"", s_ai_expected_token,
                              s_pending_ai_cmd);
                }
            } else {
                PR_ERR("robot arbitration: AI command send failed %d", rt);
            }
            s_pending_ai_active = false;
        }
    }
#if defined(ENABLE_QUADDLE_GAMEPAD) && ENABLE_QUADDLE_GAMEPAD
    macro_poll_plus_hold((s_buttons & QUADDLE_BTN_PLUS) != 0);
    poll_zl_hold();
    macro_poll_playback();
#endif
}

OPERATE_RET quaddle_robot_bridge_handle_line(const char *line)
{
#if defined(ENABLE_QUADDLE_GAMEPAD) && ENABLE_QUADDLE_GAMEPAD
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
        maybe_release_non_gait_hold_on_edge(p);

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
                OPERATE_RET rt = flush_pending_solo();
                non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_SOLO);
                return rt;
            }
            non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_SOLO);
            return OPRT_OK;
        }

        if (strcmp(p, "R2-") == 0 || strcmp(p, "ZR-") == 0) {
            s_r2_allow_next_g = true;
        }

        if (strcmp(p, "PLUS+") == 0 || strcmp(p, "PLUS-") == 0 ||
            strcmp(p, "ZL+") == 0 || strcmp(p, "ZL-") == 0 ||
            strcmp(p, "L2+") == 0 || strcmp(p, "L2-") == 0) {
            return OPRT_OK;
        }

        if (s_macro_playing) {
            macro_before_robot_output();
        }

        if (strcmp(p, "MINUS+") == 0) {
            if (s_last_combo_k_cmd[0] != '\0') {
                return send_combo_replay_k_cmd(s_last_combo_k_cmd);
            }
            return send_minus_default();
        }

        if (strcmp(p, "O+") == 0) {
            return send_ot_gb_toggle();
        }

        if (strcmp(p, "T+") == 0) {
            return send_ot_key_t();
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
            remember_combo_k_cmd(cmd);
            return OPRT_OK;
        }
        if (sscanf(p + 7, "%d,%d", &lx, &ly) == 2) {
            char r1cmd[QUADDLE_CMD_MAX];
            if (r1_lstick_override_cmd(lx, ly, r1cmd, sizeof(r1cmd))) {
                return send_k_cmd_ex(r1cmd, QUADDLE_NON_GAIT_HOLD_R1_COMBO);
            }
            non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_R1_COMBO);
        }
    }

    if (cmd[0] == 'k') {
        return send_k_cmd_ex(cmd, non_gait_hold_kind_for_payload(p, cmd));
    }
    if (cmd[0] == 'J' && cmd[1] == 'r' && (cmd[2] == ' ' || cmd[2] == '\t')) {
        int rx = 0;
        int ry = 0;
        OPERATE_RET rt;
        if (sscanf(cmd, "Jr %d %d", &rx, &ry) != 2) {
            return OPRT_OK;
        }
        cancel_pending_stick();
        if (s_jr_inited && s_last_jr_rx == rx && s_last_jr_ry == ry) {
            return OPRT_OK;
        }
        const uint8_t pkt[5] = {'J', 'r', (uint8_t)(int8_t)rx, (uint8_t)(int8_t)ry, '~'};
        macro_before_robot_output();
        rt = second_uart_send_data_force(pkt, sizeof(pkt));
        if (rt == OPRT_OK) {
            s_last_jr_rx = (int16_t)rx;
            s_last_jr_ry = (int16_t)ry;
            s_jr_inited = true;
            s_r2_allow_next_g = true;
            macro_record_jr_packet(rx, ry);
        }
        return rt;
    }
    if ((cmd[0] == 'g' || cmd[0] == 'c' || cmd[0] == 'd') && cmd[1] == '\0') {
        return send_raw_btn_cmd(cmd);
    }
    if (is_dpad_x_cmd(cmd)) {
        OPERATE_RET rt;

        cancel_pending_stick();
        s_r2_allow_next_g = true;
        macro_before_robot_output();
        rt = second_uart_send_string_force(cmd);
        if (rt == OPRT_OK) {
            macro_record_text_line(cmd);
        }
        return rt;
    }
    return OPRT_OK;
#else
    (void)line;
    return OPRT_NOT_SUPPORTED;
#endif
}

OPERATE_RET quaddle_robot_bridge_queue_ai_command(const char *cmd, const char *source)
{
    uint8_t tail;
    const char *src = source ? source : "AI";

    if (!cmd || cmd[0] == '\0') {
        return OPRT_INVALID_PARM;
    }
    if (quaddle_robot_bridge_gamepad_active()) {
        PR_NOTICE("robot arbitration: AI command \"%s\" from %s skipped; gamepad priority active %ums", cmd,
                  src, quaddle_robot_bridge_gamepad_active_remaining_ms());
        return OPRT_OK;
    }
    if (ai_source_is(src, "MCP") && ai_should_drop_mcp_duplicate(cmd)) {
        PR_NOTICE("robot arbitration: MCP command \"%s\" skipped; recent ASR command \"%s\" already queued",
                  cmd, s_last_asr_cmd);
        return OPRT_OK;
    }

    if (s_ai_queue_count >= AI_COMMAND_QUEUE_MAX) {
        PR_WARN("robot arbitration: AI queue full, drop \"%s\" from %s", cmd, src);
        return OPRT_COM_ERROR;
    }

    tail = (uint8_t)((s_ai_queue_head + s_ai_queue_count) % AI_COMMAND_QUEUE_MAX);
    strncpy(s_ai_queue_cmd[tail], cmd, sizeof(s_ai_queue_cmd[tail]) - 1);
    s_ai_queue_cmd[tail][sizeof(s_ai_queue_cmd[tail]) - 1] = '\0';
    strncpy(s_ai_queue_source[tail], src, sizeof(s_ai_queue_source[tail]) - 1);
    s_ai_queue_source[tail][sizeof(s_ai_queue_source[tail]) - 1] = '\0';
    s_ai_queue_count++;
    if (ai_source_is(src, "ASR")) {
        ai_record_asr_command(s_ai_queue_cmd[tail]);
    }
    PR_NOTICE("robot arbitration: AI command queued \"%s\" from %s depth=%u", s_ai_queue_cmd[tail],
              s_ai_queue_source[tail], s_ai_queue_count);
    return OPRT_OK;
}

void quaddle_robot_bridge_handle_robot_token(char token)
{
    if (token == '\0') {
        return;
    }

    if (!s_ai_waiting_token) {
        ai_token_line_reset();
        return;
    }

    if (token == '\r' || token == '\n') {
        if (s_ai_token_line_len == 0) {
            return;
        }
        s_ai_token_line[s_ai_token_line_len] = '\0';
        if (!ai_token_line_matches_expected()) {
            PR_DEBUG("robot arbitration: ignored robot token line \"%s\", waiting '%c'", s_ai_token_line,
                     s_ai_expected_token);
            ai_token_line_reset();
            return;
        }
    } else {
        if (s_ai_token_line_len + 1 >= sizeof(s_ai_token_line)) {
            PR_WARN("robot arbitration: robot token line overflow, waiting '%c'", s_ai_expected_token);
            ai_token_line_reset();
            return;
        }
        s_ai_token_line[s_ai_token_line_len++] = token;
        return;
    }

    PR_NOTICE("robot arbitration: robot token '%c' completed \"%s\"", s_ai_expected_token, s_pending_ai_cmd);
    s_ai_waiting_token     = false;
    s_ai_expected_token    = '\0';
    s_ai_token_deadline_ms = 0;
    ai_token_line_reset();
    s_pending_ai_cmd[0]    = '\0';
    s_pending_ai_source[0] = '\0';
}

BOOL_T quaddle_robot_bridge_gamepad_active(void)
{
    return quaddle_robot_bridge_gamepad_active_remaining_ms() > 0;
}

uint32_t quaddle_robot_bridge_gamepad_active_remaining_ms(void)
{
#if defined(ENABLE_QUADDLE_GAMEPAD) && ENABLE_QUADDLE_GAMEPAD
    uint32_t elapsed;

    if (s_last_gamepad_input_ms == 0) {
        return 0;
    }
    elapsed = now_ms() - s_last_gamepad_input_ms;
    if (elapsed >= GAMEPAD_PRIORITY_MS) {
        return 0;
    }
    return GAMEPAD_PRIORITY_MS - elapsed;
#else
    return 0;
#endif
}

#endif /* ENABLE_CHAT_BOT_ROBOT_SECOND_UART */
