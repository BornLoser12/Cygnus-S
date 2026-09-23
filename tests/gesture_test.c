/* Deterministic host doubles: no Bluetooth, USB or firmware settings are modified. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CONFIG_ZMK_SPLIT 1
#define IS_ENABLED(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONTAINER_OF(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define LOG_WRN(...) ((void)0)
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define INPUT_EV_REL 2
#define INPUT_REL_X 0
#define INPUT_REL_Y 1
#define ZMK_INPUT_PROC_CONTINUE 0
#define ZMK_INPUT_PROC_STOP 1
#define ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL 0
#define ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(d, i) (1000 + (d) * 10 + (i))

typedef long atomic_t;
typedef long atomic_val_t;
static atomic_val_t atomic_get(const atomic_t *v) { return *v; }
static atomic_val_t atomic_inc(atomic_t *v) { return (*v)++; }
struct device { const void *config; void *data; };
struct k_work { void (*handler)(struct k_work *); bool queued; };
struct k_work_delayable { struct k_work work; bool pending; int64_t due; };
struct k_msgq { char *buffer; size_t size, capacity, read, write, used; };
struct input_event { uint16_t type, code; int32_t value; bool sync; };
struct zmk_input_processor_state { uint8_t input_device_index; };
struct zmk_behavior_binding { uint32_t param1; };
struct zmk_behavior_binding_event { uint32_t position; int64_t timestamp; uint8_t source; };
struct zmk_input_processor_driver_api {
    int (*handle_event)(const struct device *, struct input_event *, uint32_t, uint32_t,
                        struct zmk_input_processor_state *);
};

static int64_t now;
static bool active, in_sysworkq, fail_press, fail_schedule, fail_submit;
static struct k_work *queue[64];
static size_t queue_count;
static struct k_work_delayable *timer;
static struct { uint32_t binding; bool pressed; } calls[128];
static size_t call_count;
static bool held[5];
static int64_t k_uptime_get(void) { return now; }
static bool zmk_keymap_layer_active(uint8_t layer) { return layer == 8 && active; }
static void k_work_init(struct k_work *w, void (*handler)(struct k_work *)) {
    *w = (struct k_work){.handler = handler};
}
static void k_work_init_delayable(struct k_work_delayable *w, void (*handler)(struct k_work *)) {
    *w = (struct k_work_delayable){0};
    k_work_init(&w->work, handler);
    timer = w;
}
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *w) {
    return CONTAINER_OF(w, struct k_work_delayable, work);
}
static int k_work_submit(struct k_work *w) {
    if (fail_submit) { fail_submit = false; return -EIO; }
    if (!w->queued) {
        assert(queue_count < ARRAY_SIZE(queue));
        queue[queue_count++] = w;
        w->queued = true;
    }
    return 1;
}
static int k_work_cancel_delayable(struct k_work_delayable *w) {
    assert(in_sysworkq);
    w->pending = false;
    for (size_t i = 0; i < queue_count;) {
        if (queue[i] == &w->work) {
            memmove(&queue[i], &queue[i + 1], (--queue_count - i) * sizeof(queue[0]));
        } else { i++; }
    }
    w->work.queued = false;
    return 0;
}
static int k_work_reschedule(struct k_work_delayable *w, int delay) {
    assert(in_sysworkq);
    if (fail_schedule) { fail_schedule = false; return -EIO; }
    w->pending = true;
    w->due = now + delay;
    return 1;
}
static void k_msgq_init(struct k_msgq *q, char *buffer, size_t size, size_t capacity) {
    *q = (struct k_msgq){.buffer = buffer, .size = size, .capacity = capacity};
}
static int k_msgq_put(struct k_msgq *q, const void *item, int timeout) {
    assert(timeout == K_NO_WAIT);
    if (q->used == q->capacity) { return -ENOMSG; }
    memcpy(q->buffer + q->write * q->size, item, q->size);
    q->write = (q->write + 1) % q->capacity;
    q->used++;
    return 0;
}
static int k_msgq_get(struct k_msgq *q, void *item, int timeout) {
    assert(timeout == K_NO_WAIT);
    if (!q->used) { return -ENOMSG; }
    memcpy(item, q->buffer + q->read * q->size, q->size);
    q->read = (q->read + 1) % q->capacity;
    q->used--;
    return 0;
}
static size_t k_msgq_num_used_get(struct k_msgq *q) { return q->used; }
static int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *b,
                                       struct zmk_behavior_binding_event event, bool press) {
    assert(in_sysworkq); // Both press and release must use the same execution context.
    assert(event.position == 1020 && event.source == 0);
    assert(b->param1 >= 1 && b->param1 <= 4);
    assert(call_count < ARRAY_SIZE(calls));
    calls[call_count].binding = b->param1;
    calls[call_count++].pressed = press;
    if (press) { assert(!held[b->param1]); }
    held[b->param1] = press;
    if (press && fail_press) { fail_press = false; return -EIO; }
    return 0;
}

/* Unmodified production function bodies, extracted by run_gesture_tests.py. */
#include "gesture_core.inc"

static struct cygnus_gesture_data data;
static const struct zmk_behavior_binding bindings[] = {{1}, {2}, {3}, {4}};
static const struct cygnus_gesture_config config = {
    .index = 0, .layer = 8, .bindings = bindings, .tick = 133, .threshold = 2,
    .max_threshold = 200, .tap_ms = 35, .wait_ms = 120, .max_delta = 266,
    .track_remainders = true,
};
static const struct device dev = {.config = &config, .data = &data};
static void pump(void) {
    size_t iterations = 0;
    while (queue_count) {
        assert(++iterations < 100);
        struct k_work *w = queue[0];
        memmove(queue, queue + 1, --queue_count * sizeof(queue[0]));
        w->queued = false;
        in_sysworkq = true;
        w->handler(w);
        in_sysworkq = false;
    }
}
static void advance(int milliseconds) {
    now += milliseconds;
    if (timer->pending && timer->due <= now) {
        timer->pending = false;
        k_work_submit(&timer->work);
    }
    pump();
}
static void layer(bool on) {
    active = on;
    gesture_layer_changed(&dev, 8);
}
static void sample(int code, int value, bool sync) {
    struct input_event ev = {.type = INPUT_EV_REL, .code = code, .value = value, .sync = sync};
    struct zmk_input_processor_state state = {.input_device_index = 2};
    assert(cygnus_gesture_driver_api.handle_event(&dev, &ev, 0, 0, &state) == ZMK_INPUT_PROC_STOP);
}
static void start(void) {
    memset(&data, 0, sizeof(data));
    memset(held, 0, sizeof(held));
    call_count = queue_count = 0;
    fail_press = fail_schedule = fail_submit = in_sysworkq = false;
    now = 1000;
    assert(cygnus_gesture_init(&dev) == 0);
    layer(true);
    pump();
}
static void no_keys_held(void) {
    for (size_t i = 0; i < ARRAY_SIZE(held); i++) { assert(!held[i]); }
    assert(data.pressed_binding == CYGNUS_GESTURE_NONE);
}
static void test_directions_and_timing(void) {
    const int codes[] = {INPUT_REL_X, INPUT_REL_X, INPUT_REL_Y, INPUT_REL_Y};
    const int signs[] = {1, -1, 1, -1};
    const int expected[] = {1, 2, 4, 3}; // Preserve the current overlay's Y orientation.
    for (size_t i = 0; i < ARRAY_SIZE(codes); i++) {
        start();
        sample(codes[i], signs[i] * 132, true);
        pump();
        assert(call_count == 0);
        sample(codes[i], signs[i] * 2, true);
        assert(call_count == 0); // No behavior invocation on the input thread.
        pump();
        assert(call_count == 1 && calls[0].binding == (uint32_t)expected[i]);
        advance(34);
        assert(call_count == 1);
        advance(1);
        assert(call_count == 2 && !calls[1].pressed);
        no_keys_held();
    }
}
static void test_no_carry_over(void) {
    start(); sample(INPUT_REL_X, 100, true); pump();
    layer(false); layer(true); // No intervening worker execution.
    sample(INPUT_REL_X, 40, true); pump();
    assert(call_count == 0 && data.delta_x == 40);
    sample(INPUT_REL_X, 93, true); pump();
    assert(call_count == 1);
    layer(false); pump();
    no_keys_held();
    assert(!timer->pending && data.delta_x == 0);
}
static void test_old_queued_frames_are_discarded(void) {
    start(); sample(INPUT_REL_X, 150, true);
    layer(false); layer(true); sample(INPUT_REL_X, 2, true); pump();
    assert(call_count == 0 && data.delta_x == 2);
    start(); sample(INPUT_REL_X, 150, false); pump();
    layer(false); layer(true); sample(INPUT_REL_Y, 2, true); pump();
    assert(call_count == 0 && data.report_x == 0 && data.delta_y == 2);
}
static void test_release_after_fast_reentry(void) {
    start(); sample(INPUT_REL_X, 133, true); pump();
    assert(held[1]);
    layer(false); layer(true); sample(INPUT_REL_X, -133, true); pump();
    assert(call_count == 3 && !held[1] && held[2]);
    advance(35); no_keys_held();
    assert(call_count == 4);
}
static void test_overflow_and_failures(void) {
    start();
    for (int i = 0; i < CYGNUS_GESTURE_EVENT_QUEUE_SIZE + 1; i++) {
        sample(INPUT_REL_X, 133, true);
    }
    pump(); assert(call_count == 0 && data.delta_x == 0);
    sample(INPUT_REL_X, 133, true); pump(); advance(35); no_keys_held();
    start(); fail_press = true;
    sample(INPUT_REL_X, 133, true); pump(); no_keys_held();
    assert(call_count == 2);
    start(); fail_schedule = true;
    sample(INPUT_REL_X, 133, true); pump(); no_keys_held();
    assert(call_count == 2);
    start(); fail_submit = true;
    sample(INPUT_REL_X, 133, true); sample(INPUT_REL_X, 2, true); pump();
    assert(call_count == 0 && data.delta_x == 2);
}
static void test_wait_and_other_layers(void) {
    start(); sample(INPUT_REL_X, 133, true); pump(); advance(35);
    sample(INPUT_REL_X, 133, true); pump(); assert(call_count == 2);
    advance(85); sample(INPUT_REL_X, 133, true); pump();
    assert(call_count == 3); advance(35); no_keys_held();
    start(); sample(INPUT_REL_X, 100, true); pump();
    gesture_layer_changed(&dev, 4); pump();
    sample(INPUT_REL_X, 33, true); pump(); assert(call_count == 1);
}
int main(void) {
    test_directions_and_timing();
    test_no_carry_over();
    test_old_queued_frames_are_discarded();
    test_release_after_fast_reentry();
    test_overflow_and_failures();
    test_wait_and_other_layers();
    puts("PASS: gesture direction, thresholds, tap/wait timing, layer reset, stale frames,");
    puts("      rapid reentry, single-workqueue ownership, overflow and failure cleanup.");
    return 0;
}
