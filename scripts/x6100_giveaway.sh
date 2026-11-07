#!/bin/sh
#
# x6100_giveaway.sh - clean a radio's image before it is given to someone else
#
# Removes what is personal or belongs to this one radio: the x6100_trx settings, state,
# birdie list (scanned on this radio), memories, messages and voice recordings, the
# wfserver login, the saved WiFi networks (passwords), the Bluetooth pairings, the ssh
# key that lets you in and the hosts you reached, the shell history. Optional: the ssh host keys (-k) and the
# start logs on /mnt/app_logs (-l). x6100_trx starts with its defaults when its files
# are missing. Kept: x6100_trx itself, x6100_ui.sh, the boot choice (boot.trx), the
# filter widths, the image.
#
# Not touched, check by hand: the root password, the OpenX6X00 GUI's own settings
# (callsign, locator), anything else you put on the radio.
#
# Lists what it will remove and asks first (-y: no question, -n: list only). Make a
# backup before. Busybox sh on the radio: POSIX only (no bash there).
#
# Usage: x6100_giveaway.sh [-n] [-y] [-k] [-l] [-p]
#        -n  list only, remove nothing
#        -y  do not ask
#        -k  also the ssh host keys (new ones at the next boot; check ssh comes back)
#        -l  also the logs in /mnt/app_logs
#        -p  power off at the end
#

set -eu

readonly C_X6100="/etc/x6100"
readonly C_FILES="x6100_trx.state x6100_trx.conf birdies.conf memories.conf messages.conf wfserver.ini"
readonly C_SYSTEM="/etc/NetworkManager/system-connections/* /var/lib/bluetooth/* /root/.ssh/authorized_keys /root/.ssh/known_hosts /root/.ash_history /root/.bash_history"
readonly C_HOSTKEYS="/etc/ssh/ssh_host_* /etc/dropbear/dropbear_*_host_key"
readonly C_LOGS="/mnt/app_logs/*"

G_LIST=0
G_YES=0
G_KEYS=0
G_LOGS=0
G_OFF=0
G_PATS=""
G_COUNT=0

#
# Print the usage and leave
#

usage() {
	printf '%s\n' "usage: $0 [-n] [-y] [-k] [-l] [-p]" >&2
	printf '%s\n' '  -n list only  -y do not ask  -k ssh host keys too  -l /mnt/app_logs too  -p power off' >&2
	exit 1
}

#
# For every existing path matching the patterns in G_PATS: list it, or remove it.
# Glob results stay whole (WiFi connection files are named after the SSID: spaces);
# the patterns themselves have none. Sets G_COUNT.
#

walk() {
	local L_PAT
	local L_P

	G_COUNT=0

	set -f	# the list: split only; each pattern globbed below
	for L_PAT in ${G_PATS}; do
		set +f
		for L_P in ${L_PAT}; do
			if [ ! -e "${L_P}" ] && [ ! -L "${L_P}" ]; then
				continue
			fi

			G_COUNT=$((G_COUNT + 1))

			if [ "$1" = "remove" ]; then
				rm -rf -- "${L_P}"
			else
				printf '  %s\n' "${L_P}"
			fi
		done
		set -f
	done
	set +f
}

#
# Stop x6100_trx first: it writes its state when it ends
#

trx_stop() {
	local L_N=0

	if ! pidof x6100_trx > /dev/null 2>&1; then
		return 0
	fi

	printf '%s\n' 'stopping x6100_trx (it saves its state on exit, so before the removal)'
	: > /tmp/x6100_trx.poweroff	# the boot wrapper (S96x6100_trx): no GUI after it
	killall x6100_trx 2> /dev/null || true

	while pidof x6100_trx > /dev/null 2>&1; do
		L_N=$((L_N + 1))

		if [ "${L_N}" -ge 10 ]; then
			printf '%s\n' 'error: x6100_trx does not end' >&2
			exit 1
		fi

		sleep 1
	done
}

while getopts 'nyklp' G_OPT; do
	case "${G_OPT}" in
		n) G_LIST=1 ;;
		y) G_YES=1 ;;
		k) G_KEYS=1 ;;
		l) G_LOGS=1 ;;
		p) G_OFF=1 ;;
		*) usage ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' "error: must run as root" >&2
	exit 1
fi

#
# What is there to remove
#

for G_F in ${C_FILES}; do
	G_PATS="${G_PATS} ${C_X6100}/${G_F}"
done

G_PATS="${G_PATS} ${C_X6100}/voice ${C_SYSTEM}"

if [ "${G_KEYS}" -eq 1 ]; then
	G_PATS="${G_PATS} ${C_HOSTKEYS}"
fi

if [ "${G_LOGS}" -eq 1 ]; then
	G_PATS="${G_PATS} ${C_LOGS}"
fi

printf '%s\n' 'to remove:'
walk list

if [ "${G_COUNT}" -eq 0 ]; then
	printf '%s\n' '  nothing: already clean'
	exit 0
fi

printf '%s\n' 'not touched: the root password, the OpenX6X00 GUI settings (check those by hand)'

if [ "${G_LIST}" -eq 1 ]; then
	exit 0
fi

if [ "${G_YES}" -eq 0 ]; then
	printf '%s' 'remove these? type yes: '
	read -r G_ANSWER || G_ANSWER=""

	if [ "${G_ANSWER}" != "yes" ]; then
		printf '%s\n' 'nothing removed'
		exit 1
	fi
fi

trx_stop
walk remove
sync
printf '%s\n' 'removed; x6100_trx starts with its defaults next time'

if [ "${G_OFF}" -eq 1 ]; then
	printf '%s\n' 'powering off'
	poweroff
else
	printf '%s\n' 'power off when ready (poweroff); with the WiFi gone, ssh ends at the next boot'
fi
