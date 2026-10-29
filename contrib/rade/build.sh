#!/usr/bin/env bash
#
# build.sh - cross-build FreeDV RADE (rade_c) for the X6100 (Allwinner R16, Cortex-A7
# NEON) with zig: static musl binaries rade_tx_wav / rade_rx_wav and a benchmark script,
# to measure whether the R16 runs RADE in real time
#
# Usage: build.sh
#
# Runs as a normal user from anywhere; works in <repo>/out/rade-work, output in
# <repo>/out/rade. Needs zig, git, make, autoconf, automake, libtool and curl or wget:
# Opus (pinned by rade_c) downloads its neural network weights from media.xiph.org and
# checks their sha256. A second run reuses the sources and the Opus build.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set -euo pipefail

readonly C_HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly C_REPO="$(cd -- "${C_HERE}/../.." && pwd)"
readonly C_WORK="${C_REPO}/out/rade-work"
readonly C_OUT="${C_REPO}/out/rade"
readonly C_RADE_URL="https://github.com/freedv/rade_c.git"
readonly C_RADE_REV="c8a3dc156045cae2cd251e1a4be0c304c9ddf2f9"	# 2026-10-28, RADE V1 + V2 (V2 pre-release)
readonly C_OPUS_URL="https://github.com/xiph/opus.git"
readonly C_TARGET="arm-linux-musleabihf"
readonly C_CPU="cortex_a7"

# librade (src/CMakeLists.txt of rade_c at C_RADE_REV)
readonly C_RADE_SRC=(rade_api.c rade_enc.c rade_dec.c rade_enc_data.c rade_dec_data.c
	rade_dsp.c rade_ofdm.c rade_bpf.c rade_acq.c rade_tx.c rade_rx.c
	rade_enc_v2.c rade_dec_v2.c rade_sync.c rade_enc_v2_data.c rade_dec_v2_data.c rade_sync_data.c
	rade_v2_ofdm.c rade_tx_v2.c rade_rx_v2.c)
readonly C_TOOLS=(rade_tx_wav rade_rx_wav)

G_ZIG="${ZIG:-zig}"

#
# Check the host tools
#

tools_check() {
	local L_T

	for L_T in "${G_ZIG}" git make autoreconf libtoolize; do
		if ! command -v -- "${L_T}" > /dev/null 2>&1; then
			printf 'error: %s not found (zypper install zig git make autoconf automake libtool)\n' "${L_T}" >&2
			exit 1
		fi
	done
}

#
# A git checkout at a fixed commit (shallow): <dir> <url> <commit>
#

checkout() {
	local L_DIR="${1}"
	local L_URL="${2}"
	local L_REV="${3}"

	if [[ "$(git -C "${L_DIR}" rev-parse HEAD 2>/dev/null || true)" == "${L_REV}" ]]; then	# not there yet: empty
		return 0
	fi

	mkdir -p -- "${L_DIR}"
	git -C "${L_DIR}" init -q
	git -C "${L_DIR}" remote remove origin 2> /dev/null || true	# a fresh repository has none
	git -C "${L_DIR}" remote add origin "${L_URL}"
	git -C "${L_DIR}" fetch -q --depth 1 origin "${L_REV}"
	git -C "${L_DIR}" checkout -q -f FETCH_HEAD
}

#
# Opus at the commit rade_c pins, with rade_c's nnet patches, its model weights and a
# static build for the target
#

opus_build() {
	local L_SRC="${C_WORK}/rade_c"
	local L_OPUS="${C_WORK}/opus"
	local L_REV
	local L_HASH

	L_REV="$(sed -n 's#.*xiph/opus/archive/\([0-9a-f]\{40\}\)\.zip.*#\1#p' "${L_SRC}/cmake/BuildOpus.cmake")"

	if [[ -z "${L_REV}" ]]; then
		printf '%s\n' "error: no Opus commit in rade_c's cmake/BuildOpus.cmake" >&2
		exit 1
	fi

	if [[ -f "${L_OPUS}/.libs/libopus.a" && "$(cat -- "${L_OPUS}/.x6100-built" 2>/dev/null || true)" == "${L_REV}" ]]; then
		printf 'opus %s: built\n' "${L_REV:0:7}"
		return 0
	fi

	checkout "${L_OPUS}" "${C_OPUS_URL}" "${L_REV}"

	(
		cd -- "${L_OPUS}"

		if ! grep -q RADE_EXPORT dnn/nnet.h; then
			patch -s dnn/nnet.h < "${L_SRC}/src/opus-nnet.h.diff"
			patch -s dnn/nnet.c < "${L_SRC}/src/opus-nnet.c.diff"
		fi

		# The weights: as autogen.sh does, but not again once unpacked
		if [[ ! -f dnn/fargan_data.c ]]; then
			L_HASH="$(sed -n 's#^dnn/download_model.sh "\([0-9a-f]*\)".*#\1#p' autogen.sh)"
			dnn/download_model.sh "${L_HASH}"
		fi

		autoreconf -isf > /dev/null
		./configure -q --host="${C_TARGET}" --disable-shared --disable-doc --disable-extra-programs \
			--enable-osce --enable-dred \
			CC="${G_ZIG} cc -target ${C_TARGET} -mcpu=${C_CPU}" AR="${G_ZIG} ar" RANLIB="${G_ZIG} ranlib" \
			CFLAGS="-O2 -DNDEBUG"
		make -s -j"$(nproc)"
		printf '%s\n' "${L_REV}" > .x6100-built
	)

	printf 'opus %s: built\n' "${L_REV:0:7}"
}

#
# librade objects and the WAV tools, linked statically with libopus
#

rade_build() {
	local L_SRC="${C_WORK}/rade_c/src"
	local L_OPUS="${C_WORK}/opus"
	local L_OBJ="${C_WORK}/obj"
	local L_FLAGS=(-target "${C_TARGET}" -mcpu="${C_CPU}" -O3 -DNDEBUG -std=gnu11
		-I"${L_SRC}" -I"${L_OPUS}/dnn" -I"${L_OPUS}/celt" -I"${L_OPUS}/include" -I"${L_OPUS}")
	local L_OBJS=()
	local L_F
	local L_T

	mkdir -p -- "${L_OBJ}" "${C_OUT}"

	for L_F in "${C_RADE_SRC[@]}"; do
		"${G_ZIG}" cc "${L_FLAGS[@]}" -DIS_BUILDING_RADE_API=1 -c "${L_SRC}/${L_F}" -o "${L_OBJ}/${L_F%.c}.o"
		L_OBJS+=("${L_OBJ}/${L_F%.c}.o")
	done

	for L_T in "${C_TOOLS[@]}"; do
		"${G_ZIG}" cc "${L_FLAGS[@]}" -static -s -o "${C_OUT}/${L_T}" "${L_SRC}/${L_T}.c" "${L_OBJS[@]}" \
			"${L_OPUS}/.libs/libopus.a" -lm
	done

	# FARGAN alone (features to speech), for the benchmark's split of the decode time
	"${G_ZIG}" cc "${L_FLAGS[@]}" -static -s -o "${C_OUT}/lpcnet_demo" "${L_SRC}/lpcnet_demo.c" \
		"${L_OPUS}/.libs/libopus.a" -lm

	# The receiver in two threads (demodulator + decoder, vocoder): contrib/rade/rade_rx_mt.c
	"${G_ZIG}" cc "${L_FLAGS[@]}" -static -s -pthread -o "${C_OUT}/rade_rx_mt" "${C_HERE}/rade_rx_mt.c" \
		"${L_OBJS[@]}" "${L_OPUS}/.libs/libopus.a" -lm

	# The patch against the original function on the radio: same result, time per call
	# (contrib/rade/test/acq_test.c). The original comes from git, its functions renamed
	git -C "${C_WORK}/rade_c" show HEAD:src/rade_acq.c > "${L_OBJ}/rade_acq_orig.c"
	"${G_ZIG}" cc "${L_FLAGS[@]}" -Drade_acq_init=o_init -Drade_acq_detect_pilots=o_detect \
		-Drade_acq_refine=o_refine -Drade_acq_check_pilots=o_check -c "${L_OBJ}/rade_acq_orig.c" \
		-o "${L_OBJ}/rade_acq_orig.o"
	"${G_ZIG}" cc "${L_FLAGS[@]}" -static -s -o "${C_OUT}/acq_test" "${C_HERE}/test/acq_test.c" \
		"${L_OBJ}/rade_acq_orig.o" "${L_OBJ}/rade_acq.o" -lm
}

#
# Main
#

tools_check
mkdir -p -- "${C_WORK}" "${C_OUT}"

printf 'zig %s, rade_c %s, NEON %s\n' "$("${G_ZIG}" version)" "${C_RADE_REV:0:7}" \
	"$("${G_ZIG}" cc -target "${C_TARGET}" -mcpu="${C_CPU}" -dM -E -x c /dev/null | grep '__ARM_NEON ' > /dev/null && printf yes || printf 'NO (Opus falls back to plain C)')"
checkout "${C_WORK}/rade_c" "${C_RADE_URL}" "${C_RADE_REV}"

# Pilot search on NEON (contrib/rade/rade_acq_neon.patch, README): applied to the original
# rade_acq.c whenever the patch differs from the one applied last
G_PATCH_SUM="$(sha256sum -- "${C_HERE}/rade_acq_neon.patch" | cut -d ' ' -f 1)"

if [[ "$(cat -- "${C_WORK}/rade_c/.x6100-acq-patch" 2>/dev/null || true)" != "${G_PATCH_SUM}" ]]; then	# none yet: empty
	git -C "${C_WORK}/rade_c" checkout -q -- src/rade_acq.c
	patch -s -d "${C_WORK}/rade_c" -p1 < "${C_HERE}/rade_acq_neon.patch"
	printf '%s\n' "${G_PATCH_SUM}" > "${C_WORK}/rade_c/.x6100-acq-patch"
	printf '%s\n' "rade_c: rade_acq_neon.patch applied"
fi

opus_build
rade_build

cp -- "${C_WORK}/rade_c/input_sample.wav" "${C_OUT}/"
cp -- "${C_HERE}/rade_bench.sh" "${C_OUT}/"
chmod 755 -- "${C_OUT}/rade_bench.sh"

printf 'done: %s (%s)\n' "${C_OUT}" "$(ls -- "${C_OUT}" | tr '\n' ' ')"
