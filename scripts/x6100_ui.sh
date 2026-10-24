#!/bin/sh
#
# x6100_ui.sh - stop/start the GUI so libx6100 tools own the baseband
#
# Supports these images:
#   stock Xiegu   x6100_ui_v100, supervised by monit (/etc/monitrc, poll 10 s)
#   R1CBU-based   x6100_gui, respawned by the /usr/sbin/x6100_daemon shell loop
#   IU1WXY        /usr/sbin/x6100-iu1wxy-gui, started from an init script
#
# After stopping, checks that nothing holds the display (/dev/dri): x6100_trx
# needs to be DRM master, else "display not available (Permission denied)".
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
readonly C_IU1WXY="/usr/sbin/x6100-iu1wxy-gui"
readonly C_GUIS="x6100_gui x6100-iu1wxy-gui x6100_ui_v100"

#
# Print the PIDs of shell processes running the respawn loop
#

daemon_pids() {
	ps | grep "${C_DAEMON}" | grep -v grep | awk '{ print $1 }'
}

#
# Print "pid name" for every process holding the display
#

dri_holders() {
	for L_P in /proc/[0-9]*; do
		if ls -l "${L_P}/fd" 2>/dev/null | grep -q '/dev/dri'; then
			printf '%s %s\n' "${L_P#/proc/}" "$(cat "${L_P}/comm" 2>/dev/null)"
		fi
	done
}

#
# Print the init script that starts the IU1WXY GUI, if any
#

iu1wxy_init() {
	grep -l 'iu1wxy-gui' /etc/init.d/* 2>/dev/null | head -n 1 || true	# none: start the binary
}

#
# Stop one GUI process by name: TERM, then KILL after a second
#

gui_kill() {
	if pidof "$1" >/dev/null 2>&1; then
		killall "$1"
		sleep 1
		killall -KILL "$1" 2>/dev/null || true	# already gone after SIGTERM is fine
		printf '%s stopped\n' "$1"
	fi
}

#
# Stop the GUI and whatever restarts it
#

ui_stop() {
	L_PIDS="$(daemon_pids)"

	if [ -n "${L_PIDS}" ]; then
		kill ${L_PIDS}
		printf 'x6100_daemon stopped (%s)\n' "${L_PIDS}"
	fi

	if pidof monit >/dev/null 2>&1; then
		kill $(pidof monit)
		printf '%s\n' 'monit stopped'
	fi

	if [ -x "${C_STOCK_CTL}" ] && pidof "${C_STOCK}" >/dev/null 2>&1; then
		"${C_STOCK_CTL}" stop
	fi

	for L_GUI in ${C_GUIS}; do
		gui_kill "${L_GUI}"
	done

	#
	# Anything still on the display (a respawn, or a GUI we do not know)
	#
	sleep 2
	L_HOLD="$(dri_holders)"

	if [ -n "${L_HOLD}" ]; then
		printf 'warning: display still held by: %s\n' "${L_HOLD}" >&2
		printf '%s\n' '(kill it, or x6100_trx runs console only)' >&2
		exit 1
	fi

	printf '%s\n' 'display free'
}

#
# Start the GUI through its normal supervisor, detached from this shell
#

ui_start() {
	if [ -x "${C_IU1WXY}" ]; then
		L_INIT="$(iu1wxy_init)"

		if pidof x6100-iu1wxy-gui >/dev/null 2>&1; then
			printf '%s\n' 'x6100-iu1wxy-gui already running'
		elif [ -n "${L_INIT}" ]; then
			"${L_INIT}" start
			printf 'x6100-iu1wxy-gui started (%s)\n' "${L_INIT}"
		else
			setsid "${C_IU1WXY}" </dev/null >/dev/null 2>&1 &
			printf '%s\n' 'x6100-iu1wxy-gui started'
		fi
	elif [ -x "${C_DAEMON}" ]; then
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

	printf 'x6100_daemon:     %s\n' "${L_DAEMON:-not running}"

	for L_GUI in ${C_GUIS}; do
		printf '%-17s %s\n' "${L_GUI}:" "$(pidof "${L_GUI}" || printf '%s' 'not running')"
	done

	printf '%-17s %s\n' 'monit:' "$(pidof monit || printf '%s' 'not running')"
	printf '%-17s %s\n' 'display held by:' "$(dri_holders | tr '\n' ' ')"
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
