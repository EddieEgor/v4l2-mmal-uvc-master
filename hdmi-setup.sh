#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Prepare the TC358743 HDMI->CSI-2 bridge on Raspberry Pi OS Bookworm/Trixie:
#   1. load an EDID so the HDMI source starts sending,
#   2. lock the receiver to the incoming timings,
#   3. on media-controller kernels, push the resulting format through the
#      media graph (tc358743 -> unicam) so STREAMON does not fail with EPIPE.
#
# Usage:  ./hdmi-setup.sh [edid-file] [mbus-format]
#   edid-file    default: 1080P50EDID.txt next to this script
#   mbus-format  default: UYVY8_1X16 (use RGB888_1X24 together with -zRGB24)
#
# Env: VIDEO=/dev/video0 (unicam capture node)
#
# Needs: sudo apt install v4l-utils   (provides v4l2-ctl and media-ctl)

DIR=$(cd "$(dirname "$0")" && pwd)
EDID=${1:-$DIR/1080P50EDID.txt}
MBUS=${2:-UYVY8_1X16}
VIDEO=${VIDEO:-/dev/video0}

command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl not found: sudo apt install v4l-utils"; exit 1; }
[ -f "$EDID" ] || { echo "EDID file $EDID not found"; exit 1; }

# Find the tc358743 subdevice node, if the kernel exposes one.
SUBDEV=
for s in /sys/class/video4linux/v4l-subdev*; do
	[ -e "$s/name" ] || continue
	case "$(cat "$s/name")" in
	*tc358743*) SUBDEV=/dev/$(basename "$s");;
	esac
done

# Legacy unicam forwards these ioctls from the video node; MC unicam does not.
if [ -n "$SUBDEV" ] && v4l2-ctl -d "$SUBDEV" --get-edid >/dev/null 2>&1; then
	TARGET=$SUBDEV
else
	TARGET=$VIDEO
fi
echo "TC358743 control node: $TARGET"

v4l2-ctl -d "$TARGET" --set-edid=file="$EDID" --fix-edid-checksums || exit 1
echo "EDID loaded; waiting for the source to lock..."

i=0
while [ $i -lt 10 ]; do
	if v4l2-ctl -d "$TARGET" --query-dv-timings >/dev/null 2>&1; then
		break
	fi
	sleep 1
	i=$((i + 1))
done

v4l2-ctl -d "$TARGET" --query-dv-timings || {
	echo "No signal: check the HDMI source is on and outputting a supported mode."
	exit 1
}
v4l2-ctl -d "$TARGET" --set-dv-bt-timings query || exit 1

W=$(v4l2-ctl -d "$TARGET" --get-dv-timings | awk -F: '/Active width/ {gsub(/ /,"",$2); print $2}')
H=$(v4l2-ctl -d "$TARGET" --get-dv-timings | awk -F: '/Active height/ {gsub(/ /,"",$2); print $2}')
echo "Locked to ${W}x${H}"

# Media-controller pipeline (only when the bridge is exposed as a subdev).
if [ -n "$SUBDEV" ] && command -v media-ctl >/dev/null; then
	for m in /dev/media*; do
		[ -e "$m" ] || continue
		TOPO=$(media-ctl -d "$m" -p 2>/dev/null) || continue
		ENT=$(echo "$TOPO" | sed -n 's/^- entity [0-9]*: \(tc358743[^(]*\) (.*/\1/p' | sed 's/ *$//' | head -n1)
		[ -n "$ENT" ] || continue
		echo "Configuring media graph $m (entity '$ENT')"
		media-ctl -d "$m" -V "'$ENT':0 [fmt:$MBUS/${W}x${H} field:none colorspace:srgb]" || true
		# Upstream-style unicam has its own subdev; set its sink pad too.
		echo "$TOPO" | sed -n 's/^- entity [0-9]*: \(unicam[^(]*\) (.*/\1/p' | sed 's/ *$//' |
		while read -r U; do
			case "$U" in
			*image*|*embedded*) ;;            # video nodes, not subdevs
			*) media-ctl -d "$m" -V "'$U':0 [fmt:$MBUS/${W}x${H} field:none]" 2>/dev/null || true;;
			esac
		done
	done
fi

echo
echo "Ready. Example:"
echo "  ./v4l2-mmal-uvc -v $VIDEO -f1 -r1     # MJPEG 1280x720"
