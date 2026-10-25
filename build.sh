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
G_CFLAGS+=("-DX6100_REV=\"$(git -C "$(dirname -- "${BASH_SOURCE[0]}")" rev-parse --short HEAD 2>/dev/null || printf '%s' dev)\"")	# shown on the GEN status page

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
# Build liquid-dsp (third_party/liquid-dsp, git submodule, MIT) as a static library
# for the radio with its own CMake, zig as the compiler: portable C, no SIMD probes
# (they would try to run ARM code here), no FFTW; out/liquid/lib/libliquid.a and
# out/liquid/include/liquid/liquid.h. Needs cmake (zypper install cmake); without
# it this step is skipped with a warning, the rest of the build does not use it yet.
# -Wno-format: liquid-dsp prints time_t with %ld (64-bit time_t on 32-bit musl);
# harmless there, and we do not patch its source.
#

build_liquid() {
	local L_SRC="third_party/liquid-dsp"
	local L_WRAP="${PWD}/${C_OUT}/zig-wrap"
	local L_BUILD="${C_OUT}/liquid-build"
	local L_TOOL

	if ! command -v cmake >/dev/null 2>&1; then
		printf '%s\n' 'warning: cmake not found, liquid-dsp skipped (zypper install cmake)' >&2
		return 0
	fi

	if [[ ! -f "${L_SRC}/include/liquid.h" ]]; then
		git submodule update --init --depth 1 -- "${L_SRC}"
	fi

	#
	# cmake wants one program per tool: small wrappers around zig
	#

	mkdir -p -- "${L_WRAP}"

	for L_TOOL in cc ar ranlib; do
		if [[ "${L_TOOL}" == cc ]]; then
			printf '#!/usr/bin/env bash\nexec "%s" cc -target %s -mcpu=%s "$@"\n' "${G_ZIG}" "${C_TARGET}" "${C_CPU}" \
				> "${L_WRAP}/zig-${L_TOOL}"
		else
			printf '#!/usr/bin/env bash\nexec "%s" %s "$@"\n' "${G_ZIG}" "${L_TOOL}" > "${L_WRAP}/zig-${L_TOOL}"
		fi

		chmod +x -- "${L_WRAP}/zig-${L_TOOL}"
	done

	cmake -S "${L_SRC}" -B "${L_BUILD}" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
		-DCMAKE_C_COMPILER="${L_WRAP}/zig-cc" -DCMAKE_CXX_COMPILER="${L_WRAP}/zig-cc" \
		-DCMAKE_AR="${L_WRAP}/zig-ar" -DCMAKE_RANLIB="${L_WRAP}/zig-ranlib" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS=-Wno-format \
		-DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_AUTOTESTS=OFF \
		-DBUILD_BENCHMARKS=OFF -DENABLE_SIMD=OFF -DFIND_SIMD=OFF -DFIND_FFTW=OFF -DENABLE_COLOR=OFF \
		-DENABLE_TIMESTAMPS=OFF > "${C_OUT}/liquid-cmake.log"
	cmake --build "${L_BUILD}" -j "$(nproc)" >> "${C_OUT}/liquid-cmake.log"

	mkdir -p -- "${C_OUT}/liquid/lib" "${C_OUT}/liquid/include/liquid"
	cp -- "${L_BUILD}/libliquid.a" "${C_OUT}/liquid/lib/"
	cp -- "${L_SRC}/include/liquid.h" "${C_OUT}/liquid/include/liquid/"
	printf 'built %s (liquid-dsp %s)\n' "${C_OUT}/liquid/lib/libliquid.a" "$(git -C "${L_SRC}" describe --tags 2>/dev/null || printf '%s' '?')"

	#
	# Link check: a tiny tool against it (also runs on the radio)
	#

	zcc -static -s -I"${C_OUT}/liquid/include" -o "${C_OUT}/x6100_liquid" tools/x6100_liquid.c \
		"${C_OUT}/liquid/lib/libliquid.a" -lm
	printf 'built %s\n' "${C_OUT}/x6100_liquid"
}

#
# Print a fingerprint of everything that goes into the build: the tracked (and
# edited) sources, this script and the zig version
#

src_fingerprint() {
	{
		git ls-files -z -- src include tools build.sh | xargs -0 sha256sum
		git submodule status 2>/dev/null || true	# none yet: nothing to add
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

	# Submodules (third_party/liquid-dsp) follow the commit recorded here
	git submodule update --init --depth 1
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

	# Built only with cmake present: copied when there
	if [[ -f "${C_OUT}/x6100_liquid" ]]; then
		scp -q -- "${C_OUT}/x6100_liquid" "${G_HOST}:/root/"
		printf 'deployed %s\n' "${C_OUT}/x6100_liquid"
	fi

	settings_seed
}

#
# Settings on the radio (/etc/x6100) and their local copy ($X6100_CONFIG): a new SD
# card image wipes the radio, so backup keeps them here and deploy puts them back
#

settings_on_radio() {
	ssh -- "${G_HOST}" "cd ${C_RADIO_SETTINGS} 2>/dev/null && ls -1 ${C_SETTINGS[*]} 2>/dev/null" || true	# none yet: empty
}

settings_seed() {
	local L_HAVE
	local L_FILE

	L_HAVE="$(settings_on_radio)"
	ssh -- "${G_HOST}" "mkdir -p ${C_RADIO_SETTINGS}"

	for L_FILE in "${C_SETTINGS[@]}"; do
		if grep -qx -- "${L_FILE}" <<< "${L_HAVE}"; then
			continue
		fi

		if [[ -f "${G_CONFIG}/${L_FILE}" ]]; then
			scp -q -- "${G_CONFIG}/${L_FILE}" "${G_HOST}:${C_RADIO_SETTINGS}/${L_FILE}"
			printf 'restored %s from %s\n' "${L_FILE}" "${G_CONFIG}"
		elif [[ "${L_FILE}" == memories.conf ]]; then
			scp -q -- docs/x6100_memories.conf.example "${G_HOST}:${C_RADIO_SETTINGS}/${L_FILE}"
			printf 'installed %s from the example\n' "${L_FILE}"
		elif [[ "${L_FILE}" == x6100_trx.conf ]]; then
			scp -q -- docs/x6100_trx.conf.example "${G_HOST}:${C_RADIO_SETTINGS}/${L_FILE}"
			printf 'installed %s from the example\n' "${L_FILE}"
		fi
	done
}

settings_backup() {
	local L_FILE

	mkdir -p -- "${G_CONFIG}"

	for L_FILE in $(settings_on_radio); do
		scp -q -- "${G_HOST}:${C_RADIO_SETTINGS}/${L_FILE}" "${G_CONFIG}/${L_FILE}"
		printf 'saved %s\n' "${G_CONFIG}/${L_FILE}"
	done

	# Saved WiFi profiles (NetworkManager keyfiles, with their passwords: kept 0600)
	mkdir -p -- "${G_CONFIG}/${C_NM_DIR##*/}"
	chmod 700 -- "${G_CONFIG}/${C_NM_DIR##*/}"

	if scp -q -p -- "${G_HOST}:${C_NM_DIR}/*.nmconnection" "${G_CONFIG}/${C_NM_DIR##*/}/"; then
		printf 'saved WiFi profiles in %s\n' "${G_CONFIG}/${C_NM_DIR##*/}"
	else
		printf '%s\n' 'no WiFi profiles on the radio'
	fi
}

settings_restore() {
	local L_FILE

	ssh -- "${G_HOST}" "mkdir -p ${C_RADIO_SETTINGS}"

	for L_FILE in "${C_SETTINGS[@]}"; do
		if [[ -f "${G_CONFIG}/${L_FILE}" ]]; then
			scp -q -- "${G_CONFIG}/${L_FILE}" "${G_HOST}:${C_RADIO_SETTINGS}/${L_FILE}"
			printf 'restored %s\n' "${L_FILE}"
		fi
	done

	# WiFi profiles: NetworkManager reads keyfiles at start, so restart it
	if compgen -G "${G_CONFIG}/${C_NM_DIR##*/}/*.nmconnection" > /dev/null; then
		scp -q -- "${G_CONFIG}/${C_NM_DIR##*/}/"*.nmconnection "${G_HOST}:${C_NM_DIR}/"
		ssh -- "${G_HOST}" "chmod 600 ${C_NM_DIR}/*.nmconnection && /etc/init.d/S45network-manager restart"
		printf '%s\n' 'restored WiFi profiles, NetworkManager restarted'
	fi
}

#
# Usage
#

usage() {
	printf '%s\n' \
		'usage: build.sh [options] [deploy]' \
		'       build.sh backup | restore' \
		'' \
		'  (none)       git pull, then build only if a source changed' \
		'  deploy       the same, then scp x6100_trx, x6100_ui.sh, the quick reference and' \
		'               x6100_liquid (when built)' \
		'               to $X6100_HOST:/root (default root@x6100)' \
		'               and settings missing on the radio from $X6100_CONFIG (or the example)' \
		'  backup       copy the radio settings (/etc/x6100) to $X6100_CONFIG' \
		'               (default ~/.config/x6100); no build' \
		'  restore      copy $X6100_CONFIG to the radio, overwriting; no build' \
		'  -f, --force  build even when nothing changed' \
		'  --no-pull    skip git pull' \
		'  -h, --help   this text'
}

readonly C_STAMP="${C_OUT}/.fingerprint"
readonly C_DEPLOY=("${C_OUT}/x6100_trx" scripts/x6100_ui.sh docs/x6100_trx_quickref.md)
readonly C_RADIO_SETTINGS="/etc/x6100"
readonly C_SETTINGS=(x6100_trx.conf x6100_trx.state filters.conf memories.conf)
readonly C_NM_DIR="/etc/NetworkManager/system-connections"

G_HOST="${X6100_HOST:-root@x6100}"
G_CONFIG="${X6100_CONFIG:-${HOME}/.config/x6100}"
G_SETTINGS=""
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
		backup|restore)
			G_SETTINGS="$1"
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

if [[ "${G_SETTINGS}" == backup ]]; then
	settings_backup
	exit 0
elif [[ "${G_SETTINGS}" == restore ]]; then
	settings_restore
	exit 0
fi

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
	build_liquid
	printf '%s\n' "${G_PRINT}" > "${C_STAMP}"
fi

if [[ "${G_DEPLOY}" -eq 1 ]]; then
	deploy
fi
