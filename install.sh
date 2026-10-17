#!/bin/sh
# pw-airplay - per-user install: AirPlay engine, daemon, configuration and
# systemd user services. The system-wide part (PTP clock daemon and firewall)
# is install-system.sh. Safe to run again to update.
set -eu

PREFIX="${PREFIX:-$HOME/.local}"
SRC="$(cd "$(dirname "$0")" && pwd)"
OWNTONE_VERSION=29.3
OWNTONE_REPO=https://github.com/owntone/owntone-server.git
BUILD_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/pw-airplay"
OWNTONE_SRC="$BUILD_DIR/owntone-$OWNTONE_VERSION"
OWNTONE_PREFIX="$PREFIX/opt/owntone"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"
UNIT_DIR="$CONF_DIR/systemd/user"
LOG="$BUILD_DIR/build.log"

step() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# Runs a build command, showing the end of the log if it fails
run() {
	if ! "$@" >>"$LOG" 2>&1; then
		tail -n 30 "$LOG" >&2
		die "'$*' failed, full log in $LOG"
	fi
}

check_deps() {
	missing=""
	for cmd in git cc make pkg-config autoreconf libtoolize gperf bison flex; do
		command -v "$cmd" >/dev/null 2>&1 || missing="$missing $cmd"
	done
	for mod in zlib libconfuse libcurl libsodium libxml-2.0 sqlite3 libevent \
		   libevent_pthreads json-c libplist-2.0 libavcodec libavformat \
		   libavfilter libavutil libswscale gnutls libwebsockets avahi-client \
		   alsa libgcrypt libpipewire-0.3 libsystemd; do
		pkg-config --exists "$mod" 2>/dev/null || missing="$missing $mod"
	done
	[ -f /usr/include/unistr.h ] || missing="$missing libunistring"
	[ -z "$missing" ] && return 0

	echo "Missing build dependencies:$missing" >&2
	if command -v dnf >/dev/null 2>&1; then
		cat >&2 <<-EOF

		On Fedora, install them with:
		  sudo dnf install git gcc make pkgconf autoconf automake libtool gettext-devel \\
		    gperf bison flex zlib-ng-compat-devel libconfuse-devel libcurl-devel \\
		    libsodium-devel libxml2-devel sqlite-devel libevent-devel json-c-devel \\
		    libplist-devel ffmpeg-free-devel gnutls-devel libwebsockets-devel \\
		    avahi-devel alsa-lib-devel libgcrypt-devel libunistring-devel \\
		    pipewire-devel systemd-devel
		(with RPM Fusion's ffmpeg, use ffmpeg-devel instead of ffmpeg-free-devel)
		EOF
	fi
	exit 1
}

build_engine() {
	if [ ! -d "$OWNTONE_SRC/.git" ]; then
		run git clone --quiet --depth 1 --branch "$OWNTONE_VERSION" "$OWNTONE_REPO" "$OWNTONE_SRC"
	fi
	cd "$OWNTONE_SRC"
	if git apply --reverse --check "$SRC/engine/owntone-$OWNTONE_VERSION.patch" 2>/dev/null; then
		: # already patched
	else
		git checkout --quiet -- .
		run git apply "$SRC/engine/owntone-$OWNTONE_VERSION.patch"
	fi
	run autoreconf -fi
	run ./configure --prefix="$OWNTONE_PREFIX" --disable-chromecast --disable-spotify --disable-mpd
	# The PTP daemon is part of the same tree but needs its own switch
	(cd src/libairptp && run ./configure --prefix="$OWNTONE_PREFIX" --enable-daemon)
	run make -j"$(nproc)"
	run make install
}

mkdir -p "$BUILD_DIR"
: >"$LOG"

step "Checking build dependencies"
check_deps

step "Building the AirPlay engine (OwnTone $OWNTONE_VERSION), this takes a few minutes"
build_engine

step "Building pw-airplayd"
cd "$SRC"
run make
run make install PREFIX="$PREFIX"
install -Dm644 README.md "$PREFIX/share/doc/pw-airplay/README.md"

step "Configuration"
if [ -f "$CONF_DIR/pw-airplay/pw-airplay.conf" ]; then
	echo "    keeping $CONF_DIR/pw-airplay/pw-airplay.conf"
else
	install -Dm644 config/pw-airplay.conf "$CONF_DIR/pw-airplay/pw-airplay.conf"
fi
# PipeWire's own AirPlay 1 sinks would show the same speakers a second time
install -Dm644 config/50-pw-airplay-no-raop.conf "$CONF_DIR/pipewire/pipewire.conf.d/50-pw-airplay-no-raop.conf"

step "systemd user services"
mkdir -p "$UNIT_DIR"
for unit in pw-airplay-engine.service pw-airplay.service; do
	sed -e "s|@PREFIX@|$PREFIX|g" -e "s|@ENGINE@|$OWNTONE_PREFIX/sbin/owntone|g" \
		"systemd/$unit" >"$UNIT_DIR/$unit"
done
systemctl --user daemon-reload
systemctl --user enable --quiet pw-airplay-engine.service pw-airplay.service
if systemctl --user is-active --quiet pw-airplay.service; then
	systemctl --user restart pw-airplay-engine.service pw-airplay.service
	restarted=1
fi

cat <<EOF

Installed. Remaining steps:
EOF
if ! systemctl is-active --quiet airptpd.service 2>/dev/null; then
	echo "  - once, as root (PTP clock for multi-room sync, firewall ports):"
	echo "      sudo $SRC/install-system.sh"
fi
if [ -z "${restarted:-}" ]; then
	echo "  - restart PipeWire, then start pw-airplay:"
	echo "      systemctl --user restart pipewire pipewire-pulse wireplumber"
	echo "      systemctl --user start pw-airplay"
fi
echo "Your AirPlay speakers then show up as audio outputs."
