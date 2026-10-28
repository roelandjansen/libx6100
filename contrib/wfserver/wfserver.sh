#!/bin/sh
#
# wfserver.sh - start wfserver from its bundle (/opt/wfserver) with the bundle's own
# libraries and glibc loader, so nothing of the image's libraries is used
#
# Usage: wfserver.sh [-s /etc/x6100/wfserver.ini] [--setup] [wfserver options]
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

exec "${G_DIR}/lib/ld-linux-armhf.so.3" --library-path "${G_DIR}/lib" "${G_DIR}/bin/wfserver" "$@"
