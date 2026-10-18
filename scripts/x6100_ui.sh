#!/bin/sh
#
# x6100_ui.sh - stop/start the GUI so libx6100 tools own the baseband
#
# Supports both images:
#   stock Xiegu   x6100_ui_v100, supervised by monit (/etc/monitrc, poll 10 s)
#   R1CBU-based   x6100_gui, respawned by the /usr/sbin/x6100_daemon shell loop
#
# Busybox sh on the radio: POSIX only.
#
# Usage: x6100_ui.sh stop|start|status
#

set -eu

readonly C_DAEMON="/usr/sbin/x6100_daemon"
readonly C_GUI="x6100_gui"
readonly C_STOCK="x6100_ui_v100"
readonly C_STOCK_CTL="/usr/share/support/userapp"
readonly C_MONITRC="/etc/monitrc"

#
# Print the PIDs of shell processes running the respawn loop
#

daemon_pids() {
	ps | grep "${C_DAEMON}" | grep -v grep | awk '{ print $1 }'
}

#
# Stop the GUI and whatever restarts it
#

ui_stop() {
	if [ -x "${C_DAEMON}" ]; then
		L_PIDS="$(daemon_pids)"

		if [ -n "${L_PIDS}" ]; then
			kill ${L_PIDS}
			printf 'x6100_daemon stopped (%s)\n' "${L_PIDS}"
		fi

		if pidof "${C_GUI}" >/dev/null 2>&1; then
			killall "${C_GUI}"
			sleep 1
			killall -KILL "${C_GUI}" 2>/dev/null || true	# already gone after SIGTERM is fine
			printf '%s stopped\n' "${C_GUI}"
		fi
	elif [ -x "${C_STOCK_CTL}" ]; then
		if pidof monit >/dev/null 2>&1; then
			kill $(pidof monit)
			printf '%s\n' 'monit stopped'
		fi

		"${C_STOCK_CTL}" stop
	else
		printf '%s\n' 'error: no known GUI launcher found' >&2
		exit 1
	fi
}

#
# Start the GUI through its normal supervisor, detached from this shell
#

ui_start() {
	if [ -x "${C_DAEMON}" ]; then
		if [ -z "$(daemon_pids)" ]; then
			setsid "${C_DAEMON}" </dev/null >/dev/null 2>&1 &
			printf '%s\n' 'x6100_daemon started'
		fi
	elif [ -x "${C_STOCK_CTL}" ]; then
		"${C_STOCK_CTL}" start

		if ! pidof monit >/dev/null 2>&1; then
			monit -c "${C_MONITRC}"
			printf '%s\n' 'monit started'
		fi
	else
		printf '%s\n' 'error: no known GUI launcher found' >&2
		exit 1
	fi
}

ui_status() {
	L_DAEMON="$(daemon_pids)"

	printf 'x6100_daemon:  %s\n' "${L_DAEMON:-not running}"
	printf '%-14s %s\n' "${C_GUI}:" "$(pidof "${C_GUI}" || printf '%s' 'not running')"
	printf '%-14s %s\n' "${C_STOCK}:" "$(pidof "${C_STOCK}" || printf '%s' 'not running')"
	printf '%-14s %s\n' 'monit:' "$(pidof monit || printf '%s' 'not running')"
}

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' 'error: must run as root' >&2
	exit 1
fi

case "${1:-}" in
	stop)
		ui_stop
		;;
	start)
		ui_start
		;;
	status)
		ui_status
		;;
	*)
		printf 'usage: %s stop|start|status\n' "$0" >&2
		exit 1
		;;
esac
