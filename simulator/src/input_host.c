/**
 * Host implementation of the input service.
 *
 * The device debounces GPIO and derives the logical event types from tick
 * counts. Here the transitions arrive already clean from SDL, so this only
 * has to reproduce the event grammar the UI reacts to: Press, then Long and
 * repeating Repeat while held, and Release followed by Short if the hold was
 * brief.
 *
 * The five mode keys are not buttons. On the device they are the positions of
 * a rotary selector, so what matters is not the edge but which position is
 * currently selected: the service publishes the events like any other key and
 * separately holds the position in a FuriState, which is what the desktop
 * service watches to decide which app should be running. Pressing a mode key
 * here means "move the lever to that position".
 *
 * Timings mirror applications/services/input/input.c.
 */
#include "input_host.h"
#include "sim_window.h"

#include <furi.h>

#include <input/input.h>

#define TAG "InputHost"

#define INPUT_PRESS_TICKS       (150)
#define INPUT_LONG_PRESS_COUNTS (2)

#define INPUT_POLL_PERIOD_MS (10)
#define INPUT_LONG_PRESS_MS  (INPUT_PRESS_TICKS * INPUT_LONG_PRESS_COUNTS)

/* The device gives the selector this long to report where it is parked before
 * assuming the first position, so that a boot with the lever already on BUSY
 * still reaches the busy app. Nothing reports a position here unless a key is
 * pressed, so in practice this is how long the simulator shows the startup app.
 */
#define INPUT_SWITCH_STARTUP_TIMEOUT_MS (1500)

#define INPUT_SWITCH_RANGE_START InputKeyBusy
#define INPUT_SWITCH_RANGE_END   InputKeySettings

typedef struct {
    bool pressed;
    uint32_t pressed_at;
    bool long_sent;
    uint32_t last_repeat;
} InputKeyState;

static FuriPubSub* input_pubsub;
static uint32_t input_sequence_counter;

/* The device's struct of the same name carries the pin table and the event
 * loop; here the selector position is the only thing callers reach through the
 * record, but it has to be a real object because desktop subscribes to it. */
struct Input {
    FuriState* switch_pos;
};

static Input input_instance;

/* Mirrors the state above for the SDL thread, which draws the selected lever
 * position and must not take furi's locks. */
static _Atomic InputKey input_switch_key = InputKeyMAX;

/* The enum carries Left and Right, but the device has no such buttons: it
 * scrolls with the dial, and the firmware's own key tables agree -- the ten in
 * api_input.c and the ten the state publisher translates both stop at the dial,
 * Ok, Back, Start and the five selector positions. A subscriber that meets a
 * key the hardware cannot produce is entitled to treat it as impossible, and
 * the state publisher does: it asserts. So the simulator refuses to invent
 * buttons rather than expecting every subscriber to tolerate them. */
static bool input_host_key_exists(InputKey key) {
    switch(key) {
    case InputKeyLeft:
    case InputKeyRight:
        return false;
    default:
        return key < InputKeyMAX;
    }
}

static void input_host_publish(InputKey key, InputType type, uint32_t sequence) {
    const InputEvent event = {
        .key = key,
        .type = type,
        .sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE,
        .sequence_number = sequence,
    };

    furi_pubsub_publish(input_pubsub, (void*)&event);
}

static void input_host_set_switch_pos(InputSwitchPosition position) {
    InputSwitchPosition current;
    furi_state_get(input_instance.switch_pos, &current);

    if(current == position) return;

    input_switch_key = (InputKey)(position + INPUT_SWITCH_RANGE_START);
    furi_state_set(input_instance.switch_pos, &position);
}

InputKey input_host_get_switch_key(void) {
    return input_switch_key;
}

static int32_t input_host_srv(void* context) {
    UNUSED(context);

    InputKeyState states[InputKeyMAX] = {0};
    uint32_t sequences[InputKeyMAX] = {0};

    const uint32_t started_at = furi_get_tick();
    bool switch_startup_elapsed = false;

    for(;;) {
        uint8_t raw_key;
        bool pressed;

        while(sim_window_poll_key(&raw_key, &pressed)) {
            const InputKey key = (InputKey)raw_key;

            if(!input_host_key_exists(key)) {
                FURI_LOG_W(TAG, "no button %u on the device, ignoring", (unsigned)key);
                continue;
            }

            InputKeyState* state = &states[key];

            if(pressed && key >= INPUT_SWITCH_RANGE_START && key <= INPUT_SWITCH_RANGE_END) {
                switch_startup_elapsed = true;
                input_host_set_switch_pos(
                    (InputSwitchPosition)(key - INPUT_SWITCH_RANGE_START));
            }

            if(pressed && !state->pressed) {
                state->pressed = true;
                state->pressed_at = furi_get_tick();
                state->long_sent = false;
                state->last_repeat = state->pressed_at;
                sequences[key] = ++input_sequence_counter;

                input_host_publish(key, InputTypePress, sequences[key]);

            } else if(!pressed && state->pressed) {
                const bool was_long = state->long_sent;
                state->pressed = false;

                /* Short comes first, as it does on the device: the release
                 * handler there publishes it before the release itself. */
                if(!was_long) {
                    input_host_publish(key, InputTypeShort, sequences[key]);
                }
                input_host_publish(key, InputTypeRelease, sequences[key]);
            }
        }

        const uint32_t now = furi_get_tick();

        if(!switch_startup_elapsed && (now - started_at) >= INPUT_SWITCH_STARTUP_TIMEOUT_MS) {
            switch_startup_elapsed = true;
            input_host_set_switch_pos(InputSwitchPositionBusy);
        }

        for(InputKey key = 0; key < InputKeyMAX; key++) {
            InputKeyState* state = &states[key];
            if(!state->pressed) continue;

            if(!state->long_sent && (now - state->pressed_at) >= INPUT_LONG_PRESS_MS) {
                state->long_sent = true;
                state->last_repeat = now;
                input_host_publish(key, InputTypeLong, sequences[key]);

            } else if(state->long_sent && (now - state->last_repeat) >= INPUT_PRESS_TICKS) {
                state->last_repeat = now;
                input_host_publish(key, InputTypeRepeat, sequences[key]);
            }
        }

        furi_delay_ms(INPUT_POLL_PERIOD_MS);
    }

    return 0;
}

void input_host_init(void) {
    input_pubsub = furi_pubsub_alloc();

    input_instance.switch_pos = furi_state_alloc(sizeof(InputSwitchPosition));
    InputSwitchPosition unknown = InputSwitchPositionMAX;
    furi_state_set(input_instance.switch_pos, &unknown);

    furi_record_create(RECORD_INPUT_EVENTS, input_pubsub);
    furi_record_create(RECORD_INPUT, &input_instance);

    FuriThread* thread = furi_thread_alloc_ex("input_host", 4096, input_host_srv, NULL);
    furi_thread_start(thread);
}

/* A press and a release, which is what /api/input?key=... asks for. Both land
 * in the same queue the keyboard feeds, so an API-driven press is
 * indistinguishable from a real one by the time the UI sees it. */
void input_key_toggle(Input* input, InputKey key) {
    UNUSED(input);

    sim_window_inject_key((uint8_t)key, true);
    sim_window_inject_key((uint8_t)key, false);
}

FuriState* input_get_switch_pos(Input* input) {
    furi_check(input);
    return input->switch_pos;
}

/* Mirrors applications/services/input/input.c, whose translation unit also
 * carries the button-matrix driver and so cannot be linked here. */
const char* input_get_key_name(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return "InputKeyUp";
    case InputKeyDown:
        return "InputKeyDown";
    case InputKeyRight:
        return "InputKeyRight";
    case InputKeyLeft:
        return "InputKeyLeft";
    case InputKeyOk:
        return "InputKeyOk";
    case InputKeyBack:
        return "InputKeyBack";
    case InputKeyStart:
        return "InputKeyStart";
    case InputKeyBusy:
        return "InputKeyBusy";
    case InputKeyCustom:
        return "InputKeyCustom";
    case InputKeyOff:
        return "InputKeyOff";
    case InputKeyApps:
        return "InputKeyApps";
    case InputKeySettings:
        return "InputKeySettings";
    default:
        furi_crash();
    }
}
