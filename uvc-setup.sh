#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Create / remove a ConfigFS USB gadget with a UVC (webcam) function and an
# ECM (USB ethernet) function. Updated for Raspberry Pi OS Bookworm/Trixie
# (6.x kernels).
#
# Usage:
#   sudo ./uvc-setup.sh start mjpeg [WxH]     (default 1280x720)
#   sudo ./uvc-setup.sh start yuyv  [WxH]
#   sudo ./uvc-setup.sh stop
#
# The WxH given here must match v4l2-mmal-uvc's -r option.
# Environment overrides: MAXPACKET (default 2048), ETHERNET=0 to skip ECM.

OP=$1
PLAT=$2
SIZE=${3:-1280x720}

CONFIGFS="/sys/kernel/config"
GADGET="$CONFIGFS/usb_gadget"
G="$GADGET/pizero"
VID="0x1d6b"
PID="0x0104"
SERIAL="0123456789"
MANUF=$(hostname)
PRODUCT="pizero"
FUNC=uvc.0
ETH=ecm.usb0
MAXPACKET=${MAXPACKET:-3072}
ETHERNET=${ETHERNET:-1}

# Frame intervals in 100ns units: 60, 50, 30, 20 fps. Keep in sync with
# UVC_DEFAULT_INTERVAL in v4l2-mmal-uvc.c.
INTERVALS="166666
200000
333333
500000"
DEFAULT_INTERVAL=333333

die() { echo "Error: $*" >&2; exit 1; }

WIDTH=${SIZE%x*}
HEIGHT=${SIZE#*x}
case "$WIDTH$HEIGHT" in
	''|*[!0-9]*) die "invalid size '$SIZE' (expected WxH)";;
esac

BOARD=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null)
UDC=$(ls /sys/class/udc 2>/dev/null | head -n1)

create_frame() {
	# create_frame <format dir: uncompressed|mjpeg> <name> <bytes per pixel>
	FORMAT=$1
	NAME=$2
	BPP=$3

	wdir=$G/functions/$FUNC/streaming/$FORMAT/$NAME/${HEIGHT}p
	mkdir -p "$wdir" || die "cannot create $wdir"
	echo "$WIDTH" > "$wdir/wWidth"
	echo "$HEIGHT" > "$wdir/wHeight"
	echo $(( WIDTH * HEIGHT * BPP )) > "$wdir/dwMaxVideoFrameBufferSize"
	echo "$INTERVALS" > "$wdir/dwFrameInterval"
	echo "$DEFAULT_INTERVAL" > "$wdir/dwDefaultFrameInterval"
	echo $(( WIDTH * HEIGHT * 80 )) > "$wdir/dwMinBitRate"
	echo $(( WIDTH * HEIGHT * 160 )) > "$wdir/dwMaxBitRate"
}

create_uvc() {
	echo "  Creating UVC function $FUNC (${WIDTH}x${HEIGHT} $PLAT)"
	F=$G/functions/$FUNC
	mkdir -p "$F" || die "cannot create UVC function (is the usb_f_uvc module available?)"

	case "$PLAT" in
	mjpeg|test) create_frame mjpeg m 1 ;;
	yuyv)       create_frame uncompressed u 2 ;;
	*)          die "unknown format '$PLAT' (mjpeg or yuyv)" ;;
	esac

	mkdir -p "$F/streaming/header/h"
	[ -d "$F/streaming/uncompressed/u" ] && ln -s "$F/streaming/uncompressed/u" "$F/streaming/header/h/u"
	[ -d "$F/streaming/mjpeg/m" ] && ln -s "$F/streaming/mjpeg/m" "$F/streaming/header/h/m"
	# 6.x kernels refuse to bind unless every speed has descriptors.
	for s in fs hs ss; do
		ln -s "$F/streaming/header/h" "$F/streaming/class/$s/h"
	done

	mkdir -p "$F/control/header/h"
	for s in fs ss; do
		ln -s "$F/control/header/h" "$F/control/class/$s/h"
	done

	# The UVC gadget accepts up to 3072 bytes per microframe (mult 3) on
	# high-speed; 2048 was the reliable maximum with dwc2 historically.
	echo "$MAXPACKET" > "$F/streaming_maxpacket"
	echo 1 > "$F/streaming_interval"

	ln -s "$F" "$G/configs/c.1/$FUNC"
}

create_eth() {
	echo "  Creating ECM (USB ethernet) function $ETH"
	mkdir -p "$G/functions/$ETH" || { echo "  (ECM not available, skipping)"; return; }
	# first byte of address must be even
	echo "48:6f:73:74:50:43" > "$G/functions/$ETH/host_addr"   # "HostPC"
	echo "42:61:64:55:53:42" > "$G/functions/$ETH/dev_addr"    # "BadUSB"
	ln -s "$G/functions/$ETH" "$G/configs/c.1/$ETH"
}

delete_uvc() {
	F=$G/functions/$FUNC
	[ -d "$F" ] || return
	echo "  Removing UVC function $FUNC"
	rm -f "$G/configs/c.1/$FUNC"
	rm -f "$F"/control/class/*/h "$F"/streaming/class/*/h
	rm -f "$F"/streaming/header/h/u "$F"/streaming/header/h/m
	for d in "$F"/streaming/uncompressed/*/* "$F"/streaming/mjpeg/*/*; do
		[ -d "$d" ] && rmdir "$d"
	done
	for d in "$F"/streaming/uncompressed/* "$F"/streaming/mjpeg/*; do
		[ -d "$d" ] && rmdir "$d"
	done
	rmdir "$F/streaming/header/h" 2>/dev/null
	rmdir "$F/control/header/h" 2>/dev/null
	rmdir "$F"
}

delete_eth() {
	[ -d "$G/functions/$ETH" ] || return
	echo "  Removing ECM function $ETH"
	rm -f "$G/configs/c.1/$ETH"
	rmdir "$G/functions/$ETH"
}

case "$OP" in
start)
	[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"
	echo "Board: $BOARD"
	modprobe libcomposite 2>/dev/null
	mountpoint -q "$CONFIGFS" || mount -t configfs none "$CONFIGFS"
	[ -d "$GADGET" ] || die "$GADGET missing (is libcomposite loaded?)"
	UDC=$(ls /sys/class/udc 2>/dev/null | head -n1)
	[ -n "$UDC" ] || die "no USB device controller found. Add 'dtoverlay=dwc2,dr_mode=peripheral' to /boot/firmware/config.txt and reboot."
	echo "UDC:   $UDC"
	[ -d "$G" ] && die "gadget already exists; run '$0 stop' first"

	echo "Creating gadget"
	mkdir -p "$G" || die "cannot create $G"
	echo $VID > "$G/idVendor"
	echo $PID > "$G/idProduct"
	echo 0x0100 > "$G/bcdDevice"
	echo 0x0200 > "$G/bcdUSB"
	# Miscellaneous / Interface Association Descriptor (needed for composite UVC)
	echo 0xEF > "$G/bDeviceClass"
	echo 0x02 > "$G/bDeviceSubClass"
	echo 0x01 > "$G/bDeviceProtocol"
	echo 64 > "$G/bMaxPacketSize0"

	mkdir -p "$G/strings/0x409"
	echo "$SERIAL" > "$G/strings/0x409/serialnumber"
	echo "$MANUF" > "$G/strings/0x409/manufacturer"
	echo "$PRODUCT" > "$G/strings/0x409/product"

	mkdir -p "$G/configs/c.1/strings/0x409"
	echo "UVC" > "$G/configs/c.1/strings/0x409/configuration"
	echo 500 > "$G/configs/c.1/MaxPower"

	create_uvc
	[ "$ETHERNET" = "1" ] && create_eth

	echo "Binding to $UDC"
	udevadm settle -t 5 2>/dev/null || :
	echo "$UDC" > "$G/UDC" || die "failed to bind gadget to $UDC (see dmesg)"
	echo "OK. Gadget video node:"
	for v in /sys/class/video4linux/video*; do
		[ "$(cat "$v/name" 2>/dev/null)" = "$UDC" ] && echo "  /dev/$(basename "$v")"
	done
	;;

stop)
	[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"
	[ -d "$G" ] || die "no gadget found"
	echo "Stopping gadget (close v4l2-mmal-uvc first)"
	echo "" > "$G/UDC" 2>/dev/null
	delete_uvc
	delete_eth
	rmdir "$G/configs/c.1/strings/0x409" 2>/dev/null
	rmdir "$G/configs/c.1" 2>/dev/null
	rmdir "$G/strings/0x409" 2>/dev/null
	rmdir "$G" && echo "OK" || echo "Some entries could not be removed; reboot to clear."
	;;

*)
	echo "Usage: $0 {start mjpeg|start yuyv} [WxH]"
	echo "       $0 stop"
	exit 1
	;;
esac
