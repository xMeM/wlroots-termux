#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "backend/termuxgui.h"

struct wlr_tgui_backend *
tgui_backend_from_backend(struct wlr_backend *wlr_backend) {
    assert(wlr_backend_is_tgui(wlr_backend));
    return (struct wlr_tgui_backend *) wlr_backend;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
    struct wlr_tgui_backend *backend = tgui_backend_from_backend(wlr_backend);
    wlr_log(WLR_INFO, "Starting Termux:GUI backend");

    struct wlr_tgui_output *output;
    wl_list_for_each(output, &backend->outputs, link) {
        wlr_output_update_enabled(&output->wlr_output, true);
        wl_signal_emit_mutable(&backend->backend.events.new_output,
                               &output->wlr_output);
        wl_signal_emit_mutable(&backend->backend.events.new_input,
                               &output->keyboard.base);
        wl_signal_emit_mutable(&backend->backend.events.new_input,
                               &output->pointer.base);
    }

    backend->started = true;
    return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
    struct wlr_tgui_backend *backend = tgui_backend_from_backend(wlr_backend);
    if (!wlr_backend) {
        return;
    }

    wl_list_remove(&backend->display_destroy.link);
    wl_event_source_remove(backend->tgui_event_source);

    struct wlr_tgui_output *output, *output_tmp;
    wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
        wlr_output_destroy(&output->wlr_output);
    }

    wlr_backend_finish(wlr_backend);

    tgui_connection_destroy(backend->conn);
    pthread_join(backend->tgui_event_thread, NULL);
    pthread_mutex_destroy(&backend->event_queue_lock);

    close(backend->fake_drm_fd);
    close(backend->tgui_event_fd);
    free(backend);
}

static uint32_t get_buffer_caps(struct wlr_backend *wlr_backend) {
    return WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_DMABUF;
}

static int get_drm_fd(struct wlr_backend *wlr_backend) {
    struct wlr_tgui_backend *backend = tgui_backend_from_backend(wlr_backend);
    if (!wlr_backend) {
        return -1;
    }

    return backend->fake_drm_fd;
}

static const struct wlr_backend_impl backend_impl = {
    .start = backend_start,
    .destroy = backend_destroy,
    .get_buffer_caps = get_buffer_caps,
    .get_drm_fd = get_drm_fd,
};

static void handle_display_destroy(struct wl_listener *listener, void *data) {
    struct wlr_tgui_backend *backend =
        wl_container_of(listener, backend, display_destroy);
    backend_destroy(&backend->backend);
}

static int handle_tgui_event(int fd, uint32_t mask, void *data) {
    struct wlr_tgui_backend *backend = data;

    if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
        if (mask & WL_EVENT_ERROR) {
            wlr_log(WLR_ERROR, "Failed to read from tgui event");
            wlr_backend_destroy(&backend->backend);
        }
        return 0;
    }

    eventfd_t event_count = 0;
    if (eventfd_read(backend->tgui_event_fd, &event_count) < 0) {
        return 0;
    }

    pthread_mutex_lock(&backend->event_queue_lock);
    if (wl_list_empty(&backend->event_queue)) {
        pthread_mutex_unlock(&backend->event_queue_lock);
        return 0;
    }

    struct wlr_tgui_event *event =
        wl_container_of(backend->event_queue.prev, event, link);
    wl_list_remove(&event->link);
    pthread_mutex_unlock(&backend->event_queue_lock);

    struct wlr_tgui_output *output, *output_tmp;
    wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
        if (event->e.activity == output->tgui_activity) {
            handle_activity_event(&event->e, output);
        }
    }
    tgui_event_destroy(&event->e);
    free(event);

    return 0;
}

static void *tgui_event_thread(void *data) {
    struct wlr_tgui_backend *backend = data;

    tgui_event event;
    while (tgui_wait_event(backend->conn, &event) == 0) {
        struct wlr_tgui_event *wlr_event = calloc(1, sizeof(*wlr_event));
        if (wlr_event) {
            wlr_event->e = event;
            wlr_event->backend = backend;

            pthread_mutex_lock(&backend->event_queue_lock);
            wl_list_insert(&backend->event_queue, &wlr_event->link);
            pthread_mutex_unlock(&backend->event_queue_lock);

            eventfd_write(backend->tgui_event_fd, 1);
        } else {
            wlr_log(WLR_ERROR, "event loss: out of memory");
            tgui_event_destroy(&event);
        }
    }

    return 0;
}

struct wlr_backend *wlr_tgui_backend_create(struct wl_display *display) {
    wlr_log(WLR_INFO, "Creating Termux:GUI backend");

    struct wlr_tgui_backend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        wlr_log(WLR_ERROR, "Failed to allocate wlr_tgui_backend");
        return NULL;
    }
    wlr_backend_init(&backend->backend, &backend_impl);

    backend->display = display;
    backend->loop = wl_display_get_event_loop(display);
    backend->fake_drm_fd = open("/dev/null", O_RDONLY);
    backend->tgui_event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);

    assert(backend->fake_drm_fd >= 0 && backend->tgui_event_fd >= 0);

    if (tgui_connection_create(&backend->conn)) {
        wlr_log(WLR_ERROR, "Failed to create tgui_connection");
        wlr_backend_finish(&backend->backend);
        free(backend);
        return NULL;
    }
    backend->backend.allocator = wlr_tgui_allocator_create(backend);

    wl_list_init(&backend->outputs);
    wl_list_init(&backend->event_queue);

    backend->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(display, &backend->display_destroy);

    uint32_t events = WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP;
    backend->tgui_event_source =
        wl_event_loop_add_fd(backend->loop, backend->tgui_event_fd, events,
                             handle_tgui_event, backend);

    pthread_mutex_init(&backend->event_queue_lock, NULL);
    pthread_create(&backend->tgui_event_thread, NULL, tgui_event_thread,
                   backend);

    return &backend->backend;
}

bool wlr_backend_is_tgui(struct wlr_backend *backend) {
    return backend->impl == &backend_impl;
}
