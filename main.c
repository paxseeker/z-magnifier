#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1.h"
#include "wlr-layer-shell-unstable-v1.h"

struct magnifier {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_output *output;

    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_screencopy_manager_v1 *screencopy_manager;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    uint32_t configure_serial;
    bool ack_pending;
    int surface_width, surface_height;

    int disp_fd;
    void *disp_data;
    size_t disp_size;
    struct wl_buffer *disp_buffer;
    int disp_stride;

    int frame_fd;
    void *frame_data;
    size_t frame_size;
    struct wl_buffer *frame_buffer;
    int frame_width, frame_height, frame_stride;
    uint32_t frame_format;
    bool y_invert;

    void *saved_data;
    int saved_fd;
    size_t saved_size;
    int saved_width, saved_height, saved_stride;
    bool saved_y_invert;
    bool saved;

    bool configured;
    bool running;
    bool capture_in_flight;

    double cursor_x, cursor_y;
    double zoom;
    int lens_radius;
    bool ctrl_pressed;
    int scroll_dir;
    bool pending_render;

    int output_width, output_height;
    bool output_configured;
};

static int
create_shm_fd(size_t size)
{
    int fd = memfd_create("magnifier-shm", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

static void *
shm_alloc(int *fd, size_t *size, size_t needed)
{
    int new_fd = create_shm_fd(needed);
    if (new_fd < 0) return NULL;
    void *data = mmap(NULL, needed, PROT_READ | PROT_WRITE, MAP_SHARED, new_fd, 0);
    if (data == MAP_FAILED) { close(new_fd); return NULL; }
    *fd = new_fd;
    *size = needed;
    return data;
}

static void
shm_free(int *fd, void **data, size_t *size)
{
    if (*fd >= 0) {
        munmap(*data, *size);
        close(*fd);
        *fd = -1;
        *data = NULL;
        *size = 0;
    }
}

static void
cleanup_frame_buffer(struct magnifier *m)
{
    if (m->frame_buffer) { wl_buffer_destroy(m->frame_buffer); m->frame_buffer = NULL; }
    shm_free(&m->frame_fd, (void**)&m->frame_data, &m->frame_size);
}

static void
cleanup_display_buffer(struct magnifier *m)
{
    if (m->disp_buffer) { wl_buffer_destroy(m->disp_buffer); m->disp_buffer = NULL; }
    shm_free(&m->disp_fd, (void**)&m->disp_data, &m->disp_size);
}

static void
create_display_buffer(struct magnifier *m, int width, int height)
{
    cleanup_display_buffer(m);
    m->disp_stride = width * 4;
    size_t size = (size_t)m->disp_stride * height;
    m->disp_data = shm_alloc(&m->disp_fd, &m->disp_size, size);
    if (!m->disp_data) {
        fprintf(stderr, "Failed to allocate display buffer (%dx%d)\n", width, height);
        return;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(m->shm, m->disp_fd, (int32_t)size);
    m->disp_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
        m->disp_stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    m->surface_width = width;
    m->surface_height = height;
}

static uint32_t
sample_pixel(const uint32_t *src, int stride_px, int fw, int fh, double sx, double sy)
{
    int ix = (int)sx;
    int iy = (int)sy;
    double fx = sx - ix;
    double fy = sy - iy;

    if (ix < 0 || ix >= fw - 1 || iy < 0 || iy >= fh - 1) {
        if (ix < 0) ix = 0; else if (ix >= fw) ix = fw - 1;
        if (iy < 0) iy = 0; else if (iy >= fh) iy = fh - 1;
        return src[iy * stride_px + ix];
    }

    const uint32_t *row0 = &src[iy * stride_px];
    const uint32_t *row1 = &src[(iy + 1) * stride_px];
    uint32_t p00 = row0[ix], p10 = row0[ix + 1];
    uint32_t p01 = row1[ix], p11 = row1[ix + 1];

    double w00 = (1 - fx) * (1 - fy);
    double w10 = fx * (1 - fy);
    double w01 = (1 - fx) * fy;
    double w11 = fx * fy;

    uint8_t r = (uint8_t)(((p00 >> 16) & 0xFF) * w00 + ((p10 >> 16) & 0xFF) * w10 +
                          ((p01 >> 16) & 0xFF) * w01 + ((p11 >> 16) & 0xFF) * w11);
    uint8_t g = (uint8_t)(((p00 >> 8) & 0xFF) * w00 + ((p10 >> 8) & 0xFF) * w10 +
                          ((p01 >> 8) & 0xFF) * w01 + ((p11 >> 8) & 0xFF) * w11);
    uint8_t b = (uint8_t)((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10 +
                          (p01 & 0xFF) * w01 + (p11 & 0xFF) * w11);

    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void
render_magnified(struct magnifier *m)
{
    if (!m->saved || !m->disp_data) return;
    int w = m->surface_width;
    int h = m->surface_height;
    uint32_t *dst = m->disp_data;
    const uint32_t *src = m->saved_data;
    int fw = m->saved_width;
    int fh = m->saved_height;
    int stride_px = m->saved_stride / 4;

    memset(dst, 0, (size_t)w * h * 4);

    double cx = m->cursor_x;
    double cy = m->cursor_y;
    double inv_zoom = 1.0 / m->zoom;

    double left = cx - m->lens_radius;
    double top = cy - m->lens_radius;
    double right = cx + m->lens_radius;
    double bottom = cy + m->lens_radius;

    int start_x = (int)(left < 0 ? 0 : left);
    int start_y = (int)(top < 0 ? 0 : top);
    int end_x = (int)(right >= w ? w - 1 : right);
    int end_y = (int)(bottom >= h ? h - 1 : bottom);

    for (int dy = start_y; dy <= end_y; dy++) {
        for (int dx = start_x; dx <= end_x; dx++) {
            double dxd = (double)dx - cx;
            double dyd = (double)dy - cy;
            double dist = sqrt(dxd * dxd + dyd * dyd);
            if (dist > m->lens_radius) continue;

            double sx = cx + dxd * inv_zoom;
            double sy = cy + dyd * inv_zoom;
            if (m->saved_y_invert) sy = fh - 1 - sy;

            uint32_t color = sample_pixel(src, stride_px, fw, fh, sx, sy);

            if (dist > m->lens_radius - 2.0) {
                double fade = (m->lens_radius - dist) / 2.0;
                if (fade < 0.0) fade = 0.0;
                if (fade > 1.0) fade = 1.0;
                uint8_t alpha = (uint8_t)(fade * 255);
                color = (color & 0x00FFFFFF) | ((uint32_t)alpha << 24);
            }

            dst[dy * w + dx] = color;
        }
    }
}

static void render_and_update(struct magnifier *m);
static void surface_frame_done(void *data, struct wl_callback *callback, uint32_t time);

static const struct wl_callback_listener surface_frame_listener = {
    .done = surface_frame_done,
};

static void
surface_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    wl_callback_destroy(callback);
    struct magnifier *m = data;
    if (m->pending_render) {
        m->pending_render = false;
        render_and_update(m);
    } else {
        struct wl_callback *cb = wl_surface_frame(m->surface);
        wl_callback_add_listener(cb, &surface_frame_listener, m);
        wl_surface_commit(m->surface);
        wl_display_flush(m->display);
    }
}

static void
update_surface(struct magnifier *m)
{
    if (!m->configured || !m->disp_buffer) return;

    struct wl_region *region = wl_compositor_create_region(m->compositor);
    int input_margin = m->lens_radius * 3;
    int rx = (int)(m->cursor_x - input_margin);
    int ry = (int)(m->cursor_y - input_margin);
    int rw = input_margin * 2;
    int rh = input_margin * 2;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rw > m->surface_width - rx) rw = m->surface_width - rx;
    if (rh > m->surface_height - ry) rh = m->surface_height - ry;
    if (rw > 0 && rh > 0)
        wl_region_add(region, rx, ry, rw, rh);
    wl_surface_set_input_region(m->surface, region);
    wl_region_destroy(region);

    wl_surface_attach(m->surface, m->disp_buffer, 0, 0);
    wl_surface_damage_buffer(m->surface, 0, 0, m->surface_width, m->surface_height);
    if (m->ack_pending) {
        zwlr_layer_surface_v1_ack_configure(m->layer_surface, m->configure_serial);
        m->ack_pending = false;
    }
    struct wl_callback *cb = wl_surface_frame(m->surface);
    wl_callback_add_listener(cb, &surface_frame_listener, m);
    wl_surface_commit(m->surface);
    wl_display_flush(m->display);
}

static void
render_and_update(struct magnifier *m)
{
    if (!m->saved || !m->configured) return;
    render_magnified(m);
    update_surface(m);
}

static void
frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
             uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    struct magnifier *mg = data;
    mg->frame_width = (int)width;
    mg->frame_height = (int)height;
    mg->frame_stride = (int)stride;
    mg->frame_format = format;
}

static void
frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags)
{
    struct magnifier *mg = data;
    mg->y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void
frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
            uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    struct magnifier *mg = data;
    if (!mg->saved && mg->frame_data) {
        mg->saved_data = mg->frame_data;
        mg->saved_fd = mg->frame_fd;
        mg->saved_size = mg->frame_size;
        mg->saved_width = mg->frame_width;
        mg->saved_height = mg->frame_height;
        mg->saved_stride = mg->frame_stride;
        mg->saved_y_invert = mg->y_invert;
        mg->saved = true;
        mg->frame_fd = -1;
        mg->frame_data = NULL;
        mg->frame_size = 0;
    }
    if (mg->frame_buffer) {
        wl_buffer_destroy(mg->frame_buffer);
        mg->frame_buffer = NULL;
    }
    zwlr_screencopy_frame_v1_destroy(frame);
    mg->capture_in_flight = false;
    if (mg->configured && mg->surface_width > 0 && mg->surface_height > 0)
        render_and_update(mg);
}

static void
frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    struct magnifier *mg = data;
    cleanup_frame_buffer(mg);
    zwlr_screencopy_frame_v1_destroy(frame);
    mg->capture_in_flight = false;
}

static void
frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    struct magnifier *mg = data;
    size_t size = (size_t)mg->frame_stride * (size_t)mg->frame_height;
    void *buf_data = shm_alloc(&mg->frame_fd, &mg->frame_size, size);
    if (!buf_data) {
        zwlr_screencopy_frame_v1_destroy(frame);
        mg->capture_in_flight = false;
        return;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(mg->shm, mg->frame_fd, (int32_t)size);
    mg->frame_buffer = wl_shm_pool_create_buffer(pool, 0,
        mg->frame_width, mg->frame_height, mg->frame_stride, mg->frame_format);
    wl_shm_pool_destroy(pool);
    mg->frame_data = buf_data;
    zwlr_screencopy_frame_v1_copy(frame, mg->frame_buffer);
    wl_display_flush(mg->display);
}

static void
frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
             uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
}

static void
frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                   uint32_t format, uint32_t width, uint32_t height)
{
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .damage = frame_damage,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done,
};

static void
start_capture(struct magnifier *m)
{
    if (m->capture_in_flight) return;
    m->capture_in_flight = true;

    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(
            m->screencopy_manager, 0, m->output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, m);
    wl_display_flush(m->display);
}

static void
pointer_enter(void *data, struct wl_pointer *pointer,
              uint32_t serial, struct wl_surface *surface,
              wl_fixed_t sx, wl_fixed_t sy)
{
    struct magnifier *m = data;
    if (surface == m->surface) {
        m->cursor_x = wl_fixed_to_double(sx);
        m->cursor_y = wl_fixed_to_double(sy);
        m->pending_render = true;
    }
}

static void
pointer_leave(void *data, struct wl_pointer *pointer,
              uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_motion(void *data, struct wl_pointer *pointer,
               uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct magnifier *m = data;
    m->cursor_x = wl_fixed_to_double(sx);
    m->cursor_y = wl_fixed_to_double(sy);
    m->pending_render = true;
}

static void
pointer_button(void *data, struct wl_pointer *pointer,
               uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    struct magnifier *m = data;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == 272)
        m->running = false;
}

static void
pointer_axis(void *data, struct wl_pointer *pointer,
             uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct magnifier *m = data;
    if (axis != 0) return;
    if (value > 0) m->scroll_dir = 1;
    else if (value < 0) m->scroll_dir = -1;
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                      uint32_t axis, int32_t discrete)
{
    struct magnifier *m = data;
    if (axis != 0) return;
    if (discrete > 0) m->scroll_dir = 1;
    else if (discrete < 0) m->scroll_dir = -1;
}

static void
pointer_axis_value120(void *data, struct wl_pointer *pointer,
                      uint32_t axis, int32_t value120)
{
    struct magnifier *m = data;
    if (axis != 0) return;
    if (value120 > 0) m->scroll_dir = 1;
    else if (value120 < 0) m->scroll_dir = -1;
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer,
                  uint32_t time, uint32_t axis) {}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
    struct magnifier *m = data;
    int dir = m->scroll_dir;
    m->scroll_dir = 0;
    if (dir == 0) return;
    if (m->ctrl_pressed) {
        int old = m->lens_radius;
        m->lens_radius += dir * 10;
        if (m->lens_radius < 50) m->lens_radius = 50;
        if (m->lens_radius > 400) m->lens_radius = 400;
        if (m->lens_radius != old) {
            m->pending_render = true;
        }
    } else {
        double old_zoom = m->zoom;
        m->zoom += dir * 0.5;
        if (m->zoom < 1.0) m->zoom = 1.0;
        if (m->zoom > 20.0) m->zoom = 20.0;
        if (m->zoom != old_zoom) {
            m->pending_render = true;
        }
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
};

static void
keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                uint32_t format, int32_t fd, uint32_t size)
{
    close(fd);
}

static void
keyboard_enter(void *data, struct wl_keyboard *keyboard,
               uint32_t serial, struct wl_surface *surface,
               struct wl_array *keys) {}

static void
keyboard_leave(void *data, struct wl_keyboard *keyboard,
               uint32_t serial, struct wl_surface *surface) {}

static void
keyboard_key(void *data, struct wl_keyboard *keyboard,
             uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    struct magnifier *m = data;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && key == 1)
        m->running = false;
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                   uint32_t serial, uint32_t mods_depressed,
                   uint32_t mods_latched, uint32_t mods_locked,
                   uint32_t group)
{
    struct magnifier *m = data;
    m->ctrl_pressed = mods_depressed & 0x04;
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                     int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t width, uint32_t height)
{
    struct magnifier *m = data;
    m->configure_serial = serial;
    m->ack_pending = true;
    if (width > 0 && height > 0 &&
        ((int)width != m->surface_width || (int)height != m->surface_height)) {
        create_display_buffer(m, (int)width, (int)height);
    }
    m->configured = true;
    if (m->saved) render_and_update(m);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    struct magnifier *m = data;
    m->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void
output_geometry(void *data, struct wl_output *output,
                int32_t x, int32_t y, int32_t width_mm, int32_t height_mm,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
}

static void
output_mode(void *data, struct wl_output *output,
            uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    struct magnifier *m = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        m->output_width = (int)width;
        m->output_height = (int)height;
        m->output_configured = true;
    }
}

static void
output_done(void *data, struct wl_output *output) {}

static void
output_scale(void *data, struct wl_output *output, int32_t factor) {}

static void
output_name(void *data, struct wl_output *output, const char *name) {}

static void
output_description(void *data, struct wl_output *output, const char *description) {}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
    struct magnifier *m = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        m->compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        m->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t seat_ver = version < 8 ? version : 8;
        m->seat = wl_registry_bind(registry, name, &wl_seat_interface, seat_ver);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        m->output = wl_registry_bind(registry, name,
            &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(m->output, &output_listener, m);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        m->layer_shell = wl_registry_bind(registry, name,
            &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        uint32_t sc_ver = version < 3 ? version : 3;
        m->screencopy_manager = wl_registry_bind(registry, name,
            &zwlr_screencopy_manager_v1_interface, sc_ver);
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

static void
init_seat(struct magnifier *m)
{
    if (!m->seat) return;
    m->pointer = wl_seat_get_pointer(m->seat);
    wl_pointer_add_listener(m->pointer, &pointer_listener, m);
    m->keyboard = wl_seat_get_keyboard(m->seat);
    wl_keyboard_add_listener(m->keyboard, &keyboard_listener, m);
}

static void
cleanup(struct magnifier *m)
{
    if (m->frame_buffer) wl_buffer_destroy(m->frame_buffer);
    shm_free(&m->frame_fd, (void**)&m->frame_data, &m->frame_size);
    shm_free(&m->saved_fd, (void**)&m->saved_data, &m->saved_size);
    cleanup_display_buffer(m);
    if (m->layer_surface) zwlr_layer_surface_v1_destroy(m->layer_surface);
    if (m->surface) wl_surface_destroy(m->surface);
    if (m->pointer) wl_pointer_destroy(m->pointer);
    if (m->keyboard) wl_keyboard_destroy(m->keyboard);
    if (m->seat) wl_seat_destroy(m->seat);
    if (m->screencopy_manager) zwlr_screencopy_manager_v1_destroy(m->screencopy_manager);
    if (m->layer_shell) zwlr_layer_shell_v1_destroy(m->layer_shell);
    if (m->compositor) wl_compositor_destroy(m->compositor);
    if (m->shm) wl_shm_destroy(m->shm);
    if (m->output) wl_output_destroy(m->output);
    if (m->registry) wl_registry_destroy(m->registry);
    if (m->display) wl_display_disconnect(m->display);
}

int main(void)
{
    struct magnifier m = {
        .disp_fd = -1, .frame_fd = -1, .saved_fd = -1,
        .running = true,
        .pending_render = false,
        .zoom = 3.0,
        .lens_radius = 180,
        .cursor_x = 960, .cursor_y = 540,
    };

    m.display = wl_display_connect(NULL);
    if (!m.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    m.registry = wl_display_get_registry(m.display);
    wl_registry_add_listener(m.registry, &registry_listener, &m);
    wl_display_roundtrip(m.display);

    if (m.output_configured) {
        m.cursor_x = m.output_width / 2.0;
        m.cursor_y = m.output_height / 2.0;
    }

    if (!m.compositor || !m.shm || !m.seat ||
        !m.layer_shell || !m.screencopy_manager) {
        fprintf(stderr, "Missing required Wayland protocols\n");
        return 1;
    }

    init_seat(&m);

    m.surface = wl_compositor_create_surface(m.compositor);
    if (!m.surface) { fprintf(stderr, "Failed to create surface\n"); return 1; }

    m.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        m.layer_shell, m.surface, m.output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "magnifier");
    zwlr_layer_surface_v1_add_listener(m.layer_surface,
        &layer_surface_listener, &m);

    zwlr_layer_surface_v1_set_anchor(m.layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_exclusive_zone(m.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(m.layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);

    wl_surface_commit(m.surface);
    wl_display_roundtrip(m.display);

    start_capture(&m);
    while (!m.saved && m.running && wl_display_dispatch(m.display) != -1);
    wl_display_flush(m.display);

    while (m.running && wl_display_dispatch(m.display) != -1);

    cleanup(&m);
    return 0;
}
