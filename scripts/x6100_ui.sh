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
# wfserver (contrib/wfserver, /opt/wfserver): stop also starts a detached watcher that
# runs wfserver while x6100_trx serves CI-V on /tmp/x6100-civ (cat pty), and restarts it
# when x6100_trx restarts; start stops it. Only when installed, set up (a user in
# /etc/x6100/wfserver.ini) and not switched off (/etc/x6100/wfserver.off).
#
# Busybox sh on the radio: POSIX only.
#
# Usage: x6100_ui.sh stop|start|status
#        x6100_ui.sh wfserver on|off    autostart on (and start now) / off (and stop now)
#

set -eu

readonly C_DAEMON="/usr/sbin/x6100_daemon"
readonly C_GUI="x6100_gui"
readonly C_STOCK="x6100_ui_v100"
readonly C_STOCK_CTL="/usr/share/support/userapp"
readonly C_MONITRC="/etc/monitrc"
readonly C_IU1WXY="/usr/sbin/x6100-iu1wxy-gui"
readonly C_GUIS="x6100_gui x6100-iu1wxy-gui x6100_ui_v100"

readonly C_WFS="/opt/wfserver/wfserver.sh"
readonly C_WFS_INI="/etc/x6100/wfserver.ini"
readonly C_WFS_OFF="/etc/x6100/wfserver.off"
readonly C_WFS_PID="/var/run/x6100_wfserver.pid"
readonly C_WFS_LOG="/tmp/wfserver.log"
readonly C_CIV_LINK="/tmp/x6100-civ"

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
	wfs_start
}

#
# Start the GUI through its normal supervisor, detached from this shell
#

ui_start() {
	wfs_stop	# the GUI takes the baseband: no CI-V pty, and the codec is its own
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

#
# True when a process is alive (an exited child stays a zombie until reaped: not alive)
#

pid_alive() {
	[ -n "$1" ] && [ -d "/proc/$1" ] && ! grep -q '^State:[[:space:]]*Z' "/proc/$1/status" 2>/dev/null
}

#
# Print the PID of the wfserver watcher, if it runs
#

wfs_pid() {
	L_PID="$(cat "${C_WFS_PID}" 2>/dev/null || true)"	# no pid file: not running

	if pid_alive "${L_PID}"; then
		printf '%s\n' "${L_PID}"
	fi
}

#
# Why wfserver does not start automatically; nothing when it does
#

wfs_why_not() {
	if [ ! -x "${C_WFS}" ]; then
		printf '%s\n' 'not installed (build.sh deploy)'
	elif [ -f "${C_WFS_OFF}" ]; then
		printf '%s\n' "off (${0##*/} wfserver on)"
	elif ! grep -q 'Users\\1\\Username=.' "${C_WFS_INI}" 2>/dev/null; then
		printf '%s\n' "no user in ${C_WFS_INI} (${C_WFS} --setup)"
	fi
}

#
# The watcher: runs wfserver while x6100_trx serves CI-V on the pty, restarts it when
# the pty changes (x6100_trx restarted) or wfserver ends
#

wfs_loop() {
	L_WPID=""
	trap 'if pid_alive "${L_WPID}"; then kill "${L_WPID}"; fi; rm -f "${C_WFS_PID}"; exit 0' TERM INT

	while :; do
		L_PTY="$(readlink "${C_CIV_LINK}" 2>/dev/null || true)"	# x6100_trx not on cat pty: wait

		if [ -z "${L_PTY}" ]; then
			sleep 2
			continue
		fi

		"${C_WFS}" -s "${C_WFS_INI}" -l "${C_WFS_LOG}" </dev/null >/dev/null 2>&1 &
		L_WPID=$!

		while pid_alive "${L_WPID}" && [ "$(readlink "${C_CIV_LINK}" 2>/dev/null || true)" = "${L_PTY}" ]; do
			sleep 2
		done

		if pid_alive "${L_WPID}"; then
			kill "${L_WPID}"
		fi

		wait "${L_WPID}" 2>/dev/null || true	# its exit status does not matter: start again
		L_WPID=""
		sleep 2
	done
}

#
# Start the watcher, detached, when wfserver is installed, on and set up
#

wfs_start() {
	L_WHY="$(wfs_why_not)"

	# The wizard offers "auto": that looks for an Icom USB port and never opens the pty
	if [ -z "${L_WHY}" ] && ! grep -q "^1\\\\SerialPortRadio=${C_CIV_LINK}\$" "${C_WFS_INI}"; then
		printf 'wfserver: warning: %s has no 1\\SerialPortRadio=%s: no CI-V, audio only\n' "${C_WFS_INI}" "${C_CIV_LINK}" >&2
	fi

	if [ -n "${L_WHY}" ]; then
		printf 'wfserver: %s\n' "${L_WHY}"
	elif [ -n "$(wfs_pid)" ]; then
		printf '%s\n' 'wfserver: already running'
	else
		setsid sh "$(readlink -f "$0")" wfloop </dev/null >/dev/null 2>&1 &
		printf '%s\n' "wfserver: started, runs while x6100_trx serves ${C_CIV_LINK} (log ${C_WFS_LOG})"
	fi
}

#
# Stop the watcher and its wfserver
#

wfs_stop() {
	L_PID="$(wfs_pid)"

	if [ -n "${L_PID}" ]; then
		kill "${L_PID}"
		printf '%s\n' 'wfserver: stopped'
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

	L_WHY="$(wfs_why_not)"
	L_PID="$(wfs_pid)"

	if [ -n "${L_PID}" ]; then
		printf '%-17s %s\n' 'wfserver:' "watcher ${L_PID}, pty ${C_CIV_LINK} $([ -L "${C_CIV_LINK}" ] && printf '%s' 'up' || printf '%s' 'not there (x6100_trx cat pty?)')"
	else
		printf '%-17s %s\n' 'wfserver:' "not running${L_WHY:+, ${L_WHY}}"
	fi
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
	wfserver)
		case "${2:-}" in
			on)
				rm -f "${C_WFS_OFF}"
				wfs_start
				;;
			off)
				mkdir -p "${C_WFS_OFF%/*}"
				: > "${C_WFS_OFF}"
				wfs_stop
				printf '%s\n' 'wfserver: autostart off'
				;;
			*)
				printf 'usage: %s wfserver on|off\n' "$0" >&2
				exit 1
				;;
		esac
		;;
	wfloop)		# internal: the detached watcher started by wfs_start
		printf '%s\n' "$$" > "${C_WFS_PID}"
		wfs_loop
		;;
	*)
		printf 'usage: %s stop|start|status|wfserver on|off\n' "$0" >&2
		exit 1
		;;
esac
