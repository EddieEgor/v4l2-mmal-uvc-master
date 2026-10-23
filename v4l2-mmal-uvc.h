/*
 * v4l2-mmal-uvc -- shared definitions
 *
 * The MMAL encoding column that used to live here is gone: format conversion
 * is now done by the kernel's bcm2835-codec V4L2 mem2mem driver, which takes
 * plain V4L2 fourccs.
 */
#ifndef V4L2_MMAL_UVC_H
#define V4L2_MMAL_UVC_H

#include <linux/videodev2.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct v4l2_format_info
{
  const char *name;
  unsigned int fourcc;
};

/* Formats that can be requested from the capture device with -z. */
static const struct v4l2_format_info pixel_formats[] = {
  {"RGB565", V4L2_PIX_FMT_RGB565},
  {"RGB565X", V4L2_PIX_FMT_RGB565X},
  {"RGB24", V4L2_PIX_FMT_RGB24},
  {"BGR24", V4L2_PIX_FMT_BGR24},
  {"BGR32", V4L2_PIX_FMT_BGR32},
  {"RGB32", V4L2_PIX_FMT_RGB32},
  {"UYVY", V4L2_PIX_FMT_UYVY},
  {"VYUY", V4L2_PIX_FMT_VYUY},
  {"YUYV", V4L2_PIX_FMT_YUYV},
  {"YVYU", V4L2_PIX_FMT_YVYU},
  {"NV12", V4L2_PIX_FMT_NV12},
  {"NV21", V4L2_PIX_FMT_NV21},
  {"YUV420", V4L2_PIX_FMT_YUV420},
  {"MJPEG", V4L2_PIX_FMT_MJPEG},
  {"JPEG", V4L2_PIX_FMT_JPEG},
};

#endif /* V4L2_MMAL_UVC_H */
