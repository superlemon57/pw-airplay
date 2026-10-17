#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Checks what a running pw-airplay installation exposes on the network and on
# disk. Makes no sound: it only sends requests that must be refused.
set -u

PORT="${PORT:-3689}"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/pw-airplay"
LAN_IP="$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')"
failed=0

pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; failed=1; }

# HTTP status of a request to the engine through the LAN address, as another
# machine on the network would reach it
status() {
	curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$@"
}

[ -n "$LAN_IP" ] || { echo "no LAN address found"; exit 1; }
curl -s --max-time 3 "http://127.0.0.1:$PORT/api/player" >/dev/null ||
	{ echo "engine not running on port $PORT"; exit 1; }

code=$(status "http://$LAN_IP:$PORT/api/outputs")
[ "$code" = 403 ] && pass "JSON API refused from the network ($code)" || fail "JSON API from the network: $code"

code=$(status -X PUT "http://$LAN_IP:$PORT/api/player/stop")
[ "$code" = 403 ] && pass "player control refused from the network ($code)" || fail "player control from the network: $code"

code=$(status "http://$LAN_IP:$PORT/")
case "$code" in 401|403) pass "web interface refused from the network ($code)" ;; *) fail "web interface from the network: $code" ;; esac

code=$(status "http://$LAN_IP:$PORT/ctrl-int/1/playpause")
[ "$code" = 403 ] && pass "DACP without a speaker id refused ($code)" || fail "DACP without a speaker id: $code"

code=$(status -H 'Active-Remote: 12345' "http://$LAN_IP:$PORT/ctrl-int/1/playpause")
[ "$code" = 403 ] && pass "DACP with an unknown speaker id refused ($code)" || fail "DACP with an unknown speaker id: $code"

code=$(status "http://$LAN_IP:$PORT/databases")
case "$code" in 401|403) pass "DAAP library refused ($code)" ;; *) fail "DAAP library: $code" ;; esac

code=$(status "http://127.0.0.1:$PORT/api/outputs")
[ "$code" = 200 ] && pass "JSON API available locally" || fail "JSON API locally: $code"

# mDNS: only the DACP service speakers need, no library or remote adverts
host=$(hostname)
adverts=$(timeout 5 avahi-browse -aptr 2>/dev/null | awk -F';' -v h="$host.local" '$1 == "=" && $7 == h {print $5}' | sort -u)
extra=$(printf '%s\n' "$adverts" | grep -vxE '_dacp\._tcp|' | grep -vE '^_(ssh|sftp-ssh|workstation|device-info|smb|kdeconnect)' | tr '\n' ' ')
[ -z "$extra" ] && pass "mDNS: only DACP advertised by pw-airplay" || fail "mDNS advertises: $extra"

# Files: pipes and state private to the user
for path in "$DATA" "$DATA/pipes" "$DATA/control"; do
	mode=$(stat -c %a "$path" 2>/dev/null)
	[ "$mode" = 700 ] && pass "$path is private ($mode)" || fail "$path mode $mode"
done
for fifo in "$DATA/pipes/pw-airplay" "$DATA/pipes/pw-airplay.metadata" "$DATA/control/remote"; do
	mode=$(stat -c %a "$fifo" 2>/dev/null)
	[ "$mode" = 600 ] && pass "$fifo is private ($mode)" || fail "$fifo mode $mode"
done
mode=$(stat -c %a "$DATA/owntone.conf" 2>/dev/null)
[ "$mode" = 600 ] && pass "engine config (holds a password) is private ($mode)" || fail "engine config mode $mode"

# Listening sockets, for the record
echo
echo "Listening sockets:"
ss -Hltunp 2>/dev/null | grep -E 'owntone|airptpd' | awk '{print "  " $1, $5}' | sort -u

exit $failed
