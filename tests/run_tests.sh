#!/usr/bin/env bash
#
# Run the libcpr C tests and the cpr CLI tests against real filesystems.
#
# With root (or passwordless sudo) this mounts tmpfs plus ext4, btrfs and
# xfs (reflink=1) loopback images, pairing a non-reflink FS (fallback path)
# with a FICLONE-capable FS (reflink path). Without root only the fallback
# tests run in a plain temp dir and the rest are reported as SKIP.
#
# Usage: run_tests.sh TEST_BIN CPR_BIN
#
# Environment:
#   CPR_TEST_STRICT=1      Fail instead of skipping when root or any of the
#                          loopback filesystems is unavailable (used in CI).
#   CPR_TEST_IMG_SIZE      Loopback image size (default 512M, sparse).
#   CPR_TEST_FALLBACK_DIR  Use this dir instead of mounting anything...
#   CPR_TEST_REFLINK_DIR   ...optionally with this reflink-capable dir.

set -u

TEST_BIN=${1:?usage: run_tests.sh TEST_BIN CPR_BIN}
CPR_BIN=${2:?usage: run_tests.sh TEST_BIN CPR_BIN}
TESTS_DIR=$(cd "$(dirname "$0")" && pwd)
IMG_SIZE=${CPR_TEST_IMG_SIZE:-512M}
STRICT=${CPR_TEST_STRICT:-0}

case $TEST_BIN in /*) ;; *) TEST_BIN=$PWD/$TEST_BIN ;; esac
case $CPR_BIN in /*) ;; *) CPR_BIN=$PWD/$CPR_BIN ;; esac

if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
  HAVE_ROOT=1
elif sudo -n true 2>/dev/null; then
  SUDO="sudo -n"
  HAVE_ROOT=1
else
  SUDO=""
  HAVE_ROOT=0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/cpr-tests.XXXXXX")
MOUNTS=()
FAILED=0

cleanup() {
  local i m
  for (( i = ${#MOUNTS[@]} - 1; i >= 0; i-- )); do
    m=${MOUNTS[i]}
    $SUDO chattr -R -i "$m" >/dev/null 2>&1 || true
    $SUDO umount "$m" 2>/dev/null || $SUDO umount -l "$m" 2>/dev/null || true
  done
  rm -rf "$WORK"
}
trap cleanup EXIT

# Mount filesystem $1 at $WORK/$1. Returns non-zero if unavailable.
mount_fs() {
  local fs=$1 dir=$WORK/$1 img=$WORK/$1.img
  mkdir -p "$dir"

  case $fs in
    tmpfs)
      $SUDO mount -t tmpfs -o size=256M tmpfs "$dir" ;;
    ext4)
      command -v mkfs.ext4 >/dev/null &&
        truncate -s "$IMG_SIZE" "$img" &&
        mkfs.ext4 -q -F "$img" >/dev/null 2>&1 &&
        $SUDO mount -o loop "$img" "$dir" ;;
    btrfs)
      command -v mkfs.btrfs >/dev/null &&
        truncate -s "$IMG_SIZE" "$img" &&
        mkfs.btrfs -q -f "$img" >/dev/null 2>&1 &&
        $SUDO mount -o loop "$img" "$dir" ;;
    xfs)
      command -v mkfs.xfs >/dev/null &&
        truncate -s "$IMG_SIZE" "$img" &&
        mkfs.xfs -q -f -m reflink=1 "$img" >/dev/null 2>&1 &&
        $SUDO mount -o loop "$img" "$dir" ;;
  esac || {
    echo "# could not mount $fs"
    [ "$STRICT" = 1 ] && FAILED=1
    return 1
  }

  MOUNTS+=("$dir")
  $SUDO chown "$(id -u):$(id -g)" "$dir"
}

# run_pair LABEL FALLBACK_DIR [REFLINK_DIR]
run_pair() {
  local label=$1 fallback=$2 reflink=${3:-}

  echo "# === $label"
  CPR_TEST_LABEL="$label: " \
  CPR_TEST_FALLBACK_DIR=$fallback \
  CPR_TEST_REFLINK_DIR=$reflink \
  CPR_TEST_SUDO=$SUDO \
    "$TEST_BIN" || FAILED=1

  "$TESTS_DIR/test_cli.sh" "$CPR_BIN" "$fallback" "$reflink" "$SUDO" \
    "$label" || FAILED=1
}

if [ -n "${CPR_TEST_FALLBACK_DIR:-}" ]; then
  run_pair "custom" "$CPR_TEST_FALLBACK_DIR" "${CPR_TEST_REFLINK_DIR:-}"
elif [ "$HAVE_ROOT" = 1 ]; then
  FALLBACK_FS=()
  REFLINK_FS=()
  for fs in tmpfs ext4; do mount_fs "$fs" && FALLBACK_FS+=("$fs"); done
  for fs in btrfs xfs; do mount_fs "$fs" && REFLINK_FS+=("$fs"); done

  if [ ${#FALLBACK_FS[@]} -eq 0 ]; then
    mkdir -p "$WORK/plain"
    FALLBACK_FS=(plain)
  fi

  runs=${#FALLBACK_FS[@]}
  [ ${#REFLINK_FS[@]} -gt "$runs" ] && runs=${#REFLINK_FS[@]}

  for (( i = 0; i < runs; i++ )); do
    f=${FALLBACK_FS[i % ${#FALLBACK_FS[@]}]}
    if [ ${#REFLINK_FS[@]} -gt 0 ]; then
      r=${REFLINK_FS[i % ${#REFLINK_FS[@]}]}
      run_pair "$f+$r" "$WORK/$f" "$WORK/$r"
    else
      run_pair "$f" "$WORK/$f"
    fi
  done
else
  echo "# no root or passwordless sudo: reflink, immutable and loopback"
  echo "# filesystem tests will be skipped"
  [ "$STRICT" = 1 ] && FAILED=1
  mkdir -p "$WORK/plain"
  run_pair "plain" "$WORK/plain"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "# FAILED"
  exit 1
fi

echo "# all tests passed"
