#include <android/keycodes.h>
#include <linux/input-event-codes.h>

#include "backend/termuxgui.h"

static void send_pointer_position(struct wlr_tgui_output *output,
                                  double x,
                                  double y,
                                  uint32_t time_ms) {
    struct wlr_pointer_motion_absolute_event ev = {
        .pointer = &output->pointer,
        .time_msec = time_ms,
        .x = x,
        .y = y,
    };
    wl_signal_emit_mutable(&output->pointer.events.motion_absolute, &ev);
    wl_signal_emit_mutable(&output->pointer.events.frame, &output->pointer);
}

static void send_pointer_button(struct wlr_tgui_output *output,
                                uint32_t button,
                                enum wlr_button_state state,
                                uint32_t time_ms) {
    struct wlr_pointer_button_event ev = {
        .pointer = &output->pointer,
        .time_msec = time_ms,
        .button = button,
        .state = state,
    };
    wl_signal_emit_mutable(&output->pointer.events.button, &ev);
    wl_signal_emit_mutable(&output->pointer.events.frame, &output->pointer);
}

static void send_pointer_axis(struct wlr_tgui_output *output,
                              int32_t delta,
                              uint64_t time_ms) {
    struct wlr_pointer_axis_event ev = {
        .pointer = &output->pointer,
        .time_msec = time_ms,
        .source = WLR_AXIS_SOURCE_WHEEL,
        .orientation = WLR_AXIS_ORIENTATION_VERTICAL,
        .delta = delta * 15,
        .delta_discrete = delta * WLR_POINTER_AXIS_DISCRETE_STEP,
    };
    wl_signal_emit_mutable(&output->pointer.events.axis, &ev);
    wl_signal_emit_mutable(&output->pointer.events.frame, &output->pointer);
}

static void move_cursor(struct wlr_tgui_output *output,
                        double dx,
                        double dy,
                        uint32_t time_ms) {
    output->cursor_x -= dx;
    output->cursor_y -= dy;

    if (output->cursor_x < 0)
        output->cursor_x = 0;
    if (output->cursor_x > 1)
        output->cursor_x = 1;

    if (output->cursor_y < 0)
        output->cursor_y = 0;
    if (output->cursor_y > 1)
        output->cursor_y = 1;

    send_pointer_position(output, output->cursor_x, output->cursor_y,
                          time_ms);
}

void handle_touch_event(tgui_event *e,
                        struct wlr_tgui_output *output,
                        uint64_t time_ms) {
    switch (e->touch.action) {
    case TGUI_TOUCH_DOWN: {
        tgui_touch_pointer *p = &e->touch.pointers[e->touch.index][0];
        memset(&output->touch_pointer, 0, sizeof(output->touch_pointer));
        output->touch_pointer.id = p->id;
        output->touch_pointer.x = (double) p->x / output->width;
        output->touch_pointer.y = (double) p->y / output->height;
        output->touch_pointer.time_ms = time_ms;
        break;
    }
    case TGUI_TOUCH_UP:
    case TGUI_TOUCH_POINTER_UP: {
        tgui_touch_pointer *p = &e->touch.pointers[e->touch.index][0];
        if (p->id == output->touch_pointer.id) {
            if (time_ms - output->touch_pointer.time_ms < 200 &&
                output->touch_pointer.moved == false) {
                send_pointer_button(output, BTN_LEFT, WLR_BUTTON_PRESSED,
                                    time_ms++);
                output->touch_pointer.down = true;
            }
            if (output->touch_pointer.down) {
                send_pointer_button(output, BTN_LEFT, WLR_BUTTON_RELEASED,
                                    time_ms);
                output->touch_pointer.down = false;
            }
        }
        break;
    }
    case TGUI_TOUCH_MOVE: {
        for (uint32_t i = 0u; i < e->touch.num_pointers; i++) {
            tgui_touch_pointer *p = &e->touch.pointers[0][i];
            if (p->id != output->touch_pointer.id) {
                break;
            }
            double x = (double) p->x / output->width;
            double y = (double) p->y / output->height;
            double dx = output->touch_pointer.x - x;
            double dy = output->touch_pointer.y - y;
            if (dx + dy > 0.0f || dx + dy < -0.0f) {
                output->touch_pointer.dx = dx;
                output->touch_pointer.dy = dy;
                output->touch_pointer.x -= dx;
                output->touch_pointer.y -= dy;
                output->touch_pointer.moved = true;
            }
            if (output->touch_pointer.moved == true &&
                e->touch.num_pointers == 2) {
                static double s;
                s += output->touch_pointer.dy;
                if (s > (double) 150 / output->height) {
                    send_pointer_axis(output, 1, time_ms);
                    s = 0;
                } else if (s < (double) -150 / output->height) {
                    send_pointer_axis(output, -1, time_ms);
                    s = 0;
                }
            } else if (output->touch_pointer.moved == false &&
                       output->touch_pointer.down == false &&
                       time_ms - output->touch_pointer.time_ms > 150 &&
                       e->touch.num_pointers == 2) {
                send_pointer_button(output, BTN_RIGHT, WLR_BUTTON_PRESSED,
                                    time_ms);
                send_pointer_button(output, BTN_RIGHT, WLR_BUTTON_RELEASED,
                                    time_ms);
                output->touch_pointer.moved = true;
            } else if (output->touch_pointer.moved == false &&
                       output->touch_pointer.down == false &&
                       time_ms - output->touch_pointer.time_ms > 200) {
                send_pointer_button(output, BTN_LEFT, WLR_BUTTON_PRESSED,
                                    time_ms);
                output->touch_pointer.down = true;
            } else if (output->touch_pointer.moved) {
                move_cursor(output, output->touch_pointer.dx,
                            output->touch_pointer.dy, time_ms);
            }
        }
        break;
    }
    default: {
        break;
    }
    }
}

const struct {
    int android;
    int linux;
} keymap[] = {
    { AKEYCODE_0, KEY_0 },           { AKEYCODE_1, KEY_1 },
    { AKEYCODE_2, KEY_2 },           { AKEYCODE_3, KEY_3 },
    { AKEYCODE_4, KEY_4 },           { AKEYCODE_5, KEY_5 },
    { AKEYCODE_6, KEY_6 },           { AKEYCODE_7, KEY_7 },
    { AKEYCODE_8, KEY_8 },           { AKEYCODE_9, KEY_9 },
    { AKEYCODE_A, KEY_A },           { AKEYCODE_B, KEY_B },
    { AKEYCODE_C, KEY_C },           { AKEYCODE_D, KEY_D },
    { AKEYCODE_E, KEY_E },           { AKEYCODE_F, KEY_F },
    { AKEYCODE_G, KEY_G },           { AKEYCODE_H, KEY_H },
    { AKEYCODE_I, KEY_I },           { AKEYCODE_J, KEY_J },
    { AKEYCODE_K, KEY_K },           { AKEYCODE_L, KEY_L },
    { AKEYCODE_M, KEY_M },           { AKEYCODE_N, KEY_N },
    { AKEYCODE_O, KEY_O },           { AKEYCODE_P, KEY_P },
    { AKEYCODE_Q, KEY_Q },           { AKEYCODE_R, KEY_R },
    { AKEYCODE_S, KEY_S },           { AKEYCODE_T, KEY_T },
    { AKEYCODE_U, KEY_U },           { AKEYCODE_V, KEY_V },
    { AKEYCODE_W, KEY_W },           { AKEYCODE_X, KEY_X },
    { AKEYCODE_Y, KEY_Y },           { AKEYCODE_Z, KEY_Z },
    { AKEYCODE_ENTER, KEY_ENTER },   { AKEYCODE_SPACE, KEY_SPACE },
    { AKEYCODE_DEL, KEY_BACKSPACE }, { AKEYCODE_SHIFT_LEFT, KEY_LEFTSHIFT },
    { AKEYCODE_COMMA, KEY_COMMA },   { AKEYCODE_PERIOD, KEY_DOT },
};

static int android_keycode_to_linux(int code) {
    for (size_t i = 0; i < sizeof(keymap) / sizeof(*keymap); i++) {
        if (code == keymap[i].android) {
            return keymap[i].linux;
        }
    }

    return KEY_UNKNOWN;
}

void handle_keyboard_event(tgui_event *e,
                           struct wlr_tgui_output *output,
                           uint64_t time_ms) {
    int keycode;
    if (e->key.code == 4 /* back */) {
        tgui_focus(output->backend->conn, output->tgui_activity,
                   output->tgui_surfaceview, true);
    } else if ((keycode = android_keycode_to_linux(e->key.code)) !=
               KEY_UNKNOWN) {
        static bool shift;
        if (keycode == KEY_LEFTSHIFT) {
            shift = true;
            return;
        }

        struct wlr_keyboard_key_event key = {
            .time_msec = time_ms,
            .keycode = keycode,
            .state = e->key.down ? WL_KEYBOARD_KEY_STATE_PRESSED
                                 : WL_KEYBOARD_KEY_STATE_RELEASED,
            .update_state = true,
        };
        struct wlr_keyboard_key_event shift_key = {
            .time_msec = time_ms,
            .keycode = KEY_LEFTSHIFT,
            .update_state = true,
        };

        if (shift) {
            shift_key.state = WL_KEYBOARD_KEY_STATE_PRESSED,
            wlr_keyboard_notify_key(&output->keyboard, &shift_key);
        }
        wlr_keyboard_notify_key(&output->keyboard, &key);
        if (shift) {
            shift_key.state = WL_KEYBOARD_KEY_STATE_RELEASED;
            wlr_keyboard_notify_key(&output->keyboard, &shift_key);
            shift = false;
        }
    } else {
        wlr_log(WLR_ERROR, "Unhandled keycode %d", e->key.code);
    }
}
