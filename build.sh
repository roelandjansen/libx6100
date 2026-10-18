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
readonly C_CPU="cortex_a7"
readonly C_OUT="out"
readonly C_TOOLS=(x6100_info x6100_rodump x6100_tune)

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
	"${G_ZIG}" ar rcs "${C_OUT}/libx6100.a" "${C_OUT}/x6100.o"
	printf 'built %s\n' "${C_OUT}/libx6100.a"
}

#
# Build tools, statically linked and stripped
#

build_tools() {
	local L_TOOL

	for L_TOOL in "${C_TOOLS[@]}"; do
		zcc -static -s -o "${C_OUT}/${L_TOOL}" "tools/${L_TOOL}.c" "${C_OUT}/libx6100.a"
		printf 'built %s\n' "${C_OUT}/${L_TOOL}"
	done
}

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
check_zig
mkdir -p -- "${C_OUT}"

#
# Private zig caches inside the build dir: avoids stale or foreign global cache state
#

export ZIG_GLOBAL_CACHE_DIR="${PWD}/${C_OUT}/zig-cache-global"
export ZIG_LOCAL_CACHE_DIR="${PWD}/${C_OUT}/zig-cache"
printf 'zig %s\n' "$("${G_ZIG}" version)"

build_lib
build_tools
