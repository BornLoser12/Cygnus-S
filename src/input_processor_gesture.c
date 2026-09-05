/*
 * Cygnus trackball gesture input processor.
 *
 * Converts dominant X/Y relative movement into one of four behavior taps.
 * Binding order is right, left, down, up.
 */

#include <stdlib.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <drivers/input_processor.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/events/position_state_changed.h>
#endif

#define DT_DRV_COMPAT cygnus_input_processor_gesture

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum cygnus_gesture_direction {
    CYGNUS_GESTURE_RIGHT = 0,
    CYGNUS_GESTURE_LEFT = 1,
    CYGNUS_GESTURE_DOWN = 2,
    CYGNUS_GESTURE_UP = 3,
    CYGNUS_GESTURE_NONE = 255,
};

struct cygnus_gesture_config {
    uint8_t index;
    const struct zmk_behavior_binding *bindings;
    uint32_t tick;
    int32_t threshold;
    int32_t max_threshold;
    uint32_t tap_ms;
    uint32_t wait_ms;
    int32_t max_delta;
    bool track_remainders;
};

struct cygnus_gesture_data {
    const struct device *dev;
    struct k_work_delayable release_work;
    int32_t report_x;
    int32_t report_y;
    int32_t delta_x;
    int32_t delta_y;
    uint32_t position;
    int64_t last_triggered_at;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    uint8_t pressed_binding;
};

static uint32_t approx_hypot(uint32_t a, uint32_t b) {
    if (a < b) {
        uint32_t tmp = a;
        a = b;
        b = tmp;
    }

    return a + ((b * 3) >> 3);
}

static struct zmk_behavior_binding_event
gesture_binding_event(const struct cygnus_gesture_data *data) {
    return (struct zmk_behavior_binding_event) {
        .position = data->position,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = data->source,
#endif
    };
}

static void release_pressed_binding(struct cygnus_gesture_data *data,
                                    const struct cygnus_gesture_config *cfg) {
    if (data->pressed_binding == CYGNUS_GESTURE_NONE) {
        return;
    }

    struct zmk_behavior_binding_event event = gesture_binding_event(data);
    zmk_behavior_invoke_binding(&cfg->bindings[data->pressed_binding], event, false);
    data->pressed_binding = CYGNUS_GESTURE_NONE;
}

static void release_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct cygnus_gesture_data *data =
        CONTAINER_OF(dwork, struct cygnus_gesture_data, release_work);
    const struct cygnus_gesture_config *cfg = data->dev->config;

    release_pressed_binding(data, cfg);
}

static void add_dominant_movement(struct cygnus_gesture_data *data,
                                  const struct cygnus_gesture_config *cfg) {
    int32_t x = data->report_x;
    int32_t y = data->report_y;
    int32_t abs_x = abs(x);
    int32_t abs_y = abs(y);

    data->report_x = 0;
    data->report_y = 0;

    if (abs_x < cfg->threshold && abs_y < cfg->threshold) {
        return;
    }

    uint32_t movement = approx_hypot(abs_x, abs_y);

    if (abs_x >= abs_y) {
        data->delta_x =
            CLAMP(data->delta_x + (x < 0 ? -(int32_t)movement : (int32_t)movement),
                  -cfg->max_delta, cfg->max_delta);
    } else {
        data->delta_y =
            CLAMP(data->delta_y + (y < 0 ? -(int32_t)movement : (int32_t)movement),
                  -cfg->max_delta, cfg->max_delta);
    }
}

static enum cygnus_gesture_direction pick_direction(struct cygnus_gesture_data *data,
                                                    const struct cygnus_gesture_config *cfg) {
    int32_t abs_x = abs(data->delta_x);
    int32_t abs_y = abs(data->delta_y);

    if (abs_x < cfg->tick && abs_y < cfg->tick) {
        return CYGNUS_GESTURE_NONE;
    }

    if (abs_x >= abs_y) {
        if (data->delta_x > 0) {
            if (cfg->track_remainders) {
                data->delta_x -= cfg->tick;
            }
            data->delta_y = 0;
            return CYGNUS_GESTURE_RIGHT;
        }

        if (cfg->track_remainders) {
            data->delta_x += cfg->tick;
        }
        data->delta_y = 0;
        return CYGNUS_GESTURE_LEFT;
    }

    if (data->delta_y > 0) {
        if (cfg->track_remainders) {
            data->delta_y -= cfg->tick;
        }
        data->delta_x = 0;
        return CYGNUS_GESTURE_UP;
    }

    if (cfg->track_remainders) {
        data->delta_y += cfg->tick;
    }
    data->delta_x = 0;
    return CYGNUS_GESTURE_DOWN;
}

static int trigger_binding(struct cygnus_gesture_data *data,
                           const struct cygnus_gesture_config *cfg,
                           enum cygnus_gesture_direction direction) {
    int64_t now = k_uptime_get();

    if (cfg->wait_ms > 0 && data->last_triggered_at > 0 &&
        now - data->last_triggered_at < cfg->wait_ms) {
        return 0;
    }

    k_work_cancel_delayable(&data->release_work);
    release_pressed_binding(data, cfg);

    struct zmk_behavior_binding_event event = gesture_binding_event(data);
    int ret = zmk_behavior_invoke_binding(&cfg->bindings[direction], event, true);
    if (ret < 0) {
        return ret;
    }

    data->pressed_binding = direction;
    data->last_triggered_at = now;
    k_work_schedule(&data->release_work, K_MSEC(cfg->tap_ms));

    if (!cfg->track_remainders) {
        data->delta_x = 0;
        data->delta_y = 0;
    }

    return 0;
}

static int cygnus_gesture_handle_event(const struct device *dev, struct input_event *event,
                                       uint32_t param1, uint32_t param2,
                                       struct zmk_input_processor_state *state) {
    const struct cygnus_gesture_config *cfg = dev->config;
    struct cygnus_gesture_data *data = dev->data;
    int32_t value = event->value;

    ARG_UNUSED(param1);
    ARG_UNUSED(param2);

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    data->position =
        ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(state->input_device_index, cfg->index);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    data->source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    if (abs(value) > cfg->max_threshold) {
        data->report_x = 0;
        data->report_y = 0;
        return ZMK_INPUT_PROC_STOP;
    }

    if (event->code == INPUT_REL_X) {
        data->report_x = value;
    } else {
        data->report_y = value;
    }

    if (!event->sync) {
        return ZMK_INPUT_PROC_STOP;
    }

    add_dominant_movement(data, cfg);
    enum cygnus_gesture_direction direction = pick_direction(data, cfg);

    if (direction != CYGNUS_GESTURE_NONE) {
        trigger_binding(data, cfg, direction);
    }

    return ZMK_INPUT_PROC_STOP;
}

static struct zmk_input_processor_driver_api cygnus_gesture_driver_api = {
    .handle_event = cygnus_gesture_handle_event,
};

static int cygnus_gesture_init(const struct device *dev) {
    struct cygnus_gesture_data *data = dev->data;

    data->dev = dev;
    data->pressed_binding = CYGNUS_GESTURE_NONE;
    k_work_init_delayable(&data->release_work, release_work_cb);

    return 0;
}

#define GESTURE_BINDINGS(n)                                                                        \
    {LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))}

#define CYGNUS_GESTURE_INST(n)                                                                     \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, bindings) == 4, "Cygnus gesture requires 4 bindings");       \
    static struct cygnus_gesture_data cygnus_gesture_data_##n = {                                  \
        .pressed_binding = CYGNUS_GESTURE_NONE,                                                    \
    };                                                                                             \
    static struct zmk_behavior_binding cygnus_gesture_bindings_##n[] = GESTURE_BINDINGS(n);        \
    static const struct cygnus_gesture_config cygnus_gesture_config_##n = {                        \
        .index = n,                                                                                \
        .bindings = cygnus_gesture_bindings_##n,                                                   \
        .tick = DT_INST_PROP_OR(n, tick, 80),                                                      \
        .threshold = DT_INST_PROP_OR(n, threshold, 2),                                             \
        .max_threshold = DT_INST_PROP_OR(n, max_threshold, 200),                                   \
        .tap_ms = DT_INST_PROP_OR(n, tap_ms, 35),                                                  \
        .wait_ms = DT_INST_PROP_OR(n, wait_ms, 120),                                               \
        .max_delta = DT_INST_PROP_OR(n, max_pending_activations, 2) *                              \
                     DT_INST_PROP_OR(n, tick, 80),                                                 \
        .track_remainders = DT_INST_PROP_OR(n, track_remainders, false),                           \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, &cygnus_gesture_init, NULL, &cygnus_gesture_data_##n,                 \
                          &cygnus_gesture_config_##n, POST_KERNEL,                                 \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cygnus_gesture_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CYGNUS_GESTURE_INST)
