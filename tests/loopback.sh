#!/bin/sh
#
# Run 'make test' against loopback file systems so every tier is exercised:
# xfs -m reflink=1 (clone), xfs -m reflink=0 and ext4 (copy_file_range), and
# cross-file-system copies (read/write). Needs root (or passwordless sudo) and
# mkfs.xfs / mkfs.ext4. Invoked by 'make test-loopback'.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
sudo=""
[ "$(id -u)" -eq 0 ] || sudo="sudo -n"
work=$(mktemp -d)
mounts=""

cleanup () {
  for m in $mounts; do $sudo umount "$m" || true; done
  rm -rf "$work"
}
trap cleanup EXIT

# mount_fs NAME MKFS-ARGS...
mount_fs () {
  name=$1
  shift
  truncate -s 512M "$work/$name.img"
  $sudo "$@" "$work/$name.img" >/dev/null
  mkdir -p "$work/$name"
  $sudo mount -o loop "$work/$name.img" "$work/$name"
  $sudo chown "$(id -u):$(id -g)" "$work/$name"
  mounts="$work/$name $mounts"
}

mount_fs xfs_reflink mkfs.xfs -q -f -m reflink=1
mount_fs xfs_noreflink mkfs.xfs -q -f -m reflink=0
mount_fs ext4 mkfs.ext4 -q -F

for base in xfs_noreflink ext4; do
  other=ext4
  [ "$base" = ext4 ] && other=xfs_noreflink
  echo "=== base: $base, cross-fs: $other, reflink: xfs_reflink"
  CPR_TEST_DIR="$work/$base" CPR_TEST_XDEV_DIR="$work/$other" \
    CPR_TEST_REFLINK_DIR="$work/xfs_reflink" "$here/run_tests.sh"
done
