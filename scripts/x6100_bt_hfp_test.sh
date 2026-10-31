#!/usr/bin/env bash
#
# x6100_bt_hfp_test.sh - can a Bluetooth headset microphone reach Linux on the X6100?
#
# The IU1WXY image runs Bluetooth audio as A2DP only (btaudio-ctl, btaudio.pa:
# headset=ofono without ofono): per IU1WXY, HFP / HSP (SCO) gives no audio on this
# radio and drops the WiFi. This script checks it for ourselves: it stops the
# btaudio-ctl monitor (it forces every headset back to A2DP), switches pulseaudio's
# Bluetooth module to its native headset backend, reconnects the headset, puts it
# on the headset / handsfree profile and records its microphone. Then it puts
# everything back as the image has it. Everything goes to the log too.
#
# WiFi may drop while SCO runs: run it on the USB console (DEV USB-C, first port,
# 115200), or detached:
#
#   nohup ./x6100_bt_hfp_test.sh 70:26:05:C0:19:7B >/dev/null 2>&1 &
#
# then speak into the headset for the record time, log in again, read the log.
#
#   x6100_bt_hfp_test.sh MAC [seconds]     record seconds (default 15)
#
# A reboot also restores the image's Bluetooth audio, should anything be left over.
#

set -euo pipefail

readonly C_LOG="/tmp/x6100_bt_hfp.log"
readonly C_WAV="/tmp/x6100_bt_hfp.wav"
readonly C_MON_PID="/tmp/btaudio-monitor.pid"
readonly C_BTAUDIO="/usr/sbin/btaudio-ctl"

export PULSE_SERVER="${PULSE_SERVER:-unix:/var/run/pulse/native}"

G_MAC=""
G_CARD=""
G_SECS=15
G_PROFILE=""
G_SRC=""
G_MON_STOPPED=0

#
# Print a step header
#

step() {
	printf '\n== %s\n' "$1"
}

#
# Unload every module-bluetooth-discover instance
#

discover_unload() {
	local L_ID

	while IFS= read -r L_ID; do
		pactl unload-module "${L_ID}" || true	# already gone: nothing to do
	done < <(pactl list short modules | awk '$2 == "module-bluetooth-discover" {print $1}')
}

#
# Put the image's Bluetooth audio back: A2DP, the ofono setting, the btaudio monitor
#

restore() {
	step 'restore'

	if [[ -n "${G_CARD}" ]]; then
		pactl set-card-profile "${G_CARD}" a2dp_sink || printf '%s\n' 'restore: a2dp_sink not set (reconnect the headset)'
	fi

	discover_unload
	pactl load-module module-bluetooth-discover headset=ofono >/dev/null || printf '%s\n' 'restore: discover module not loaded: reboot'

	if [[ "${G_MON_STOPPED}" -eq 1 && -x "${C_BTAUDIO}" ]]; then
		setsid "${C_BTAUDIO}" monitor </dev/null >/dev/null 2>&1 &
		printf '%s\n' 'restore: btaudio-ctl monitor started again'
	fi

	if ! ip link show wlan0 2>/dev/null | grep -q 'state UP'; then
		ip link set wlan0 up || true	# NetworkManager reconnects
		printf '%s\n' 'restore: wlan0 set up again'
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

G_MAC="$1"
G_SECS="${2:-15}"
G_CARD="bluez_card.${G_MAC//:/_}"

exec > >(tee "${C_LOG}") 2>&1
trap restore EXIT

step 'versions and modules'
pulseaudio --version || true	# informational
pactl list short modules | grep -i bluetooth || true	# none: shown as nothing
ip -4 addr show wlan0 | grep inet || printf '%s\n' 'wlan0: no IPv4 address'

step 'btaudio monitor'
monitor_stop

step 'pulseaudio: native headset backend'
discover_unload
pactl load-module module-bluetooth-discover headset=native
sleep 1

step 'reconnect the headset'
bluetoothctl disconnect "${G_MAC}" || true	# not connected: fine
sleep 3
bluetoothctl connect "${G_MAC}"
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
pactl set-card-profile "${G_CARD}" "${G_PROFILE}"
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
