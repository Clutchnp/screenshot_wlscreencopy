#define _GNU_SOURCE
// #include "viewport.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-shell.h"
#include "zwp-dmabuf-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <gbm.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

struct frame_data {
    struct wl_buffer* buffer;
    struct gbm_device* gbm;
    struct gbm_bo* bo;
    // struct wp_viewport* viewport;
    struct wl_display* display;
    int render_node;
    int width, height, stride;
    uint32_t format;
    size_t size;
    struct wl_surface* surface;
};

static void* compositor = NULL;
static void* output = NULL;
static void* wl_shm = NULL;
static void* dmabuf = NULL;
static void* xdg_wm_base = NULL;
static void* viewporter = NULL;
static struct xdg_toplevel* xdg_toplevel;
static struct xdg_surface* xdg_surface;
static struct zwlr_screencopy_manager_v1* screencopy_manager;
static struct zwlr_screencopy_frame_v1* frame;
volatile sig_atomic_t should_exit = 0;

void handle_sigint(int signum) {
    should_exit = 1;
}

static void cleanup(struct frame_data* fdata) {
    if (frame)
        zwlr_screencopy_frame_v1_destroy(frame);
    if (fdata->buffer)
        wl_buffer_destroy(fdata->buffer);
    if (fdata->bo)
        gbm_bo_destroy(fdata->bo);
    if (fdata->gbm)
        gbm_device_destroy(fdata->gbm);
    if (fdata->render_node >= 0)
        close(fdata->render_node);
    if (fdata)
        free(fdata);
    if (xdg_surface)
        xdg_surface_destroy(xdg_surface);
    if (xdg_toplevel)
        xdg_toplevel_destroy(xdg_toplevel);
    if (screencopy_manager)
        zwlr_screencopy_manager_v1_destroy(screencopy_manager);
}

static void frame_buffer(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t format,
  uint32_t width, uint32_t height, uint32_t stride) {
    // Ignored: using linux_dmabuf instead
}

static void frame_ready(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t tv_sec_hi,
  uint32_t tv_sec_lo, uint32_t tv_nsec) {
    struct frame_data* fdata = data;
    printf("Frame ready: %dx%d stride=%d\n", fdata->width, fdata->height, fdata->stride);

    wl_surface_attach(fdata->surface, fdata->buffer, 0, 0);
    // wl_surface_damage(fdata->surface, 0, 0, fdata->width, fdata->height);
    wl_surface_commit(fdata->surface);
    wl_display_flush(fdata->display);
}

static void frame_failed(void* data, struct zwlr_screencopy_frame_v1* frame) {
    fprintf(stderr, "Frame capture failed\n");
    should_exit = 1;
}

static void dmabuf_handler(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t format,
  uint32_t width, uint32_t height) {
    struct frame_data* fdata = data;
    fdata->format = format;
    fdata->width = width;
    fdata->height = height;

    fdata->render_node = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fdata->render_node < 0) {
        perror("open render node");
        should_exit = 1;
        return;
    }

    fdata->gbm = gbm_create_device(fdata->render_node);
    if (!fdata->gbm) {
        perror("create gbm device");
        close(fdata->render_node);
        should_exit = 1;
        return;
    }

    fdata->bo = gbm_bo_create(fdata->gbm, width, height, format, GBM_BO_USE_RENDERING);
    if (!fdata->bo) {
        perror("create gbm buffer");
        gbm_device_destroy(fdata->gbm);
        close(fdata->render_node);
        should_exit = 1;
        return;
    }

    int fd = gbm_bo_get_fd(fdata->bo);
    if (fd < 0) {
        perror("get gbm fd");
        should_exit = 1;
        return;
    }

    fdata->stride = gbm_bo_get_stride(fdata->bo);
    uint64_t modifier = gbm_bo_get_modifier(fdata->bo);

    struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    zwp_linux_buffer_params_v1_add(
      params, fd, 0, 0, fdata->stride, modifier >> 32, modifier & 0xffffffff);

    fdata->buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
}

static void buffer_done(void* data, struct zwlr_screencopy_frame_v1* frame) {
    struct frame_data* fdata = data;
    fprintf(stdout, "Buffer done, issuing copy\n");
    zwlr_screencopy_frame_v1_copy(frame, fdata->buffer);
}

static void flags_received(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t flags) {
    fprintf(stdout, "Flags received (ignored)\n");
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .ready = frame_ready,
    .failed = frame_failed,
    .buffer_done = buffer_done,
    .flags = flags_received,
    .linux_dmabuf = dmabuf_handler,
};

static void global_handler(
  void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 6);
    else if (strcmp(interface, wl_output_interface.name) == 0)
        output = wl_registry_bind(registry, id, &wl_output_interface, 4);
    else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0)
        screencopy_manager = wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, 3);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
        dmabuf = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 5);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 6);
    // else if (strcmp(interface, wp_viewport_interface.name) == 0)
    //     viewporter = wl_registry_bind(registry, id, &wp_viewport_interface, 6);
}

static void global_remove_handler(void* data, struct wl_registry* registry, uint32_t id) {}

static void xdg_surface_configure(void* data, struct xdg_surface* xdg_surface, uint32_t serial) {
    xdg_surface_ack_configure(xdg_surface, serial);
}

static void xdg_toplevel_configure(void* data, struct xdg_toplevel* xdg_toplevel, int32_t width,
  int32_t height, struct wl_array* states) {
    printf("Toplevel configure event: width=%d, height=%d\n", width, height);
}
static void xdg_toplevel_close(void* data, struct xdg_toplevel* xdg_toplevel) {
    should_exit = 1;
}

static void xdg_toplevel_configure_bounds(
  void* data, struct xdg_toplevel* xdg_toplevel, int32_t width, int32_t height) {}

static void xdg_toplevel_wm_capabilities(
  void* data, struct xdg_toplevel* xdg_toplevel, struct wl_array* capabilities) {}

static void xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct wl_registry_listener registry_listener = {
    .global = global_handler,
    .global_remove = global_remove_handler,
};

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities = xdg_toplevel_wm_capabilities,

};

int main() {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !output || !wl_shm || !screencopy_manager || !dmabuf || !xdg_wm_base) {
        fprintf(stderr, "Missing required globals\n");
        return 1;
    }

    struct frame_data* fdata = calloc(1, sizeof(struct frame_data));
    fdata->surface = wl_compositor_create_surface(compositor);
    fdata->display = display;
    // fdata->viewport = wp_viewporter_get_viewport(viewporter, fdata->surface);
    xdg_wm_base_add_listener(xdg_wm_base, &wm_base_listener, NULL);
    xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, fdata->surface);
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, fdata);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(xdg_toplevel, "Wayland Capture Display");
    wl_surface_commit(fdata->surface);

    frame = zwlr_screencopy_manager_v1_capture_output(screencopy_manager, 0, output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, fdata);

    while (!should_exit && wl_display_dispatch(display) != -1)
        ;

    cleanup(fdata);
    wl_display_disconnect(display);
    return 0;
}
