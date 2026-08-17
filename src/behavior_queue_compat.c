/*
 * Compatibility shim for DYA runtime-sensor-rotate on ZMK bases that do not
 * export zmk_behavior_queue_add().
 *
 * The symbol is weak, so if the selected ZMK tree provides the real behavior
 * queue implementation, that implementation wins.
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>

#ifndef CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE
#define CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE 64
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct cygnus_behavior_queue_item {
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    struct zmk_behavior_binding binding;
    bool press : 1;
    uint32_t wait : 31;
};

K_MSGQ_DEFINE(cygnus_behavior_queue_msgq, sizeof(struct cygnus_behavior_queue_item),
              CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE, 4);

static void cygnus_behavior_queue_process_next(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(cygnus_queue_work, cygnus_behavior_queue_process_next);

static void cygnus_behavior_queue_process_next(struct k_work *work) {
    struct cygnus_behavior_queue_item item = {.wait = 0};

    while (k_msgq_get(&cygnus_behavior_queue_msgq, &item, K_NO_WAIT) == 0) {
        struct zmk_behavior_binding_event event = {
            .position = item.position,
            .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = item.source,
#endif
        };

        zmk_behavior_invoke_binding(&item.binding, event, item.press);

        if (item.wait > 0) {
            k_work_schedule(&cygnus_queue_work, K_MSEC(item.wait));
            break;
        }
    }
}

__attribute__((weak)) int
zmk_behavior_queue_add(const struct zmk_behavior_binding_event *event,
                       const struct zmk_behavior_binding binding, bool press, uint32_t wait) {
    struct cygnus_behavior_queue_item item = {
        .press = press,
        .binding = binding,
        .wait = wait,
        .position = event->position,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = event->source,
#endif
    };

    const int ret = k_msgq_put(&cygnus_behavior_queue_msgq, &item, K_NO_WAIT);
    if (ret < 0) {
        return ret;
    }

    if (!k_work_delayable_is_pending(&cygnus_queue_work)) {
        cygnus_behavior_queue_process_next(&cygnus_queue_work.work);
    }

    return 0;
}
