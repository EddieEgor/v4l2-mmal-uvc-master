/*
 * v4l2-mmal-uvc application
 *
 * Copyright (C) 2020 Kin Wei Lee
 *
 * based on uvc-gadget laurent.pinchart@ideasonboard.com
 * based on v4l2_mmal by 6by9 Raspberry Pi (Trading) Ltd.
 *
 * Raspberry Pi OS Bookworm / Trixie port:
 *   The userland MMAL / VCSM / bcm_host libraries (/opt/vc) no longer exist.
 *   The same VideoCore hardware blocks are now driven through the kernel's
 *   bcm2835-codec V4L2 mem2mem driver, and buffers are shared between the
 *   devices as dmabufs allocated from the CMA dma-heap (zero copy).
 *
 *   -f0 (YUYV)
 *     unicam --dmabuf--> ISP m2m (UYVY->YUYV, scale) --dmabuf--> copy --> UVC
 *   -f1 (MJPEG)
 *     unicam --dmabuf--> ISP m2m (->I420, scale) --dmabuf--> JPEG m2m --> copy --> UVC
 *
 *   The only CPU copy is the final one into the UVC gadget's buffer (cached
 *   memory, ~1.8MB/frame for 720p YUYV, ~100-300KB/frame for MJPEG).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <linux/usb/ch9.h>
#include <linux/usb/video.h>
#include <linux/usb/g_uvc.h>

#include "v4l2-mmal-uvc.h"

/* Interface numbers of the UVC function (were in the old local uvc.h). */
#ifndef UVC_INTF_CONTROL
#define UVC_INTF_CONTROL   0
#endif
#ifndef UVC_INTF_STREAMING
#define UVC_INTF_STREAMING 1
#endif

#ifndef V4L2_BUF_FLAG_ERROR
#define V4L2_BUF_FLAG_ERROR 0x0040
#endif

static int debug = 0;
#define dbg(...)  do { if (debug) printf(__VA_ARGS__); } while (0)
#define err(...)  fprintf(stderr, __VA_ARGS__)

#define CLEAR(x)   memset(&(x), 0, sizeof(x))
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#define clamp(val, lo, hi) ((val) < (lo) ? (lo) : ((val) > (hi) ? (hi) : (val)))
#define pixfmtstr(x) (char)((x) & 0xff), (char)(((x) >> 8) & 0xff), \
                     (char)(((x) >> 16) & 0xff), (char)(((x) >> 24) & 0xff)

#define PAGE_ALIGN(x) (((x) + 4095) & ~((size_t) 4095))

/* Brightness PU defaults (static responses, see original uvc-gadget). */
#define PU_BRIGHTNESS_MIN_VAL     0
#define PU_BRIGHTNESS_MAX_VAL     255
#define PU_BRIGHTNESS_STEP_SIZE   1
#define PU_BRIGHTNESS_DEFAULT_VAL 7

/* Must match dwFrameInterval / dwDefaultFrameInterval in uvc-setup.sh. */
#define UVC_DEFAULT_INTERVAL 333333

/* Maximum ISP->JPEG frames in flight; keeps latency low. */
#define MAX_JPEG_INFLIGHT 2

static volatile sig_atomic_t quit_requested;

/* Exit status used when the HDMI signal is lost or changes, so a service
 * manager (systemd Restart=always) restarts us and hdmi-setup.sh re-locks. */
#define EXIT_HDMI_LOST 3
static int exit_status;

/* HDMI watchdog state: timings locked at start-up, last captured frame. */
static unsigned int hdmi_w, hdmi_h;
static int hdmi_fd = -1;         /* capture node used for DV queries */
static struct timespec last_cap_ts;

static int
xioctl (int fd, unsigned long req, void *arg)
{
  int r;
  do
    r = ioctl (fd, req, arg);
  while (r == -1 && errno == EINTR);
  return r;
}

/* ---------------------------------------------------------------------------
 * Formats
 */

struct fmt
{
  unsigned int width;
  unsigned int height;
  unsigned int pixelformat;
  unsigned int bytesperline;
  unsigned int sizeimage;
  unsigned int field;
  unsigned int colorspace;
};

static const struct v4l2_format_info *
v4l2_format_by_name (const char *name)
{
  unsigned int i;
  for (i = 0; i < ARRAY_SIZE (pixel_formats); ++i)
    if (strcasecmp (pixel_formats[i].name, name) == 0)
      return &pixel_formats[i];
  return NULL;
}

static const struct
{
  const char *name;
  enum v4l2_field field;
} fields[] = {
  {"any", V4L2_FIELD_ANY},
  {"none", V4L2_FIELD_NONE},
  {"top", V4L2_FIELD_TOP},
  {"bottom", V4L2_FIELD_BOTTOM},
  {"interlaced", V4L2_FIELD_INTERLACED},
  {"seq-tb", V4L2_FIELD_SEQ_TB},
  {"seq-bt", V4L2_FIELD_SEQ_BT},
  {"alternate", V4L2_FIELD_ALTERNATE},
  {"interlaced-tb", V4L2_FIELD_INTERLACED_TB},
  {"interlaced-bt", V4L2_FIELD_INTERLACED_BT},
};

static int
v4l2_field_from_string (const char *name)
{
  unsigned int i;
  for (i = 0; i < ARRAY_SIZE (fields); ++i)
    if (strcasecmp (fields[i].name, name) == 0)
      return fields[i].field;
  return -1;
}

static const char *
v4l2_field_name (unsigned int field)
{
  unsigned int i;
  for (i = 0; i < ARRAY_SIZE (fields); ++i)
    if (fields[i].field == field)
      return fields[i].name;
  return "unknown";
}

static bool
type_is_mplane (enum v4l2_buf_type t)
{
  return t == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
         t == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

static void
fmt_from_v4l2 (const struct v4l2_format *v, struct fmt *f)
{
  if (type_is_mplane (v->type)) {
    f->width = v->fmt.pix_mp.width;
    f->height = v->fmt.pix_mp.height;
    f->pixelformat = v->fmt.pix_mp.pixelformat;
    f->bytesperline = v->fmt.pix_mp.plane_fmt[0].bytesperline;
    f->sizeimage = v->fmt.pix_mp.plane_fmt[0].sizeimage;
    f->field = v->fmt.pix_mp.field;
    f->colorspace = v->fmt.pix_mp.colorspace;
  }
  else {
    f->width = v->fmt.pix.width;
    f->height = v->fmt.pix.height;
    f->pixelformat = v->fmt.pix.pixelformat;
    f->bytesperline = v->fmt.pix.bytesperline;
    f->sizeimage = v->fmt.pix.sizeimage;
    f->field = v->fmt.pix.field;
    f->colorspace = v->fmt.pix.colorspace;
  }
}

static int
fmt_get (int fd, enum v4l2_buf_type type, struct fmt *f)
{
  struct v4l2_format v;
  CLEAR (v);
  v.type = type;
  if (xioctl (fd, VIDIOC_G_FMT, &v) < 0)
    return -errno;
  fmt_from_v4l2 (&v, f);
  return 0;
}

/* Sets the format; 'f' is updated with what the driver actually accepted. */
static int
fmt_set (int fd, enum v4l2_buf_type type, struct fmt *f)
{
  struct v4l2_format v;
  CLEAR (v);
  v.type = type;
  if (type_is_mplane (type)) {
    v.fmt.pix_mp.width = f->width;
    v.fmt.pix_mp.height = f->height;
    v.fmt.pix_mp.pixelformat = f->pixelformat;
    v.fmt.pix_mp.field = f->field;
    v.fmt.pix_mp.colorspace = f->colorspace;
    v.fmt.pix_mp.num_planes = 1;
    v.fmt.pix_mp.plane_fmt[0].bytesperline = f->bytesperline;
    v.fmt.pix_mp.plane_fmt[0].sizeimage = f->sizeimage;
  }
  else {
    v.fmt.pix.width = f->width;
    v.fmt.pix.height = f->height;
    v.fmt.pix.pixelformat = f->pixelformat;
    v.fmt.pix.field = f->field;
    v.fmt.pix.colorspace = f->colorspace;
    v.fmt.pix.bytesperline = f->bytesperline;
    v.fmt.pix.sizeimage = f->sizeimage;
  }
  if (xioctl (fd, VIDIOC_S_FMT, &v) < 0)
    return -errno;
  fmt_from_v4l2 (&v, f);
  return 0;
}

static void
fmt_print (const char *who, const struct fmt *f)
{
  printf ("%-10s %c%c%c%c %ux%u stride %u size %u field %s\n", who,
          pixfmtstr (f->pixelformat), f->width, f->height, f->bytesperline,
          f->sizeimage, v4l2_field_name (f->field));
}

/* ---------------------------------------------------------------------------
 * dma-heap buffers (replace VCSM)
 */

struct dmabuf
{
  int fd;
  size_t size;
  void *map;                    /* CPU mapping, only where we read it */
};

struct pool
{
  unsigned int count;
  struct dmabuf *bufs;
};

static int heap_fd = -1;

static int
heap_open (void)
{
  /* Same search order as rpicam-apps. Both are physically contiguous CMA
   * heaps, which unicam and bcm2835-codec require (no IOMMU on these SoCs). */
  static const char *const heaps[] = {
    "/dev/dma_heap/vidbuf_cached",
    "/dev/dma_heap/linux,cma",
  };
  unsigned int i;

  for (i = 0; i < ARRAY_SIZE (heaps); i++) {
    heap_fd = open (heaps[i], O_RDWR | O_CLOEXEC);
    if (heap_fd >= 0) {
      dbg ("Using dma-heap %s\n", heaps[i]);
      return 0;
    }
  }
  err ("No CMA dma-heap found (/dev/dma_heap/vidbuf_cached or linux,cma): %s\n"
       "Is this a Raspberry Pi kernel, and are you in the 'video' group?\n",
       strerror (errno));
  return -1;
}

static int
pool_alloc (struct pool *p, unsigned int count, size_t size, bool map)
{
  unsigned int i;

  size = PAGE_ALIGN (size);
  p->bufs = calloc (count, sizeof (*p->bufs));
  if (!p->bufs)
    return -ENOMEM;
  p->count = count;
  for (i = 0; i < count; i++)
    p->bufs[i].fd = -1;

  for (i = 0; i < count; i++) {
    struct dma_heap_allocation_data a;
    CLEAR (a);
    a.len = size;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    if (xioctl (heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) {
      err ("dma-heap allocation of %zu bytes failed: %s\n"
           "Increase CMA (e.g. dtoverlay=vc4-kms-v3d,cma-128 in config.txt)\n",
           size, strerror (errno));
      return -errno;
    }
    p->bufs[i].fd = a.fd;
    p->bufs[i].size = size;
    if (map) {
      p->bufs[i].map = mmap (NULL, size, PROT_READ, MAP_SHARED, a.fd, 0);
      if (p->bufs[i].map == MAP_FAILED) {
        p->bufs[i].map = NULL;
        err ("mmap of dmabuf failed: %s\n", strerror (errno));
        return -errno;
      }
    }
  }
  dbg ("Allocated %u dmabufs of %zu bytes\n", count, size);
  return 0;
}

static void
pool_free (struct pool *p)
{
  unsigned int i;
  for (i = 0; i < p->count; i++) {
    if (p->bufs[i].map)
      munmap (p->bufs[i].map, p->bufs[i].size);
    if (p->bufs[i].fd >= 0)
      close (p->bufs[i].fd);
  }
  free (p->bufs);
  p->bufs = NULL;
  p->count = 0;
}

static void
dmabuf_sync (struct dmabuf *d, bool start)
{
  struct dma_buf_sync s;
  s.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ;
  xioctl (d->fd, DMA_BUF_IOCTL_SYNC, &s);
}

/* ---------------------------------------------------------------------------
 * Generic V4L2 buffer queue
 */

struct vq
{
  const char *name;
  int fd;
  enum v4l2_buf_type type;
  enum v4l2_memory memory;
  unsigned int count;
  unsigned int nqueued;
  bool streaming;
  bool *queued;
  struct pool *pool;            /* V4L2_MEMORY_DMABUF: index i imports pool->bufs[i] */
  void **maps;                  /* V4L2_MEMORY_MMAP */
  size_t *lens;
};

static void
vq_init (struct vq *q, const char *name, int fd, enum v4l2_buf_type type,
         enum v4l2_memory memory, struct pool *pool)
{
  memset (q, 0, sizeof (*q));
  q->name = name;
  q->fd = fd;
  q->type = type;
  q->memory = memory;
  q->pool = pool;
}

static int
vq_reqbufs (struct vq *q, unsigned int count)
{
  struct v4l2_requestbuffers rb;

  CLEAR (rb);
  rb.count = count;
  rb.type = q->type;
  rb.memory = q->memory;
  if (xioctl (q->fd, VIDIOC_REQBUFS, &rb) < 0) {
    err ("%s: VIDIOC_REQBUFS(%u) failed: %s\n", q->name, count, strerror (errno));
    return -errno;
  }
  if (count && rb.count < count)
    printf ("%s: requested %u buffers, got %u\n", q->name, count, rb.count);

  free (q->queued);
  q->queued = NULL;
  q->count = MIN (count, rb.count);
  q->nqueued = 0;
  if (q->count) {
    q->queued = calloc (q->count, sizeof (*q->queued));
    if (!q->queued)
      return -ENOMEM;
  }
  return 0;
}

static int
vq_qbuf (struct vq *q, unsigned int index, unsigned int bytesused,
         const struct timeval *ts)
{
  struct v4l2_buffer b;
  struct v4l2_plane p;

  if (index >= q->count || q->queued[index])
    return -EINVAL;

  CLEAR (b);
  CLEAR (p);
  b.index = index;
  b.type = q->type;
  b.memory = q->memory;
  b.field = V4L2_FIELD_NONE;
  if (ts)
    b.timestamp = *ts;

  if (type_is_mplane (q->type)) {
    b.m.planes = &p;
    b.length = 1;
    p.bytesused = bytesused;
    if (q->memory == V4L2_MEMORY_DMABUF) {
      p.m.fd = q->pool->bufs[index].fd;
      p.length = q->pool->bufs[index].size;
    }
  }
  else {
    b.bytesused = bytesused;
    if (q->memory == V4L2_MEMORY_DMABUF) {
      b.m.fd = q->pool->bufs[index].fd;
      b.length = q->pool->bufs[index].size;
    }
  }

  if (xioctl (q->fd, VIDIOC_QBUF, &b) < 0) {
    int e = errno;
    if (e != ENODEV)
      err ("%s: VIDIOC_QBUF(%u) failed: %s\n", q->name, index, strerror (e));
    return -e;
  }
  q->queued[index] = true;
  q->nqueued++;
  return 0;
}

/* Returns 0 on success, -EAGAIN when nothing is ready. */
static int
vq_dqbuf (struct vq *q, unsigned int *index, unsigned int *bytesused,
          unsigned int *flags, struct timeval *ts)
{
  struct v4l2_buffer b;
  struct v4l2_plane p;

  CLEAR (b);
  CLEAR (p);
  b.type = q->type;
  b.memory = q->memory;
  if (type_is_mplane (q->type)) {
    b.m.planes = &p;
    b.length = 1;
  }

  if (xioctl (q->fd, VIDIOC_DQBUF, &b) < 0) {
    int e = errno;
    if (e != EAGAIN)
      dbg ("%s: VIDIOC_DQBUF failed: %s\n", q->name, strerror (e));
    return -e;
  }

  if (b.index < q->count && q->queued[b.index]) {
    q->queued[b.index] = false;
    q->nqueued--;
  }
  *index = b.index;
  if (bytesused)
    *bytesused = type_is_mplane (q->type) ? p.bytesused : b.bytesused;
  if (flags)
    *flags = b.flags;
  if (ts)
    *ts = b.timestamp;
  return 0;
}

static int
vq_stream (struct vq *q, bool on)
{
  int type = q->type;

  if (on && q->streaming)
    return 0;
  /* STREAMOFF is also how buffers queued on a never-started queue are
   * reclaimed (e.g. after a failed start), so only skip it when idle. */
  if (!on && !q->streaming && !q->nqueued)
    return 0;
  if (q->fd < 0)
    return 0;
  if (xioctl (q->fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type) < 0) {
    int e = errno;
    err ("%s: %s failed: %s\n", q->name, on ? "STREAMON" : "STREAMOFF",
         strerror (e));
    return -e;
  }
  q->streaming = on;
  if (!on) {
    /* STREAMOFF returns every buffer to userspace. */
    if (q->queued)
      memset (q->queued, 0, q->count * sizeof (*q->queued));
    q->nqueued = 0;
  }
  dbg ("%s: %s\n", q->name, on ? "STREAMON" : "STREAMOFF");
  return 0;
}

static int
vq_mmap (struct vq *q)
{
  unsigned int i;

  q->maps = calloc (q->count, sizeof (*q->maps));
  q->lens = calloc (q->count, sizeof (*q->lens));
  if (!q->maps || !q->lens)
    return -ENOMEM;

  for (i = 0; i < q->count; i++) {
    struct v4l2_buffer b;
    struct v4l2_plane p;
    unsigned int off;

    CLEAR (b);
    CLEAR (p);
    b.index = i;
    b.type = q->type;
    b.memory = V4L2_MEMORY_MMAP;
    if (type_is_mplane (q->type)) {
      b.m.planes = &p;
      b.length = 1;
    }
    if (xioctl (q->fd, VIDIOC_QUERYBUF, &b) < 0) {
      err ("%s: VIDIOC_QUERYBUF failed: %s\n", q->name, strerror (errno));
      return -errno;
    }
    if (type_is_mplane (q->type)) {
      q->lens[i] = p.length;
      off = p.m.mem_offset;
    }
    else {
      q->lens[i] = b.length;
      off = b.m.offset;
    }
    q->maps[i] = mmap (NULL, q->lens[i], PROT_READ | PROT_WRITE, MAP_SHARED,
                       q->fd, off);
    if (q->maps[i] == MAP_FAILED) {
      q->maps[i] = NULL;
      err ("%s: mmap failed: %s\n", q->name, strerror (errno));
      return -errno;
    }
  }
  return 0;
}

static void
vq_munmap (struct vq *q)
{
  unsigned int i;
  if (!q->maps)
    return;
  for (i = 0; i < q->count; i++)
    if (q->maps[i])
      munmap (q->maps[i], q->lens[i]);
  free (q->maps);
  free (q->lens);
  q->maps = NULL;
  q->lens = NULL;
}

/* ---------------------------------------------------------------------------
 * Device discovery
 */

static int
sysfs_name (const char *node, char *buf, size_t len)
{
  char path[128];
  FILE *f;

  snprintf (path, sizeof (path), "/sys/class/video4linux/%s/name", node);
  f = fopen (path, "r");
  if (!f)
    return -1;
  if (!fgets (buf, len, f)) {
    fclose (f);
    return -1;
  }
  fclose (f);
  buf[strcspn (buf, "\n")] = '\0';
  return 0;
}

/* bcm2835-codec nodes are named e.g. "bcm2835-codec-isp". */
static int
find_video_by_name (const char *name, char *out, size_t len)
{
  char node[32], buf[64];
  int n;

  for (n = 0; n < 64; n++) {
    snprintf (node, sizeof (node), "video%d", n);
    if (sysfs_name (node, buf, sizeof (buf)) == 0 && strcmp (buf, name) == 0) {
      snprintf (out, len, "/dev/%s", node);
      return 0;
    }
  }
  return -ENOENT;
}

static int
find_subdev_containing (const char *substr, char *out, size_t len)
{
  char node[32], buf[64];
  int n;

  for (n = 0; n < 64; n++) {
    snprintf (node, sizeof (node), "v4l-subdev%d", n);
    if (sysfs_name (node, buf, sizeof (buf)) == 0 && strstr (buf, substr)) {
      snprintf (out, len, "/dev/%s", node);
      return 0;
    }
  }
  return -ENOENT;
}

/* The UVC gadget's video node reports driver "g_uvc". */
static int
find_uvc_gadget (char *out, size_t len)
{
  struct v4l2_capability cap;
  char path[32];
  int n, fd;

  for (n = 0; n < 64; n++) {
    snprintf (path, sizeof (path), "/dev/video%d", n);
    fd = open (path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
      continue;
    CLEAR (cap);
    if (xioctl (fd, VIDIOC_QUERYCAP, &cap) == 0 &&
        strcmp ((const char *) cap.driver, "g_uvc") == 0) {
      close (fd);
      snprintf (out, len, "%s", path);
      return 0;
    }
    close (fd);
  }
  return -ENOENT;
}

static int
open_m2m (const char *path, const char *expect_name)
{
  struct v4l2_capability cap;
  unsigned int caps;
  int fd;

  fd = open (path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    err ("Cannot open %s (%s): %s\n", expect_name, path, strerror (errno));
    return -1;
  }
  CLEAR (cap);
  if (xioctl (fd, VIDIOC_QUERYCAP, &cap) < 0) {
    err ("%s: VIDIOC_QUERYCAP failed: %s\n", path, strerror (errno));
    close (fd);
    return -1;
  }
  caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_M2M_MPLANE)) {
    err ("%s (%s) is not a multiplanar mem2mem device\n", path, (char *) cap.card);
    close (fd);
    return -1;
  }
  printf ("%-10s %s (%s)\n", expect_name, path, (char *) cap.card);
  return fd;
}

/* ---------------------------------------------------------------------------
 * HDMI input timing (TC358743)
 */

static int
dv_timings_apply (int fd, unsigned int *w, unsigned int *h, unsigned int *fps)
{
  struct v4l2_dv_timings t;
  const struct v4l2_bt_timings *bt = &t.bt;
  double tot_w, tot_h;

  CLEAR (t);
  if (xioctl (fd, VIDIOC_QUERY_DV_TIMINGS, &t) < 0)
    return -errno;
  if (xioctl (fd, VIDIOC_S_DV_TIMINGS, &t) < 0)
    return -errno;

  tot_h = bt->height + bt->vfrontporch + bt->vsync + bt->vbackporch +
    bt->il_vfrontporch + bt->il_vsync + bt->il_vbackporch;
  tot_w = bt->width + bt->hfrontporch + bt->hsync + bt->hbackporch;
  *w = bt->width;
  *h = bt->height;
  *fps = (tot_w * tot_h) > 0 ? (unsigned int) ((double) bt->pixelclock / (tot_w * tot_h)) : 0;
  printf ("HDMI input %ux%u%s @ %u fps (pixclk %llu)\n", bt->width, bt->height,
          bt->interlaced ? "i" : "p", *fps, (unsigned long long) bt->pixelclock);
  return 0;
}

static void
hdmi_lock (int cap_fd, unsigned int *w, unsigned int *h, unsigned int *fps)
{
  char subdev[64];
  int ret, sfd;

  /* Legacy (video-node-centric) unicam forwards DV ioctls to the bridge. */
  ret = dv_timings_apply (cap_fd, w, h, fps);
  if (ret == 0)
    return;

  /* Media-controller unicam: talk to the tc358743 subdev directly. */
  if (ret == -ENOTTY || ret == -EINVAL) {
    if (find_subdev_containing ("tc358743", subdev, sizeof (subdev)) == 0) {
      sfd = open (subdev, O_RDWR);
      if (sfd >= 0) {
        ret = dv_timings_apply (sfd, w, h, fps);
        close (sfd);
        if (ret == 0)
          return;
      }
    }
  }

  if (ret == -ENOLINK)
    err ("No HDMI signal detected (ENOLINK). Is the source on and EDID loaded? (run hdmi-setup.sh)\n");
  else if (ret == -ENOLCK)
    err ("HDMI signal not stable (ENOLCK).\n");
  else if (ret != -ENOTTY && ret != -EINVAL)
    err ("Unable to query/set DV timings: %s\n", strerror (-ret));
  else {
    /* SD analogue sources */
    v4l2_std_id std = 0;
    if (xioctl (cap_fd, VIDIOC_QUERYSTD, &std) == 0 &&
        xioctl (cap_fd, VIDIOC_S_STD, &std) == 0)
      *fps = 25;
  }
}

/* Query (without applying) the current HDMI timings. 0 = signal present. */
static int
hdmi_query (int cap_fd, unsigned int *w, unsigned int *h)
{
  struct v4l2_dv_timings t;
  char subdev[64];
  int ret, sfd;

  CLEAR (t);
  ret = xioctl (cap_fd, VIDIOC_QUERY_DV_TIMINGS, &t) < 0 ? -errno : 0;
  if ((ret == -ENOTTY || ret == -EINVAL) &&
      find_subdev_containing ("tc358743", subdev, sizeof (subdev)) == 0) {
    sfd = open (subdev, O_RDWR);
    if (sfd >= 0) {
      CLEAR (t);
      ret = xioctl (sfd, VIDIOC_QUERY_DV_TIMINGS, &t) < 0 ? -errno : 0;
      close (sfd);
    }
  }
  if (ret == 0) {
    *w = t.bt.width;
    *h = t.bt.height;
  }
  return ret;
}

static double
ts_elapsed (const struct timespec *since)
{
  struct timespec now;

  clock_gettime (CLOCK_MONOTONIC, &now);
  return (now.tv_sec - since->tv_sec) + (now.tv_nsec - since->tv_nsec) / 1e9;
}

static void
hdmi_lost (const char *why)
{
  printf ("HDMI: %s; exiting so the service can re-lock the input\n", why);
  exit_status = EXIT_HDMI_LOST;
  quit_requested = 1;
}

/* ---------------------------------------------------------------------------
 * UVC gadget
 */

struct uvc_frame_info
{
  unsigned int width;
  unsigned int height;
  unsigned int intervals[8];    /* ascending, 0 terminated */
};

struct uvc_format_info
{
  unsigned int fcc;
  const struct uvc_frame_info *frames;
};

/* One format with one frame size, chosen on the command line; this must
 * describe the same thing uvc-setup.sh put into configfs. */
static struct uvc_frame_info uvc_frames[2] = {
  {1280, 720, {333333, 0}},       /* 30 fps only */
  {0, 0, {0}},
};

static struct uvc_format_info uvc_formats[1] = {
  {V4L2_PIX_FMT_MJPEG, uvc_frames},
};

struct uvc_device
{
  int fd;
  struct uvc_streaming_control probe;
  struct uvc_streaming_control commit;
  int control;
  struct uvc_request_data request_error_code;
  unsigned int brightness_val;

  unsigned int fcc;
  unsigned int width;
  unsigned int height;
  unsigned int imgsize;
  unsigned int nbufs;

  int bulk;
  int mult;
  int burst;
  int maxpkt;
  enum usb_device_speed speed;

  struct vq q;
  bool streaming;               /* VIDIOC_STREAMON issued on the gadget */

  unsigned long long frames;
  unsigned long long dropped;
  struct timespec stat_ts;
  unsigned long long stat_frames;
};

static int pipeline_start (void);
static void pipeline_stop (void);

static int
uvc_open (struct uvc_device *dev, const char *devname)
{
  struct v4l2_capability cap;
  unsigned int caps;

  dev->fd = open (devname, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (dev->fd < 0) {
    err ("UVC: cannot open %s: %s\n", devname, strerror (errno));
    return -1;
  }
  CLEAR (cap);
  if (xioctl (dev->fd, VIDIOC_QUERYCAP, &cap) < 0) {
    err ("UVC: VIDIOC_QUERYCAP failed: %s\n", strerror (errno));
    return -1;
  }
  caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_OUTPUT)) {
    err ("UVC: %s is not a video output device (is it the gadget?)\n", devname);
    return -1;
  }
  printf ("%-10s %s (%s on %s)\n", "uvc", devname, (char *) cap.card, (char *) cap.bus_info);
  vq_init (&dev->q, "uvc", dev->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_MMAP, NULL);
  return 0;
}

static int
uvc_video_set_format (struct uvc_device *dev)
{
  struct v4l2_format fmt;

  CLEAR (fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width = dev->width;
  fmt.fmt.pix.height = dev->height;
  fmt.fmt.pix.pixelformat = dev->fcc;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  fmt.fmt.pix.sizeimage = dev->imgsize;
  if (dev->fcc == V4L2_PIX_FMT_YUYV)
    fmt.fmt.pix.bytesperline = dev->width * 2;

  if (xioctl (dev->fd, VIDIOC_S_FMT, &fmt) < 0) {
    err ("UVC: unable to set %c%c%c%c %ux%u: %s\n", pixfmtstr (dev->fcc),
         dev->width, dev->height, strerror (errno));
    return -errno;
  }
  dbg ("UVC: format %c%c%c%c %ux%u size %u\n", pixfmtstr (dev->fcc),
       dev->width, dev->height, fmt.fmt.pix.sizeimage);
  return 0;
}

/*
 * Tell the gadget driver the committed frame interval. Newer kernels
 * (6.8+) size each isochronous request so that one frame is spread over
 * this interval, and default to 666666 (15 fps) if it is never set.
 */
static void
uvc_video_set_interval (struct uvc_device *dev, unsigned int interval)
{
  struct v4l2_streamparm parm;

  CLEAR (parm);
  parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  parm.parm.output.timeperframe.numerator = interval;
  parm.parm.output.timeperframe.denominator = 10000000;
  if (xioctl (dev->fd, VIDIOC_S_PARM, &parm) < 0)
    err ("UVC: unable to set frame interval %u: %s\n", interval, strerror (errno));
  else
    printf ("UVC: gadget frame interval set to %u (%.1f fps)\n", interval,
            10000000.0 / interval);
}

static void
uvc_fill_streaming_control (struct uvc_device *dev,
                            struct uvc_streaming_control *ctrl,
                            int iframe, int iformat)
{
  const struct uvc_format_info *format;
  const struct uvc_frame_info *frame;
  unsigned int nframes;

  if (iformat < 0)
    iformat = ARRAY_SIZE (uvc_formats) + iformat;
  if (iformat < 0 || iformat >= (int) ARRAY_SIZE (uvc_formats))
    return;
  format = &uvc_formats[iformat];

  nframes = 0;
  while (format->frames[nframes].width != 0)
    ++nframes;

  if (iframe < 0)
    iframe = nframes + iframe;
  if (iframe < 0 || iframe >= (int) nframes)
    return;
  frame = &format->frames[iframe];

  memset (ctrl, 0, sizeof *ctrl);

  ctrl->bmHint = 1;             /* keep dwFrameInterval constant */
  ctrl->bFormatIndex = iformat + 1;
  ctrl->bFrameIndex = iframe + 1;
  ctrl->dwFrameInterval = UVC_DEFAULT_INTERVAL;
  switch (format->fcc) {
  case V4L2_PIX_FMT_YUYV:
    ctrl->dwMaxVideoFrameSize = frame->width * frame->height * 2;
    break;
  case V4L2_PIX_FMT_MJPEG:
    ctrl->dwMaxVideoFrameSize = dev->imgsize;
    break;
  }

  if (!dev->bulk)
    ctrl->dwMaxPayloadTransferSize = dev->maxpkt * (dev->mult + 1) * (dev->burst + 1);
  else
    ctrl->dwMaxPayloadTransferSize = ctrl->dwMaxVideoFrameSize;

  ctrl->bmFramingInfo = 3;
  ctrl->bPreferedVersion = 1;
  ctrl->bMaxVersion = 1;
}

static void
uvc_events_process_control (struct uvc_device *dev, uint8_t req,
                            uint8_t cs, uint8_t entity_id,
                            uint8_t len, struct uvc_request_data *resp)
{
  switch (entity_id) {
  case 0:
    switch (cs) {
    case UVC_VC_REQUEST_ERROR_CODE_CONTROL:
      /* Send the request error code last prepared. */
      resp->data[0] = dev->request_error_code.data[0];
      resp->length = dev->request_error_code.length;
      break;

    default:
      dev->request_error_code.data[0] = 0x06;
      dev->request_error_code.length = 1;
      break;
    }
    break;

    /* Camera terminal unit 'UVC_VC_INPUT_TERMINAL'. */
  case 1:
    switch (cs) {
      /* Only auto exposure mode is supported on the camera terminal. */
    case UVC_CT_AE_MODE_CONTROL:
      switch (req) {
      case UVC_SET_CUR:
        resp->data[0] = 0x01;
        resp->length = 1;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;

      case UVC_GET_INFO:
        resp->data[0] = 0x03;
        resp->length = 1;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;

      case UVC_GET_CUR:
      case UVC_GET_DEF:
      case UVC_GET_RES:
        /* Auto Mode: auto Exposure Time, auto Iris. */
        resp->data[0] = 0x02;
        resp->length = 1;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;

      default:
        /* STALL the control ep. */
        resp->length = -EL2HLT;
        dev->request_error_code.data[0] = 0x07;
        dev->request_error_code.length = 1;
        break;
      }
      break;

    default:
      resp->length = -EL2HLT;
      dev->request_error_code.data[0] = 0x06;
      dev->request_error_code.length = 1;
      break;
    }
    break;

    /* Processing unit 'UVC_VC_PROCESSING_UNIT' */
  case 2:
    switch (cs) {
    case UVC_PU_BRIGHTNESS_CONTROL:
      switch (req) {
      case UVC_SET_CUR:
        resp->data[0] = 0x0;
        resp->length = len;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;
      case UVC_GET_MIN:
        resp->data[0] = PU_BRIGHTNESS_MIN_VAL;
        resp->length = 2;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;
      case UVC_GET_MAX:
        resp->data[0] = PU_BRIGHTNESS_MAX_VAL;
        resp->length = 2;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;
      case UVC_GET_CUR:
        resp->length = 2;
        memcpy (&resp->data[0], &dev->brightness_val, resp->length);
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;
      case UVC_GET_INFO:
        resp->data[0] = 0x03;
        resp->length = 1;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;
      case UVC_GET_DEF:
        resp->data[0] = PU_BRIGHTNESS_DEFAULT_VAL;
        resp->length = 2;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;
      case UVC_GET_RES:
        resp->data[0] = PU_BRIGHTNESS_STEP_SIZE;
        resp->length = 2;
        dev->request_error_code.data[0] = 0x00;
        dev->request_error_code.length = 1;
        break;
      default:
        resp->length = -EL2HLT;
        dev->request_error_code.data[0] = 0x07;
        dev->request_error_code.length = 1;
        break;
      }
      break;

    default:
      resp->length = -EL2HLT;
      dev->request_error_code.data[0] = 0x06;
      dev->request_error_code.length = 1;
      break;
    }
    break;

  default:
    dev->request_error_code.data[0] = 0x06;
    dev->request_error_code.length = 1;
    break;
  }

  dbg ("control request (req %02x cs %02x entity %02x)\n", req, cs, entity_id);
}

static void
uvc_events_process_streaming (struct uvc_device *dev, uint8_t req, uint8_t cs,
                              struct uvc_request_data *resp)
{
  struct uvc_streaming_control *ctrl;

  dbg ("streaming request (req %02x cs %02x)\n", req, cs);

  if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)
    return;

  ctrl = (struct uvc_streaming_control *) &resp->data;
  resp->length = sizeof *ctrl;

  switch (req) {
  case UVC_SET_CUR:
    dev->control = cs;
    resp->length = 34;
    break;

  case UVC_GET_CUR:
    if (cs == UVC_VS_PROBE_CONTROL)
      memcpy (ctrl, &dev->probe, sizeof *ctrl);
    else
      memcpy (ctrl, &dev->commit, sizeof *ctrl);
    break;

  case UVC_GET_MIN:
  case UVC_GET_MAX:
  case UVC_GET_DEF:
    uvc_fill_streaming_control (dev, ctrl, req == UVC_GET_MAX ? -1 : 0,
                                req == UVC_GET_MAX ? -1 : 0);
    break;

  case UVC_GET_RES:
    memset (ctrl, 0, sizeof *ctrl);
    break;

  case UVC_GET_LEN:
    resp->data[0] = 0x00;
    resp->data[1] = 0x22;
    resp->length = 2;
    break;

  case UVC_GET_INFO:
    resp->data[0] = 0x03;
    resp->length = 1;
    break;

  default:
    dbg ("unhandled streaming request (req %02x cs %02x)\n", req, cs);
    break;
  }
}

static void
uvc_events_process_class (struct uvc_device *dev,
                          struct usb_ctrlrequest *ctrl,
                          struct uvc_request_data *resp)
{
  if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE)
    return;

  switch (ctrl->wIndex & 0xff) {
  case UVC_INTF_CONTROL:
    uvc_events_process_control (dev, ctrl->bRequest, ctrl->wValue >> 8,
                                ctrl->wIndex >> 8, ctrl->wLength, resp);
    break;

  case UVC_INTF_STREAMING:
    uvc_events_process_streaming (dev, ctrl->bRequest, ctrl->wValue >> 8, resp);
    break;

  default:
    break;
  }
}

static void
uvc_events_process_setup (struct uvc_device *dev,
                          struct usb_ctrlrequest *ctrl,
                          struct uvc_request_data *resp)
{
  dev->control = 0;

  dbg ("bRequestType %02x bRequest %02x wValue %04x wIndex %04x wLength %04x\n",
       ctrl->bRequestType, ctrl->bRequest, ctrl->wValue, ctrl->wIndex,
       ctrl->wLength);

  switch (ctrl->bRequestType & USB_TYPE_MASK) {
  case USB_TYPE_STANDARD:
    break;

  case USB_TYPE_CLASS:
    uvc_events_process_class (dev, ctrl, resp);
    break;

  default:
    break;
  }
}

static int
uvc_events_process_data (struct uvc_device *dev, struct uvc_request_data *data)
{
  struct uvc_streaming_control *target;
  struct uvc_streaming_control *ctrl;
  const struct uvc_format_info *format;
  const struct uvc_frame_info *frame;
  const unsigned int *interval;
  unsigned int iformat, iframe;
  unsigned int nframes;
  unsigned int val = 0;

  switch (dev->control) {
  case UVC_VS_PROBE_CONTROL:
    dbg ("setting probe control, length = %d\n", data->length);
    target = &dev->probe;
    break;

  case UVC_VS_COMMIT_CONTROL:
    dbg ("setting commit control, length = %d\n", data->length);
    target = &dev->commit;
    break;

  default:
    /* Only BRIGHTNESS is supported as a control. */
    memcpy (&val, data->data, MIN ((size_t) MAX (data->length, 0), sizeof (val)));
    if (val > PU_BRIGHTNESS_MAX_VAL)
      return -EINVAL;
    dev->brightness_val = val;
    return 0;
  }

  ctrl = (struct uvc_streaming_control *) &data->data;
  iformat = clamp ((unsigned int) ctrl->bFormatIndex, 1U,
                   (unsigned int) ARRAY_SIZE (uvc_formats));
  format = &uvc_formats[iformat - 1];

  nframes = 0;
  while (format->frames[nframes].width != 0)
    ++nframes;

  iframe = clamp ((unsigned int) ctrl->bFrameIndex, 1U, nframes);
  frame = &format->frames[iframe - 1];
  interval = frame->intervals;

  while (interval[0] < ctrl->dwFrameInterval && interval[1])
    ++interval;

  target->bFormatIndex = iformat;
  target->bFrameIndex = iframe;

  switch (format->fcc) {
  case V4L2_PIX_FMT_YUYV:
    target->dwMaxVideoFrameSize = frame->width * frame->height * 2;
    break;
  case V4L2_PIX_FMT_MJPEG:
    target->dwMaxVideoFrameSize = dev->imgsize;
    break;
  }
  target->dwFrameInterval = *interval;

  if (dev->control == UVC_VS_COMMIT_CONTROL) {
    printf ("UVC: host committed %c%c%c%c %ux%u, interval %u\n",
            pixfmtstr (format->fcc), frame->width, frame->height, *interval);
    dev->fcc = format->fcc;
    dev->width = frame->width;
    dev->height = frame->height;
    uvc_video_set_interval (dev, *interval);
    return uvc_video_set_format (dev);
  }
  return 0;
}

static void
uvc_events_process (struct uvc_device *dev)
{
  struct v4l2_event v4l2_event;
  struct uvc_event *uvc_event = (void *) &v4l2_event.u.data;
  struct uvc_request_data resp;
  int ret;

  CLEAR (v4l2_event);
  if (xioctl (dev->fd, VIDIOC_DQEVENT, &v4l2_event) < 0) {
    if (errno != ENOENT && errno != EAGAIN)
      err ("VIDIOC_DQEVENT failed: %s\n", strerror (errno));
    return;
  }

  memset (&resp, 0, sizeof resp);
  resp.length = -EL2HLT;

  switch (v4l2_event.type) {
  case UVC_EVENT_CONNECT:
    printf ("UVC: host connected\n");
    return;

  case UVC_EVENT_DISCONNECT:
    printf ("UVC: host disconnected\n");
    pipeline_stop ();
    return;

  case UVC_EVENT_SETUP:
    uvc_events_process_setup (dev, &uvc_event->req, &resp);
    break;

  case UVC_EVENT_DATA:
    ret = uvc_events_process_data (dev, &uvc_event->data);
    if (ret < 0)
      break;
    return;

  case UVC_EVENT_STREAMON:
    printf ("UVC: host started streaming\n");
    if (pipeline_start () < 0) {
      unsigned int w, h;
      err ("Failed to start the video pipeline\n");
      pipeline_stop ();
      if (hdmi_w && (hdmi_query (hdmi_fd, &w, &h) < 0 || w != hdmi_w || h != hdmi_h))
        hdmi_lost ("no usable signal when the host started streaming");
    }
    return;

  case UVC_EVENT_STREAMOFF:
    printf ("UVC: host stopped streaming\n");
    pipeline_stop ();
    return;

  default:
    dbg ("unknown UVC event %u\n", v4l2_event.type);
    break;
  }

  if (xioctl (dev->fd, UVCIOC_SEND_RESPONSE, &resp) < 0)
    err ("UVCIOC_SEND_RESPONSE failed: %s\n", strerror (errno));
}

static void
uvc_events_init (struct uvc_device *dev)
{
  static const unsigned int events[] = {
    UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT, UVC_EVENT_SETUP,
    UVC_EVENT_DATA, UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF,
  };
  struct v4l2_event_subscription sub;
  unsigned int i;

  uvc_fill_streaming_control (dev, &dev->probe, 0, 0);
  uvc_fill_streaming_control (dev, &dev->commit, 0, 0);

  for (i = 0; i < ARRAY_SIZE (events); i++) {
    memset (&sub, 0, sizeof sub);
    sub.type = events[i];
    if (xioctl (dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
      err ("UVC: failed to subscribe to event %u: %s\n", events[i], strerror (errno));
  }
}

/* ---------------------------------------------------------------------------
 * Pipeline: capture -> ISP -> (JPEG) -> UVC
 */

enum { MODE_YUYV = 0, MODE_MJPEG = 1 };

struct pipeline
{
  int mode;
  struct uvc_device *uvc;

  int cap_fd, isp_fd, jpeg_fd;
  struct vq cap;                /* unicam capture, imports pool A */
  struct vq isp_out;            /* ISP input, imports pool A */
  struct vq isp_cap;            /* ISP output, imports pool B */
  struct vq jpeg_out;           /* JPEG encoder input, imports pool B */
  struct vq jpeg_cap;           /* JPEG bitstream, imports pool C */
  struct pool pa, pb, pc;
  struct fmt cap_fmt, isp_out_fmt, isp_cap_fmt, jpeg_out_fmt, jpeg_cap_fmt;

  bool running;
  unsigned long long frames_in;
};

static struct pipeline pl = { .cap_fd = -1, .isp_fd = -1, .jpeg_fd = -1 };

static int
pipeline_setup (unsigned int in_w, unsigned int in_h, unsigned int in_pixfmt,
                unsigned int field, unsigned int cap_nbufs, int quality)
{
  struct uvc_device *uvc = pl.uvc;
  unsigned int mid_nbufs = 4;
  struct fmt f;

  /* --- Capture (unicam) --- */
  CLEAR (f);
  f.width = in_w;
  f.height = in_h;
  f.pixelformat = in_pixfmt;
  f.field = field;
  if (fmt_set (pl.cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &f) < 0) {
    err ("Capture: unable to set %c%c%c%c %ux%u: %s\n", pixfmtstr (in_pixfmt),
         in_w, in_h, strerror (errno));
    return -1;
  }
  if (f.pixelformat != in_pixfmt)
    printf ("Capture: driver chose %c%c%c%c instead of %c%c%c%c\n",
            pixfmtstr (f.pixelformat), pixfmtstr (in_pixfmt));
  pl.cap_fmt = f;

  /* --- ISP input: must use the same memory layout as the capture --- */
  CLEAR (f);
  f.width = pl.cap_fmt.width;
  f.height = pl.cap_fmt.height;
  f.pixelformat = pl.cap_fmt.pixelformat;
  f.bytesperline = pl.cap_fmt.bytesperline;
  f.colorspace = pl.cap_fmt.colorspace;
  f.field = V4L2_FIELD_NONE;
  if (fmt_set (pl.isp_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &f) < 0) {
    err ("ISP: input format %c%c%c%c %ux%u rejected: %s\n",
         pixfmtstr (pl.cap_fmt.pixelformat), pl.cap_fmt.width,
         pl.cap_fmt.height, strerror (errno));
    return -1;
  }
  pl.isp_out_fmt = f;

  if (pl.isp_out_fmt.pixelformat != pl.cap_fmt.pixelformat) {
    err ("ISP does not accept %c%c%c%c input; try -zUYVY or -zRGB24\n",
         pixfmtstr (pl.cap_fmt.pixelformat));
    return -1;
  }
  if (pl.isp_out_fmt.bytesperline != pl.cap_fmt.bytesperline) {
    /* Same idea as the old MMAL stride adjustment. */
    printf ("Adjusting capture stride %u -> %u to suit the ISP\n",
            pl.cap_fmt.bytesperline, pl.isp_out_fmt.bytesperline);
    f = pl.cap_fmt;
    f.bytesperline = pl.isp_out_fmt.bytesperline;
    f.sizeimage = 0;
    if (fmt_set (pl.cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &f) < 0 ||
        f.bytesperline != pl.isp_out_fmt.bytesperline) {
      err ("Capture and ISP cannot agree on a stride (%u vs %u)\n",
           f.bytesperline, pl.isp_out_fmt.bytesperline);
      return -1;
    }
    pl.cap_fmt = f;
  }

  /* --- ISP output: YUYV for UVC, or I420 to feed the JPEG encoder --- */
  CLEAR (f);
  f.width = uvc->width;
  f.height = uvc->height;
  f.pixelformat = pl.mode == MODE_YUYV ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_YUV420;
  f.field = V4L2_FIELD_NONE;
  if (fmt_set (pl.isp_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &f) < 0) {
    err ("ISP: output format rejected: %s\n", strerror (errno));
    return -1;
  }
  if (f.width != uvc->width || f.height != uvc->height) {
    err ("ISP cannot produce %ux%u (got %ux%u)\n", uvc->width, uvc->height,
         f.width, f.height);
    return -1;
  }
  pl.isp_cap_fmt = f;

  /* --- JPEG encoder --- */
  if (pl.mode == MODE_MJPEG) {
    struct v4l2_control c;

    CLEAR (f);
    f.width = pl.isp_cap_fmt.width;
    f.height = pl.isp_cap_fmt.height;
    f.pixelformat = pl.isp_cap_fmt.pixelformat;
    f.bytesperline = pl.isp_cap_fmt.bytesperline;
    f.field = V4L2_FIELD_NONE;
    if (fmt_set (pl.jpeg_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &f) < 0) {
      err ("JPEG: input format rejected: %s\n", strerror (errno));
      return -1;
    }
    if (f.bytesperline != pl.isp_cap_fmt.bytesperline ||
        f.pixelformat != pl.isp_cap_fmt.pixelformat) {
      err ("ISP output and JPEG input layouts differ (stride %u vs %u)\n",
           pl.isp_cap_fmt.bytesperline, f.bytesperline);
      return -1;
    }
    pl.jpeg_out_fmt = f;

    CLEAR (f);
    f.width = pl.isp_cap_fmt.width;
    f.height = pl.isp_cap_fmt.height;
    f.pixelformat = V4L2_PIX_FMT_JPEG;
    f.sizeimage = uvc->imgsize;
    f.field = V4L2_FIELD_NONE;
    if (fmt_set (pl.jpeg_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &f) < 0) {
      err ("JPEG: output format rejected: %s\n", strerror (errno));
      return -1;
    }
    pl.jpeg_cap_fmt = f;

    c.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    c.value = quality;
    if (xioctl (pl.jpeg_fd, VIDIOC_S_CTRL, &c) < 0)
      err ("JPEG: unable to set quality %d: %s\n", quality, strerror (errno));
  }

  fmt_print ("capture", &pl.cap_fmt);
  fmt_print ("isp in", &pl.isp_out_fmt);
  fmt_print ("isp out", &pl.isp_cap_fmt);
  if (pl.mode == MODE_MJPEG) {
    fmt_print ("jpeg in", &pl.jpeg_out_fmt);
    fmt_print ("jpeg out", &pl.jpeg_cap_fmt);
  }

  /* --- Shared buffers. Each pool is sized for the larger of its two users,
   * since the codec may pad height (e.g. 1080 -> 1088). --- */
  if (pool_alloc (&pl.pa, cap_nbufs,
                  MAX (pl.cap_fmt.sizeimage, pl.isp_out_fmt.sizeimage), false) < 0)
    return -1;
  if (pool_alloc (&pl.pb, mid_nbufs,
                  MAX (pl.isp_cap_fmt.sizeimage, pl.jpeg_out_fmt.sizeimage),
                  pl.mode == MODE_YUYV) < 0)
    return -1;
  if (pl.mode == MODE_MJPEG &&
      pool_alloc (&pl.pc, 4, pl.jpeg_cap_fmt.sizeimage, true) < 0)
    return -1;

  vq_init (&pl.cap, "capture", pl.cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
           V4L2_MEMORY_DMABUF, &pl.pa);
  vq_init (&pl.isp_out, "isp-in", pl.isp_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
           V4L2_MEMORY_DMABUF, &pl.pa);
  vq_init (&pl.isp_cap, "isp-out", pl.isp_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
           V4L2_MEMORY_DMABUF, &pl.pb);
  if (vq_reqbufs (&pl.cap, pl.pa.count) < 0 ||
      vq_reqbufs (&pl.isp_out, pl.pa.count) < 0 ||
      vq_reqbufs (&pl.isp_cap, pl.pb.count) < 0)
    return -1;
  /* Buffer index i on capture is buffer index i on the ISP input. */
  pl.cap.count = pl.isp_out.count = MIN (pl.cap.count, pl.isp_out.count);

  if (pl.mode == MODE_MJPEG) {
    vq_init (&pl.jpeg_out, "jpeg-in", pl.jpeg_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
             V4L2_MEMORY_DMABUF, &pl.pb);
    vq_init (&pl.jpeg_cap, "jpeg-out", pl.jpeg_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
             V4L2_MEMORY_DMABUF, &pl.pc);
    if (vq_reqbufs (&pl.jpeg_out, pl.pb.count) < 0 ||
        vq_reqbufs (&pl.jpeg_cap, pl.pc.count) < 0)
      return -1;
    pl.isp_cap.count = pl.jpeg_out.count = MIN (pl.isp_cap.count, pl.jpeg_out.count);
  }
  return 0;
}

static void
pipeline_stop (void)
{
  struct uvc_device *uvc = pl.uvc;

  if (!pl.running && !uvc->streaming && !uvc->q.count)
    return;

  vq_stream (&pl.cap, false);
  vq_stream (&pl.isp_out, false);
  vq_stream (&pl.isp_cap, false);
  if (pl.mode == MODE_MJPEG) {
    vq_stream (&pl.jpeg_out, false);
    vq_stream (&pl.jpeg_cap, false);
  }

  if (uvc->streaming) {
    vq_stream (&uvc->q, false);
    uvc->streaming = false;
  }
  vq_munmap (&uvc->q);
  if (uvc->q.count)
    vq_reqbufs (&uvc->q, 0);

  if (pl.running)
    printf ("Pipeline stopped (%llu frames sent, %llu dropped)\n",
            uvc->frames, uvc->dropped);
  pl.running = false;
}

static int
pipeline_start (void)
{
  struct uvc_device *uvc = pl.uvc;
  unsigned int i;
  int ret;

  if (pl.running)
    return 0;

  /* UVC side: MMAP buffers that we fill from the last stage. */
  if (uvc_video_set_format (uvc) < 0)
    return -1;
  if (vq_reqbufs (&uvc->q, uvc->nbufs) < 0 || vq_mmap (&uvc->q) < 0)
    return -1;
  uvc->frames = uvc->dropped = 0;
  uvc->stat_frames = 0;
  clock_gettime (CLOCK_MONOTONIC, &uvc->stat_ts);

  /* Prime every capture-type queue with empty buffers. */
  for (i = 0; i < pl.isp_cap.count; i++)
    if (vq_qbuf (&pl.isp_cap, i, 0, NULL) < 0)
      return -1;
  if (pl.mode == MODE_MJPEG)
    for (i = 0; i < pl.jpeg_cap.count; i++)
      if (vq_qbuf (&pl.jpeg_cap, i, 0, NULL) < 0)
        return -1;
  for (i = 0; i < pl.cap.count; i++)
    if (vq_qbuf (&pl.cap, i, 0, NULL) < 0)
      return -1;

  /* Downstream first so nothing is produced into a stopped queue. */
  if (pl.mode == MODE_MJPEG) {
    if (vq_stream (&pl.jpeg_cap, true) < 0 || vq_stream (&pl.jpeg_out, true) < 0)
      return -1;
  }
  if (vq_stream (&pl.isp_cap, true) < 0 || vq_stream (&pl.isp_out, true) < 0)
    return -1;

  ret = vq_stream (&pl.cap, true);
  if (ret < 0) {
    if (ret == -EPIPE)
      err ("Capture STREAMON failed with EPIPE: the media pipeline formats do not match.\n"
           "Run hdmi-setup.sh (it configures media-ctl) and use the same -g/-z values.\n");
    else if (ret == -ENOLINK || ret == -ENOLCK)
      err ("No stable HDMI signal.\n");
    return -1;
  }

  pl.running = true;
  clock_gettime (CLOCK_MONOTONIC, &last_cap_ts);
  printf ("Pipeline running\n");
  return 0;
}

static void
uvc_report_stats (struct uvc_device *uvc)
{
  struct timespec now;
  double dt;

  clock_gettime (CLOCK_MONOTONIC, &now);
  dt = (now.tv_sec - uvc->stat_ts.tv_sec) + (now.tv_nsec - uvc->stat_ts.tv_nsec) / 1e9;
  if (dt < 5.0)
    return;
  printf ("UVC: %.1f fps (captured %llu, sent %llu, dropped %llu)\n",
          (uvc->frames - uvc->stat_frames) / dt, pl.frames_in, uvc->frames,
          uvc->dropped);
  uvc->stat_ts = now;
  uvc->stat_frames = uvc->frames;
}

/*
 * Copy a finished frame into a free UVC buffer and queue it.
 * row_bytes/rows: if non-zero, repack lines of src_stride bytes into
 * tightly packed lines (UVC YUYV has no line padding).
 */
static void
deliver_to_uvc (struct dmabuf *src, unsigned int len, unsigned int src_stride,
                unsigned int row_bytes, unsigned int rows)
{
  struct uvc_device *uvc = pl.uvc;
  unsigned int i, r, out_len;
  uint8_t *dst;
  const uint8_t *s;
  int ret;

  for (i = 0; i < uvc->q.count; i++)
    if (!uvc->q.queued[i])
      break;
  if (i == uvc->q.count) {
    uvc->dropped++;           /* host is not keeping up */
    return;
  }

  dst = uvc->q.maps[i];
  s = src->map;

  dmabuf_sync (src, true);
  if (row_bytes && src_stride != row_bytes) {
    out_len = row_bytes * rows;
    if (out_len > uvc->q.lens[i])
      out_len = 0;
    else
      for (r = 0; r < rows; r++)
        memcpy (dst + (size_t) r * row_bytes, s + (size_t) r * src_stride, row_bytes);
  }
  else {
    out_len = row_bytes ? row_bytes * rows : len;
    if (out_len > uvc->q.lens[i] || out_len > src->size)
      out_len = 0;
    else
      memcpy (dst, s, out_len);
  }
  dmabuf_sync (src, false);

  if (!out_len) {
    dbg ("Frame of %u bytes does not fit UVC buffer of %zu bytes\n", len, uvc->q.lens[i]);
    uvc->dropped++;
    return;
  }

  ret = vq_qbuf (&uvc->q, i, out_len, NULL);
  if (ret == -ENODEV) {
    printf ("UVC: gadget went away (ENODEV)\n");
    pipeline_stop ();
    return;
  }
  if (ret < 0)
    return;

  if (!uvc->streaming) {
    if (vq_stream (&uvc->q, true) == 0)
      uvc->streaming = true;
  }
  uvc->frames++;
  uvc_report_stats (uvc);
}

static void
on_capture_ready (void)
{
  unsigned int idx, used, flags;
  struct timeval ts;

  while (pl.running && vq_dqbuf (&pl.cap, &idx, &used, &flags, &ts) == 0) {
    pl.frames_in++;
    clock_gettime (CLOCK_MONOTONIC, &last_cap_ts);
    if ((flags & V4L2_BUF_FLAG_ERROR) || idx >= pl.isp_out.count ||
        vq_qbuf (&pl.isp_out, idx, pl.isp_out_fmt.sizeimage, &ts) < 0)
      vq_qbuf (&pl.cap, idx, 0, NULL);     /* drop, recycle */
  }
}

static void
on_isp_input_done (void)
{
  unsigned int idx;

  while (pl.running && vq_dqbuf (&pl.isp_out, &idx, NULL, NULL, NULL) == 0)
    vq_qbuf (&pl.cap, idx, 0, NULL);
}

static void
on_isp_output_ready (void)
{
  unsigned int idx, used, flags;
  struct timeval ts;

  while (pl.running && vq_dqbuf (&pl.isp_cap, &idx, &used, &flags, &ts) == 0) {
    if (flags & V4L2_BUF_FLAG_ERROR) {
      vq_qbuf (&pl.isp_cap, idx, 0, NULL);
      continue;
    }
    if (pl.mode == MODE_YUYV) {
      deliver_to_uvc (&pl.pb.bufs[idx], used, pl.isp_cap_fmt.bytesperline,
                      pl.isp_cap_fmt.width * 2, pl.isp_cap_fmt.height);
      if (pl.running)
        vq_qbuf (&pl.isp_cap, idx, 0, NULL);
    }
    else {
      if (pl.jpeg_out.nqueued >= MAX_JPEG_INFLIGHT || idx >= pl.jpeg_out.count ||
          vq_qbuf (&pl.jpeg_out, idx, pl.jpeg_out_fmt.sizeimage, &ts) < 0)
        vq_qbuf (&pl.isp_cap, idx, 0, NULL);   /* encoder busy: drop */
    }
  }
}

static void
on_jpeg_input_done (void)
{
  unsigned int idx;

  while (pl.running && vq_dqbuf (&pl.jpeg_out, &idx, NULL, NULL, NULL) == 0)
    vq_qbuf (&pl.isp_cap, idx, 0, NULL);
}

static void
on_jpeg_output_ready (void)
{
  unsigned int idx, used, flags;

  while (pl.running && vq_dqbuf (&pl.jpeg_cap, &idx, &used, &flags, NULL) == 0) {
    if (!(flags & V4L2_BUF_FLAG_ERROR) && used)
      deliver_to_uvc (&pl.pc.bufs[idx], used, 0, 0, 0);
    if (pl.running)
      vq_qbuf (&pl.jpeg_cap, idx, 0, NULL);
  }
}

static void
on_uvc_buffer_done (void)
{
  struct uvc_device *uvc = pl.uvc;
  unsigned int idx, flags;

  while (uvc->streaming && vq_dqbuf (&uvc->q, &idx, NULL, &flags, NULL) == 0) {
    if (flags & V4L2_BUF_FLAG_ERROR)
      dbg ("UVC: buffer %u completed with error (host stopping?)\n", idx);
  }
}

/* ---------------------------------------------------------------------------
 * main
 */

static void
on_signal (int sig)
{
  (void) sig;
  quit_requested = 1;
}

static void
usage (const char *argv0)
{
  fprintf (stderr, "Usage: %s [options]\n", argv0);
  fprintf (stderr, "Available options are\n");
  fprintf (stderr, " -c n        Number of capture buffers (default 4)\n");
  fprintf (stderr, " -d          Debug output\n");
  fprintf (stderr, " -f <format> UVC output format\n\t0 = YUYV (uncompressed)\n\t1 = MJPEG\n");
  fprintf (stderr, " -F <field>  Capture field order (none, interlaced, ...; default none)\n");
  fprintf (stderr, " -g WxH      Capture frame size (default: detected HDMI timings)\n");
  fprintf (stderr, " -h          Print this help screen and exit\n");
  fprintf (stderr, " -i device   ISP mem2mem device (default: auto, bcm2835-codec-isp)\n");
  fprintf (stderr, " -j device   JPEG encoder device (default: auto, bcm2835-codec-encode_image)\n");
  fprintf (stderr, " -m n        Streaming mult for ISOC (0-2)\n");
  fprintf (stderr, " -n n        Number of UVC buffers (2-32, default 3)\n");
  fprintf (stderr, " -q n        JPEG quality 1-100 (default 80)\n");
  fprintf (stderr, " -r <res>    Output resolution: 0 = 640x360, 1 = 1280x720 (default),\n"
                   "             2 = 1920x1080, or WxH. Must match uvc-setup.sh.\n");
  fprintf (stderr, " -s <speed>  USB bus speed: 0 = FS, 1 = HS (default), 2 = SS\n");
  fprintf (stderr, " -t n        Streaming burst (0-15)\n");
  fprintf (stderr, " -u device   UVC gadget device (default: auto)\n");
  fprintf (stderr, " -v device   V4L2 capture device (default /dev/video0)\n");
  fprintf (stderr, " -z fourcc   Capture pixel format (UYVY default, RGB24, BGR24...)\n");
  fprintf (stderr, " -b n        Ignored (old MMAL bitrate); use -q\n");
}

static int
parse_size (const char *s, unsigned int *w, unsigned int *h)
{
  char *end;
  unsigned long a, b;

  a = strtoul (s, &end, 10);
  if (end == s || (*end != 'x' && *end != 'X'))
    return -1;
  b = strtoul (end + 1, &end, 10);
  if (*end != '\0' || !a || !b)
    return -1;
  *w = a;
  *h = b;
  return 0;
}

int
main (int argc, char *argv[])
{
  static struct uvc_device udev_storage;
  struct uvc_device *udev = &udev_storage;
  const char *v4l2_devname = "/dev/video0";
  const char *uvc_devname = NULL;
  const char *isp_devname = NULL;
  const char *jpeg_devname = NULL;
  char uvc_path[64], isp_path[64], jpeg_path[64];
  const struct v4l2_format_info *info;
  unsigned int pixelformat = V4L2_PIX_FMT_UYVY;
  unsigned int in_w = 0, in_h = 0, dv_w = 0, dv_h = 0, fps = 0;
  unsigned int out_w = 1280, out_h = 720;
  unsigned int cap_nbufs = 4;
  int field = V4L2_FIELD_NONE;
  int format = MODE_YUYV;
  int nbufs = 3;
  int quality = 80;
  int mult = 0, burst = 0;
  enum usb_device_speed speed = USB_SPEED_HIGH;
  struct sigaction sa;
  int opt, ret = 1;

  setvbuf (stdout, NULL, _IOLBF, 0);

  while ((opt = getopt (argc, argv, "b:c:dF:f:g:hi:j:m:n:q:r:s:t:u:v:z:")) != -1) {
    switch (opt) {
    case 'b':
      printf ("Note: -b (bitrate) is ignored; JPEG size is controlled with -q\n");
      break;
    case 'c':
      cap_nbufs = clamp (atoi (optarg), 2, 16);
      break;
    case 'd':
      debug = 1;
      break;
    case 'F':
      field = v4l2_field_from_string (optarg);
      if (field < 0) {
        err ("Unknown field '%s'\n", optarg);
        return 1;
      }
      break;
    case 'f':
      format = atoi (optarg);
      if (format != MODE_YUYV && format != MODE_MJPEG) {
        usage (argv[0]);
        return 1;
      }
      break;
    case 'g':
      if (parse_size (optarg, &in_w, &in_h) < 0) {
        err ("Invalid size '%s'\n", optarg);
        return 1;
      }
      break;
    case 'h':
      usage (argv[0]);
      return 0;
    case 'i':
      isp_devname = optarg;
      break;
    case 'j':
      jpeg_devname = optarg;
      break;
    case 'm':
      mult = clamp (atoi (optarg), 0, 2);
      break;
    case 'n':
      nbufs = clamp (atoi (optarg), 2, 32);
      break;
    case 'q':
      quality = clamp (atoi (optarg), 1, 100);
      break;
    case 'r':
      if (strchr (optarg, 'x') || strchr (optarg, 'X')) {
        if (parse_size (optarg, &out_w, &out_h) < 0) {
          err ("Invalid resolution '%s'\n", optarg);
          return 1;
        }
      }
      else {
        switch (atoi (optarg)) {
        case 0: out_w = 640;  out_h = 360;  break;
        case 1: out_w = 1280; out_h = 720;  break;
        case 2: out_w = 1920; out_h = 1080; break;
        default:
          usage (argv[0]);
          return 1;
        }
      }
      break;
    case 's':
      speed = (enum usb_device_speed) clamp (atoi (optarg), 0, 2);
      break;
    case 't':
      burst = clamp (atoi (optarg), 0, 15);
      break;
    case 'u':
      uvc_devname = optarg;
      break;
    case 'v':
      v4l2_devname = optarg;
      break;
    case 'z':
      info = v4l2_format_by_name (optarg);
      if (!info) {
        err ("Unsupported capture format '%s'\n", optarg);
        return 1;
      }
      pixelformat = info->fourcc;
      break;
    default:
      usage (argv[0]);
      return 1;
    }
  }

  memset (&sa, 0, sizeof (sa));
  sa.sa_handler = on_signal;
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);
  signal (SIGPIPE, SIG_IGN);

  /* --- Locate devices --- */
  if (!uvc_devname) {
    if (find_uvc_gadget (uvc_path, sizeof (uvc_path)) < 0) {
      err ("No UVC gadget video device found. Run 'sudo ./uvc-setup.sh start ...' first,\n"
           "or pass it with -u.\n");
      return 1;
    }
    uvc_devname = uvc_path;
  }
  if (!isp_devname) {
    if (find_video_by_name ("bcm2835-codec-isp", isp_path, sizeof (isp_path)) < 0)
      snprintf (isp_path, sizeof (isp_path), "/dev/video12");
    isp_devname = isp_path;
  }
  if (format == MODE_MJPEG && !jpeg_devname) {
    if (find_video_by_name ("bcm2835-codec-encode_image", jpeg_path, sizeof (jpeg_path)) < 0)
      snprintf (jpeg_path, sizeof (jpeg_path), "/dev/video31");
    jpeg_devname = jpeg_path;
  }

  /* --- UVC gadget --- */
  udev->width = out_w;
  udev->height = out_h;
  udev->fcc = format == MODE_MJPEG ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
  udev->imgsize = format == MODE_MJPEG ? out_w * out_h : out_w * out_h * 2;
  udev->nbufs = nbufs;
  udev->mult = mult;
  udev->burst = burst;
  udev->speed = speed;
  udev->bulk = 0;
  udev->brightness_val = PU_BRIGHTNESS_DEFAULT_VAL;
  switch (speed) {
  case USB_SPEED_FULL:  udev->maxpkt = 1023; break;
  case USB_SPEED_HIGH:  udev->maxpkt = 2048; break;
  default:              udev->maxpkt = 1024; break;
  }

  uvc_frames[0].width = out_w;
  uvc_frames[0].height = out_h;
  uvc_formats[0].fcc = udev->fcc;

  if (uvc_open (udev, uvc_devname) < 0)
    return 1;
  pl.uvc = udev;
  pl.mode = format;

  /* --- Capture --- */
  pl.cap_fd = open (v4l2_devname, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (pl.cap_fd < 0) {
    err ("Cannot open capture device %s: %s\n", v4l2_devname, strerror (errno));
    return 1;
  }
  printf ("%-10s %s\n", "capture", v4l2_devname);

  hdmi_lock (pl.cap_fd, &dv_w, &dv_h, &fps);
  hdmi_w = dv_w;                /* 0 = no DV timings: watchdog disabled */
  hdmi_h = dv_h;
  hdmi_fd = pl.cap_fd;
  if (!in_w || !in_h) {
    if (dv_w && dv_h) {
      in_w = dv_w;
      in_h = dv_h;
    }
    else {
      struct fmt cur;
      if (fmt_get (pl.cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cur) == 0) {
        in_w = cur.width;
        in_h = cur.height;
      }
    }
  }
  if (!in_w || !in_h) {
    err ("Unknown capture size; pass -g WxH\n");
    return 1;
  }

  /* --- Hardware converters --- */
  pl.isp_fd = open_m2m (isp_devname, "isp");
  if (pl.isp_fd < 0) {
    err ("The bcm2835-codec driver is required (it is loaded by default on Pi OS).\n");
    return 1;
  }
  if (format == MODE_MJPEG) {
    pl.jpeg_fd = open_m2m (jpeg_devname, "jpeg");
    if (pl.jpeg_fd < 0)
      return 1;
  }

  if (heap_open () < 0)
    return 1;

  if (pipeline_setup (in_w, in_h, pixelformat, field, cap_nbufs, quality) < 0)
    goto cleanup;

  uvc_events_init (udev);
  printf ("Ready: waiting for the USB host to start streaming %c%c%c%c %ux%u\n",
          pixfmtstr (udev->fcc), out_w, out_h);

  /* --- Event loop ---
   * select() is used on purpose: exceptfds only reports POLLPRI (UVC events),
   * so the gadget fd can be watched while idle without spinning on the
   * POLLERR it reports when not streaming. A queue's fd is only added to the
   * read/write sets while that queue has buffers queued, for the same reason.
   */
  while (!quit_requested) {
    fd_set rfds, wfds, efds;
    struct timeval tv;
    int maxfd = udev->fd;
    int n;

    FD_ZERO (&rfds);
    FD_ZERO (&wfds);
    FD_ZERO (&efds);
    FD_SET (udev->fd, &efds);

#define WATCH(fd, set) do { FD_SET ((fd), (set)); if ((fd) > maxfd) maxfd = (fd); } while (0)
    if (pl.running) {
      if (pl.cap.nqueued)
        WATCH (pl.cap_fd, &rfds);
      if (pl.isp_cap.nqueued)
        WATCH (pl.isp_fd, &rfds);
      if (pl.isp_out.nqueued)
        WATCH (pl.isp_fd, &wfds);
      if (pl.mode == MODE_MJPEG) {
        if (pl.jpeg_cap.nqueued)
          WATCH (pl.jpeg_fd, &rfds);
        if (pl.jpeg_out.nqueued)
          WATCH (pl.jpeg_fd, &wfds);
      }
      if (udev->streaming && udev->q.nqueued)
        WATCH (udev->fd, &wfds);
    }
#undef WATCH

    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    n = select (maxfd + 1, &rfds, &wfds, &efds, &tv);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      err ("select: %s\n", strerror (errno));
      break;
    }

    /* HDMI watchdog: frames stopped while streaming, or the source went
     * away / changed resolution (checked once a second, also when idle). */
    if (hdmi_w) {
      static struct timespec last_check;
      unsigned int w, h;

      if (pl.running && ts_elapsed (&last_cap_ts) > 2.0) {
        hdmi_lost ("no frames captured for 2 s");
        continue;
      }
      if (ts_elapsed (&last_check) >= 1.0) {
        clock_gettime (CLOCK_MONOTONIC, &last_check);
        if (hdmi_query (pl.cap_fd, &w, &h) < 0) {
          hdmi_lost ("signal lost");
          continue;
        }
        if (w != hdmi_w || h != hdmi_h) {
          char why[64];
          snprintf (why, sizeof why, "resolution changed to %ux%u", w, h);
          hdmi_lost (why);
          continue;
        }
      }
    }
    if (n == 0)
      continue;

    if (FD_ISSET (udev->fd, &efds))
      uvc_events_process (udev);
    if (!pl.running)
      continue;

    if (FD_ISSET (udev->fd, &wfds))
      on_uvc_buffer_done ();
    if (FD_ISSET (pl.cap_fd, &rfds))
      on_capture_ready ();
    if (FD_ISSET (pl.isp_fd, &wfds))
      on_isp_input_done ();
    if (FD_ISSET (pl.isp_fd, &rfds))
      on_isp_output_ready ();
    if (pl.mode == MODE_MJPEG) {
      if (FD_ISSET (pl.jpeg_fd, &wfds))
        on_jpeg_input_done ();
      if (FD_ISSET (pl.jpeg_fd, &rfds))
        on_jpeg_output_ready ();
    }
  }
  ret = exit_status;
  printf ("Exiting\n");

cleanup:
  pipeline_stop ();
  if (pl.cap.queued)
    vq_reqbufs (&pl.cap, 0);
  if (pl.isp_out.queued)
    vq_reqbufs (&pl.isp_out, 0);
  if (pl.isp_cap.queued)
    vq_reqbufs (&pl.isp_cap, 0);
  if (pl.jpeg_out.queued)
    vq_reqbufs (&pl.jpeg_out, 0);
  if (pl.jpeg_cap.queued)
    vq_reqbufs (&pl.jpeg_cap, 0);
  pool_free (&pl.pa);
  pool_free (&pl.pb);
  pool_free (&pl.pc);
  if (pl.jpeg_fd >= 0)
    close (pl.jpeg_fd);
  if (pl.isp_fd >= 0)
    close (pl.isp_fd);
  if (pl.cap_fd >= 0)
    close (pl.cap_fd);
  if (heap_fd >= 0)
    close (heap_fd);
  close (udev->fd);
  return ret;
}
