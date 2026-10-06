#!/bin/sh
#
# Benchmark cpr's copy tiers on loopback ext4 and xfs (reflink=0 and
# reflink=1). Shows which tier each mode picks ('cpr -v') and how long it
# takes with a cold page cache. Needs root (or passwordless sudo), mkfs.xfs
# and mkfs.ext4. Run from the repository root after 'make'.
#
# Usage: bench/bench_tiers.sh [SIZE_MIB] [RUNS]

set -eu

size_mib=${1:-1024}
runs=${2:-3}
top=$(cd "$(dirname "$0")/.." && pwd)
cpr="$top/cpr"
sudo=""
[ "$(id -u)" -eq 0 ] || sudo="sudo -n"
work=$(mktemp -d)
mounts=""

cleanup () {
  for m in $mounts; do $sudo umount "$m" || true; done
  rm -rf "$work"
}
trap cleanup EXIT

img_mib=$((size_mib * 3 + 256))

mount_fs () {
  name=$1
  shift
  truncate -s "${img_mib}M" "$work/$name.img"
  $sudo "$@" "$work/$name.img" >/dev/null
  mkdir -p "$work/$name"
  $sudo mount -o loop "$work/$name.img" "$work/$name"
  $sudo chown "$(id -u):$(id -g)" "$work/$name"
  mounts="$work/$name $mounts"
}

mount_fs ext4 mkfs.ext4 -q -F
mount_fs xfs mkfs.xfs -q -f -m reflink=0
mount_fs xfs_reflink mkfs.xfs -q -f -m reflink=1

now_ms () {
  echo $(($(date +%s%N) / 1000000))
}

printf '%-12s %-14s %-8s %10s %10s\n' fs mode tier "best ms" "MiB/s"

for fs in ext4 xfs xfs_reflink; do
  dir="$work/$fs"
  head -c "$((size_mib * 1024 * 1024))" /dev/urandom > "$dir/src"
  sync

  for mode in "-c" "-T cfr" "-T rw" "-T clone,rw"; do
    best=""
    tier=""
    for _ in $(seq "$runs"); do
      rm -f "$dir/dst"
      sync
      echo 3 | $sudo tee /proc/sys/vm/drop_caches >/dev/null
      t0=$(now_ms)
      # shellcheck disable=SC2086
      tier=$("$cpr" -v $mode "$dir/src" "$dir/dst" 2>&1) || tier="error: $tier"
      t1=$(now_ms)
      ms=$((t1 - t0))
      if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then best=$ms; fi
    done
    cmp -s "$dir/src" "$dir/dst" || tier="MISMATCH"
    rate=$(awk -v s="$size_mib" -v ms="$best" \
             'BEGIN { if (ms > 0) printf "%.0f", s * 1000 / ms; else print "inf" }')
    printf '%-12s %-14s %-8s %10s %10s\n' "$fs" "$mode" "${tier#tier: }" \
           "$best" "$rate"
  done
done
