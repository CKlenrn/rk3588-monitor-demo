/*
 * Copyright © 2015 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/uio.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <linux/videodev2.h>
#include <linux/input.h>
#include <drm.h>
#include <drm_mode.h>
#include <drm/rockchip_drm.h>
#include <rga/im2d.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <libweston/zalloc.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "weston-direct-display-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-client-protocol.h"

#include "shared/helpers.h"
#include "shared/weston-drm-fourcc.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define OPT_FLAG_INVERT (1 << 0)
#define OPT_FLAG_DIRECT_DISPLAY (1 << 1)
#define OPT_FLAG_IMPLICIT_SYNC (1 << 2)
#define OPT_FLAG_TRACE_SYNC (1 << 3)
#define WIN_FLAG_FULLSCREEN (1 << 0)
#define WIN_FLAG_FULLSCREEN_CURSOR (1 << 1)
#define RGA_DST_WIDTH 1080
#define RGA_DST_HEIGHT 1920
#define RGA_TRANSFORM IM_HAL_TRANSFORM_ROT_90
#define FALSE_COLOR_LUT_SIZE 256
#define FALSE_COLOR_LUT_WIDTH 16
#define FALSE_COLOR_LUT_HEIGHT 16
#define ANALYSIS_WIDTH 240
#define ANALYSIS_HEIGHT 135
#define ANALYSIS_INTERVAL 5
#define ANALYSIS_PIXELS (ANALYSIS_WIDTH * ANALYSIS_HEIGHT)
#define ANALYSIS_BYTES ((ANALYSIS_PIXELS + 7) / 8)
/* Gradient span in destination pixels. The 4K source is already scaled down
 * to 1080x1920, so immediate neighbours are interpolated and carry almost no
 * edge energy; sampling across a few pixels recovers it. */
#define ANALYSIS_GRADIENT_SPAN 2
#define ANALYSIS_LUMA_BLACK 16
#define ANALYSIS_LUMA_WHITE 235
#define CAPTURE_TIMEOUT_MS 3000
#define RGA2_POWER_CONTROL_PATH \
	"/sys/devices/platform/fdb80000.rga/power/control"

enum monitor_effect {
	MONITOR_EFFECT_NONE,
	MONITOR_EFFECT_FALSE_COLOR,
};

struct window;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time);

static volatile sig_atomic_t running = 1;
static int exit_status = EXIT_SUCCESS;

static void
runtime_fail(void)
{
	exit_status = EXIT_FAILURE;
	running = 0;
}

static int
xioctl(int fh, unsigned long request, void *arg)
{
	int r;

	do {
		r = ioctl(fh, request, arg);
	} while (r == -1 && errno == EINTR && running);

	return r;
}

static int
wait_for_capture_frame(int fd, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	int ret;

	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret == -1 && errno == EINTR && running);
	if (ret <= 0)
		return ret;
	if (!(pfd.revents & POLLIN)) {
		errno = EIO;
		return -1;
	}
	return 1;
}

static uint32_t
parse_format(const char fmt[4])
{
	return fourcc_code(fmt[0], fmt[1], fmt[2], fmt[3]);
}

static void
build_false_color_lut(uint8_t lut[FALSE_COLOR_LUT_SIZE * 4])
{
	static const uint8_t colors[8][3] = {
		{ 92,  34, 145}, /* underexposed */
		{ 31,  80, 220},
		{  0, 184, 235},
		{ 42, 190,  96},
		{128, 128, 128},
		{238, 112, 165},
		{255, 207,  51},
		{235,  45,  52}, /* clipped highlights */
	};
	int i;

	for (i = 0; i < FALSE_COLOR_LUT_SIZE; ++i) {
		const uint8_t *color = colors[i / 32];

		lut[i * 4 + 0] = color[0];
		lut[i * 4 + 1] = color[1];
		lut[i * 4 + 2] = color[2];
		lut[i * 4 + 3] = 0xff;
	}
}

static int
valid_zoom_factor(int factor)
{
	return factor == 1 || factor == 2 || factor == 4;
}

static int
parse_percentage(const char *text, int minimum, int *value)
{
	char *end = NULL;
	long parsed = strtol(text, &end, 10);

	if (end == text || *end != '\0' || parsed < minimum || parsed > 100)
		return 0;
	*value = (int)parsed;
	return 1;
}

static int
run_self_test(int zoom_factor)
{
	uint8_t lut[FALSE_COLOR_LUT_SIZE * 4];
	int poll_pipe[2];
	uint8_t marker = 1;
	int percentage;

	if (!valid_zoom_factor(zoom_factor))
		return 0;
	if (!parse_percentage("95", 0, &percentage) || percentage != 95 ||
	    parse_percentage("101", 0, &percentage))
		return 0;
	build_false_color_lut(lut);
	if (lut[3] != 0xff || lut[(FALSE_COLOR_LUT_SIZE - 1) * 4 + 3] != 0xff)
		return 0;
	if (memcmp(lut, lut + (FALSE_COLOR_LUT_SIZE - 1) * 4, 3) == 0)
		return 0;
	if (pipe(poll_pipe) == -1)
		return 0;
	if (wait_for_capture_frame(poll_pipe[0], 0) != 0 ||
	    write(poll_pipe[1], &marker, sizeof(marker)) !=
	            (ssize_t)sizeof(marker) ||
	    wait_for_capture_frame(poll_pipe[0], 0) != 1) {
		close(poll_pipe[0]);
		close(poll_pipe[1]);
		return 0;
	}
	close(poll_pipe[0]);
	close(poll_pipe[1]);
	return 1;
}

static inline const char *
dump_format(uint32_t format, char out[4])
{
#if BYTE_ORDER == BIG_ENDIAN
	format = bswap32(format);
#endif
	memcpy(out, &format, 4);
	return out;
}

struct buffer_format {
	int width;
	int height;
	enum v4l2_buf_type type;
	uint32_t format;

	unsigned num_planes;
	unsigned strides[VIDEO_MAX_PLANES];
};

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_shm *shm;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor *default_cursor;
	struct wl_surface *cursor_surface;
	struct xdg_wm_base *wm_base;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct zwp_linux_explicit_synchronization_v1 *explicit_sync;
	struct weston_direct_display_v1 *direct_display;
	struct wp_viewporter *viewporter;
	bool requested_format_found;
	uint32_t opts;

	int v4l_fd;
	struct buffer_format format;
	uint32_t drm_format;
	int rga_drm_fd;
	bool rga_enabled;
	bool rga_checked;
	enum monitor_effect effect;
	int zoom_factor;
	bool zebra_enabled;
	int zebra_level;
	bool peaking_enabled;
	int peaking_sensitivity;
	bool waveform_enabled;
	bool rga_power_policy_changed;
	int false_color_lut_fd;
	uint32_t false_color_lut_gem_handle;
	uint32_t false_color_lut_pitch;
	uint64_t false_color_lut_size;
	rga_buffer_handle_t false_color_lut_handle;
	rga_buffer_t false_color_lut_buffer;
	im_rect false_color_lut_rect;
	bool analysis_output_failed;
	struct window *window;
};

struct buffer {
	struct wl_buffer *buffer;
	struct display *display;
	int busy;
	int index;
	uint32_t sequence;
	struct zwp_linux_buffer_release_v1 *buffer_release;

	int dmabuf_fds[VIDEO_MAX_PLANES];
	int data_offsets[VIDEO_MAX_PLANES];

	int rga_dst_fd;
	uint32_t rga_gem_handle;
	uint32_t rga_dst_pitch;
	uint64_t rga_dst_size;
	rga_buffer_handle_t rga_src_handle;
	rga_buffer_handle_t rga_dst_handle;
	int rga_stage_fd;
	uint32_t rga_stage_gem_handle;
	uint32_t rga_stage_pitch;
	uint64_t rga_stage_size;
	rga_buffer_handle_t rga_stage_handle;
	int rga_completion_fd;
	void *analysis_map;
	size_t analysis_map_size;
	bool analysis_map_failed;
	bool analysis_sync_failed;
	/* This frame was selected for analysis; sampled on the release path. */
	bool analysis_due;
};

#define NUM_BUFFERS 4

struct window {
	struct display *display;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct zwp_linux_surface_synchronization_v1 *surface_sync;
	struct buffer buffers[NUM_BUFFERS];
	struct wl_callback *callback;
	struct wp_viewport *viewport;
	bool wait_for_configure;
	bool initialized;
	bool reported_ready;
	bool fullscreen;
	bool fullscreen_cursor;
};

static void
requeue_released_buffer(struct buffer *buffer);

static void
sample_analysis_frame(struct display *display, struct buffer *buffer);

static void
trace_sync(const struct buffer *buffer, const char *event, int fence_fd)
{
	if (!(buffer->display->opts & OPT_FLAG_TRACE_SYNC))
		return;

	fprintf(stderr,
		"SYNC event=%s sequence=%u index=%d fence_fd=%d\n",
		event, buffer->sequence, buffer->index, fence_fd);
}

static bool
is_sync_file(int fd)
{
	struct sync_file_info info;

	CLEAR(info);
	return ioctl(fd, SYNC_IOC_FILE_INFO, &info) == 0;
}

static int
queue(struct display *display, struct buffer *buffer)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	unsigned i;

	CLEAR(buf);
	buf.type = display->format.type;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = buffer->index;

	if (display->format.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		CLEAR(planes);
		buf.length = VIDEO_MAX_PLANES;
		buf.m.planes = planes;
	}

	if (xioctl(display->v4l_fd, VIDIOC_QUERYBUF, &buf) == -1) {
		perror("VIDIOC_QUERYBUF");
		return 0;
	}

	if (xioctl(display->v4l_fd, VIDIOC_QBUF, &buf) == -1) {
		perror("VIDIOC_QBUF");
		return 0;
	}

	if (display->format.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		if (display->format.num_planes != buf.length) {
			fprintf(stderr, "Wrong number of planes returned by "
			                "QUERYBUF\n");
			return 0;
		}

		for (i = 0; i < buf.length; ++i)
			buffer->data_offsets[i] = buf.m.planes[i].data_offset;
	}

	return 1;
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;

	if (!(mybuf->display->opts & OPT_FLAG_IMPLICIT_SYNC))
		return;
	if (!running)
		return;

	trace_sync(mybuf, "implicit-release", -1);
	requeue_released_buffer(mybuf);
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static int
wait_for_sync_fence(int fence_fd, bool interruptible, const char *name)
{
	struct pollfd pfd = {
		.fd = fence_fd,
		.events = POLLIN,
	};
	int ret;
	int saved_errno;

	if (interruptible && !running) {
		close(fence_fd);
		return -1;
	}

	do {
		ret = poll(&pfd, 1, -1);
	} while (ret == -1 && errno == EINTR &&
		 (!interruptible || running));

	saved_errno = errno;
	close(fence_fd);
	if (ret == -1 && saved_errno == EINTR && interruptible && !running)
		return -1;
	if (ret < 1 || !(pfd.revents & POLLIN) ||
	    (pfd.revents & (POLLERR | POLLNVAL))) {
		fprintf(stderr, "Invalid %s sync_file fence\n", name);
		return 0;
	}

	return 1;
}

static int
wait_for_release_fence(int fence_fd)
{
	return wait_for_sync_fence(fence_fd, true, "release");
}

static int
finish_rga_job(struct buffer *buffer)
{
	int fence_fd;
	int wait_status;

	if (buffer->rga_completion_fd < 0)
		return 1;

	fence_fd = buffer->rga_completion_fd;
	buffer->rga_completion_fd = -1;
	trace_sync(buffer, "rga-wait", fence_fd);
	wait_status = wait_for_sync_fence(fence_fd, false, "RGA completion");
	if (wait_status > 0)
		trace_sync(buffer, "rga-done", fence_fd);

	return wait_status;
}

static void
requeue_released_buffer(struct buffer *buffer)
{
	if (!running)
		return;
	if (finish_rga_job(buffer) <= 0) {
		runtime_fail();
		return;
	}

	/* The RGA job has retired, so the destination holds the exact pixels
	 * that were displayed. Sample before requeueing to V4L2. */
	if (buffer->analysis_due) {
		buffer->analysis_due = false;
		sample_analysis_frame(buffer->display, buffer);
	}

	buffer->busy = 0;
	if (!queue(buffer->display, buffer)) {
		runtime_fail();
		return;
	}
	trace_sync(buffer, "qbuf", -1);
}

static void
buffer_fenced_release(void *data,
		      struct zwp_linux_buffer_release_v1 *release,
		      int32_t fence_fd)
{
	struct buffer *buffer = data;
	int wait_status;

	assert(release == buffer->buffer_release);
	zwp_linux_buffer_release_v1_destroy(release);
	buffer->buffer_release = NULL;
	trace_sync(buffer, "fenced-release", fence_fd);

	wait_status = wait_for_release_fence(fence_fd);
	if (wait_status < 0)
		return;
	if (wait_status == 0) {
		runtime_fail();
		return;
	}

	trace_sync(buffer, "release-signaled", fence_fd);
	requeue_released_buffer(buffer);
}

static void
buffer_immediate_release(void *data,
			 struct zwp_linux_buffer_release_v1 *release)
{
	struct buffer *buffer = data;

	assert(release == buffer->buffer_release);
	zwp_linux_buffer_release_v1_destroy(release);
	buffer->buffer_release = NULL;
	trace_sync(buffer, "immediate-release", -1);
	requeue_released_buffer(buffer);
}

static const struct zwp_linux_buffer_release_v1_listener
buffer_release_listener = {
	buffer_fenced_release,
	buffer_immediate_release,
};

static unsigned int
set_format(struct display *display, uint32_t format)
{
	struct v4l2_format fmt;

	CLEAR(fmt);

	fmt.type = display->format.type;

	if (xioctl(display->v4l_fd, VIDIOC_G_FMT, &fmt) == -1) {
		perror("VIDIOC_G_FMT");
		return 0;
	}

	/* NOTE: pix and pix_mp are in a union, pixelformat member maps between them. */
	const int format_matches = fmt.fmt.pix.pixelformat == format;

	/* No need to set the format if it already is the one we want */
	if (display->format.type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
            format_matches)
		return 1;
	if (display->format.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
            format_matches)
		return fmt.fmt.pix_mp.num_planes;

	fmt.fmt.pix.pixelformat = format;

	if (xioctl(display->v4l_fd, VIDIOC_S_FMT, &fmt) == -1) {
		perror("VIDIOC_S_FMT");
		return 0;
	}

	const int format_was_set = fmt.fmt.pix.pixelformat == format;
	if (!format_was_set) {
		char want_name[4];
		char have_name[4];

		dump_format(format, want_name);
		dump_format(fmt.fmt.pix.pixelformat, have_name);
		fprintf(stderr, "Tried to set format: %.4s but have: %.4s\n",
				 want_name, have_name);
		return 0;
	}

	if (display->format.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return fmt.fmt.pix_mp.num_planes;

	return 1;
}

static int
v4l_connect(struct display *display, const char *dev_name)
{
	struct v4l2_capability cap;
	struct v4l2_requestbuffers req;
	struct v4l2_input input;
	int index_input = -1;
	unsigned int num_planes;

	display->v4l_fd = open(dev_name, O_RDWR);
	if (display->v4l_fd < 0) {
		perror(dev_name);
		return 0;
	}

	if (xioctl(display->v4l_fd, VIDIOC_QUERYCAP, &cap) == -1) {
		if (errno == EINVAL) {
			fprintf(stderr, "%s is no V4L2 device\n", dev_name);
		} else {
			perror("VIDIOC_QUERYCAP");
		}
		return 0;
	}

	if (xioctl(display->v4l_fd, VIDIOC_G_INPUT, &index_input) == 0) {
		input.index = index_input;
		if (xioctl(display->v4l_fd, VIDIOC_ENUMINPUT, &input) == 0) {
			if (input.status & V4L2_IN_ST_VFLIP) {
				fprintf(stdout, "Found camera sensor y-flipped\n");
				display->opts |= OPT_FLAG_INVERT;
			}
		}
	}

	if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
		display->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
		display->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	else {
		fprintf(stderr, "%s is no video capture device\n", dev_name);
		return 0;
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "%s does not support dmabuf i/o\n", dev_name);
		return 0;
	}

	/* Select video input, video standard and tune here */

	num_planes = set_format(display, display->format.format);
	if (num_planes < 1)
		return 0;

	CLEAR(req);

	req.type = display->format.type;
	req.memory = V4L2_MEMORY_MMAP;
	req.count = NUM_BUFFERS * num_planes;

	if (xioctl(display->v4l_fd, VIDIOC_REQBUFS, &req) == -1) {
		if (errno == EINVAL) {
			fprintf(stderr, "%s does not support dmabuf\n",
			        dev_name);
		} else {
			perror("VIDIOC_REQBUFS");
		}
		return 0;
	}

	if (req.count < NUM_BUFFERS * num_planes) {
		fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);
		return 0;
	}

	printf("Created %d buffers\n", req.count);

	return 1;
}

static void
v4l_shutdown(struct display *display)
{
	if (display->v4l_fd >= 0) {
		close(display->v4l_fd);
		display->v4l_fd = -1;
	}
}

static int
rga_power_policy_read(char policy[8])
{
	int fd;
	ssize_t length;

	fd = open(RGA2_POWER_CONTROL_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open " RGA2_POWER_CONTROL_PATH);
		return 0;
	}
	do {
		length = read(fd, policy, 7);
	} while (length < 0 && errno == EINTR);
	close(fd);
	if (length <= 0) {
		if (length < 0)
			perror("read " RGA2_POWER_CONTROL_PATH);
		else
			fprintf(stderr, "RGA2 power policy is empty\n");
		return 0;
	}
	while (length > 0 && (policy[length - 1] == '\n' ||
			      policy[length - 1] == '\r'))
		--length;
	policy[length] = '\0';
	return 1;
}

static int
rga_power_policy_write(const char *policy)
{
	int fd;
	ssize_t written;
	size_t length = strlen(policy);

	fd = open(RGA2_POWER_CONTROL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open " RGA2_POWER_CONTROL_PATH);
		return 0;
	}
	do {
		written = write(fd, policy, length);
	} while (written < 0 && errno == EINTR);
	close(fd);
	if (written != (ssize_t)length) {
		if (written < 0)
			perror("write " RGA2_POWER_CONTROL_PATH);
		else
			fprintf(stderr, "Short write to RGA2 power policy\n");
		return 0;
	}
	return 1;
}

static void
rga_power_policy_restore(struct display *display)
{
	if (!display->rga_power_policy_changed)
		return;
	if (rga_power_policy_write("auto"))
		display->rga_power_policy_changed = false;
	else
		fprintf(stderr, "Failed to restore RGA2 power policy to auto\n");
}

static int
rga_power_policy_acquire(struct display *display)
{
	char policy[8];

	if (display->effect != MONITOR_EFFECT_FALSE_COLOR)
		return 1;
	if (!rga_power_policy_read(policy))
		return 0;
	if (strcmp(policy, "on") == 0)
		return 1;
	if (strcmp(policy, "auto") != 0) {
		fprintf(stderr, "Unsupported RGA2 power policy: %s\n", policy);
		return 0;
	}

	display->rga_power_policy_changed = true;
	if (!rga_power_policy_write("on") ||
	    !rga_power_policy_read(policy) || strcmp(policy, "on") != 0) {
		fprintf(stderr, "Failed to hold RGA2 power policy on\n");
		rga_power_policy_restore(display);
		return 0;
	}
	fprintf(stderr, "RGA2 power policy held on for false color\n");
	return 1;
}

static void
rga_cleanup(struct display *display, struct buffer buffers[NUM_BUFFERS])
{
	int i;

	for (i = 0; i < NUM_BUFFERS; ++i) {
		struct buffer *buffer = &buffers[i];

		if (buffer->rga_completion_fd >= 0 &&
		    finish_rga_job(buffer) <= 0)
			fprintf(stderr, "Failed to retire RGA job for buffer %d\n", i);
		if (buffer->analysis_map) {
			munmap(buffer->analysis_map, buffer->analysis_map_size);
			buffer->analysis_map = NULL;
			buffer->analysis_map_size = 0;
		}
		if (buffer->rga_src_handle) {
			releasebuffer_handle(buffer->rga_src_handle);
			buffer->rga_src_handle = 0;
		}
		if (buffer->rga_dst_handle) {
			releasebuffer_handle(buffer->rga_dst_handle);
			buffer->rga_dst_handle = 0;
		}
		if (buffer->rga_stage_handle) {
			releasebuffer_handle(buffer->rga_stage_handle);
			buffer->rga_stage_handle = 0;
		}
		if (buffer->rga_dst_fd >= 0) {
			close(buffer->rga_dst_fd);
			buffer->rga_dst_fd = -1;
		}
		if (buffer->rga_stage_fd >= 0) {
			close(buffer->rga_stage_fd);
			buffer->rga_stage_fd = -1;
		}
		if (buffer->rga_gem_handle && display->rga_drm_fd >= 0) {
			struct drm_mode_destroy_dumb destroy = {
				.handle = buffer->rga_gem_handle,
			};

			if (ioctl(display->rga_drm_fd,
				  DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
				perror("DRM_IOCTL_MODE_DESTROY_DUMB");
			buffer->rga_gem_handle = 0;
		}
		if (buffer->rga_stage_gem_handle && display->rga_drm_fd >= 0) {
			struct drm_mode_destroy_dumb destroy = {
				.handle = buffer->rga_stage_gem_handle,
			};

			if (ioctl(display->rga_drm_fd,
				  DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
				perror("DRM_IOCTL_MODE_DESTROY_DUMB stage");
			buffer->rga_stage_gem_handle = 0;
		}
	}
	if (display->false_color_lut_handle) {
		releasebuffer_handle(display->false_color_lut_handle);
		display->false_color_lut_handle = 0;
	}
	if (display->false_color_lut_fd >= 0) {
		close(display->false_color_lut_fd);
		display->false_color_lut_fd = -1;
	}
	if (display->false_color_lut_gem_handle && display->rga_drm_fd >= 0) {
		struct drm_mode_destroy_dumb destroy = {
			.handle = display->false_color_lut_gem_handle,
		};

		if (ioctl(display->rga_drm_fd,
			  DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
			perror("DRM_IOCTL_MODE_DESTROY_DUMB LUT");
		display->false_color_lut_gem_handle = 0;
	}
	display->false_color_lut_pitch = 0;
	display->false_color_lut_size = 0;
	CLEAR(display->false_color_lut_buffer);
	CLEAR(display->false_color_lut_rect);

	if (display->rga_drm_fd >= 0) {
		close(display->rga_drm_fd);
		display->rga_drm_fd = -1;
	}
	display->rga_enabled = false;
	display->rga_checked = false;
}

static int
rga_validate_false_color_lut(struct display *display,
			     struct buffer buffers[NUM_BUFFERS])
{
	struct buffer *buffer = &buffers[0];
	rga_buffer_t stage;
	rga_buffer_t dst;
	im_rect image_rect;
	IM_STATUS status;

	stage = wrapbuffer_handle_t(buffer->rga_stage_handle,
			RGA_DST_WIDTH, RGA_DST_HEIGHT,
			buffer->rga_stage_pitch, RGA_DST_HEIGHT,
			RK_FORMAT_YCbCr_400);
	dst = wrapbuffer_handle_t(buffer->rga_dst_handle,
			RGA_DST_WIDTH, RGA_DST_HEIGHT,
			buffer->rga_dst_pitch / 4, RGA_DST_HEIGHT,
			RK_FORMAT_RGBA_8888);
	display->false_color_lut_buffer = wrapbuffer_handle_t(
			display->false_color_lut_handle,
			FALSE_COLOR_LUT_WIDTH, FALSE_COLOR_LUT_HEIGHT,
			FALSE_COLOR_LUT_WIDTH, FALSE_COLOR_LUT_HEIGHT,
			RK_FORMAT_RGBA_8888);
	CLEAR(image_rect);
	CLEAR(display->false_color_lut_rect);
	image_rect.width = RGA_DST_WIDTH;
	image_rect.height = RGA_DST_HEIGHT;
	display->false_color_lut_rect.width = FALSE_COLOR_LUT_WIDTH;
	display->false_color_lut_rect.height = FALSE_COLOR_LUT_HEIGHT;

	status = imcheck_t(stage, dst, display->false_color_lut_buffer,
			   image_rect, image_rect,
			   display->false_color_lut_rect,
			   IM_COLOR_PALETTE);
	if (status != IM_STATUS_NOERROR) {
		fprintf(stderr, "RGA palette imcheck failed: %s\n",
			imStrError(status));
		return 0;
	}
	fprintf(stderr, "RGA false-color LUT validated\n");
	return 1;
}

static int
rga_init(struct display *display, struct buffer buffers[NUM_BUFFERS])
{
	bool false_color = display->effect == MONITOR_EFFECT_FALSE_COLOR;
	int i;

	if (!(display->opts & OPT_FLAG_DIRECT_DISPLAY) ||
	    display->format.format != V4L2_PIX_FMT_NV16 ||
	    display->format.num_planes != 1) {
		if (false_color)
			fprintf(stderr, "False color requires direct-display NV16 input\n");
		return false_color ? 0 : 1;
	}
	if ((!false_color && display->drm_format != DRM_FORMAT_NV16) ||
	    (false_color && display->drm_format != DRM_FORMAT_XBGR8888)) {
		fprintf(stderr, "RGA output format does not match monitor effect\n");
		return 0;
	}
	if (!valid_zoom_factor(display->zoom_factor))
		return 0;

	display->rga_drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (display->rga_drm_fd < 0) {
		perror("open /dev/dri/card0 for RGA buffers");
		return 0;
	}
	if (false_color) {
		uint8_t lut[FALSE_COLOR_LUT_SIZE * 4];
		struct drm_mode_create_dumb create;
		struct drm_prime_handle prime;
		im_handle_param_t lut_param;
		struct dma_buf_sync sync;
		uint8_t *map;
		int y;

		CLEAR(create);
		create.width = FALSE_COLOR_LUT_WIDTH;
		create.height = FALSE_COLOR_LUT_HEIGHT;
		create.bpp = 32;
		create.flags = ROCKCHIP_BO_DMA32;
		if (ioctl(display->rga_drm_fd,
			  DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
			perror("DRM_IOCTL_MODE_CREATE_DUMB LUT");
			goto fail;
		}
		display->false_color_lut_gem_handle = create.handle;
		display->false_color_lut_pitch = create.pitch;
		display->false_color_lut_size = create.size;
		if (create.pitch < FALSE_COLOR_LUT_WIDTH * 4 ||
		    create.size < (uint64_t)create.pitch * FALSE_COLOR_LUT_HEIGHT) {
			fprintf(stderr, "Invalid RGA false-color LUT pitch/size\n");
			goto fail;
		}

		CLEAR(prime);
		prime.handle = create.handle;
		prime.flags = DRM_CLOEXEC | DRM_RDWR;
		if (ioctl(display->rga_drm_fd,
			  DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
			perror("DRM_IOCTL_PRIME_HANDLE_TO_FD LUT");
			goto fail;
		}
		display->false_color_lut_fd = prime.fd;
		map = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, prime.fd, 0);
		if (map == MAP_FAILED) {
			perror("mmap RGA false-color LUT");
			goto fail;
		}

		build_false_color_lut(lut);
		CLEAR(sync);
		sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
		if (ioctl(prime.fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
			perror("DMA_BUF_IOCTL_SYNC START LUT");
			munmap(map, create.size);
			goto fail;
		}
		for (y = 0; y < FALSE_COLOR_LUT_HEIGHT; ++y)
			memcpy(map + (size_t)y * create.pitch,
			       lut + (size_t)y * FALSE_COLOR_LUT_WIDTH * 4,
			       FALSE_COLOR_LUT_WIDTH * 4);
		sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
		if (ioctl(prime.fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
			perror("DMA_BUF_IOCTL_SYNC END LUT");
			munmap(map, create.size);
			goto fail;
		}
		munmap(map, create.size);

		CLEAR(lut_param);
		lut_param.width = create.pitch / 4;
		lut_param.height = FALSE_COLOR_LUT_HEIGHT;
		lut_param.format = RK_FORMAT_RGBA_8888;
		display->false_color_lut_handle = importbuffer_fd(prime.fd, &lut_param);
		if (!display->false_color_lut_handle) {
			fprintf(stderr, "RGA false-color LUT import failed\n");
			goto fail;
		}
	}

	for (i = 0; i < NUM_BUFFERS; ++i) {
		struct buffer *buffer = &buffers[i];
		struct drm_mode_create_dumb create;
		struct drm_mode_create_dumb stage_create;
		struct drm_prime_handle prime;
		im_handle_param_t import_param;
		uint32_t minimum_pitch;
		uint64_t minimum_size;

		CLEAR(create);
		create.width = RGA_DST_WIDTH;
		create.height = false_color ? RGA_DST_HEIGHT : RGA_DST_HEIGHT * 2;
		create.bpp = false_color ? 32 : 8;
		create.flags = ROCKCHIP_BO_DMA32 |
			(false_color ? ROCKCHIP_BO_CONTIG : 0);
		if (ioctl(display->rga_drm_fd,
			  DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
			perror("DRM_IOCTL_MODE_CREATE_DUMB");
			goto fail;
		}

		buffer->rga_gem_handle = create.handle;
		buffer->rga_dst_pitch = create.pitch;
		buffer->rga_dst_size = create.size;
		minimum_pitch = RGA_DST_WIDTH * (false_color ? 4 : 1);
		minimum_size = (uint64_t)create.pitch * RGA_DST_HEIGHT *
			(false_color ? 1 : 2);
		if (create.pitch < minimum_pitch || create.size < minimum_size) {
			fprintf(stderr, "Invalid RGA dumb buffer pitch/size\n");
			goto fail;
		}

		CLEAR(prime);
		prime.handle = create.handle;
		prime.flags = DRM_CLOEXEC | DRM_RDWR;
		if (ioctl(display->rga_drm_fd,
			  DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
			perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
			goto fail;
		}
		buffer->rga_dst_fd = prime.fd;

		CLEAR(import_param);
		import_param.width = false_color ? create.pitch / 4 : create.pitch;
		import_param.height = RGA_DST_HEIGHT;
		import_param.format = false_color ? RK_FORMAT_RGBA_8888
						   : RK_FORMAT_YCbCr_422_SP;
		buffer->rga_dst_handle =
			importbuffer_fd(buffer->rga_dst_fd, &import_param);
		if (!buffer->rga_dst_handle) {
			fprintf(stderr, "RGA destination import failed for buffer %d\n", i);
			goto fail;
		}

		fprintf(stderr,
			"RGA output buffer %d: %dx%d %s pitch=%u size=%llu fd=%d\n",
			i, RGA_DST_WIDTH, RGA_DST_HEIGHT,
			false_color ? "XB24" : "NV16",
			buffer->rga_dst_pitch,
			(unsigned long long)buffer->rga_dst_size,
			buffer->rga_dst_fd);

		if (!false_color)
			continue;

		CLEAR(stage_create);
		stage_create.width = RGA_DST_WIDTH;
		stage_create.height = RGA_DST_HEIGHT;
		stage_create.bpp = 8;
		stage_create.flags = ROCKCHIP_BO_CONTIG | ROCKCHIP_BO_DMA32;
		if (ioctl(display->rga_drm_fd,
			  DRM_IOCTL_MODE_CREATE_DUMB, &stage_create) < 0) {
			perror("DRM_IOCTL_MODE_CREATE_DUMB stage");
			goto fail;
		}
		buffer->rga_stage_gem_handle = stage_create.handle;
		buffer->rga_stage_pitch = stage_create.pitch;
		buffer->rga_stage_size = stage_create.size;
		if (stage_create.pitch < RGA_DST_WIDTH ||
		    stage_create.size < (uint64_t)stage_create.pitch * RGA_DST_HEIGHT) {
			fprintf(stderr, "Invalid RGA false-color stage pitch/size\n");
			goto fail;
		}

		CLEAR(prime);
		prime.handle = stage_create.handle;
		prime.flags = DRM_CLOEXEC | DRM_RDWR;
		if (ioctl(display->rga_drm_fd,
			  DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
			perror("DRM_IOCTL_PRIME_HANDLE_TO_FD stage");
			goto fail;
		}
		buffer->rga_stage_fd = prime.fd;

		CLEAR(import_param);
		import_param.width = stage_create.pitch;
		import_param.height = RGA_DST_HEIGHT;
		import_param.format = RK_FORMAT_YCbCr_400;
		buffer->rga_stage_handle =
			importbuffer_fd(buffer->rga_stage_fd, &import_param);
		if (!buffer->rga_stage_handle) {
			fprintf(stderr, "RGA false-color stage import failed for buffer %d\n", i);
			goto fail;
		}
	}
	if (false_color && !rga_validate_false_color_lut(display, buffers))
		goto fail;

	display->rga_enabled = true;
	fprintf(stderr, "RGA landscape path enabled: ROT_90 zoom=%dx effect=%s\n",
		display->zoom_factor, false_color ? "false-color" : "none");
	return 1;

fail:
	rga_cleanup(display, buffers);
	return 0;
}

static int
rga_import_source(struct display *display, struct buffer *buffer)
{
	im_handle_param_t import_param;

	if (!display->rga_enabled)
		return 1;
	if (buffer->data_offsets[0] != 0) {
		fprintf(stderr, "RGA does not support source data_offset=%d\n",
			buffer->data_offsets[0]);
		return 0;
	}

	CLEAR(import_param);
	import_param.width = display->format.strides[0];
	import_param.height = display->format.height;
	import_param.format = display->effect == MONITOR_EFFECT_FALSE_COLOR
			? RK_FORMAT_YCbCr_400 : RK_FORMAT_YCbCr_422_SP;
	buffer->rga_src_handle =
		importbuffer_fd(buffer->dmabuf_fds[0], &import_param);
	if (!buffer->rga_src_handle) {
		fprintf(stderr, "RGA source import failed for buffer %d\n",
			buffer->index);
		return 0;
	}

	return 1;
}

static int
rga_process(struct display *display, struct buffer *buffer,
	    int *acquire_fence_fd, int *release_fence_fd)
{
	rga_buffer_t src;
	rga_buffer_t dst;
	rga_buffer_t stage;
	im_rect src_rect;
	im_rect dst_rect;
	im_rect stage_rect;
	IM_STATUS status;
	bool false_color = display->effect == MONITOR_EFFECT_FALSE_COLOR;
	int stage_fence_fd = -1;
	int usage = RGA_TRANSFORM | IM_ASYNC;
	int crop_width = (display->format.width / display->zoom_factor) & ~1;
	int crop_height = (display->format.height / display->zoom_factor) & ~1;

	if (crop_width < 2 || crop_height < 2) {
		fprintf(stderr, "RGA zoom crop is too small\n");
		return 0;
	}

	src = wrapbuffer_handle_t(buffer->rga_src_handle,
			display->format.width, display->format.height,
			display->format.strides[0], display->format.height,
			false_color ? RK_FORMAT_YCbCr_400
				    : RK_FORMAT_YCbCr_422_SP);
	dst = wrapbuffer_handle_t(buffer->rga_dst_handle,
			RGA_DST_WIDTH, RGA_DST_HEIGHT,
			false_color ? buffer->rga_dst_pitch / 4
				    : buffer->rga_dst_pitch,
			RGA_DST_HEIGHT,
			false_color ? RK_FORMAT_RGBA_8888
				    : RK_FORMAT_YCbCr_422_SP);
	CLEAR(stage);
	CLEAR(src_rect);
	CLEAR(dst_rect);
	CLEAR(stage_rect);
	src_rect.x = ((display->format.width - crop_width) / 2) & ~1;
	src_rect.y = ((display->format.height - crop_height) / 2) & ~1;
	src_rect.width = crop_width;
	src_rect.height = crop_height;
	dst_rect.width = RGA_DST_WIDTH;
	dst_rect.height = RGA_DST_HEIGHT;

	if (false_color) {
		stage = wrapbuffer_handle_t(buffer->rga_stage_handle,
				RGA_DST_WIDTH, RGA_DST_HEIGHT,
				buffer->rga_stage_pitch, RGA_DST_HEIGHT,
				RK_FORMAT_YCbCr_400);
		stage_rect.width = RGA_DST_WIDTH;
		stage_rect.height = RGA_DST_HEIGHT;
	}

	if (!display->rga_checked) {
		status = imcheck_t(src, false_color ? stage : dst, (rga_buffer_t){0},
				   src_rect, false_color ? stage_rect : dst_rect,
				   (im_rect){0}, usage);
		if (status != IM_STATUS_NOERROR) {
			fprintf(stderr, "RGA transform imcheck failed: %s\n",
				imStrError(status));
			return 0;
		}
		display->rga_checked = true;
	}

	*release_fence_fd = -1;
	status = improcessOpt(src, false_color ? stage : dst, (rga_buffer_t){0},
			      src_rect, false_color ? stage_rect : dst_rect,
			      (im_rect){0}, *acquire_fence_fd,
			      false_color ? &stage_fence_fd : release_fence_fd,
			      NULL, usage);
	if (status != IM_STATUS_SUCCESS) {
		fprintf(stderr, "RGA transform failed: %s\n", imStrError(status));
		return 0;
	}

	/* Driver 1.3+ consumes the acquire fence after a successful submit. */
	*acquire_fence_fd = -1;
	if (false_color) {
		/*
		 * The palette pass runs on RGA2, which does not reliably honour
		 * an input acquire fence: chaining the two jobs asynchronously
		 * let the palette read the stage buffer while the transform was
		 * still writing it, so every frame tore and the picture
		 * flickered. Retire the transform on the CPU first, then run the
		 * palette synchronously. The destination is final on return, so
		 * this frame needs no acquire fence at all.
		 */
		if (stage_fence_fd < 0 || !is_sync_file(stage_fence_fd)) {
			fprintf(stderr,
				"RGA transform did not return a valid stage sync_file\n");
			if (stage_fence_fd >= 0)
				close(stage_fence_fd);
			return 0;
		}
		if (wait_for_sync_fence(stage_fence_fd, false,
					"RGA false-color stage") <= 0)
			return 0;

		status = improcessOpt(stage, dst,
				      display->false_color_lut_buffer,
				      stage_rect, dst_rect,
				      display->false_color_lut_rect,
				      -1, NULL, NULL, IM_COLOR_PALETTE);
		if (status != IM_STATUS_SUCCESS) {
			fprintf(stderr, "RGA false-color palette failed: %s\n",
				imStrError(status));
			return 0;
		}

		*release_fence_fd = -1;
		return 1;
	}
	if (*release_fence_fd < 0 || !is_sync_file(*release_fence_fd)) {
		fprintf(stderr, "RGA did not return a valid release sync_file\n");
		if (*release_fence_fd >= 0)
			close(*release_fence_fd);
		*release_fence_fd = -1;
		return 0;
	}

	return 1;
}

static bool
analysis_enabled(const struct display *display)
{
	return display->zebra_enabled || display->peaking_enabled ||
		display->waveform_enabled;
}

static void
analysis_set_bit(uint8_t bitmap[ANALYSIS_BYTES], int x, int y)
{
	int bit = y * ANALYSIS_WIDTH + x;

	bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static int
analysis_zebra_threshold(int level)
{
	/* HDMI RX delivers limited-range Y, so a percentage has to map onto
	 * 16..235 rather than 0..255 or high percentages never trigger. */
	return ANALYSIS_LUMA_BLACK +
		level * (ANALYSIS_LUMA_WHITE - ANALYSIS_LUMA_BLACK) / 100;
}

static int
analysis_peaking_threshold(int sensitivity)
{
	int threshold = 110 - sensitivity;

	return threshold < 12 ? 12 : threshold;
}

static int
run_analysis_self_test(void)
{
	uint8_t bitmap[ANALYSIS_BYTES];

	CLEAR(bitmap);
	analysis_set_bit(bitmap, 0, 0);
	analysis_set_bit(bitmap, ANALYSIS_WIDTH - 1, ANALYSIS_HEIGHT - 1);
	if (ANALYSIS_BYTES != 4050 || (bitmap[0] & 1u) == 0 ||
	    (bitmap[ANALYSIS_BYTES - 1] & 0x80u) == 0)
		return 0;

	/* Zebra thresholds must stay inside limited-range Y so that the
	 * default 95% is reachable by real highlights. */
	if (analysis_zebra_threshold(0) != ANALYSIS_LUMA_BLACK ||
	    analysis_zebra_threshold(100) != ANALYSIS_LUMA_WHITE ||
	    analysis_zebra_threshold(95) >= ANALYSIS_LUMA_WHITE ||
	    analysis_zebra_threshold(95) <= 200)
		return 0;

	/* Peaking must stay positive and monotonically easier to trigger as
	 * sensitivity rises. */
	if (analysis_peaking_threshold(1) <= analysis_peaking_threshold(100) ||
	    analysis_peaking_threshold(100) < 1)
		return 0;

	/* Gradient sampling must stay inside the destination buffer. */
	return ANALYSIS_GRADIENT_SPAN >= 1 &&
		ANALYSIS_GRADIENT_SPAN < RGA_DST_WIDTH / 2 &&
		ANALYSIS_GRADIENT_SPAN < RGA_DST_HEIGHT / 2;
}

static int
write_analysis_frame(int fd, uint32_t sequence,
			 const uint8_t zebra[ANALYSIS_BYTES],
			 const uint8_t peaking[ANALYSIS_BYTES],
			 const uint8_t waveform[ANALYSIS_BYTES])
{
	char header[64];
	struct iovec parts[4];
	int header_size;
	int flags;
	int pipe_size;
	int pending;
	size_t frame_size;
	ssize_t written;

	header_size = snprintf(header, sizeof header, "MDA1 %u %d %d %d\n",
			       sequence, ANALYSIS_WIDTH, ANALYSIS_HEIGHT,
			       ANALYSIS_BYTES);
	if (header_size <= 0 || header_size >= (int)sizeof header)
		return -1;
	frame_size = (size_t)header_size + ANALYSIS_BYTES * 3;
	flags = fcntl(fd, F_GETFL);
	pipe_size = fcntl(fd, F_GETPIPE_SZ);
	if (flags < 0 || pipe_size < 0 || ioctl(fd, FIONREAD, &pending) < 0)
		return -1;
	if (pending < 0 || pipe_size < pending ||
	    (size_t)(pipe_size - pending) < frame_size)
		return 0;
	if (!(flags & O_NONBLOCK) && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	parts[0] = (struct iovec){ .iov_base = header,
					  .iov_len = (size_t)header_size };
	parts[1] = (struct iovec){ .iov_base = (void *)zebra,
					  .iov_len = ANALYSIS_BYTES };
	parts[2] = (struct iovec){ .iov_base = (void *)peaking,
					  .iov_len = ANALYSIS_BYTES };
	parts[3] = (struct iovec){ .iov_base = (void *)waveform,
					  .iov_len = ANALYSIS_BYTES };
	written = writev(fd, parts, 4);
	if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return 0;
	return written == (ssize_t)frame_size ? 1 : -1;
}

static int
run_analysis_output_self_test(void)
{
	uint8_t bitmap[ANALYSIS_BYTES];
	int output_pipe[2];
	int frames = 0;
	int status;

	CLEAR(bitmap);
	if (pipe(output_pipe) == -1)
		return 0;
	do {
		status = write_analysis_frame(output_pipe[1], (uint32_t)frames,
					      bitmap, bitmap, bitmap);
		if (status == 1)
			++frames;
	} while (status == 1 && frames < 16);
	close(output_pipe[0]);
	close(output_pipe[1]);
	return frames > 0 && status == 0;
}

static void
sample_analysis_frame(struct display *display, struct buffer *buffer)
{
	uint8_t zebra[ANALYSIS_BYTES];
	uint8_t peaking[ANALYSIS_BYTES];
	uint8_t waveform[ANALYSIS_BYTES];
	struct dma_buf_sync sync;
	uint8_t *luma;
	uint32_t pitch;
	uint64_t size;
	int fd;
	int zebra_threshold = analysis_zebra_threshold(display->zebra_level);
	int peaking_threshold =
		analysis_peaking_threshold(display->peaking_sensitivity);
	int gx;
	int gy;

	/* Callers only reach here after the producing RGA job has retired. */
	if (!analysis_enabled(display))
		return;

	if (display->effect == MONITOR_EFFECT_FALSE_COLOR) {
		fd = buffer->rga_stage_fd;
		pitch = buffer->rga_stage_pitch;
		size = buffer->rga_stage_size;
	} else {
		fd = buffer->rga_dst_fd;
		pitch = buffer->rga_dst_pitch;
		size = buffer->rga_dst_size;
	}
	if (fd < 0 || pitch < RGA_DST_WIDTH ||
	    size < (uint64_t)pitch * RGA_DST_HEIGHT)
		return;

	if (!buffer->analysis_map && !buffer->analysis_map_failed) {
		buffer->analysis_map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
		if (buffer->analysis_map == MAP_FAILED) {
			buffer->analysis_map = NULL;
			buffer->analysis_map_failed = true;
			fprintf(stderr, "Analysis DMA-BUF mmap unavailable for buffer %d\n",
				buffer->index);
			return;
		}
		buffer->analysis_map_size = (size_t)size;
	}
	if (!buffer->analysis_map || buffer->analysis_sync_failed)
		return;

	CLEAR(sync);
	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
	if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
		buffer->analysis_sync_failed = true;
		fprintf(stderr, "Analysis DMA-BUF read sync unavailable for buffer %d\n",
			buffer->index);
		return;
	}

	CLEAR(zebra);
	CLEAR(peaking);
	CLEAR(waveform);
	luma = buffer->analysis_map;

	for (gx = 0; gx < ANALYSIS_WIDTH; ++gx) {
		int py = (gx * RGA_DST_HEIGHT + RGA_DST_HEIGHT / 2) /
			ANALYSIS_WIDTH;
		uint8_t *row;

		if (py < ANALYSIS_GRADIENT_SPAN)
			py = ANALYSIS_GRADIENT_SPAN;
		if (py > RGA_DST_HEIGHT - 1 - ANALYSIS_GRADIENT_SPAN)
			py = RGA_DST_HEIGHT - 1 - ANALYSIS_GRADIENT_SPAN;
		row = luma + (size_t)py * pitch;
		for (gy = 0; gy < ANALYSIS_HEIGHT; ++gy) {
			int px = RGA_DST_WIDTH - 1 -
				(gy * RGA_DST_WIDTH + RGA_DST_WIDTH / 2) /
				ANALYSIS_HEIGHT;
			int value;

			if (px < ANALYSIS_GRADIENT_SPAN)
				px = ANALYSIS_GRADIENT_SPAN;
			if (px > RGA_DST_WIDTH - 1 - ANALYSIS_GRADIENT_SPAN)
				px = RGA_DST_WIDTH - 1 - ANALYSIS_GRADIENT_SPAN;
			value = row[px];

			if (display->zebra_enabled && value >= zebra_threshold &&
			    (gx + gy) % 10 < 4)
				analysis_set_bit(zebra, gx, gy);
			if (display->peaking_enabled) {
				int horizontal = abs(
					luma[(size_t)(py + ANALYSIS_GRADIENT_SPAN) * pitch + px] -
					luma[(size_t)(py - ANALYSIS_GRADIENT_SPAN) * pitch + px]);
				int vertical = abs(row[px + ANALYSIS_GRADIENT_SPAN] -
						   row[px - ANALYSIS_GRADIENT_SPAN]);

				if (horizontal + vertical >= peaking_threshold)
					analysis_set_bit(peaking, gx, gy);
			}
			if (display->waveform_enabled) {
				int level = value - ANALYSIS_LUMA_BLACK;
				int span = ANALYSIS_LUMA_WHITE - ANALYSIS_LUMA_BLACK;
				int wave_y;

				if (level < 0)
					level = 0;
				if (level > span)
					level = span;
				wave_y = ANALYSIS_HEIGHT - 1 -
					level * (ANALYSIS_HEIGHT - 1) / span;
				analysis_set_bit(waveform, gx, wave_y);
			}
		}
	}

	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
		buffer->analysis_sync_failed = true;

	if (!display->analysis_output_failed) {
		int output_status = write_analysis_frame(STDOUT_FILENO,
				buffer->sequence, zebra, peaking, waveform);

		if (output_status < 0) {
			display->analysis_output_failed = true;
			fprintf(stderr, "Analysis output failed; video continues\n");
		}
	}
}

/*
 * Sampling right after commit always lost the fence race: the RGA job was
 * submitted microseconds earlier and needs about a millisecond, so the
 * non-blocking fence poll never saw it ready and every analysis frame was
 * dropped. Mark the buffer instead and sample it once its release path has
 * waited on the RGA fence, where the pixels are guaranteed final and the
 * buffer has not yet been requeued to V4L2.
 */
static void
schedule_analysis_frame(struct display *display, struct buffer *buffer)
{
	buffer->analysis_due = analysis_enabled(display) &&
		buffer->sequence % ANALYSIS_INTERVAL == 0;
}

static void
create_succeeded(void *data,
		 struct zwp_linux_buffer_params_v1 *params,
		 struct wl_buffer *new_buffer)
{
	struct buffer *buffer = data;
	unsigned i;

	buffer->buffer = new_buffer;
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

	zwp_linux_buffer_params_v1_destroy(params);

	if (!buffer->display->rga_enabled) {
		for (i = 0; i < buffer->display->format.num_planes; ++i) {
			close(buffer->dmabuf_fds[i]);
			buffer->dmabuf_fds[i] = -1;
		}
	}
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	struct buffer *buffer = data;
	unsigned i;

	buffer->buffer = NULL;

	zwp_linux_buffer_params_v1_destroy(params);

	if (!buffer->display->rga_enabled) {
		for (i = 0; i < buffer->display->format.num_planes; ++i) {
			close(buffer->dmabuf_fds[i]);
			buffer->dmabuf_fds[i] = -1;
		}
	}

	runtime_fail();

	fprintf(stderr, "Error: zwp_linux_buffer_params.create failed.\n");
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	create_succeeded,
	create_failed
};

static void
create_dmabuf_buffer(struct display *display, struct buffer *buffer)
{
	struct zwp_linux_buffer_params_v1 *params;
	uint64_t modifier;
	uint32_t flags;
	int i;

	modifier = 0;
	flags = 0;

	if (display->opts & OPT_FLAG_INVERT)
		flags |= ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT;

	params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);

	if ((display->opts & OPT_FLAG_DIRECT_DISPLAY) && display->direct_display) {
		weston_direct_display_v1_enable(display->direct_display, params);

		if (display->opts & OPT_FLAG_INVERT) {
			flags &= ~ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT;
			fprintf(stdout, "dmabuf y-inverted attribute flag was removed"
					", as display-direct flag was set\n");
		}
	}

	if (display->rga_enabled) {
		if (display->effect == MONITOR_EFFECT_FALSE_COLOR) {
			zwp_linux_buffer_params_v1_add(params,
					       buffer->rga_dst_fd,
					       0, 0,
					       buffer->rga_dst_pitch,
					       modifier >> 32,
					       modifier & 0xffffffff);
			zwp_linux_buffer_params_v1_add_listener(params,
						    &params_listener, buffer);
			fprintf(stderr,
				"creating RGA buffer %d of size %dx%d format XB24 "
				"stride %u flags %u\n",
				buffer->index, RGA_DST_WIDTH, RGA_DST_HEIGHT,
				buffer->rga_dst_pitch, flags);
			zwp_linux_buffer_params_v1_create(params,
						  RGA_DST_WIDTH,
						  RGA_DST_HEIGHT,
						  DRM_FORMAT_XBGR8888,
						  flags);
			return;
		}

		uint32_t uv_offset =
			buffer->rga_dst_pitch * RGA_DST_HEIGHT;

		zwp_linux_buffer_params_v1_add(params,
					       buffer->rga_dst_fd,
					       0, 0,
					       buffer->rga_dst_pitch,
					       modifier >> 32,
					       modifier & 0xffffffff);
		zwp_linux_buffer_params_v1_add(params,
					       buffer->rga_dst_fd,
					       1, uv_offset,
					       buffer->rga_dst_pitch,
					       modifier >> 32,
					       modifier & 0xffffffff);
		zwp_linux_buffer_params_v1_add_listener(params,
						    &params_listener, buffer);
		fprintf(stderr,
			"creating RGA buffer %d of size %dx%d format NV16 "
			"stride %u uv_offset %u flags %u\n",
			buffer->index, RGA_DST_WIDTH, RGA_DST_HEIGHT,
			buffer->rga_dst_pitch, uv_offset, flags);
		zwp_linux_buffer_params_v1_create(params,
						  RGA_DST_WIDTH,
						  RGA_DST_HEIGHT,
						  DRM_FORMAT_NV16,
						  flags);
		return;
	}

	const int num_planes = (int) display->format.num_planes;

	for (i = 0; i < num_planes; ++i) {
		fprintf(stderr, "buffer %d, plane %d has dma fd %d and stride "
				"%d and modifier %" PRIu64 "\n",
				buffer->index, i, buffer->dmabuf_fds[i],
				display->format.strides[i], modifier);
		zwp_linux_buffer_params_v1_add(params,
		                               buffer->dmabuf_fds[i],
		                               i, /* plane_idx */
		                               buffer->data_offsets[i], /* offset */
		                               display->format.strides[i],
		                               modifier >> 32,
		                               modifier & 0xffffffff);
	}

	/* Some v4l2 devices can output NV12, but will do so without the MPLANE
	 * api. Instead, it outputs both the luminance and chrominance planes
	 * in the same dma buffer. Here we account for that, and add an extra
	 * plane from the same buffer if necessary. If it needs an extra plane,
	 * set the stride of the chrominance plane. NOTE: Also handles cases
	 * where 3 planes are expected in 1 dma buffer (untested)
	 */
	enum plane_layout_t {
		DISJOINT = 0,
		CONTIGUOUS,
	};
	enum chrom_packing_t {
		CHROM_SEPARATE = 0, /* Cr/Cb are in their own planes. */
		CHROM_COMBINED,     /* Cr/Cb are interleaved. */
	};

	/* This table contains some planar formats we could fix-up and support. */
	const struct planar_layout_t {
		/* Format identification. */
		uint32_t v4l_fourcc;
		/* Disjoint or contigious planes? */
		enum plane_layout_t plane_layout;
		/* Zero if Cb/Cr in separate planes. */
		enum chrom_packing_t chrom_packing;
		/* Expected plane count. */
		int num_planes;
		/* Horizontal sub-sampling for chroma. */
		int chroma_subsample_hori;
		/* Vertical sub-sampling for chroma. */
		int chroma_subsample_vert;
	} planar_layouts[] = {
		{ V4L2_PIX_FMT_NV12M,	DISJOINT,	CHROM_COMBINED,	2, 2,2 },
		{ V4L2_PIX_FMT_NV21M,	DISJOINT,	CHROM_COMBINED,	2, 2,2 },
		{ V4L2_PIX_FMT_NV16M,	DISJOINT,	CHROM_COMBINED,	2, 2,1 },
		{ V4L2_PIX_FMT_NV61M,	DISJOINT,	CHROM_COMBINED,	2, 2,1 },
		{ V4L2_PIX_FMT_NV12,	CONTIGUOUS,	CHROM_COMBINED,	2, 2,2 },
		{ V4L2_PIX_FMT_NV21,	CONTIGUOUS,	CHROM_COMBINED,	2, 2,2 },
		{ V4L2_PIX_FMT_NV16,	CONTIGUOUS,	CHROM_COMBINED,	2, 2,1 },
		{ V4L2_PIX_FMT_NV61,	CONTIGUOUS,	CHROM_COMBINED,	2, 2,1 },
		{ V4L2_PIX_FMT_NV24,	CONTIGUOUS,	CHROM_COMBINED,	2, 1,1 },
		{ V4L2_PIX_FMT_NV42,	CONTIGUOUS,	CHROM_COMBINED,	2, 1,1 },
		{ V4L2_PIX_FMT_YUV420,	CONTIGUOUS,	CHROM_SEPARATE,	3, 2,2 },
		{ V4L2_PIX_FMT_YVU420,	CONTIGUOUS,    	CHROM_SEPARATE,	3, 2,2 },
		{ V4L2_PIX_FMT_YUV420M,	DISJOINT,	CHROM_SEPARATE,	3, 2,2 },
		{ V4L2_PIX_FMT_YVU420M,	DISJOINT,	CHROM_SEPARATE,	3, 2,2 },
		{ 0, 0, 0, 0, 0 },
	};

	int layoutnr = 0;
	int num_missing_planes = 0;	/* Non-zero if format needs more planes in dma buf. */
	int stride_extra_plane = 0;
	int vrtres_extra_plane = 0;
	const uint32_t stride0 = display->format.strides[0];

	/* Search the table. */
	while (planar_layouts[layoutnr].v4l_fourcc) {
		const struct planar_layout_t *layout =
			planar_layouts + layoutnr;

		if (layout->v4l_fourcc == display->format.format) {
			/* If disjoint planes are missing, there is nothing to
			 * salvage. */
			if (layout->plane_layout == DISJOINT)
				assert(num_planes == layout->num_planes);

			/* Is this a case where we need to add 1 or 2 missing
			 * planes? */
			num_missing_planes = layout->num_planes - num_planes;
			if (num_missing_planes > 0) {
				/* With this knowledge:
				 * - Stride for Y
				 * - Packing of chrominance
				 * - Horizontal subsampling ...we can compute
				 *   the stride for Cr and Cb.
				 */
				const uint32_t num_chrom_parts =
                                        layout->chrom_packing == CHROM_COMBINED ? 2 : 1;
				stride_extra_plane =
					stride0 * num_chrom_parts /
					layout->chroma_subsample_hori;
				vrtres_extra_plane =
                                        display->format.height /
					layout->chroma_subsample_vert;
				break;
			}
		}
		layoutnr += 1;
	}
	/* If we determined we need additional planes, add them. */
	int offset_in_buffer = buffer->data_offsets[0] +
			       display->format.height * stride0;

	for (i = 0; i < num_missing_planes; ++i) {
		/* Add same dma buffer, but with offset for chromimance plane. */
		fprintf(stderr,"Adding additional chrominance plane.\n");
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fds[0],
					       1 + i, /* plane_idx */
					       offset_in_buffer,
					       stride_extra_plane,
					       modifier >> 32,
					       modifier & 0xffffffff);
		offset_in_buffer += vrtres_extra_plane * stride_extra_plane;
	}

	zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer);

	fprintf(stderr,"creating buffer of size %dx%d format %c%c%c%c flags %d\n",
		display->format.width,
		display->format.height,
		(display->drm_format >>  0) & 0xff,
		(display->drm_format >>  8) & 0xff,
		(display->drm_format >> 16) & 0xff,
		(display->drm_format >> 24) & 0xff,
		flags
	);
	zwp_linux_buffer_params_v1_create(params,
	                                  display->format.width,
	                                  display->format.height,
	                                  display->drm_format,
	                                  flags);
}

static int
buffer_export(struct display *display, int index, int dmafd[])
{
	struct v4l2_exportbuffer expbuf;
	unsigned i;

	CLEAR(expbuf);

	for (i = 0; i < display->format.num_planes; ++i) {
		expbuf.type = display->format.type;
		expbuf.index = index;
		expbuf.plane = i;
		if (xioctl(display->v4l_fd, VIDIOC_EXPBUF, &expbuf) == -1) {
			perror("VIDIOC_EXPBUF");
			while (i)
				close(dmafd[--i]);
			return 0;
		}
		dmafd[i] = expbuf.fd;
	}

	return 1;
}

static int
queue_initial_buffers(struct display *display,
                      struct buffer buffers[NUM_BUFFERS])
{
	struct buffer *buffer;
	int index;

	for (index = 0; index < NUM_BUFFERS; ++index) {
		buffer = &buffers[index];
		buffer->display = display;
		buffer->index = index;
		for (unsigned i = 0; i < VIDEO_MAX_PLANES; ++i)
			buffer->dmabuf_fds[i] = -1;

		if (!queue(display, buffer)) {
			fprintf(stderr, "Failed to queue buffer\n");
			return 0;
		}

		assert(!buffer->buffer);
		if (!buffer_export(display, index, buffer->dmabuf_fds))
			return 0;
		if (!rga_import_source(display, buffer))
			return 0;

		create_dmabuf_buffer(display, buffer);
	}

	return 1;
}

static int
dequeue(struct display *display, int *acquire_fence_fd, uint32_t *sequence)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	uint32_t raw_fence_fd;
	int wait_status;

	CLEAR(buf);
	CLEAR(planes);
	buf.type = display->format.type;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.length = VIDEO_MAX_PLANES;
	buf.m.planes = planes;

	wait_status = wait_for_capture_frame(display->v4l_fd,
	                                     CAPTURE_TIMEOUT_MS);
	if (wait_status == 0) {
		fprintf(stderr, "Timed out waiting for a V4L2 capture frame\n");
		errno = ETIMEDOUT;
		return -1;
	}
	if (wait_status < 0) {
		if (!running && errno == EINTR)
			return -2;
		perror("poll V4L2 capture frame");
		return -1;
	}

	if (xioctl(display->v4l_fd, VIDIOC_DQBUF, &buf) == -1) {
		if (!running && errno == EINTR)
			return -2;
		perror("VIDIOC_DQBUF");
		return -1;
	}

	*sequence = buf.sequence;
	if (acquire_fence_fd) {
		raw_fence_fd = (uint32_t)buf.timecode.userbits[0] |
			((uint32_t)buf.timecode.userbits[1] << 8) |
			((uint32_t)buf.timecode.userbits[2] << 16) |
			((uint32_t)buf.timecode.userbits[3] << 24);
		if (raw_fence_fd == 0 || raw_fence_fd == UINT32_MAX ||
		    raw_fence_fd > INT32_MAX ||
		    fcntl((int)raw_fence_fd, F_GETFD) == -1 ||
		    !is_sync_file((int)raw_fence_fd)) {
			if (raw_fence_fd > 0 && raw_fence_fd <= INT32_MAX &&
			    fcntl((int)raw_fence_fd, F_GETFD) != -1)
				close((int)raw_fence_fd);
			fprintf(stderr,
				"HDMI RX did not return a valid sync_file acquire fence; "
				"low_latency must be enabled before QBUF\n");
			return -1;
		}
		*acquire_fence_fd = (int)raw_fence_fd;
	}

	return buf.index;
}

static int
fill_buffer_format(struct display *display)
{
	struct v4l2_format fmt;
	struct v4l2_pix_format *pix;
	struct v4l2_pix_format_mplane *pix_mp;
	int i;
	char buf[4];

	CLEAR(fmt);
	fmt.type = display->format.type;

	/* Preserve original settings as set by v4l2-ctl for example */
	if (xioctl(display->v4l_fd, VIDIOC_G_FMT, &fmt) == -1) {
		perror("VIDIOC_G_FMT");
		return 0;
	}

	if (display->format.type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		pix = &fmt.fmt.pix;

		printf("%d×%d, %.4s\n", pix->width, pix->height,
		       dump_format(pix->pixelformat, buf));

		display->format.num_planes = 1;
		display->format.width = pix->width;
		display->format.height = pix->height;
		display->format.strides[0] = pix->bytesperline;
	} else {
		pix_mp = &fmt.fmt.pix_mp;

		display->format.num_planes = pix_mp->num_planes;
		display->format.width = pix_mp->width;
		display->format.height = pix_mp->height;

		for (i = 0; i < pix_mp->num_planes; ++i)
			display->format.strides[i] = pix_mp->plane_fmt[i].bytesperline;

		printf("%d×%d, %.4s, %d planes\n",
		       pix_mp->width, pix_mp->height,
		       dump_format(pix_mp->pixelformat, buf),
		       pix_mp->num_planes);
	}

	return 1;
}

static int
v4l_init(struct display *display, struct buffer buffers[NUM_BUFFERS]) {
	int i;

	for (i = 0; i < NUM_BUFFERS; ++i) {
		buffers[i].rga_dst_fd = -1;
		buffers[i].rga_stage_fd = -1;
		buffers[i].rga_completion_fd = -1;
	}

	if (!fill_buffer_format(display)) {
		fprintf(stderr, "Failed to fill buffer format\n");
		return 0;
	}
	if (!rga_init(display, buffers)) {
		fprintf(stderr, "Failed to initialize RGA landscape path\n");
		return 0;
	}

	if (!queue_initial_buffers(display, buffers)) {
		fprintf(stderr, "Failed to queue initial buffers\n");
		rga_cleanup(display, buffers);
		return 0;
	}

	return 1;
}

static int
start_capture(struct display *display)
{
	int type = display->format.type;

	if (xioctl(display->v4l_fd, VIDIOC_STREAMON, &type) == -1) {
		perror("VIDIOC_STREAMON");
		return 0;
	}

	return 1;
}

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *surface,
			     uint32_t serial)
{
	struct window *window = data;

	xdg_surface_ack_configure(surface, serial);

	if (window->initialized && window->wait_for_configure)
		redraw(window, NULL, 0);
	window->wait_for_configure = false;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	xdg_surface_handle_configure,
};

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *toplevel,
			      int32_t width, int32_t height,
			      struct wl_array *states)
{
	struct window *window = data;
	uint32_t *p;

	window->fullscreen = 0;
	wl_array_for_each(p, states) {
		uint32_t state = *p;
		switch (state) {
		case XDG_TOPLEVEL_STATE_FULLSCREEN:
			window->fullscreen = 1;
			break;
		}
	}

	if (!window->viewport)
		return;

	if (window->fullscreen) {
		int source_width = window->display->format.height;
		int source_height = window->display->format.width;
		float ratio_w;
		float ratio_h;
		int32_t viewport_w;
		int32_t viewport_h;

		if (window->display->opts & OPT_FLAG_DIRECT_DISPLAY) {
			int source_w = window->display->rga_enabled ?
				RGA_DST_WIDTH : window->display->format.width;
			int source_h = window->display->rga_enabled ?
				RGA_DST_HEIGHT : window->display->format.height;

			wp_viewport_set_source(window->viewport, 0, 0,
					       source_w * 256, source_h * 256);
			wp_viewport_set_destination(window->viewport, width, height);
			return;
		}

		ratio_w = (float)width / source_width;
		ratio_h = (float)height / source_height;

		if (ratio_w > ratio_h) {
			viewport_w = width / ratio_w * ratio_h;
			viewport_h = height;
		} else {
			viewport_w = width;
			viewport_h = height / ratio_h * ratio_w;
		}

		wp_viewport_set_destination(window->viewport, viewport_w,
					    viewport_h);
	} else {
		wp_viewport_set_destination(window->viewport, -1, -1);
	}
}

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	xdg_toplevel_handle_configure,
	xdg_toplevel_handle_close,
};

static struct window *
create_window(struct display *display, uint32_t win_flags)
{
	struct window *window;
	int i;
	unsigned j;

	window = zalloc(sizeof *window);
	if (!window)
		return NULL;

	window->callback = NULL;
	window->display = display;
	for (i = 0; i < NUM_BUFFERS; ++i) {
		for (j = 0; j < VIDEO_MAX_PLANES; ++j)
			window->buffers[i].dmabuf_fds[j] = -1;
		window->buffers[i].rga_dst_fd = -1;
		window->buffers[i].rga_stage_fd = -1;
		window->buffers[i].rga_completion_fd = -1;
	}
	window->surface = wl_compositor_create_surface(display->compositor);
	if (!(display->opts & OPT_FLAG_DIRECT_DISPLAY)) {
		if (wl_proxy_get_version((struct wl_proxy *)window->surface) < 2) {
			fprintf(stderr, "wl_surface buffer transforms are unavailable\n");
			goto error;
		}
		wl_surface_set_buffer_transform(window->surface,
						WL_OUTPUT_TRANSFORM_90);
	}
	if (!(display->opts & OPT_FLAG_IMPLICIT_SYNC)) {
		window->surface_sync =
			zwp_linux_explicit_synchronization_v1_get_synchronization(
				display->explicit_sync, window->surface);
		if (!window->surface_sync)
			goto error;
	}

	if (display->wm_base) {
		if (display->viewporter) {
			window->viewport =
				wp_viewporter_get_viewport(display->viewporter,
							   window->surface);
		}

		window->xdg_surface =
			xdg_wm_base_get_xdg_surface(display->wm_base,
						    window->surface);

		assert(window->xdg_surface);

		xdg_surface_add_listener(window->xdg_surface,
					 &xdg_surface_listener, window);

		window->xdg_toplevel =
			xdg_surface_get_toplevel(window->xdg_surface);

		assert(window->xdg_toplevel);

		xdg_toplevel_add_listener(window->xdg_toplevel,
					  &xdg_toplevel_listener, window);

		xdg_toplevel_set_title(window->xdg_toplevel, "simple-dmabuf-v4l");
		xdg_toplevel_set_app_id(window->xdg_toplevel,
				"org.freedesktop.weston.simple-dmabuf-v4l");

		if (win_flags & WIN_FLAG_FULLSCREEN)
			xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
		if (win_flags & WIN_FLAG_FULLSCREEN_CURSOR)
			window->fullscreen_cursor = true;

		/* Set empty input region so touch/pointer events pass
		 * through to the Qt overlay surface beneath us in the
		 * compositor's stacking order. */
		{
			struct wl_region *empty =
				wl_compositor_create_region(display->compositor);
			wl_surface_set_input_region(window->surface, empty);
			wl_region_destroy(empty);
		}

		window->wait_for_configure = true;
		wl_surface_commit(window->surface);
	} else {
		goto error;
	}

	return window;

error:
	if (window->surface_sync)
		zwp_linux_surface_synchronization_v1_destroy(window->surface_sync);
	if (window->surface)
		wl_surface_destroy(window->surface);
	free(window);
	return NULL;
}

static void
destroy_window(struct window *window)
{
	int i;
	unsigned j;

	if (window->display->opts & OPT_FLAG_TRACE_SYNC)
		fprintf(stderr,
			"SYNC event=shutdown sequence=0 index=-1 fence_fd=-1\n");

	if (window->callback)
		wl_callback_destroy(window->callback);

	if (window->viewport)
		wp_viewport_destroy(window->viewport);
	if (window->surface_sync)
		zwp_linux_surface_synchronization_v1_destroy(window->surface_sync);

	if (window->xdg_toplevel)
		xdg_toplevel_destroy(window->xdg_toplevel);
	if (window->xdg_surface)
		xdg_surface_destroy(window->xdg_surface);
	wl_surface_destroy(window->surface);

	for (i = 0; i < NUM_BUFFERS; i++) {
		if (window->buffers[i].buffer_release)
			zwp_linux_buffer_release_v1_destroy(
				window->buffers[i].buffer_release);
		if (window->buffers[i].buffer)
			wl_buffer_destroy(window->buffers[i].buffer);
	}

	rga_cleanup(window->display, window->buffers);

	for (i = 0; i < NUM_BUFFERS; i++) {
		for (j = 0; j < window->display->format.num_planes; ++j) {
			if (window->buffers[i].dmabuf_fds[j] >= 0) {
				close(window->buffers[i].dmabuf_fds[j]);
				window->buffers[i].dmabuf_fds[j] = -1;
			}
		}
	}

	v4l_shutdown(window->display);

	free(window);
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	struct buffer *buffer;
	int acquire_fence_fd = -1;
	int rga_release_fence_fd = -1;
	int wait_status;
	int index, num_busy = 0;
	uint32_t sequence;
	bool use_explicit_sync =
		!(window->display->opts & OPT_FLAG_IMPLICIT_SYNC);

	if (!running)
		return;

	/* Check for a deadlock situation where we would block forever trying
	 * to dequeue a buffer while all of them are locked by the compositor.
	 */
	for (index = 0; index < NUM_BUFFERS; ++index)
		if (window->buffers[index].busy)
			++num_busy;

	/* A robust application would just postpone redraw until it has queued
	 * a buffer.
	 */
	assert(num_busy < NUM_BUFFERS);

	index = dequeue(window->display,
			use_explicit_sync ? &acquire_fence_fd : NULL,
			&sequence);
	if (index == -2)
		return;
	if (index < 0) {
		/* We couldn’t get any buffer out of the camera, exiting. */
		runtime_fail();
		return;
	}

	buffer = &window->buffers[index];
	buffer->sequence = sequence;
	assert(!buffer->busy);
	assert(!buffer->buffer_release);
	trace_sync(buffer, "dqbuf", acquire_fence_fd);

	if (window->display->rga_enabled) {
		assert(buffer->rga_completion_fd < 0);
		trace_sync(buffer, "rga-submit", acquire_fence_fd);
		if (!rga_process(window->display, buffer,
				 &acquire_fence_fd, &rga_release_fence_fd))
			goto rga_fail;

		/* The false-color palette pass already retired on the CPU, so
		 * there is no fence to hand over and the pixels are final. */
		if (rga_release_fence_fd < 0) {
			trace_sync(buffer, "rga-synchronous", -1);
			if (acquire_fence_fd >= 0) {
				close(acquire_fence_fd);
				acquire_fence_fd = -1;
			}
			goto rga_done;
		}

		trace_sync(buffer, "rga-release", rga_release_fence_fd);
		buffer->rga_completion_fd =
			fcntl(rga_release_fence_fd, F_DUPFD_CLOEXEC, 0);
		if (buffer->rga_completion_fd < 0) {
			perror("dup RGA completion fence");
			buffer->rga_completion_fd = rga_release_fence_fd;
			rga_release_fence_fd = -1;
			goto rga_fail;
		}

		if (use_explicit_sync) {
			acquire_fence_fd = rga_release_fence_fd;
			rga_release_fence_fd = -1;
		} else {
			close(rga_release_fence_fd);
			rga_release_fence_fd = -1;
			wait_status = finish_rga_job(buffer);
			if (wait_status <= 0)
				goto rga_fail;
		}
	}

rga_done:
	if (use_explicit_sync) {
		/* A synchronous RGA path leaves no acquire fence to set, but the
		 * release listener is still what requeues the V4L2 buffer. */
		if (acquire_fence_fd >= 0) {
			zwp_linux_surface_synchronization_v1_set_acquire_fence(
				window->surface_sync, acquire_fence_fd);
			trace_sync(buffer, "acquire-set", acquire_fence_fd);
			close(acquire_fence_fd);
			acquire_fence_fd = -1;
		}
		buffer->buffer_release =
			zwp_linux_surface_synchronization_v1_get_release(
				window->surface_sync);
		zwp_linux_buffer_release_v1_add_listener(
			buffer->buffer_release, &buffer_release_listener, buffer);
	}

	/* Marked before commit so the release callback for this frame always
	 * observes the flag, whenever the compositor delivers it. */
	schedule_analysis_frame(window->display, buffer);

	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, INT32_MAX, INT32_MAX);

	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
	wl_surface_commit(window->surface);
	trace_sync(buffer, "commit", -1);
	if (!window->reported_ready) {
		fprintf(stderr, "STREAM_READY sync=%s sequence=%u\n",
			use_explicit_sync ? "explicit" : "implicit", sequence);
		window->reported_ready = true;
	}
	buffer->busy = 1;
	return;

rga_fail:
	if (acquire_fence_fd >= 0)
		close(acquire_fence_fd);
	if (rga_release_fence_fd >= 0)
		close(rga_release_fence_fd);
	runtime_fail();
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
dmabuf_modifier(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct display *d = data;
	uint64_t modifier = u64_from_u32s(modifier_hi, modifier_lo);

	if (format == d->drm_format && !DRM_MOD_VALID(modifier))
		d->requested_format_found = true;
}


static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
              uint32_t format)
{
	/* deprecated */
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format,
	dmabuf_modifier
};

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
		     uint32_t serial, struct wl_surface *surface,
		     wl_fixed_t sx, wl_fixed_t sy)
{
	struct display *display = data;
	struct wl_buffer *buffer;
	struct wl_cursor *cursor = display->default_cursor;
	struct wl_cursor_image *image;

	if (display->window->fullscreen && !display->window->fullscreen_cursor)
		wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
	else if (cursor) {
		image = cursor->images[0];
		buffer = wl_cursor_image_get_buffer(image);
		if (!buffer)
			return;
		wl_pointer_set_cursor(pointer, serial,
				      display->cursor_surface,
				      image->hotspot_x,
				      image->hotspot_y);
		wl_surface_attach(display->cursor_surface, buffer, 0, 0);
		wl_surface_damage(display->cursor_surface, 0, 0,
				  image->width, image->height);
		wl_surface_commit(display->cursor_surface);
	}
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
		     uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
		      uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
}

static void
pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, uint32_t time, uint32_t button,
		      uint32_t state)
{
	struct display *display = data;

	if (!display->window->xdg_toplevel)
		return;

	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED)
		xdg_toplevel_move(display->window->xdg_toplevel,
				  display->seat, serial);
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
		    uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,
	pointer_handle_leave,
	pointer_handle_motion,
	pointer_handle_button,
	pointer_handle_axis,
};

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                       uint32_t format, int fd, uint32_t size)
{
	/* Just so we don’t leak the keymap fd */
	close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                      uint32_t serial, struct wl_surface *surface,
                      struct wl_array *keys)
{
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
                      uint32_t serial, struct wl_surface *surface)
{
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                    uint32_t serial, uint32_t time, uint32_t key,
                    uint32_t state)
{
	struct display *d = data;

	if (!d->wm_base)
		return;

	if (key == KEY_F11 && state) {
		if (d->window->fullscreen)
			xdg_toplevel_unset_fullscreen(d->window->xdg_toplevel);
		else
			xdg_toplevel_set_fullscreen(d->window->xdg_toplevel, NULL);
	} else if (key == KEY_ESC && state)
		running = false;
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                          uint32_t serial, uint32_t mods_depressed,
                          uint32_t mods_latched, uint32_t mods_locked,
                          uint32_t group)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
                         enum wl_seat_capability caps)
{
	struct display *d = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
		d->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(d->pointer, &pointer_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
		wl_pointer_destroy(d->pointer);
		d->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
		d->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
		wl_keyboard_destroy(d->keyboard);
		d->keyboard = NULL;
	}
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
	xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	xdg_wm_base_ping,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
                       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		d->compositor =
			wl_registry_bind(registry,
			                 id, &wl_compositor_interface,
			                 version < 4 ? version : 4);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		d->seat = wl_registry_bind(registry,
		                           id, &wl_seat_interface, 1);
		wl_seat_add_listener(d->seat, &seat_listener, d);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		d->shm = wl_registry_bind(registry, id,
					  &wl_shm_interface, 1);
		d->cursor_theme = wl_cursor_theme_load(NULL, 32, d->shm);
		if (!d->cursor_theme) {
			fprintf(stderr, "unable to load default theme\n");
			return;
		}
		d->default_cursor =
			wl_cursor_theme_get_cursor(d->cursor_theme, "left_ptr");
		if (!d->default_cursor) {
			fprintf(stderr, "unable to load default left pointer\n");
			// TODO: abort ?
		}
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		d->wm_base = wl_registry_bind(registry,
					      id, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(d->wm_base, &wm_base_listener, d);
	} else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		d->dmabuf = wl_registry_bind(registry,
		                             id, &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener,
		                                 d);
	} else if (strcmp(interface,
			  zwp_linux_explicit_synchronization_v1_interface.name) == 0) {
		d->explicit_sync = wl_registry_bind(
			registry, id,
			&zwp_linux_explicit_synchronization_v1_interface, 1);
	} else if (strcmp(interface, weston_direct_display_v1_interface.name) == 0) {
		d->direct_display = wl_registry_bind(registry,
						     id, &weston_direct_display_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		d->viewporter = wl_registry_bind(registry, id,
						 &wp_viewporter_interface,
						 1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static struct display *
create_display(uint32_t requested_format, uint32_t opt_flags)
{
	struct display *display;

	display = zalloc(sizeof *display);
	if (display == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	display->display = wl_display_connect(NULL);
	assert(display->display);

	display->drm_format = requested_format;
	display->opts = opt_flags;
	display->v4l_fd = -1;
	display->rga_drm_fd = -1;
	display->false_color_lut_fd = -1;

	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry,
	                         &registry_listener, display);
	wl_display_roundtrip(display->display);
	if (display->dmabuf == NULL) {
		fprintf(stderr, "No zwp_linux_dmabuf global\n");
		exit(1);
	}
	if (!(opt_flags & OPT_FLAG_IMPLICIT_SYNC) &&
	    display->explicit_sync == NULL) {
		fprintf(stderr,
			"No zwp_linux_explicit_synchronization_v1 global\n");
		exit(1);
	}

	wl_display_roundtrip(display->display);

	if (!display->requested_format_found &&
	    !((opt_flags & OPT_FLAG_DIRECT_DISPLAY) &&
	      display->direct_display)) {
		char want_name[4];

		dump_format(requested_format, want_name);
		fprintf(stderr, "Requested DRM format %4s not available\n", want_name);
		exit(1);
	}

	display->cursor_surface =
		wl_compositor_create_surface(display->compositor);

	return display;
}

static void
destroy_display(struct display *display)
{
	rga_power_policy_restore(display);
	wl_surface_destroy(display->cursor_surface);

	if (display->dmabuf)
		zwp_linux_dmabuf_v1_destroy(display->dmabuf);
	if (display->explicit_sync)
		zwp_linux_explicit_synchronization_v1_destroy(
			display->explicit_sync);

	if (display->viewporter)
		wp_viewporter_destroy(display->viewporter);

	if (display->wm_base)
		xdg_wm_base_destroy(display->wm_base);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);
	free(display);
}

static void
usage(const char *argv0)
{
	printf("Usage: %s [-v v4l2_device] [-f v4l2_format] [-d drm_format] [-i|--y-invert] [-g|--d-display] [-s|--fullscreen] [-q|--implicit-sync] [-t|--trace-sync] [-p|--probe] [-e|--false-color] [-z|--zoom 1|2|4] [-Z|--zebra 0..100] [-P|--peaking 1..100] [-W|--waveform] [-T|--self-test]\n"
	       "\n"
	       "The default V4L2 device is /dev/video0\n"
	       "\n"
	       "Both formats are FOURCC values (see http://fourcc.org/)\n"
	       "V4L2 formats are defined in <linux/videodev2.h>\n"
	       "DRM formats are defined in <libdrm/drm_fourcc.h>\n"
	       "The default for both formats is YUYV.\n"
	       "If the V4L2 and DRM formats differ, the data is simply "
	       "reinterpreted rather than converted.\n\n"
	       "Flags:\n"
	       "- y-invert force the image to be y-flipped;\n  note will be "
	       "automatically added if we detect if the camera sensor is "
	       "y-flipped\n"
	       "- d-display skip importing dmabuf-based buffer into the GPU\n  "
	       "and attempt pass the buffer straight to the display controller\n"
	       "- fullscreen make the window fullscreen and scale up the image\n"
	       "- false-color use a two-stage RGA luminance palette path; requires NV16 and XB24\n"
	       "- zoom select a centered 1x, 2x, or 4x RGA source crop\n"
	       "- zebra add a sampled luma warning mask at the selected percentage\n"
	       "- peaking add a sampled luma-gradient mask at the selected sensitivity\n"
	       "- waveform emit a sampled luma waveform for the Qt HUD\n"
	       "- trace-sync print per-buffer synchronization transitions\n"
	       "- fs-cursor show the cursor in fullscreen mode\n",
	       argv0);

	printf("\n"
	       "How to set up Vivid the virtual video driver for testing:\n"
	       "- build your kernel with CONFIG_VIDEO_VIVID=m\n"
	       "- add this to a /etc/modprobe.d/ file:\n"
	       "    options vivid node_types=0x1 num_inputs=1 input_types=0x00\n"
	       "- modprobe vivid and check which device was created,\n"
	       "  here we assume /dev/video0\n"
	       "- set the pixel format:\n"
	       "    $ v4l2-ctl -d /dev/video0 --set-fmt-video=width=640,pixelformat=XR24\n"
	       "- optionally could add 'allocators=0x1' to options as to create"
	       "  the buffer in a dmabuf-contiguous way\n"
	       "  (as some display-controllers require it)\n"
	       "- launch the demo:\n"
	       "    $ %s -v /dev/video0 -f XR24 -d XR24\n"
	       "You should see a test pattern with color bars, and some text.\n"
	       "\n"
	       "More about vivid: https://www.kernel.org/doc/Documentation/video4linux/vivid.txt\n"
	       "\n", argv0);

	exit(0);
}

static void
signal_int(int signum)
{
	running = false;
}

int
main(int argc, char **argv)
{
	struct sigaction sigint;
	pid_t parent_pid;
	struct display *display;
	struct window *window;
	const char *v4l_device = NULL;
	uint32_t v4l_format = 0x0;
	uint32_t drm_format = 0x0;
	uint32_t opts_flags = 0x0;
	uint32_t win_flags = 0x0;
	enum monitor_effect effect = MONITOR_EFFECT_NONE;
	int zoom_factor = 1;
	bool zebra_enabled = false;
	int zebra_level = 95;
	bool peaking_enabled = false;
	int peaking_sensitivity = 50;
	bool waveform_enabled = false;
	bool probe_only = false;
	bool self_test = false;
	int c, opt_index, ret = 0;

	parent_pid = getppid();
	if (prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
		perror("PR_SET_PDEATHSIG");
		return 1;
	}
	if (getppid() != parent_pid)
		return 1;

	static struct option long_options[] = {
		{ "v4l2-device", required_argument, NULL, 'v' },
		{ "v4l2-format", required_argument, NULL, 'f' },
		{ "drm-format",	 required_argument, NULL, 'd' },
		{ "y-invert",    no_argument, 	    NULL, 'i' },
		{ "d-display",   no_argument, 	    NULL, 'g' },
		{ "fullscreen",  no_argument, 	    NULL, 's' },
		{ "fs-cursor",   no_argument, 	    NULL, 'c' },
		{ "implicit-sync", no_argument,     NULL, 'q' },
		{ "trace-sync",  no_argument,       NULL, 't' },
		{ "probe",       no_argument,       NULL, 'p' },
		{ "false-color", no_argument,       NULL, 'e' },
		{ "zoom",        required_argument, NULL, 'z' },
		{ "zebra",       required_argument, NULL, 'Z' },
		{ "peaking",     required_argument, NULL, 'P' },
		{ "waveform",    no_argument,       NULL, 'W' },
		{ "self-test",   no_argument,       NULL, 'T' },
		{ "help",        no_argument,       NULL, 'h' },
		{ 0,             0,                 NULL,  0  }
	};

	while ((c = getopt_long(argc, argv, "heiv:d:f:gscqtpz:Z:P:WT", long_options,
				&opt_index)) != -1) {
		switch (c) {
		case 'v':
			v4l_device = optarg;
			break;
		case 'f':
			v4l_format = parse_format(optarg);
			break;
		case 'd':
			drm_format = parse_format(optarg);
			break;
		case 'i':
			opts_flags |= OPT_FLAG_INVERT;
			break;
		case 'g':
			opts_flags |= OPT_FLAG_DIRECT_DISPLAY;
			break;
		case 's':
			win_flags |= WIN_FLAG_FULLSCREEN;
			break;
		case 'c':
			win_flags |= WIN_FLAG_FULLSCREEN_CURSOR;
			break;
		case 'q':
			opts_flags |= OPT_FLAG_IMPLICIT_SYNC;
			break;
		case 't':
			opts_flags |= OPT_FLAG_TRACE_SYNC;
			break;
		case 'p':
			probe_only = true;
			break;
		case 'e':
			effect = MONITOR_EFFECT_FALSE_COLOR;
			break;
		case 'z': {
			char *end = NULL;
			long value = strtol(optarg, &end, 10);

			if (end == optarg || *end != '\0' ||
			    (value != 1 && value != 2 && value != 4)) {
				fprintf(stderr, "zoom must be 1, 2, or 4\n");
				return EXIT_FAILURE;
			}
			zoom_factor = (int)value;
			break;
		}
		case 'Z':
			if (!parse_percentage(optarg, 0, &zebra_level)) {
				fprintf(stderr, "zebra must be between 0 and 100\n");
				return EXIT_FAILURE;
			}
			zebra_enabled = true;
			break;
		case 'P':
			if (!parse_percentage(optarg, 1, &peaking_sensitivity)) {
				fprintf(stderr, "peaking must be between 1 and 100\n");
				return EXIT_FAILURE;
			}
			peaking_enabled = true;
			break;
		case 'W':
			waveform_enabled = true;
			break;
		case 'T':
			self_test = true;
			break;
		default:
		case 'h':
			usage(argv[0]);
			break;
		}
	}
	if (self_test) {
		if (!run_self_test(zoom_factor) || !run_analysis_self_test() ||
		    !run_analysis_output_self_test())
			return EXIT_FAILURE;
		printf("SELF_TEST_OK zoom=%d lut=%d analysis=%dx%d\n",
		       zoom_factor, FALSE_COLOR_LUT_SIZE,
		       ANALYSIS_WIDTH, ANALYSIS_HEIGHT);
		return EXIT_SUCCESS;
	}

	if (!v4l_device)
		v4l_device = "/dev/video0";

	if (v4l_format == 0x0)
		v4l_format = parse_format("YUYV");

	if (drm_format == 0x0)
		drm_format = v4l_format;
	if ((zebra_enabled || peaking_enabled || waveform_enabled) &&
	    (v4l_format != V4L2_PIX_FMT_NV16 ||
	     !(opts_flags & OPT_FLAG_DIRECT_DISPLAY))) {
		fprintf(stderr,
			"Analysis assists disabled: direct-display NV16 RGA output is unavailable\n");
		zebra_enabled = false;
		peaking_enabled = false;
		waveform_enabled = false;
	}
	if (effect == MONITOR_EFFECT_FALSE_COLOR &&
	    drm_format != DRM_FORMAT_XBGR8888) {
		fprintf(stderr, "false-color requires DRM format XB24\n");
		return EXIT_FAILURE;
	}
	if ((effect == MONITOR_EFFECT_FALSE_COLOR || zoom_factor != 1) &&
	    !(opts_flags & OPT_FLAG_DIRECT_DISPLAY)) {
		fprintf(stderr, "false-color and zoom require direct-display mode\n");
		return EXIT_FAILURE;
	}

	display = create_display(drm_format, opts_flags);
	display->format.format = v4l_format;
	display->effect = effect;
	display->zoom_factor = zoom_factor;
	display->zebra_enabled = zebra_enabled;
	display->zebra_level = zebra_level;
	display->peaking_enabled = peaking_enabled;
	display->peaking_sensitivity = peaking_sensitivity;
	display->waveform_enabled = waveform_enabled;
	if (probe_only) {
		fprintf(stderr, "Wayland DMA-BUF and explicit sync are available\n");
		destroy_display(display);
		return 0;
	}

	display->window = window = create_window(display, win_flags);
	if (!window) {
		destroy_display(display);
		return EXIT_FAILURE;
	}
	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = 0;
	sigaction(SIGINT, &sigint, NULL);
	sigaction(SIGTERM, &sigint, NULL);

	if (!v4l_connect(display, v4l_device))
		goto fail;
	if (!running)
		goto fail;
	if (!rga_power_policy_acquire(display))
		goto fail;
	if (!running)
		goto fail;

	if (!v4l_init(display, window->buffers))
		goto fail;

	/* Here we retrieve the linux-dmabuf objects, or error */
	wl_display_roundtrip(display->display);

	/* In case of error, running will be 0 */
	if (!running)
		goto fail;

	/* We got all of our buffers, we can start the capture! */
	if (!start_capture(display))
		goto fail;

	window->initialized = true;

	if (!window->wait_for_configure)
		redraw(window, NULL, 0);

	while (running && ret != -1)
		ret = wl_display_dispatch(display->display);
	if (ret == -1 && running)
		exit_status = EXIT_FAILURE;

	fprintf(stderr, "v4l2-wayland-explicit-sync exiting\n");
	destroy_window(window);
	destroy_display(display);

	return exit_status;

fail:
	exit_status = EXIT_FAILURE;
	destroy_window(window);
	destroy_display(display);
	return exit_status;
}
