#include <assert.h>
#include <drm_fourcc.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "backend/termuxgui.h"
#include "types/wlr_output.h"
#include "util/time.h"

static const uint32_t SUPPORTED_OUTPUT_STATE =
    WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER |
    WLR_OUTPUT_STATE_MODE;

static size_t last_output_num = 0;

static struct wlr_tgui_output *
tgui_output_from_output(struct wlr_output *wlr_output) {
    assert(wlr_output_is_tgui(wlr_output));
    return (struct wlr_tgui_output *) wlr_output;
}

static bool output_test(struct wlr_output *wlr_output,
                        const struct wlr_output_state *state) {
    uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
    if (unsupported != 0) {
        wlr_log(WLR_DEBUG, "Unsupported output state fields: 0x%" PRIx32,
                unsupported);
        return false;
    }

    return true;
}

static bool output_commit(struct wlr_output *wlr_output,
                          const struct wlr_output_state *state) {
    struct wlr_tgui_output *output = tgui_output_from_output(wlr_output);

    if (!output_test(wlr_output, state)) {
        return false;
    }

    if (state->committed & WLR_OUTPUT_STATE_MODE) {
        wlr_output_update_mode(&output->wlr_output, state->mode);
    }

    if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
        bool scanout = output_is_direct_scanout(wlr_output, state->buffer);

        if (scanout) {
            wlr_log(WLR_ERROR, "Unsupported scanout mode");
        } else {
            struct wlr_tgui_buffer *buffer =
                tgui_buffer_from_buffer(state->buffer);

            pthread_mutex_lock(&output->present_queue.pending.lock);
            wlr_buffer_lock(&buffer->wlr_buffer);
            wl_list_insert(&output->present_queue.pending.buffers,
                           &buffer->link);
            pthread_mutex_unlock(&output->present_queue.pending.lock);

            pthread_cond_broadcast(&output->present_queue.thread_cond);
        }
    }

    return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
    struct wlr_tgui_output *output = tgui_output_from_output(wlr_output);
    wl_list_remove(&output->link);
    wl_event_source_remove(output->present_queue.idle_event_source);

    wlr_pointer_finish(&output->pointer);
    wlr_keyboard_finish(&output->keyboard);
    tgui_activity_finish(output->backend->conn, output->tgui_activity);

    pthread_mutex_lock(&output->present_queue.thread_lock);
    output->present_queue.state = TGUI_ERR_ACTIVITY_DESTROYED;
    pthread_mutex_unlock(&output->present_queue.thread_lock);
    pthread_cond_broadcast(&output->present_queue.thread_cond);
    pthread_join(output->present_queue.thread, NULL);

    struct wlr_tgui_buffer *buffer, *buffer_tmp;
    wl_list_for_each_safe(buffer, buffer_tmp,
                          &output->present_queue.pending.buffers, link) {
        wl_list_remove(&buffer->link);
        wlr_buffer_unlock(&buffer->wlr_buffer);
    }
    wl_list_for_each_safe(buffer, buffer_tmp,
                          &output->present_queue.idle.buffers, link) {
        wl_list_remove(&buffer->link);
        wlr_buffer_unlock(&buffer->wlr_buffer);
    }

    struct wlr_output_mode *mode, *tmp_mode;
    wl_list_for_each_safe(mode, tmp_mode, &output->wlr_output.modes, link) {
        wl_list_remove(&mode->link);
        free(mode);
    }

    pthread_mutex_destroy(&output->present_queue.pending.lock);
    pthread_mutex_destroy(&output->present_queue.idle.lock);
    pthread_mutex_destroy(&output->present_queue.thread_lock);
    pthread_cond_destroy(&output->present_queue.thread_cond);
    close(output->present_queue.idle_event_fd);
    free(output);
}

static const struct wlr_output_impl output_impl = {
    .destroy = output_destroy,
    .commit = output_commit,
};

bool wlr_output_is_tgui(struct wlr_output *wlr_output) {
    return wlr_output->impl == &output_impl;
}

static void output_configure_surfaceview(struct wlr_tgui_output *output) {
    tgui_activity_set_orientation(output->backend->conn,
                                  output->tgui_activity,
                                  TGUI_ORIENTATION_LANDSCAPE);
    tgui_activity_configure_insets(
        output->backend->conn, output->tgui_activity,
        TGUI_INSET_NAVIGATION_BAR, TGUI_INSET_BEHAVIOUR_TRANSIENT);
    tgui_create_surface_view(output->backend->conn, output->tgui_activity,
                             &output->tgui_surfaceview, NULL,
                             TGUI_VIS_VISIBLE, true);
    tgui_surface_view_config(output->backend->conn, output->tgui_activity,
                             output->tgui_surfaceview, 0,
                             TGUI_MISMATCH_CENTER_AXIS,
                             TGUI_MISMATCH_CENTER_AXIS, 120);
    tgui_send_touch_event(output->backend->conn, output->tgui_activity,
                          output->tgui_surfaceview, true);
    tgui_focus(output->backend->conn, output->tgui_activity,
               output->tgui_surfaceview, false);
}

int handle_activity_event(tgui_event *e, struct wlr_tgui_output *output) {
    uint64_t time_ms = get_current_time_msec();
    switch (e->type) {
    case TGUI_EVENT_CREATE: {
        output_configure_surfaceview(output);
        break;
    }
    case TGUI_EVENT_START:
    case TGUI_EVENT_RESUME: {
        output->tgui_activity_is_foreground = true;
        pthread_cond_broadcast(&output->present_queue.thread_cond);
        break;
    }
    case TGUI_EVENT_PAUSE: {
        output->tgui_activity_is_foreground = false;
        break;
    }
    case TGUI_EVENT_DESTROY: {
        wlr_output_destroy(&output->wlr_output);
        break;
    }
    case TGUI_EVENT_KEY: {
        handle_keyboard_event(e, output, time_ms);
        break;
    }
    case TGUI_EVENT_TOUCH: {
        handle_touch_event(e, output, time_ms);
        break;
    }
    case TGUI_EVENT_SURFACE_CHANGED: {
        struct wlr_pointer_motion_absolute_event ev = {
            .pointer = &output->pointer,
            .time_msec = time_ms,
            .x = output->cursor_x = 0.5f,
            .y = output->cursor_y = 0.5f,
        };
        wl_signal_emit_mutable(&output->pointer.events.motion_absolute, &ev);
        wl_signal_emit_mutable(&output->pointer.events.frame,
                               &output->pointer);
        break;
    }
    case TGUI_EVENT_FRAME_COMPLETE: {
        break;
    }
    default:
        break;
    }

    return 0;
}

static void *present_queue_thread(void *data) {
    struct wlr_tgui_output *output = data;
    pthread_mutex_lock(&output->present_queue.thread_lock);
    while (output->present_queue.state != TGUI_ERR_ACTIVITY_DESTROYED) {
        pthread_mutex_lock(&output->present_queue.pending.lock);
        if (wl_list_empty(&output->present_queue.pending.buffers) ||
            output->tgui_activity_is_foreground == false) {
            pthread_mutex_unlock(&output->present_queue.pending.lock);
            pthread_cond_wait(&output->present_queue.thread_cond,
                              &output->present_queue.thread_lock);
        } else {
            struct wlr_tgui_buffer *buffer = wl_container_of(
                output->present_queue.pending.buffers.prev, buffer, link);
            wl_list_remove(&buffer->link);
            pthread_mutex_unlock(&output->present_queue.pending.lock);

            output->present_queue.state = tgui_surface_view_set_buffer(
                output->backend->conn, output->tgui_activity,
                output->tgui_surfaceview, &buffer->buffer);

            pthread_mutex_lock(&output->present_queue.idle.lock);
            wl_list_insert(&output->present_queue.idle.buffers,
                           &buffer->link);
            pthread_mutex_unlock(&output->present_queue.idle.lock);

            uint64_t count = 1;
            write(output->present_queue.idle_event_fd, &count, sizeof(count));
        }
    }
    pthread_mutex_unlock(&output->present_queue.thread_lock);
    return 0;
}

static int present_idle_event(int fd, uint32_t mask, void *data) {
    struct wlr_tgui_output *output = data;

    if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
        if (mask & WL_EVENT_ERROR) {
            wlr_log(WLR_ERROR, "Failed to read from idle event");
        }
        return 0;
    }

    uint64_t count = 0;
    if (read(fd, &count, sizeof(count)) == EAGAIN) {
        return 0;
    }

    struct wlr_tgui_buffer *buffer, *tmp;
    pthread_mutex_lock(&output->present_queue.idle.lock);
    wl_list_for_each_safe(buffer, tmp, &output->present_queue.idle.buffers,
                          link) {
        wl_list_remove(&buffer->link);
        wlr_buffer_unlock(&buffer->wlr_buffer);
        break;
    }
    pthread_mutex_unlock(&output->present_queue.idle.lock);

    struct wlr_output_event_present present_event = {
        .commit_seq = output->wlr_output.commit_seq + 1,
        .presented = true,
        .flags = WLR_OUTPUT_PRESENT_ZERO_COPY,
    };
    wlr_output_send_present(&output->wlr_output, &present_event);
    wlr_output_send_frame(&output->wlr_output);
    return 0;
}

static bool output_create_mode(struct wlr_tgui_output *output,
                               int32_t width,
                               int32_t height,
                               int32_t refresh,
                               bool preferred) {
    struct wlr_output_mode *mode = calloc(1, sizeof(*mode));
    if (mode == NULL) {
        wlr_log(WLR_ERROR, "Failed to allocate wlr_output_mode");
        return false;
    }

    mode->width = width;
    mode->height = height;
    mode->refresh = refresh;
    mode->preferred = preferred;
    mode->picture_aspect_ratio = WLR_OUTPUT_MODE_ASPECT_RATIO_16_9;

    wl_list_insert(&output->wlr_output.modes, &mode->link);
    return true;
}

const struct wlr_pointer_impl tgui_pointer_impl = {
    .name = "tgui-pointer",
};

const struct wlr_keyboard_impl tgui_keyboard_impl = {
    .name = "tgui-keyboard",
};

struct wlr_output *wlr_tgui_add_output(struct wlr_backend *wlr_backend) {
    struct wlr_tgui_backend *backend = tgui_backend_from_backend(wlr_backend);

    struct wlr_tgui_output *output = calloc(1, sizeof(*output));
    if (output == NULL) {
        wlr_log(WLR_ERROR, "Failed to allocate wlr_tgui_output");
        return NULL;
    }
    output->backend = backend;

    wl_list_init(&output->present_queue.pending.buffers);
    wl_list_init(&output->present_queue.idle.buffers);

    wlr_pointer_init(&output->pointer, &tgui_pointer_impl, "tgui-pointer");
    wlr_keyboard_init(&output->keyboard, &tgui_keyboard_impl,
                      "tgui-keyboard");

    if (tgui_activity_create(backend->conn, &output->tgui_activity,
                             TGUI_ACTIVITY_NORMAL, NULL, true)) {
        wlr_log(WLR_ERROR, "Failed to create tgui_activity");
        free(output);
        return NULL;
    }
    tgui_activity_configuration conf;
    tgui_activity_get_configuration(backend->conn, output->tgui_activity,
                                    &conf);

    wlr_output_init(&output->wlr_output, &backend->backend, &output_impl,
                    backend->display);
    struct wlr_output *wlr_output = &output->wlr_output;

    wlr_output->adaptive_sync_status = WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
    wlr_output_set_render_format(wlr_output, DRM_FORMAT_ABGR8888);
    wlr_output_set_transform(wlr_output, WL_OUTPUT_TRANSFORM_FLIPPED_180);

    bool preferred = true;
    if (conf.screen_width * conf.density > 1080) {
        output_create_mode(output, 2560, 1440, 0, preferred);
        preferred = false;
    }
    if (conf.screen_width * conf.density > 720) {
        output_create_mode(output, 1920, 1080, 0, preferred);
        preferred = false;
    }
    output_create_mode(output, 1280, 720, 0, preferred);

    size_t output_num = ++last_output_num;

    char name[64];
    snprintf(name, sizeof(name), "TGUI-%zu", output_num);
    wlr_output_set_name(wlr_output, name);
    tgui_activity_set_task_description(output->backend->conn,
                                       output->tgui_activity, NULL, 0, name);

    char description[128];
    snprintf(description, sizeof(description), "Termux:GUI output %zu",
             output_num);
    wlr_output_set_description(wlr_output, description);

    wl_list_insert(&backend->outputs, &output->link);

    uint32_t events = WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP;
    output->present_queue.idle_event_fd =
        eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    output->present_queue.idle_event_source = wl_event_loop_add_fd(
        backend->loop, output->present_queue.idle_event_fd, events,
        present_idle_event, output);

    assert(output->present_queue.idle_event_fd >= 0 &&
           output->present_queue.idle_event_source != NULL);

    pthread_mutex_init(&output->present_queue.pending.lock, NULL);
    pthread_mutex_init(&output->present_queue.idle.lock, NULL);
    pthread_mutex_init(&output->present_queue.thread_lock, NULL);
    pthread_cond_init(&output->present_queue.thread_cond, NULL);
    pthread_create(&output->present_queue.thread, NULL, present_queue_thread,
                   output);

    if (backend->started) {
        wlr_output_update_enabled(wlr_output, true);
        wl_signal_emit_mutable(&backend->backend.events.new_output,
                               wlr_output);
        wl_signal_emit_mutable(&backend->backend.events.new_input,
                               &output->keyboard.base);
        wl_signal_emit_mutable(&backend->backend.events.new_input,
                               &output->pointer.base);
    }

    return wlr_output;
}
