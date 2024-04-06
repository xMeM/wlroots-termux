#ifndef BACKEND_TERMUXGUI_H
#define BACKEND_TERMUXGUI_H

#include <android/hardware_buffer.h>
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

struct wlr_tgui_backend {
    struct wlr_backend backend;
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_list outputs;
    struct wl_listener display_destroy;
    bool started;

    tgui_connection conn;
    int fake_drm_fd;
    int tgui_event_fd;
    pthread_t tgui_event_thread;
    pthread_mutex_t event_queue_lock;
    struct wl_list event_queue;
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

    struct {
        struct {
            struct wl_list buffers;
            pthread_mutex_t lock;
        } pending, idle;

        tgui_err state;
        pthread_t thread;
        pthread_cond_t thread_cond;
        pthread_mutex_t thread_lock;
        int idle_event_fd;
        struct wl_event_source *idle_event_source;
    } present_queue;

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
    struct wlr_tgui_backend *backend;
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
