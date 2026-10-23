#!/bin/sh
# Install (or remove) the systemd services that start the camera at boot.
#
#   sudo sh install-service.sh            install and start
#   sudo sh install-service.sh uninstall  stop and remove
#   sudo sh install-service.sh readonly   protect the SD card against power
#                                         loss (read-only root, takes effect
#                                         after reboot)
#   sudo sh install-service.sh readwrite  undo 'readonly' (to update/edit)
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
UNITS="uvc-gadget.service v4l2-mmal-uvc.service"

[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo sh $0"; exit 1; }

case "$1" in
readonly)
	command -v raspi-config >/dev/null || { echo "raspi-config not found"; exit 1; }
	raspi-config nonint do_overlayfs 0
	echo "Read-only root (overlay) enabled. Reboot to apply: sudo reboot"
	echo "Changes made after that are lost at power-off; use 'readwrite' to edit."
	exit 0 ;;
readwrite)
	command -v raspi-config >/dev/null || { echo "raspi-config not found"; exit 1; }
	raspi-config nonint do_overlayfs 1
	echo "Read-only root disabled. Reboot to apply: sudo reboot"
	exit 0 ;;
esac

if [ "$1" = "uninstall" ]; then
	systemctl disable --now v4l2-mmal-uvc.service uvc-gadget.service 2>/dev/null || :
	for u in $UNITS; do rm -f "/etc/systemd/system/$u"; done
	systemctl daemon-reload
	echo "Removed. (/etc/default/v4l2-mmal-uvc was kept.)"
	exit 0
fi

[ -x "$DIR/v4l2-mmal-uvc" ] || { echo "Build first: make clean && make"; exit 1; }
grep -qs '^dtoverlay=dwc2' /boot/firmware/config.txt /boot/config.txt ||
	echo "WARNING: 'dtoverlay=dwc2,dr_mode=peripheral' not found in config.txt"

chmod +x "$DIR/uvc-setup.sh" "$DIR/hdmi-setup.sh" "$DIR/v4l2-mmal-uvc"
# Windows copies may have CRLF line endings, which break the shell scripts.
sed -i 's/\r$//' "$DIR/uvc-setup.sh" "$DIR/hdmi-setup.sh"

for u in $UNITS; do
	sed "s|@DIR@|$DIR|g" "$DIR/systemd/$u" > "/etc/systemd/system/$u"
done
[ -f /etc/default/v4l2-mmal-uvc ] ||
	sed 's/\r$//' "$DIR/systemd/v4l2-mmal-uvc.default" > /etc/default/v4l2-mmal-uvc

# Stop anything started by hand before the services take over.
pkill -x v4l2-mmal-uvc 2>/dev/null || :
systemctl daemon-reload
systemctl enable --now uvc-gadget.service v4l2-mmal-uvc.service
echo
systemctl --no-pager status v4l2-mmal-uvc.service | head -n 12
echo
echo "Installed. Logs:      journalctl -u v4l2-mmal-uvc -f"
echo "Settings:            /etc/default/v4l2-mmal-uvc"
