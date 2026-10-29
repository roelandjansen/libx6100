#!/bin/sh
#
# rade_bench.sh - time FreeDV RADE on the radio: encode (speech -> modem audio) and
# decode (modem audio -> speech) of a speech file, against its length
#
# Usage: rade_bench.sh [speech.wav]    (16 kHz 16-bit mono; default input_sample.wav)
#
# Real-time factor (RTF) = processing time / audio length, on one core (rade_c is single
# threaded). Below 1.0 keeps up; for a live radio with x6100_trx and wfserver running
# beside it, below about 0.5 leaves room. Run it once with x6100_trx stopped and once
# with it running.
#
# Busybox sh on the radio: POSIX only. Works in /tmp (RAM), not on the SD card.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set -eu

G_DIR=$(dirname "$(readlink -f "$0")")
G_IN="${1:-${G_DIR}/input_sample.wav}"
G_TMP=$(mktemp -d /tmp/rade_bench.XXXXXX)

trap 'rm -f "${G_TMP}"/*.wav "${G_TMP}"/*.f32 "${G_TMP}"/*.pcm; rmdir "${G_TMP}"' EXIT

#
# Centiseconds since boot (/proc/uptime: no need for date +%N)
#

now_cs() {
	L_UP=$(cut -d ' ' -f 1 /proc/uptime)
	printf '%s\n' "$(( ${L_UP%.*} * 100 + 1${L_UP#*.} - 100 ))"
}

#
# Audio length of a 16-bit mono WAV in centiseconds: <file> <rate>
#

wav_cs() {
	L_BYTES=$(wc -c < "$1")
	printf '%s\n' "$(( (L_BYTES - 44) * 100 / ($2 * 2) ))"
}

#
# 10 s of white noise as an 8 kHz 16-bit mono WAV: header (data 160000 bytes), then random
#

noise_wav() {
	printf 'RIFF\044\161\002\000WAVEfmt \020\000\000\000\001\000\001\000\100\037\000\000\200\076\000\000\002\000\020\000data\000\161\002\000' > "$1"
	head -c 160000 /dev/urandom >> "$1"
}

#
# Run one step and print its time and real-time factor: <name> <audio cs> <command...>
#

step() {
	L_NAME="$1"
	L_LEN="$2"
	shift 2

	L_T0=$(now_cs)

	if ! "$@" > "${G_TMP}/log" 2>&1; then
		printf '%-12s failed:\n' "${L_NAME}"
		cat "${G_TMP}/log"
		rm -f "${G_TMP}/log"
		return 1
	fi

	L_DT=$(( $(now_cs) - L_T0 ))
	rm -f "${G_TMP}/log"
	printf '%-12s %3d.%02d s for %3d.%02d s audio   RTF %d.%02d\n' "${L_NAME}" \
		"$((L_DT / 100))" "$((L_DT % 100))" "$((L_LEN / 100))" "$((L_LEN % 100))" \
		"$((L_DT / L_LEN))" "$((L_DT * 100 / L_LEN % 100))"
}

printf 'cpu: %s x %s, %s MHz; load %s\n' "$(grep -c '^processor' /proc/cpuinfo)" \
	"$(sed -n 's/^CPU part[[:space:]]*: //p' /proc/cpuinfo | head -n 1)" \
	"$(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || printf '0') / 1000 ))" \
	"$(cut -d ' ' -f 1-3 /proc/loadavg)"
G_CF=/sys/devices/system/cpu/cpu0/cpufreq
printf 'cpufreq: governor %s, max %s MHz, steps %s\n' "$(cat "${G_CF}/scaling_governor" 2>/dev/null || printf '-')" \
	"$(( $(cat "${G_CF}/scaling_max_freq" 2>/dev/null || printf '0') / 1000 ))" \
	"$(cat "${G_CF}/scaling_available_frequencies" 2>/dev/null || printf '-')"
printf 'input: %s\n' "${G_IN}"

G_LEN=$(wav_cs "${G_IN}" 16000)

for G_V in v1 v2; do
	G_OPT=""

	if [ "${G_V}" = v2 ]; then
		G_OPT="--v2"
	fi

	step "${G_V} encode" "${G_LEN}" "${G_DIR}/rade_tx_wav" ${G_OPT} "${G_IN}" "${G_TMP}/tx.wav"
	G_TXLEN=$(wav_cs "${G_TMP}/tx.wav" 8000)
	step "${G_V} decode" "${G_TXLEN}" "${G_DIR}/rade_rx_wav" ${G_OPT} -f "${G_TMP}/feat.f32" "${G_TMP}/tx.wav" \
		"${G_TMP}/rx.wav"

	# The vocoder alone on the decoded features: 36 floats per 10 ms frame, the first 5
	# prime it. Decode minus this is the demodulator and the RADE decoder
	if [ -x "${G_DIR}/lpcnet_demo" ]; then
		G_FLEN=$(( $(wc -c < "${G_TMP}/feat.f32") / 144 - 5 ))
		step "${G_V} fargan" "${G_FLEN}" "${G_DIR}/lpcnet_demo" -fargan-synthesis "${G_TMP}/feat.f32" "${G_TMP}/rx.pcm"
	fi

	# The same decode as two threads (contrib/rade/rade_rx_mt.c)
	if [ -x "${G_DIR}/rade_rx_mt" ]; then
		step "${G_V} 2 threads" "${G_TXLEN}" "${G_DIR}/rade_rx_mt" ${G_OPT} "${G_TMP}/tx.wav" "${G_TMP}/rx.wav"
	fi

	# No signal: 10 s of noise, the receiver searching for sync (its idle load on the air)
	noise_wav "${G_TMP}/noise.wav"
	step "${G_V} no signal" 1000 "${G_DIR}/rade_rx_wav" ${G_OPT} "${G_TMP}/noise.wav" "${G_TMP}/rx.wav"
done
