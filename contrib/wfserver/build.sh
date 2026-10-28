#!/usr/bin/env bash
#
# build.sh - cross-build wfview's wfserver for the X6100 (Allwinner R16, armhf) and
# bundle it, self-contained, for /opt/wfserver on the radio
#
# Usage: build.sh <wfview source tree> [output dir]
#
# The radio's image (IU1WXY) brings no Qt; the bundle carries its own libraries and
# the glibc loader, so it depends on nothing in the image. Libraries come from Ubuntu
# 24.04 armhf (ports.ubuntu.com), unpacked into a sysroot; the compiler is Ubuntu's
# arm-linux-gnueabihf cross g++. Host packages are installed with apt: run as root.
# See README.md next to this script.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
	printf '%s\n' "error: must run as root (apt)" >&2
	exit 1
fi

readonly C_PREFIX=/opt/wfserver
readonly C_TRIPLET=arm-linux-gnueabihf
readonly C_SPEC=linux-armhf-sysroot-g++
readonly C_HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# armhf packages wfserver builds against (their dependencies are added)
readonly C_PKGS=(qtbase5-dev libqt5serialport5-dev qtmultimedia5-dev libqt5xml5t64 libqt5websockets5-dev
	libopus-dev portaudio19-dev librtaudio-dev libpulse-dev libqt5multimedia5-plugins)

#
# Print usage and exit
#

usage() {
	printf '%s\n' "usage: build.sh <wfview source tree> [output dir]" >&2
	exit 1
}

#
# Host tools and the armhf package source (ports.ubuntu.com)
#

host_setup() {
	local L_SRC=/etc/apt/sources.list.d/ubuntu-ports-armhf.sources

	if ! dpkg --print-foreign-architectures | grep -qx armhf; then
		dpkg --add-architecture armhf
	fi

	if [[ ! -f "${L_SRC}" ]]; then
		# The amd64 sources must not be asked for armhf (archive.ubuntu.com has none)
		sed -i 's#^Types: deb$#Types: deb\nArchitectures: amd64#' /etc/apt/sources.list.d/ubuntu.sources
		printf '%s\n' "Types: deb" "URIs: http://ports.ubuntu.com/ubuntu-ports/" \
			"Suites: noble noble-updates noble-security" "Components: main universe restricted multiverse" \
			"Architectures: armhf" "Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg" > "${L_SRC}"
	fi

	apt-get update -q
	apt-get install -y -q --no-install-recommends g++-${C_TRIPLET} qtbase5-dev-tools qt5-qmake qt5-qmake-bin libeigen3-dev
}

#
# Sysroot: the armhf packages and their dependencies unpacked, links made relative
#

sysroot_make() {
	local L_ROOT="${1}"
	local L_DEBS="${2}"
	local L_ARGS=()
	local L_LINK
	local L_TGT
	local L_PKG

	mkdir -p -- "${L_ROOT}" "${L_DEBS}"

	for L_PKG in "${C_PKGS[@]}"; do
		L_ARGS+=("${L_PKG}:armhf")
	done

	(
		cd -- "${L_DEBS}"
		apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces \
			--no-enhances --no-pre-depends "${L_ARGS[@]}" | grep -E '^[a-z0-9]' | grep ':armhf$' | sort -u > pkgs.txt
		xargs apt-get download -q < pkgs.txt
		# Architecture-independent, so not in the armhf list: libasound's own alsa.conf
		apt-get download -q libasound2-data
	)

	for L_PKG in "${L_DEBS}"/*.deb; do
		dpkg -x "${L_PKG}" "${L_ROOT}"
	done

	# Absolute links would point into the host
	while IFS= read -r L_LINK; do
		L_TGT="$(readlink -- "${L_LINK}")"
		ln -sfn -- "$(realpath -m --relative-to="$(dirname -- "${L_LINK}")" -- "${L_ROOT}${L_TGT}")" "${L_LINK}"
	done < <(find "${L_ROOT}" -type l -lname '/*')

	# Merged /usr: glibc's linker script names /lib/..., the base system's links are not here
	mkdir -p -- "${L_ROOT}/lib"
	ln -sfn ../usr/lib/${C_TRIPLET} "${L_ROOT}/lib/${C_TRIPLET}"
	ln -sfn ../usr/lib/ld-linux-armhf.so.3 "${L_ROOT}/lib/ld-linux-armhf.so.3"

	cp -r /usr/include/eigen3 "${L_ROOT}/usr/include/"
}

#
# qmake spec and qt.conf for the cross build against the sysroot
#

qmake_setup() {
	local L_ROOT="${1}"
	local L_CONF="${2}"
	local L_SPECS="${L_ROOT}/usr/lib/${C_TRIPLET}/qt5/mkspecs"
	local L_DIR="${L_SPECS}/${C_SPEC}"
	local L_FLAGS="--sysroot=${L_ROOT} -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"

	mkdir -p -- "${L_DIR}"
	cp -- "${L_SPECS}/linux-arm-gnueabi-g++/qplatformdefs.h" "${L_DIR}/"

	{
		printf '%s\n' "MAKEFILE_GENERATOR = UNIX" "CONFIG += incremental" "QMAKE_INCREMENTAL_STYLE = sublib"
		printf '%s\n' "include(../common/linux.conf)" "include(../common/gcc-base-unix.conf)"
		printf '%s\n' "include(../common/g++-unix.conf)"
		printf 'QMAKE_CC = %s-gcc\n' "${C_TRIPLET}"
		printf 'QMAKE_CXX = %s-g++\n' "${C_TRIPLET}"
		printf 'QMAKE_LINK = %s-g++\n' "${C_TRIPLET}"
		printf 'QMAKE_LINK_SHLIB = %s-g++\n' "${C_TRIPLET}"
		printf 'QMAKE_AR = %s-ar cqs\n' "${C_TRIPLET}"
		printf 'QMAKE_OBJCOPY = %s-objcopy\n' "${C_TRIPLET}"
		printf 'QMAKE_NM = %s-nm -P\n' "${C_TRIPLET}"
		printf 'QMAKE_STRIP = %s-strip\n' "${C_TRIPLET}"
		printf 'QMAKE_CFLAGS += %s\n' "${L_FLAGS}"
		printf 'QMAKE_CXXFLAGS += %s\n' "${L_FLAGS}"
		printf 'QMAKE_LFLAGS += --sysroot=%s -Wl,-rpath-link,%s/usr/lib/%s\n' "${L_ROOT}" "${L_ROOT}" "${C_TRIPLET}"
		printf '%s\n' "load(qt_config)"
	} > "${L_DIR}/qmake.conf"

	printf '%s\n' "[Paths]" "Sysroot=${L_ROOT}" "SysrootifyPrefix=true" "Prefix=/usr" \
		"ArchData=/usr/lib/${C_TRIPLET}/qt5" "Data=/usr/share/qt5" "Headers=/usr/include/${C_TRIPLET}/qt5" \
		"Libraries=/usr/lib/${C_TRIPLET}" "Plugins=/usr/lib/${C_TRIPLET}/qt5/plugins" "HostPrefix=/usr" \
		"HostBinaries=/usr/lib/qt5/bin" "HostData=${L_ROOT}/usr/lib/${C_TRIPLET}/qt5" \
		"HostLibraries=/usr/lib/x86_64-linux-gnu" > "${L_CONF}"
}

#
# The shared libraries a binary needs, found in the sysroot, recursively
#

lib_closure() {
	local L_ROOT="${1}"
	local L_LIB="${2}"
	local L_DIR="${3}"
	local L_NEED
	local L_FILE
	local L_D

	while IFS= read -r L_NEED; do
		[[ -e "${L_DIR}/${L_NEED}" ]] && continue

		L_FILE=""

		for L_D in "${L_ROOT}/usr/lib/${C_TRIPLET}" "${L_ROOT}/lib/${C_TRIPLET}" "${L_ROOT}/usr/lib" \
			"${L_ROOT}/usr/lib/${C_TRIPLET}/pulseaudio"; do
			if [[ -e "${L_D}/${L_NEED}" ]]; then
				L_FILE="${L_D}/${L_NEED}"
				break
			fi
		done

		if [[ -z "${L_FILE}" ]]; then
			printf 'warning: %s not in the sysroot\n' "${L_NEED}" >&2
			continue
		fi

		cp -L -- "${L_FILE}" "${L_DIR}/${L_NEED}"
		lib_closure "${L_ROOT}" "${L_DIR}/${L_NEED}" "${L_DIR}"
	done < <(${C_TRIPLET}-readelf -d "${L_LIB}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
}

#
# The bundle: bin, lib (with the loader), Qt audio plugins, rig files, launcher
#

bundle_make() {
	local L_ROOT="${1}"
	local L_BIN="${2}"
	local L_SRC="${3}"
	local L_OUT="${4}"
	local L_P
	local L_PLUG="${L_ROOT}/usr/lib/${C_TRIPLET}/qt5/plugins"

	rm -rf -- "${L_OUT}"
	mkdir -p -- "${L_OUT}/bin" "${L_OUT}/lib" "${L_OUT}/plugins/audio" "${L_OUT}/share/wfview/rigs" "${L_OUT}/share/alsa"
	cp -- "${L_BIN}" "${L_OUT}/bin/wfserver"
	${C_TRIPLET}-strip "${L_OUT}/bin/wfserver"

	# Qt audio over ALSA only: no PulseAudio daemon runs on the radio. The bundled libasound
	# reads its own configuration (the launcher points it there), not the image's
	cp -- "${L_PLUG}/audio/libqtaudio_alsa.so" "${L_OUT}/plugins/audio/"
	cp -r -- "${L_ROOT}/usr/share/alsa/." "${L_OUT}/share/alsa/"

	lib_closure "${L_ROOT}" "${L_OUT}/bin/wfserver" "${L_OUT}/lib"

	for L_P in "${L_OUT}"/plugins/*/*.so; do
		lib_closure "${L_ROOT}" "${L_P}" "${L_OUT}/lib"
	done

	cp -L -- "${L_ROOT}/usr/lib/ld-linux-armhf.so.3" "${L_OUT}/lib/" 2> /dev/null \
		|| cp -L -- "${L_ROOT}/lib/ld-linux-armhf.so.3" "${L_OUT}/lib/"
	${C_TRIPLET}-strip --strip-unneeded "${L_OUT}"/lib/*.so* "${L_OUT}"/plugins/*/*.so 2> /dev/null || true	# loader and odd files: keep

	cp -- "${L_SRC}"/rigs/*.rig "${L_OUT}/share/wfview/rigs/"
	cp -- "${C_HERE}/../wfview/X6100.rig" "${L_OUT}/share/wfview/rigs/"
	cp -- "${C_HERE}/wfserver.sh" "${C_HERE}/wfserver.ini.example" "${L_OUT}/"
	chmod 755 -- "${L_OUT}/wfserver.sh"
}

#
# Main
#

if [[ $# -lt 1 || ! -f "${1}/wfserver.pro" ]]; then
	usage
fi

G_SRC="$(cd -- "${1}" && pwd)"
G_OUT="${2:-${PWD}/wfserver-armhf}"
G_WORK="${PWD}/wfserver-work"
G_ROOT="${G_WORK}/sysroot"

host_setup

if [[ ! -d "${G_ROOT}/usr/include/${C_TRIPLET}/qt5" ]]; then
	sysroot_make "${G_ROOT}" "${G_WORK}/debs"
fi

qmake_setup "${G_ROOT}" "${G_WORK}/qt.conf"
mkdir -p -- "${G_WORK}/build"

(
	cd -- "${G_WORK}/build"
	/usr/lib/qt5/bin/qmake -qtconf "${G_WORK}/qt.conf" -spec "${G_ROOT}/usr/lib/${C_TRIPLET}/qt5/mkspecs/${C_SPEC}" \
		"${G_SRC}/wfserver.pro" "CONFIG+=release" "PREFIX=${C_PREFIX}" -after "DEFINES-=FTDI_SUPPORT" "QT+=websockets"
	make -j"$(nproc)"
)

bundle_make "${G_ROOT}" "${G_WORK}/build/wfserver" "${G_SRC}" "${G_OUT}"
tar -C "$(dirname -- "${G_OUT}")" -czf "${G_OUT}.tar.gz" "$(basename -- "${G_OUT}")"
printf 'bundle: %s (%s)\n' "${G_OUT}.tar.gz" "$(du -sh -- "${G_OUT}" | cut -f1)"
