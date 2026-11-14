#!/bin/sh
#
# post_install.sh - the install step on the radio, run by install.sh right after the
# files are unpacked (also by hand: sh /root/x6100_dist/post_install.sh)
#
# Owner and modes of what was installed, /etc/x6100 with the examples where a file is
# missing (existing settings are kept), wfserver.ini when wfserver came along and has
# none. Never a remote.conf (passwords) and never the boot choice: both stay yours.
# Busybox sh on the radio: POSIX only.
#

set -eu

readonly C_DIST="/root/x6100_dist"
readonly C_SET="/etc/x6100"
readonly C_EXEC="/root/x6100_trx /root/x6100_ui.sh /root/x6100_giveaway.sh /etc/init.d/S96x6100_trx ${C_DIST}/post_install.sh"
readonly C_WFS="/opt/wfserver"

#
# Copy an example into /etc/x6100 when that file is not there yet
#

seed() {
	if [ -f "${C_SET}/$2" ]; then
		printf '  %s kept\n' "${C_SET}/$2"
	elif [ -f "$1" ]; then
		cp -- "$1" "${C_SET}/$2"
		printf '  %s from the example\n' "${C_SET}/$2"
	fi
}

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' "error: must run as root" >&2
	exit 1
fi

for G_F in ${C_EXEC}; do
	if [ -f "${G_F}" ]; then
		chown 0:0 "${G_F}"
		chmod 755 "${G_F}"
	fi
done

chown -R 0:0 "${C_DIST}" /root/x6100_trx_quickref.md 2> /dev/null || true	# not all there: fine

if [ -d "${C_WFS}" ]; then
	chown -R 0:0 "${C_WFS}"
fi

printf '%s\n' 'settings:'
mkdir -p "${C_SET}"
seed "${C_DIST}/x6100_trx.conf.example" x6100_trx.conf
seed "${C_DIST}/x6100_memories.conf.example" memories.conf
seed "${C_DIST}/x6100_messages.conf.example" messages.conf

if [ -x "${C_WFS}/wfserver.sh" ]; then
	seed "${C_WFS}/wfserver.ini.example" wfserver.ini
fi

printf '%s\n' \
	'' \
	'installed. Next, on the radio:' \
	'  /root/x6100_ui.sh stop          the GUI off (wfserver starts with x6100_trx when set up)' \
	'  /root/x6100_trx                 start it (PWR or Ctrl-C to quit)' \
	'  /root/x6100_ui.sh boot trx      x6100_trx at every boot instead of the GUI (boot gui: back)' \
	'  remote rigs: cp /root/x6100_dist/x6100_remote.conf.example /etc/x6100/remote.conf,' \
	'               edit it, chmod 600 /etc/x6100/remote.conf' \
	'  keys and console: /root/x6100_trx_quickref.md'

if [ -x "${C_WFS}/wfserver.sh" ] && ! grep -q 'Users\\1\\Username=.' "${C_SET}/wfserver.ini" 2> /dev/null; then
	printf '%s\n' '  wfserver: /opt/wfserver/wfserver.sh --setup (users, the CI-V port /tmp/x6100-civ)'
fi
