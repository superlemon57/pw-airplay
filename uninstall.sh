#!/bin/sh
# pw-airplay - removes what install.sh installed. Run as root with --system to
# remove what install-system.sh installed. The configuration is kept unless
# --purge is given.
set -eu

PREFIX="${PREFIX:-$HOME/.local}"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/pw-airplay"
BUILD_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/pw-airplay"
purge=0

for arg in "$@"; do
	case "$arg" in
	--system)
		[ "$(id -u)" -eq 0 ] || { echo "--system must be run as root" >&2; exit 1; }
		systemctl disable --now airptpd.service 2>/dev/null || true
		rm -f /etc/systemd/system/airptpd.service /usr/local/sbin/airptpd
		systemctl daemon-reload
		if command -v firewall-cmd >/dev/null && firewall-cmd --state >/dev/null 2>&1; then
			firewall-cmd --quiet --permanent --remove-service=ptp || true
			firewall-cmd --quiet --reload
		fi
		echo "System part removed."
		exit 0
		;;
	--purge)
		purge=1
		;;
	*)
		echo "usage: $0 [--purge] | --system" >&2
		exit 1
		;;
	esac
done

systemctl --user disable --now pw-airplay.service pw-airplay-engine.service 2>/dev/null || true
rm -f "$CONF_DIR/systemd/user/pw-airplay.service" "$CONF_DIR/systemd/user/pw-airplay-engine.service"
systemctl --user daemon-reload
rm -f "$PREFIX/bin/pw-airplayd" "$CONF_DIR/pipewire/pipewire.conf.d/50-pw-airplay-no-raop.conf"
rm -rf "$PREFIX/opt/owntone" "$PREFIX/share/doc/pw-airplay" "$DATA_DIR" "$BUILD_DIR"
[ "$purge" -eq 1 ] && rm -rf "$CONF_DIR/pw-airplay"

echo "Removed. Restart PipeWire to get its own AirPlay sinks back:"
echo "  systemctl --user restart pipewire pipewire-pulse wireplumber"
echo "The system part (if installed) is removed with: sudo $0 --system"
