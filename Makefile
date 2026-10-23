# v4l2-mmal-uvc for Raspberry Pi OS Bookworm / Trixie (32- or 64-bit).
# No /opt/vc userland libraries are needed any more; only the kernel's
# bcm2835-codec, unicam and dma-heap drivers, and the standard libc headers.
#
#   sudo apt install build-essential v4l-utils
#   make

CROSS_COMPILE	?=
CC		:= $(CROSS_COMPILE)gcc
CFLAGS		?= -O2 -g
CFLAGS		+= -std=gnu11 -W -Wall -Wno-unused-parameter -pipe
LDFLAGS		?=
LIBS		:=

src = v4l2-mmal-uvc.c
obj = $(src:.c=.o)

all: v4l2-mmal-uvc

%.o : %.c v4l2-mmal-uvc.h
	$(CC) $(CFLAGS) -o $@ -c $<

v4l2-mmal-uvc: $(obj)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f *.o v4l2-mmal-uvc

.PHONY: all clean
