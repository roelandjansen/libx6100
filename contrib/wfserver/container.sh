#!/usr/bin/env bash
#
# container.sh - run build.sh (the wfserver cross-build) in an Ubuntu 24.04 container,
# for hosts that are not Ubuntu (openSUSE with zig for the rest of libx6100)
#
# Usage: container.sh <wfview source tree>
#
# Runs as a normal user. Engine: podman (rootless, no daemon; zypper install podman),
# else docker; $X6100_ENGINE picks one. The first run makes a local image with the cross
# tools (x6100-wfserver-build); the sysroot and objects stay in <repo>/out/wfserver-work.
# Output: <repo>/out/wfserver-armhf.tar.gz, where `build.sh deploy` picks it up.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set -euo pipefail

readonly C_HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly C_REPO="$(cd -- "${C_HERE}/../.." && pwd)"
readonly C_BASE="docker.io/library/ubuntu:24.04"
readonly C_IMAGE="localhost/x6100-wfserver-build"

#
# Print usage and exit
#

usage() {
	printf '%s\n' "usage: container.sh <wfview source tree>" >&2
	exit 1
}

#
# The container engine: $X6100_ENGINE, else podman, else docker
#

engine_pick() {
	local L_E

	for L_E in ${X6100_ENGINE:-podman docker}; do
		if command -v -- "${L_E}" > /dev/null 2>&1; then
			printf '%s\n' "${L_E}"
			return 0
		fi
	done

	printf '%s\n' "error: no podman or docker (zypper install podman)" >&2
	return 1
}

#
# Run build.sh in a container from the given image; extra engine options first
#

run_build() {
	local L_IMAGE="${1}"
	shift
	local L_CHOWN="true"

	# docker runs as the real root: hand the output back to the caller, also after a
	# failure. Rootless podman maps its root to the caller already
	if [[ "${G_ENGINE}" == docker ]]; then
		L_CHOWN="chown -R $(id -u):$(id -g) /x6100/out/wfserver-armhf* /x6100/out/wfserver-work 2> /dev/null"
	fi

	"${G_ENGINE}" run "$@" --network host \
		-e http_proxy -e https_proxy -e HTTP_PROXY -e HTTPS_PROXY -e no_proxy -e NO_PROXY \
		-v "${C_REPO}:/x6100:z" -v "${G_SRC}:/wfview:z" -w /x6100/out \
		"${L_IMAGE}" bash -c "trap '${L_CHOWN}' EXIT; bash /x6100/contrib/wfserver/build.sh /wfview"
}

#
# Main
#

if [[ $# -ne 1 || ! -f "${1}/wfserver.pro" ]]; then
	usage
fi

G_SRC="$(cd -- "${1}" && pwd)"
G_ENGINE="$(engine_pick)"

mkdir -p -- "${C_REPO}/out"

if "${G_ENGINE}" image inspect "${C_IMAGE}" > /dev/null 2>&1; then
	run_build "${C_IMAGE}" --rm
else
	# First run: keep the container (cross tools installed) as the build image
	"${G_ENGINE}" rm -f x6100-wfserver-first > /dev/null 2>&1 || true	# none left over is fine
	run_build "${C_BASE}" --name x6100-wfserver-first
	"${G_ENGINE}" commit x6100-wfserver-first "${C_IMAGE}" > /dev/null
	"${G_ENGINE}" rm x6100-wfserver-first > /dev/null
	printf 'image %s kept for the next runs\n' "${C_IMAGE}"
fi

printf 'bundle: %s\n' "${C_REPO}/out/wfserver-armhf.tar.gz"
