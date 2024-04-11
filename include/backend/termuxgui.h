#ifndef BACKEND_TERMUXGUI_H
#define BACKEND_TERMUXGUI_H

#include <android/hardware_buffer.h>
#include <assert.h>
#include <pthread.h>
#include <termuxgui/termuxgui.h>

#include <wlr/backend/interface.h>
#include <wlr/backend/termuxgui.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/allocator.h>
#include <wlr/util/log.h>

#define DEFAULT_REFRESH (60 * 1000) // 60 Hz

#define TRY_LOG(func, ...)                                                   \
    do {                                                                     \
        tgui_err ret = func(__VA_ARGS__);                                    \
        if (ret != TGUI_ERR_OK) {                                            \
            wlr_log(WLR_ERROR, #func " failed: %s", TGUI_ERR_TO_STR(ret));   \
        }                                                                    \
    } while (0)

static const char *TGUI_ERR_STR[] = {
    [TGUI_ERR_SYSTEM] = "TGUI_ERR_SYSTEM",
    [TGUI_ERR_CONNECTION_LOST] = "TGUI_ERR_CONNECTION_LOST",
    [TGUI_ERR_ACTIVITY_DESTROYED] = "TGUI_ERR_ACTIVITY_DESTROYED",
    [TGUI_ERR_MESSAGE] = "TGUI_ERR_MESSAGE",
    [TGUI_ERR_NOMEM] = "TGUI_ERR_NOMEM",
    [TGUI_ERR_EXCEPTION] = "TGUI_ERR_EXCEPTION",
    [TGUI_ERR_VIEW_INVALID] = "TGUI_ERR_VIEW_INVALID",
    [TGUI_ERR_API_LEVEL] = "TGUI_ERR_API_LEVEL",
};

static inline const char *TGUI_ERR_TO_STR(tgui_err err) {
    if (err > TGUI_ERR_API_LEVEL) {
        return "UNKNOWN_ERROR";
    }
    return TGUI_ERR_STR[err];
}

typedef struct native_handle {
    int version; /* sizeof(native_handle_t) */
    int numFds;  /* number of file-descriptors at &data[0] */
    int numInts; /* number of ints at &data[numFds] */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#endif
    int data[0]; /* numFds + numInts ints */
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
} native_handle_t;

struct wlr_queue {
    struct wl_list base;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
};

static inline void wlr_queue_init(struct wlr_queue *queue) {
    wl_list_init(&queue->base);
    pthread_cond_init(&queue->cond, NULL);
    pthread_mutex_init(&queue->mutex, NULL);
}

static inline void wlr_queue_destroy(struct wlr_queue *queue) {
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
}

static inline struct wl_list *wlr_queue_pull(struct wlr_queue *queue,
                                             bool nonblock) {
    pthread_mutex_lock(&queue->mutex);

    if (wl_list_empty(&queue->base)) {
        if (nonblock) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    assert(wl_list_length(&queue->base) > 0);

    struct wl_list *elm = queue->base.prev;
    wl_list_remove(elm);

    pthread_mutex_unlock(&queue->mutex);
    return elm;
}

static inline void wlr_queue_push(struct wlr_queue *queue,
                                  struct wl_list *elm) {
    pthread_mutex_lock(&queue->mutex);
    if (wl_list_empty(&queue->base)) {
        pthread_cond_signal(&queue->cond);
    }
    wl_list_insert(&queue->base, elm);
    pthread_mutex_unlock(&queue->mutex);
}

struct wlr_tgui_backend {
    struct wlr_backend backend;
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_list outputs;
    struct wl_listener display_destroy;
    bool started;

    tgui_connection conn;
    struct wlr_queue event_queue;
    int fake_drm_fd;
    int tgui_event_fd;
    pthread_t tgui_event_thread;
    struct wl_event_source *tgui_event_source;
};

struct wlr_tgui_allocator {
    struct wlr_allocator wlr_allocator;

    void *libandroid_handle;
    int (*AHardwareBuffer_lock)(AHardwareBuffer *buffer,
                                uint64_t usage,
                                int32_t fence,
                                const ARect *rect,
                                void **outVirtualAddress);
    int (*AHardwareBuffer_unlock)(AHardwareBuffer *buffer, int32_t *fence);
    void (*AHardwareBuffer_describe)(const AHardwareBuffer *buffer,
                                     AHardwareBuffer_Desc *outDesc);
    const native_handle_t *(*AHardwareBuffer_getNativeHandle)(
        const AHardwareBuffer *buffer);

    tgui_connection conn;
};

struct wlr_tgui_buffer {
    struct wlr_buffer wlr_buffer;

    void *data;
    uint32_t format;
    tgui_connection conn;
    tgui_hardware_buffer buffer;
    AHardwareBuffer_Desc desc;
    struct wl_list link;
    struct wlr_dmabuf_attributes dmabuf;
    struct wlr_tgui_allocator *allocator;
};

struct wlr_tgui_output {
    struct wlr_output wlr_output;

    struct wlr_tgui_backend *backend;
    struct wl_list link;

    tgui_activity tgui_activity;
    tgui_view tgui_surfaceview;
    bool tgui_activity_is_foreground;

    struct wlr_queue present_queue;
    struct wlr_queue idle_queue;
    bool present_thread_run;
    pthread_t present_thread;
    int present_complete_fd;
    struct wl_event_source *present_complete_source;

    struct wlr_pointer pointer;
    struct wlr_keyboard keyboard;

    struct {
        int id, max;
        double x, y;
        bool moved, down;
        uint64_t time_ms;
    } touch_pointer;

    double cursor_x, cursor_y;
};

struct wlr_tgui_event {
    tgui_event e;
    struct wl_list link;
};

struct wlr_tgui_backend *
tgui_backend_from_backend(struct wlr_backend *wlr_backend);

struct wlr_allocator *
wlr_tgui_allocator_create(struct wlr_tgui_backend *backend);

struct wlr_tgui_buffer *
tgui_buffer_from_buffer(struct wlr_buffer *wlr_buffer);

int handle_activity_event(tgui_event *e, struct wlr_tgui_output *output);

void handle_touch_event(tgui_event *e,
                        struct wlr_tgui_output *output,
                        uint64_t time_ms);

void handle_keyboard_event(tgui_event *e,
                           struct wlr_tgui_output *output,
                           uint64_t time_ms);

#endif
