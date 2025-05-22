#define _GNU_SOURCE
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include <fcntl.h>
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
    void* shm_data;
    int width, height, stride;
    uint32_t format;
    size_t size;
};

static void* compositor = NULL;
static void* output = NULL;
static void* wl_shm = NULL;
static struct zwlr_screencopy_manager_v1* screencopy_manager;
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

static void frame_linux_dmabuf(void* data, struct zwlr_screencopy_frame_v1* zwlr_screencopy_frame_v1,
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
    .linux_dmabuf = frame_linux_dmabuf,

};

static void frame_buffer(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t format,
  uint32_t width, uint32_t height, uint32_t stride) {
    struct frame_data* fdata = data;
    if (!fdata) {
        fprintf(stderr, "Failed to allocate frame_data\n");
        return;
    }
    fdata->format = format;
    fdata->width = width;
    fdata->height = height;
    fdata->stride = stride;
    fdata->size = stride * height;

    int fd = create_shm_file(fdata->size);
    if (fd < 0) {
        fprintf(stderr, "Failed to create shm file\n");
        free(fdata);
        return;
    }

    fdata->shm_data = mmap(NULL, fdata->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fdata->shm_data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        free(fdata);
        return;
    }
    struct wl_shm_pool* pool = wl_shm_create_pool(wl_shm, fd, fdata->size);
    fdata->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
}

static void frame_ready(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t tv_sec_hi,
  uint32_t tv_sec_lo, uint32_t tv_nsec) {
    struct frame_data* fdata = data;
    fprintf(stdout, "Frame ready, saving to capture.raw\n");
    printf("w: %d, h: %d, stride: %d\n", fdata->width, fdata->height, fdata->stride);
    process_pixels(
      fdata->shm_data, fdata->width, fdata->height, fdata->stride, WL_SHM_FORMAT_XRGB8888);
    munmap(fdata->shm_data, fdata->size);
    wl_buffer_destroy(fdata->buffer);
    zwlr_screencopy_frame_v1_destroy(frame);
    free(fdata);
    exit(0);
}

static void frame_failed(void* data, struct zwlr_screencopy_frame_v1* frame) {
    struct frame_data* fdata = data;
    fprintf(stderr, "Frame capture failed\n");
    munmap(fdata->shm_data, fdata->size);
    wl_buffer_destroy(fdata->buffer);
    zwlr_screencopy_frame_v1_destroy(frame);
    free(fdata);
    exit(1);
}

static void frame_linux_dmabuf(void* data, struct zwlr_screencopy_frame_v1* zwlr_screencopy_frame_v1,
  uint32_t format, uint32_t width, uint32_t height) {
    fprintf(stderr, "linux_dmabuf event received (ignored)\n");
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
}

static void global_remove_handler(void* data, struct wl_registry* registry, uint32_t id) {}

static const struct wl_registry_listener registry_listener = {
    .global = global_handler,
    .global_remove = global_remove_handler,
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
    fprintf(stdout, "I am heren\n");
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, fdata);
    fprintf(stdout, "I am heren\n");
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
