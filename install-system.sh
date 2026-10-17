#!/bin/sh
# pw-airplay - system part, run once as root:
#  - airptpd, the PTP clock AirPlay 2 speakers synchronise to. It needs the
#    privileged UDP ports 319/320, hence a small system service.
#  - firewall: let the speakers reach those ports
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0" >&2
	exit 1
fi

SRC="$(cd "$(dirname "$0")" && pwd)"
USER_HOME="${SUDO_USER:+$(getent passwd "$SUDO_USER" | cut -d: -f6)}"
AIRPTPD="${AIRPTPD:-${USER_HOME:-$HOME}/.local/opt/owntone/sbin/airptpd}"

if [ ! -x "$AIRPTPD" ]; then
	echo "airptpd not found at $AIRPTPD, run install.sh first" >&2
	exit 1
fi

install -Dm755 "$AIRPTPD" /usr/local/sbin/airptpd
# SELinux: label it like other locally installed binaries
command -v restorecon >/dev/null && restorecon /usr/local/sbin/airptpd || true
install -Dm644 "$SRC/systemd/airptpd.service" /etc/systemd/system/airptpd.service
systemctl daemon-reload
systemctl enable --now airptpd.service

if command -v firewall-cmd >/dev/null && firewall-cmd --state >/dev/null 2>&1; then
	firewall-cmd --permanent --add-service=ptp
	firewall-cmd --reload
fi

systemctl --no-pager --lines=0 status airptpd.service
echo "airptpd installed. Restart the engine: systemctl --user restart pw-airplay-engine"
