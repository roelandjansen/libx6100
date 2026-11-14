#!/usr/bin/env bash
set -euo pipefail

#
# install.sh - put x6100_trx and its files on an X6100 running OpenX6X00 (run on the PC)
#
# Usage: ./install.sh [root@]<address of the radio>
#        or change C_DEFAULT_HOST below and run ./install.sh
#
# Streams payload/ to the radio as one tar over ssh and runs the install step there in
# the same session: one connection. OpenX6X00 takes root with a key only (no password):
# add yours first on http://<radio>/ssh (README.md). Existing settings
# in /etc/x6100 are kept; a remote.conf (passwords) is never installed. Needs ssh and
# GNU tar on this PC; nothing here needs elevated rights on the PC.
#

readonly C_DEFAULT_HOST="root@192.168.1.100"	# change to your radio, or give it as the argument
readonly C_POST="/root/x6100_dist/post_install.sh"

G_HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
G_HOST="${1:-${C_DEFAULT_HOST}}"
G_LIST=""

#
# Print the usage and leave
#

usage() {
	printf '%s\n' "usage: $0 [root@]<address of the radio>" >&2
	printf '%s\n' "       (default ${C_DEFAULT_HOST}: change C_DEFAULT_HOST in this script)" >&2
	exit 1
}

#
# Remove the file list on the way out
#

cleanup() {
	if [[ -n "${G_LIST}" ]]; then
		rm -f -- "${G_LIST}"
	fi
}

if [[ "${G_HOST}" == "-h" || "${G_HOST}" == "--help" ]]; then
	usage
fi

if [[ "${G_HOST}" != *@* ]]; then
	G_HOST="root@${G_HOST}"
fi

if [[ ! -d "${G_HERE}/payload" ]]; then
	printf '%s\n' "error: ${G_HERE}/payload missing: unpack the whole archive" >&2
	exit 1
fi

for G_TOOL in ssh tar; do
	if ! command -v -- "${G_TOOL}" > /dev/null 2>&1; then
		printf '%s\n' "error: ${G_TOOL} not found on this PC" >&2
		exit 1
	fi
done

trap cleanup EXIT
G_LIST="$(mktemp)"

# Files and links only: directory entries would set the modes of /root, /etc and /opt
# on the radio from this PC; the radio's tar makes missing parents itself
(cd -- "${G_HERE}/payload" && find . -type f -o -type l) > "${G_LIST}"

printf '%s\n' "installing $(wc -l < "${G_LIST}") files on ${G_HOST}"

if ! tar -C "${G_HERE}/payload" --owner=0 --group=0 --numeric-owner -cf - -T "${G_LIST}" \
	| ssh -C -- "${G_HOST}" "tar -xf - -C / && sh ${C_POST}"; then
	printf '%s\n' 'error: the install did not finish.' \
		'  "Permission denied (publickey)": OpenX6X00 takes root with a key only. Make one' \
		'  (ssh-keygen -t ed25519), paste ~/.ssh/id_ed25519.pub on http://<radio>/ssh, then' \
		'  run this again (README.md, "First: your ssh key on the radio")' >&2
	exit 1
fi

printf '%s\n' 'done'
