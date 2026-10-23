#!/bin/sh
# Example start-up (run uvc-setup.sh first, with the same format and size).
#   sudo ./uvc-setup.sh start mjpeg 1280x720
cd "$(dirname "$0")" || exit 1
./hdmi-setup.sh || exit 1

# MJPEG 1280x720 (UVC gadget device is auto-detected; add -u /dev/videoN to force)
./v4l2-mmal-uvc -v /dev/video0 -zUYVY -n3 -r1 -f1 -q80

# YUYV instead (needs 'uvc-setup.sh start yuyv'):
#./v4l2-mmal-uvc -v /dev/video0 -zUYVY -n3 -r1 -f0
