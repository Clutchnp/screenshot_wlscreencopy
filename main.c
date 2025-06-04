#define _GNU_SOURCE
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-shell.h"
#include "zwp-dmabuf-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <gbm.h>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

struct frame_data {
    struct wl_buffer* buffer;
    struct gbm_device* gbm;
    struct gbm_bo* bo;
    struct wl_display* display;
    int render_node;
    void* shm_data;
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
static struct xdg_toplevel* xdg_toplevel;
static struct xdg_surface* xdg_surface;
static struct zwlr_screencopy_manager_v1* screencopy_manager;
static const struct xdg_surface_listener xdg_surface_listener;
static struct zwlr_screencopy_frame_v1* frame;
int create_shm_file(size_t size) {
    int fd = memfd_create("screencap-shm", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void process_pixels(void* data, int width, int height, int stride, uint32_t format) {
    FILE* f = fopen("capture.png", "wb");
    if (!f) {
        perror("fopen");
        return;
    }
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fprintf(stderr, "Could not allocate write struct\n");
        fclose(f);
    }

    // Create info struct
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        fprintf(stderr, "Could not allocate info struct\n");
        png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
        fclose(f);
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        fprintf(stderr, "Error during png creation\n");
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(f);
    }
    png_init_io(png_ptr, f);

    // Write header (8 bit color depth)
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
      PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

    png_write_info(png_ptr, info_ptr);

    uint8_t* ndata = (uint8_t*)data;
    for (int y = 0; y < height; y++) {
        png_bytep row = malloc(width * 3);
        if (!row) {
            fprintf(stderr, "Failed to allocate row buffer\n");
            break;
        }
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = ndata[y * stride + x * 4 + 2];
            row[x * 3 + 1] = ndata[y * stride + x * 4 + 1];
            row[x * 3 + 2] = ndata[y * stride + x * 4 + 0];
        }
        png_write_row(png_ptr, row);
        free(row);
    }

    png_write_end(png_ptr, info_ptr);

    // Cleanup
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(f);
}

static void frame_buffer(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t format,
  uint32_t width, uint32_t height, uint32_t stride);

static void frame_ready(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t tv_sec_hi,
  uint32_t tv_sec_lo, uint32_t tv_nsec);

static void frame_failed(void* data, struct zwlr_screencopy_frame_v1* frame);

static void dmabuf_handler(void* data, struct zwlr_screencopy_frame_v1* zwlr_screencopy_frame_v1,
  uint32_t format, uint32_t width, uint32_t height);
static void buffer_done(void* data, struct zwlr_screencopy_frame_v1* zwlr_screencopy_frame_v1);
static void flags_recieved(
  void* data, struct zwlr_screencopy_frame_v1* zwlr_screencopy_frame_v1, uint32_t flags);

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .ready = frame_ready,
    .failed = frame_failed,
    .buffer_done = buffer_done,
    .flags = flags_recieved,
    .linux_dmabuf = dmabuf_handler,

};

static void frame_buffer(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t format,
  uint32_t width, uint32_t height, uint32_t stride) {
    printf("Buffer event recieved (ignored)\n");
    // struct frame_data* fdata = data;
    // if (!fdata) {
    //     fprintf(stderr, "Failed to allocate frame_data\n");
    //     return;
    // }
    // fdata->format = format;
    // fdata->width = width;
    // fdata->height = height;
    // fdata->stride = stride;
    // fdata->size = stride * height;
    //
    // int fd = create_shm_file(fdata->size);
    // if (fd < 0) {
    //     fprintf(stderr, "Failed to create shm file\n");
    //     free(fdata);
    //     return;
    // }
    //
    // fdata->shm_data = mmap(NULL, fdata->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    // if (fdata->shm_data == MAP_FAILED) {
    //     perror("mmap");
    //     close(fd);
    //     free(fdata);
    //     return;
    // }
    // struct wl_shm_pool* pool = wl_shm_create_pool(wl_shm, fd, fdata->size);
    // fdata->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
    // WL_SHM_FORMAT_XRGB8888); wl_shm_pool_destroy(pool); close(fd);
}

static void frame_ready(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t tv_sec_hi,
  uint32_t tv_sec_lo, uint32_t tv_nsec) {
    struct frame_data* fdata = data;
    fprintf(stdout, "Frame ready, saving to capture.raw\n");
    printf("w: %d, h: %d, stride: %d\n", fdata->width, fdata->height, fdata->stride);
    // process_pixels(
    //  fdata->shm_data, fdata->width, fdata->height, fdata->stride, fdata->format);
    //
  //
    wl_surface_attach(fdata->surface, fdata->buffer, 0, 0);
    wl_surface_damage(fdata->surface, 0, 0, fdata->width, fdata->height);
    wl_surface_commit(fdata->surface);
    wl_display_flush(fdata->display);
    zwlr_screencopy_frame_v1_destroy(frame);
    wl_buffer_destroy(fdata->buffer);
    gbm_bo_destroy(fdata->bo);
    gbm_device_destroy(fdata->gbm);
    close(fdata->render_node);
    free(fdata);
    exit(0);
}

static void frame_failed(void* data, struct zwlr_screencopy_frame_v1* frame) {
    struct frame_data* fdata = data;
    fprintf(stderr, "Frame capture failed\n");
    zwlr_screencopy_frame_v1_destroy(frame);
    wl_buffer_destroy(fdata->buffer);
    gbm_bo_destroy(fdata->bo);
    gbm_device_destroy(fdata->gbm);
    close(fdata->render_node);
    free(fdata);
    exit(1);
}

static void dmabuf_handler(void* data, struct zwlr_screencopy_frame_v1* zwlr_screencopy_frame_v1,
  uint32_t format, uint32_t width, uint32_t height) {
    struct frame_data* fdata = data;
    fdata->format = format;
    printf("%d \n", format);
    fdata->width = width;
    fdata->height = height;
    fdata->render_node = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fdata->render_node < 0) {
        perror("Failed to open render node");
    }
    fdata->gbm = gbm_create_device(fdata->render_node);
    if (!fdata->gbm) {
        perror("Failed to create gbm device");
        close(fdata->render_node);
    }
    fdata->bo
      = gbm_bo_create(fdata->gbm, fdata->width, fdata->height, fdata->format, GBM_BO_USE_RENDERING);
    if (!fdata->bo) {
        perror("Failed to create gbm buffer");
        gbm_device_destroy(fdata->gbm);
    }
    int fd = gbm_bo_get_fd(fdata->bo);
    if (fd < 0) {
        perror("gbm_bo_get_fd failed");
    }
    fdata->stride = gbm_bo_get_stride(fdata->bo);
    uint64_t modifier = gbm_bo_get_modifier(fdata->bo);
    uint32_t offset = 0; // Usually 0 unless multi-plane
    struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabuf);

    zwp_linux_buffer_params_v1_add(
      params, fd, 0, 0, fdata->stride, modifier >> 32, modifier & 0XFFFFFFFF);
    fdata->buffer = zwp_linux_buffer_params_v1_create_immed(
      params, fdata->width, fdata->height, fdata->format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
}
static void buffer_done(void* data, struct zwlr_screencopy_frame_v1* zwlr_screencopy_frame_v1) {
    struct frame_data* fdata = data;
    zwlr_screencopy_frame_v1_copy(frame, fdata->buffer);
    fprintf(stdout, "buffer done event, copying\n");
}
static void flags_recieved(
  void* data, struct zwlr_screencopy_frame_v1* zwlr_screencopy_frame_v1, uint32_t flags) {
    fprintf(stdout, "flags recieved event, ignored\n");
}

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
}

static void global_remove_handler(void* data, struct wl_registry* registry, uint32_t id) {}

static void configure(void* data, struct xdg_surface* xdg_surface, uint32_t serial){
    xdg_surface_ack_configure(xdg_surface, serial);
    struct frame_data* fdata = data;
    wl_surface_attach(fdata->surface, fdata->buffer, 0, 0);
    wl_surface_commit(fdata->surface);
    sleep(10);
}

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
    .configure = configure,
};

int main() {
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !output || !wl_shm || !screencopy_manager) {
        fprintf(stderr, "Missing required globals\n");
        return 1;
    }

    frame = zwlr_screencopy_manager_v1_capture_output(screencopy_manager, 1, output);
    struct frame_data* fdata = calloc(1, sizeof(struct frame_data));
    fdata->surface = wl_compositor_create_surface(compositor);
    fdata->display = display;
    xdg_wm_base_add_listener(xdg_wm_base, &wm_base_listener, NULL);
    xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, fdata->surface);
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener,fdata);
    xdg_toplevel_set_title(xdg_toplevel, "Wayland Capture Display");
    wl_surface_commit(fdata->surface);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, fdata);
    wl_display_flush(display);
    while (wl_display_dispatch(display) != -1)
        ;
    if (!frame) {
        fprintf(stderr, "Failed to start capture\n");
        return 1;
    }
    zwlr_screencopy_manager_v1_destroy(screencopy_manager);
    wl_display_disconnect(display);
    return 0;
}
