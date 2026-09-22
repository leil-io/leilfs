#!/usr/bin/env bash
set -euo pipefail

# /mnt/hdd_* start out as plain directories baked into the image's overlay
# layer (mounts done during `docker build` don't survive into the built
# image). Tools like fincore can't see real page-cache residency for files
# on that overlay - confirmed by comparing the same just-written bytes
# through the container's overlay merged view (0 cached pages reported)
# against the real path backing that overlay layer on the host (100%
# cached). Seen on Ubuntu 26.04 test hosts; not observed on 24.04, so this
# only runs there rather than changing behavior for every distro this
# image is built for. Give each /mnt/hdd_* a real, loop-mounted ext4
# filesystem at container start instead, so chunk data lives on an
# ordinary block-backed filesystem like it does on bare metal.
#
# Loop devices are a host-kernel-global resource, not container-namespaced,
# and creating them requires elevated container privileges (a plain
# `docker run` can't reach /dev/loop-control at all) - the container this
# runs in needs --privileged and -v /dev:/dev, or equivalent, for this to
# work rather than fail outright.
#
# An earlier version of this script relied on `mount -o loop`'s implicit
# LO_FLAGS_AUTOCLEAR to release the loop device when the container exits,
# and that didn't reliably happen - loop devices accumulated across many
# short-lived containers until the host ran out and Docker itself could no
# longer create new containers. Track exactly which loop devices this
# script creates and explicitly `losetup -d` them on exit instead of
# relying on autoclear.

. /etc/os-release
if [[ "${ID}" != "ubuntu" || "${VERSION_ID}" != "26.04" ]]; then
	exec "$@"
fi

image_dir=/var/lib/leilfs_hdd_images
image_size=8G
created_loops=()

cleanup() {
	local loop
	for loop in "${created_loops[@]}"; do
		umount "${loop}" 2>/dev/null || true
		losetup -d "${loop}" 2>/dev/null || true
	done
}
trap cleanup EXIT

mkdir -p "${image_dir}"

for hdd in /mnt/hdd_*; do
	if mountpoint -q "${hdd}"; then
		continue
	fi

	image="${image_dir}/$(basename "${hdd}").img"
	if [[ ! -f "${image}" ]]; then
		truncate -s "${image_size}" "${image}"
		mkfs.ext4 -Fq "${image}"
	fi

	loop_dev=$(losetup --find --show "${image}")
	created_loops+=("${loop_dev}")
	mount "${loop_dev}" "${hdd}"
	chown saunafstest:saunafstest "${hdd}"
done

"$@"
