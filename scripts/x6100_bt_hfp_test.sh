#!/usr/bin/env bash
#
# x6100_bt_hfp_test.sh - can a Bluetooth headset microphone reach Linux on the X6100?
#
# The IU1WXY image runs Bluetooth audio as A2DP only (btaudio-ctl, btaudio.pa:
# headset=ofono without ofono): per IU1WXY, HFP / HSP (SCO) gives no audio on this
# radio (RTL8723BU on the MUSB host: SCO needs isochronous USB) and drops the WiFi.
# This script checks it for ourselves.
#
# Loading module-bluetooth-discover again while pulseaudio runs aborts pulseaudio
# 16.1 (bluez5-util.c assertion, 2026-10-31), so the native headset backend is set
# at start instead: the image's pulseaudio is stopped (its init script) and a copy
# started from /tmp with the same configuration, headset=native in place of ofono.
# Nothing in /etc changes. Then the headset is reconnected, put on the headset /
# handsfree profile and its microphone recorded. At the end the copy is stopped and
# the image's pulseaudio and btaudio-ctl monitor started again; a reboot does the
# same. Everything goes to the log too.
#
# WiFi may drop while SCO runs: run it on the USB console (DEV USB-C, first port,
# 115200), or detached:
#
#   nohup ./x6100_bt_hfp_test.sh 70:26:05:C0:19:7B >/dev/null 2>&1 &
#
# then speak into the headset from about 20 s on for the record time, log in again,
# read the log.
#
#   x6100_bt_hfp_test.sh MAC [seconds]     record seconds (default 15)
#

set -euo pipefail

readonly C_LOG="/tmp/x6100_bt_hfp.log"
readonly C_WAV="/tmp/x6100_bt_hfp.wav"
readonly C_MON_PID="/tmp/btaudio-monitor.pid"
readonly C_BTAUDIO="/usr/sbin/btaudio-ctl"
readonly C_PA_INIT="/etc/init.d/S50pulseaudio"
readonly C_PA_ETC="/etc/pulse"

export PULSE_SERVER="${PULSE_SERVER:-unix:/var/run/pulse/native}"

G_MAC=""
G_CARD=""
G_SECS=15
G_PROFILE=""
G_SRC=""
G_MON_STOPPED=0
G_PA_OURS=0
G_DIR=""

#
# Print a step header
#

step() {
	printf '\n== %s\n' "$1"
}

#
# Wait until pulseaudio answers (up to 10 s); false when it does not
#

pa_wait() {
	local L_N=0

	while ! pactl info >/dev/null 2>&1; do
		if [[ "${L_N}" -ge 10 ]]; then
			return 1
		fi

		sleep 1
		L_N=$((L_N + 1))
	done
}

#
# Stop whatever pulseaudio runs: the init script, then any copy left
#

pa_stop() {
	local L_PIDS

	"${C_PA_INIT}" stop || true	# not running: nothing to stop
	sleep 1
	L_PIDS="$(pidof pulseaudio || true)"	# none left: empty

	if [[ -n "${L_PIDS}" ]]; then
		# shellcheck disable=SC2086	# one PID per word
		kill ${L_PIDS} || true	# gone meanwhile: fine
		sleep 1
	fi
}

#
# The image's configuration, copied to G_DIR, with headset=native in btaudio.pa
#

pa_config() {
	mkdir -p "${G_DIR}/system.pa.d"
	cp -- "${C_PA_ETC}/system.pa.d/"* "${G_DIR}/system.pa.d/"
	sed -i 's/headset=ofono/headset=native/' "${G_DIR}/system.pa.d/btaudio.pa"
	sed "s|${C_PA_ETC}/system.pa.d|${G_DIR}/system.pa.d|" "${C_PA_ETC}/system.pa" > "${G_DIR}/system.pa"

	if ! grep -q "${G_DIR}/system.pa.d" "${G_DIR}/system.pa"; then
		printf '.include %s\n' "${G_DIR}/system.pa.d" >> "${G_DIR}/system.pa"
	fi

	grep -h 'bluetooth-discover' "${G_DIR}/system.pa" "${G_DIR}/system.pa.d/"* || true	# shown for the log
}

#
# Put the image's Bluetooth audio back: its own pulseaudio, the btaudio monitor
#

restore() {
	step 'restore'

	if [[ "${G_PA_OURS}" -eq 1 ]]; then
		pa_stop
		"${C_PA_INIT}" start || printf '%s\n' 'restore: pulseaudio not started: reboot'

		if pa_wait; then
			printf '%s\n' 'restore: the image pulseaudio runs again'
		else
			printf '%s\n' 'restore: pulseaudio does not answer: reboot'
		fi
	fi

	if [[ "${G_MON_STOPPED}" -eq 1 && -x "${C_BTAUDIO}" ]]; then
		setsid "${C_BTAUDIO}" monitor </dev/null >/dev/null 2>&1 &
		printf '%s\n' 'restore: btaudio-ctl monitor started again'
	fi

	if ! ip -4 addr show wlan0 | grep -q inet; then
		ip link set wlan0 up || true	# NetworkManager reconnects
		printf '%s\n' 'restore: wlan0 had no address, set up again'
	fi

	bluetoothctl connect "${G_MAC}" >/dev/null 2>&1 || printf '%s\n' 'restore: headset not connected again'

	if [[ -n "${G_DIR}" ]]; then
		rm -rf -- "${G_DIR}"
	fi

	printf '%s\n' "log: ${C_LOG}, recording: ${C_WAV}"
}

#
# Stop the btaudio-ctl monitor: it puts every headset card back on A2DP
#

monitor_stop() {
	local L_PID

	L_PID="$(cat "${C_MON_PID}" 2>/dev/null || true)"	# no pid file: no monitor

	if [[ -n "${L_PID}" ]] && kill "${L_PID}" 2>/dev/null; then
		G_MON_STOPPED=1
		printf 'btaudio-ctl monitor %s stopped\n' "${L_PID}"
	else
		printf '%s\n' 'no btaudio-ctl monitor running'
	fi
}

#
# Peak and RMS of the recording (16-bit samples after the 44-byte WAV header)
#

wav_stats() {
	local L_BYTES

	if [[ ! -f "${C_WAV}" ]]; then
		printf '%s\n' 'recording: no file (parecord missing or the source refused)'
		return 0
	fi

	L_BYTES="$(wc -c < "${C_WAV}")"
	printf 'recording: %s bytes\n' "${L_BYTES}"

	if [[ "${L_BYTES}" -le 44 ]]; then
		printf '%s\n' 'recording: empty (no audio from the headset)'
		return 0
	fi

	tail -c +45 "${C_WAV}" | od -An -v -td2 | awk '
		{ for (i = 1; i <= NF; i++) { v = $i; n++; s += v * v; if (v < 0) v = -v; if (v > p) p = v } }
		END {
			if (n == 0) { print "recording: no samples"; exit }
			r = sqrt(s / n)
			printf "samples: %d  peak: %d  rms: %.0f  (%.1f / %.1f dBFS)\n", n, p, r,
				p > 0 ? 20 * log(p / 32768) / log(10) : -99, r > 0 ? 20 * log(r / 32768) / log(10) : -99
		}'
}

if [[ "${EUID}" -ne 0 ]]; then
	printf '%s\n' "error: must run as root" >&2
	exit 1
fi

if [[ $# -lt 1 || ! "$1" =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]]; then
	printf 'usage: %s MAC [seconds]\n' "$0" >&2
	exit 1
fi

if [[ ! -x "${C_PA_INIT}" ]]; then
	printf 'error: %s not found: not the IU1WXY image?\n' "${C_PA_INIT}" >&2
	exit 1
fi

G_MAC="$1"
G_SECS="${2:-15}"
G_CARD="bluez_card.${G_MAC//:/_}"
G_DIR="$(mktemp -d /tmp/x6100_pa.XXXXXX)"

exec > >(tee "${C_LOG}") 2>&1
trap restore EXIT

step 'versions and modules'
pulseaudio --version || true	# informational
pactl list short modules | grep -i bluetooth || true	# none: shown as nothing
ip -4 addr show wlan0 | grep inet || printf '%s\n' 'wlan0: no IPv4 address'

step 'btaudio monitor'
monitor_stop

step 'pulseaudio with the native headset backend'
pa_config
pa_stop
G_PA_OURS=1
pulseaudio --system --daemonize=yes --disallow-exit --exit-idle-time=-1 -n -F "${G_DIR}/system.pa"

if ! pa_wait; then
	printf '%s\n' 'our pulseaudio does not answer'
	grep -i pulseaudio /var/log/messages | tail -n 10 || true	# why, if it says
	exit 0
fi

pactl list short modules | grep -i bluetooth || true	# the headset= setting in use

step 'reconnect the headset'
bluetoothctl disconnect "${G_MAC}" || true	# not connected: fine
sleep 3
bluetoothctl connect "${G_MAC}" || true	# shown in the log; the profiles below tell
sleep 6

step 'card profiles'
pactl list cards | awk -v c="${G_CARD}" '
	$1 == "Name:" { on = ($2 == c) }
	on && /^[[:space:]]+(a2dp|headset|handsfree|off)[a-z_]*:/ { print }
	on && /Active Profile:/ { print }'

for P in handsfree_head_unit headset_head_unit; do
	if pactl list cards | awk -v c="${G_CARD}" -v p="${P}:" '$1 == "Name:" { on = ($2 == c) } on && $1 == p { f = 1 } END { exit !f }'; then
		G_PROFILE="${P}"
		break
	fi
done

if [[ -z "${G_PROFILE}" ]]; then
	printf '%s\n' 'no headset / handsfree profile on the card: pulseaudio offers no HFP / HSP gateway here'
	exit 0
fi

step "profile ${G_PROFILE}"
pactl set-card-profile "${G_CARD}" "${G_PROFILE}" || true	# refused: the log says why
sleep 3
hcitool con || true	# SCO / eSCO link listed when the audio channel is up
pactl list short sources | grep -i bluez || printf '%s\n' 'no bluez source'

G_SRC="bluez_source.${G_MAC//:/_}.${G_PROFILE}"

step "recording ${G_SECS} s from ${G_SRC}: speak into the headset"
rm -f "${C_WAV}"
timeout "${G_SECS}" parecord -d "${G_SRC}" --file-format=wav "${C_WAV}" || true	# timeout ends it
wav_stats

step 'WiFi after SCO'
ip -4 addr show wlan0 | grep inet || printf '%s\n' 'wlan0: no IPv4 address (WiFi dropped)'
dmesg | tail -n 15
grep -i -E 'pulseaudio|sco' /var/log/messages | tail -n 10 || true	# none: shown as nothing
