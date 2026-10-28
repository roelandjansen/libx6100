#!/bin/sh
#
# wfserver.sh - start wfserver from its bundle (/opt/wfserver) with the bundle's own
# libraries and glibc loader, so nothing of the image's libraries is used
#
# Usage: wfserver.sh [--setup] [wfserver options]   (settings: /etc/x6100/wfserver.ini unless -s)
#
# POSIX sh: the radio's image may have no bash.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set -eu

G_DIR=$(dirname "$(readlink -f "$0")")

QT_PLUGIN_PATH="${G_DIR}/plugins"
ALSA_CONFIG_DIR="${G_DIR}/share/alsa"
ALSA_CONFIG_PATH="${G_DIR}/share/alsa/alsa.conf"
export QT_PLUGIN_PATH ALSA_CONFIG_DIR ALSA_CONFIG_PATH

#
# No -s / --settings given: /etc/x6100/wfserver.ini, so --setup and the autostart in
# x6100_ui.sh use the same file (wfserver's own default is ~/.config/wfview/wfserver.conf)
#

G_SETTINGS=1

for G_ARG in "$@"; do
	case "${G_ARG}" in
		-s|--settings)
			G_SETTINGS=0
			;;
	esac
done

if [ "${G_SETTINGS}" -eq 1 ]; then
	set -- -s /etc/x6100/wfserver.ini "$@"
fi

exec "${G_DIR}/lib/ld-linux-armhf.so.3" --library-path "${G_DIR}/lib" "${G_DIR}/bin/wfserver" "$@"
