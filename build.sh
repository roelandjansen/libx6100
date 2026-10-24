#!/usr/bin/env bash
set -euo pipefail

#
# Cross-compile libx6100 and tools on openSUSE Tumbleweed
#
# Target: Allwinner R16 (Cortex-A7, ARMv7 hard-float).
# Uses zig as C compiler driver with its bundled musl: fully static binaries,
# independent of the radio's glibc. Install: zypper install zig
#

readonly C_TARGET="arm-linux-musleabihf"
readonly C_TARGET_GLIBC="arm-linux-gnueabihf.2.23"
readonly C_CPU="cortex_a7"
readonly C_OUT="out"
readonly C_TOOLS=(x6100_audio x6100_info x6100_input x6100_iqcal x6100_lcdtest x6100_rodump x6100_spk x6100_trx x6100_tune x6100_tx)

G_ZIG="${ZIG:-zig}"
G_CFLAGS=(-std=c11 -D_GNU_SOURCE -Wall -Wextra -O2 -Iinclude)

#
# Check that zig is available
#

check_zig() {
	if ! command -v -- "${G_ZIG}" >/dev/null 2>&1; then
		printf 'error: %s not found; install with: zypper install zig\n' "${G_ZIG}" >&2
		exit 1
	fi
}

#
# Compile wrapper for the ARM target
#

zcc() {
	"${G_ZIG}" cc -target "${C_TARGET}" -mcpu="${C_CPU}" "${G_CFLAGS[@]}" "$@"
}

#
# Build static library
#

build_lib() {
	zcc -c -o "${C_OUT}/x6100.o" src/x6100.c
	zcc -c -o "${C_OUT}/x6100_lcd.o" src/x6100_lcd.c
	zcc -c -o "${C_OUT}/x6100_font_freq.o" src/x6100_font_freq.c
	zcc -c -o "${C_OUT}/x6100_pcm.o" src/x6100_pcm.c
	zcc -c -o "${C_OUT}/x6100_civ.o" src/x6100_civ.c
	"${G_ZIG}" ar rcs "${C_OUT}/libx6100.a" "${C_OUT}/x6100.o" "${C_OUT}/x6100_lcd.o" "${C_OUT}/x6100_font_freq.o" \
		"${C_OUT}/x6100_pcm.o" "${C_OUT}/x6100_civ.o"
	printf 'built %s\n' "${C_OUT}/libx6100.a"
}

#
# Build tools, statically linked and stripped
#

#
# Build the LD_PRELOAD register sniffer against the radio's glibc (not musl)
#

build_sniff() {
	"${G_ZIG}" cc -target "${C_TARGET_GLIBC}" -mcpu="${C_CPU}" "${G_CFLAGS[@]}" -shared -fPIC -s \
		-o "${C_OUT}/x6100_i2csniff.so" tools/x6100_i2csniff.c -ldl -lpthread
	printf 'built %s\n' "${C_OUT}/x6100_i2csniff.so"
}

build_tools() {
	local L_TOOL

	for L_TOOL in "${C_TOOLS[@]}"; do
		zcc -static -s -o "${C_OUT}/${L_TOOL}" "tools/${L_TOOL}.c" "${C_OUT}/libx6100.a"
		printf 'built %s\n' "${C_OUT}/${L_TOOL}"
	done

	#
	# x6100_rx: the x6100_trx source built receive-only
	#

	zcc -static -s -DX6100_RX_ONLY -o "${C_OUT}/x6100_rx" tools/x6100_trx.c "${C_OUT}/libx6100.a"
	printf 'built %s\n' "${C_OUT}/x6100_rx"
}

#
# Print a fingerprint of everything that goes into the build: the tracked (and
# edited) sources, this script and the zig version
#

src_fingerprint() {
	{
		git ls-files -z -- src include tools build.sh | xargs -0 sha256sum
		"${G_ZIG}" version
	} | sha256sum | cut -d ' ' -f 1
}

#
# Fast-forward to the remote branch; refuse to merge or rebase on our own
#

update_tree() {
	if ! git -C . rev-parse --git-dir >/dev/null 2>&1; then
		printf '%s\n' 'not a git checkout: no pull'
		return 0
	fi

	if ! git pull --ff-only; then
		printf '%s\n' 'error: git pull --ff-only failed (local commits or conflicts?)' >&2
		exit 1
	fi
}

#
# Copy the binaries and the quick reference to the radio
#

deploy() {
	local L_FILE

	for L_FILE in "${C_DEPLOY[@]}"; do
		if [[ ! -f "${L_FILE}" ]]; then
			printf 'error: %s missing\n' "${L_FILE}" >&2
			exit 1
		fi
	done

	scp -- "${C_DEPLOY[@]}" "${G_HOST}:/root/"
	printf 'deployed to %s:/root: %s\n' "${G_HOST}" "${C_DEPLOY[*]}"
}

#
# Usage
#

usage() {
	printf '%s\n' \
		'usage: build.sh [options] [deploy]' \
		'' \
		'  (none)       git pull, then build only if a source changed' \
		'  deploy       the same, then scp x6100_trx, x6100_ui.sh and the quick reference' \
		'               to $X6100_HOST:/root (default root@x6100)' \
		'  -f, --force  build even when nothing changed' \
		'  --no-pull    skip git pull' \
		'  -h, --help   this text'
}

readonly C_STAMP="${C_OUT}/.fingerprint"
readonly C_DEPLOY=("${C_OUT}/x6100_trx" scripts/x6100_ui.sh docs/x6100_trx_quickref.md)

G_HOST="${X6100_HOST:-root@x6100}"
G_FORCE=0
G_PULL=1
G_DEPLOY=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		-f|--force)
			G_FORCE=1
			;;
		--no-pull)
			G_PULL=0
			;;
		deploy)
			G_DEPLOY=1
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			usage >&2
			exit 1
			;;
	esac

	shift
done

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
check_zig

if [[ "${G_PULL}" -eq 1 ]]; then
	update_tree
fi

mkdir -p -- "${C_OUT}"

#
# Private zig caches inside the build dir: avoids stale or foreign global cache state
#

export ZIG_GLOBAL_CACHE_DIR="${PWD}/${C_OUT}/zig-cache-global"
export ZIG_LOCAL_CACHE_DIR="${PWD}/${C_OUT}/zig-cache"

G_PRINT="$(src_fingerprint)"

if [[ "${G_FORCE}" -eq 0 && -f "${C_STAMP}" && "$(cat -- "${C_STAMP}")" == "${G_PRINT}" ]]; then
	printf '%s\n' 'sources unchanged: no build (use -f to force)'
else
	printf 'zig %s\n' "$("${G_ZIG}" version)"
	build_lib
	build_tools
	build_sniff
	printf '%s\n' "${G_PRINT}" > "${C_STAMP}"
fi

if [[ "${G_DEPLOY}" -eq 1 ]]; then
	deploy
fi
