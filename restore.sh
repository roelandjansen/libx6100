#!/usr/bin/env bash
set -euo pipefail

#
# libx6100: put back the tags v0.5.0-v0.10.0 that the mirror push removed from
# GitHub (same commits, same messages), add v0.11.0, and push those seven tags.
# Run it inside a clone of roelandjansen/libx6100; it fetches master first.
#

readonly C_REMOTE='origin'
readonly C_V011='c3e536f383a085aed7bcaeb4e65fdc9b85272aab'

G_MSG=''

#
# Remove the message file on exit
#

cleanup() {
	if [[ -n "${G_MSG}" ]]; then
		rm -f -- "${G_MSG}"
	fi
}

#
# Tag a commit: annotated from G_MSG when the third argument is "a", else lightweight;
# a tag that already exists locally is left alone
#

make_tag() {
	local L_TAG="$1"
	local L_COMMIT="$2"
	local L_KIND="${3:-}"

	if git rev-parse -q --verify "refs/tags/${L_TAG}" > /dev/null; then
		printf '%s\n' "skip ${L_TAG}: already a local tag"
		return 0
	fi

	if [[ "${L_KIND}" == "a" ]]; then
		git tag -a -F "${G_MSG}" "${L_TAG}" "${L_COMMIT}"
	else
		git tag "${L_TAG}" "${L_COMMIT}"
	fi

	printf '%s\n' "tagged ${L_TAG} on ${L_COMMIT:0:7}"
}

trap cleanup EXIT
G_MSG="$(mktemp)"

if ! git remote get-url "${C_REMOTE}" | grep -q 'libx6100'; then
	printf '%s\n' "error: run this inside a libx6100 clone" >&2
	exit 1
fi

git fetch "${C_REMOTE}" master

if ! git cat-file -e "${C_V011}^{commit}"; then
	printf '%s\n' "error: ${C_V011:0:7} not here after the fetch" >&2
	exit 1
fi

make_tag v0.5.0 2a3258182053a4aab027ccb6608b392ee55ed326
make_tag v0.6.0 12ef92d360165bf289253a298d87f5620d61a84b
make_tag v0.7.0 cdfd411395fba49e5a0122d7b8d810011f2d4f89

cat > "${G_MSG}" <<'EOF'
x6100_trx front end on the IU1WXY image: CI-V as an IC-705, settings files, GEN menu, VFO A / B

- CI-V engine rewritten to the IC-705 manual; --cat on ttyS2 (19200), scope data,
  band stacking registers, meters; tested with wfview
- Scope: F1 scroll / FIX family, F2 span, audio scope (F3), split RF / AF view
  (F3 long), reference levels (F2 long)
- NR / NB / manual notch with DSP panel, filter panel, tune panel with L / C / S
- VFO A / B (A/B key, long A=B), shared with CI-V
- Settings: x6100_trx.conf (yours) and x6100_trx.state (last state); build.sh
  backup / restore / deploy seeding, WiFi profiles included
- GEN: brightness, CAT, NTP, WiFi (NetworkManager), Bluetooth; status page
- Regions r1 / r2 / r3 / nl for the TX check; oob / nolimit with OOB marker
- PWR: display off, quit, power off; UTC ISO 8601 clock; OVF detection
- Quick reference, TODO, README photo and renders
EOF
make_tag v0.8.0 263a41a96e65464dbc00261780dc956de9cf4620 a

cat > "${G_MSG}" <<'EOF'
x6100_trx: split, RIT / XIT, memories, tuner memories, CW keyer, liquid-dsp

- Split (V/M long) with RX / TX lines on the scope; PTT key and hand mic transmit on
  the TX side; RIT / XIT panel (AGC long), one offset as on Icom; CI-V 0F, 21
- 60 m channels (FST CH, MFK picks the channel); console shows RX / TX
- Memory channels 1-99 (V/M panel, memories.conf, memr list, example set, in backup)
- Tuner memories per frequency: nearest within 0.5 % of the TX frequency
- CW keyer: KEY short key type, KEY long panel (speed, iambic, pitch, sidetone,
  break-in, ratio); CI-V 14 09 / 0C / 0F
- Battery charge and external supply: info line colour, status page, low battery notes
- Console: own line editing after the status line, cut to the terminal width, TX and
  battery readings folded in
- liquid-dsp v1.8.3 as a submodule, static build with cmake and zig, x6100_liquid check
- STATUS.md: where we are, open radio checks, next steps
EOF
make_tag v0.9.0 33b74aa65edc3da6f8b33ec328f76ae4c67367af a

cat > "${G_MSG}" <<'EOF'
x6100_trx: repeater duplex, TX frequency display, keyer fixes, state saved after tune
EOF
make_tag v0.10.0 40d00ac5f2db541edb2fee09c687fe0a76833df5 a

cat > "${G_MSG}" <<'EOF'
x6100_trx: TX panel, birdies, CAT findings, CI-V on a pseudo-terminal

- TX panel (F4 long): mic select and gains, compressor, VOX (gain, anti-VOX, delay),
  monitor, squelch; state, console, CI-V 14 03 / 0B / 0E / 15-17 and 16 44-46
- Birdies: receive-only scan per band or all bands (GEN BIRDIE SCAN, birdiescan),
  tuner out during the scan, birdies.conf (in backup), yellow ticks on the scope,
  automatic notch in USB / LSB / AM (off until checked); own design, idea from the
  IU1WXY release notes
- CAT: ttyS2 is R_UART; the kernel never writes its divisor, x6100_trx now does;
  9600 / 19200 work, 38400 and up do not: 19200 is the limit of the DEV port, the
  GEN CAT row offers 4800-19200
- CAT on a pseudo-terminal (cat pty, --cat=pty): /tmp/x6100-civ, any speed, for a
  client on the radio (wfserver, rigctld, socat to TCP)
- Docs: quickref, CI-V notes, birdie reference scans (80 m IU1WXY, 20 m comb), netaudio
  design (wfserver option reopened), STATUS / TODO
EOF
make_tag v0.11.0 "${C_V011}" a

git push "${C_REMOTE}" v0.5.0 v0.6.0 v0.7.0 v0.8.0 v0.9.0 v0.10.0 v0.11.0
printf '%s\n' "pushed v0.5.0-v0.11.0"
