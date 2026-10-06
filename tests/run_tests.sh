#!/bin/sh
#
# Run the libcpr/cpr tests. Invoked by 'make test'.
#
# Optional environment:
#   CPR_TEST_DIR          Scratch directory (default: a temp dir under tests/).
#   CPR_TEST_XDEV_DIR     Scratch directory on another file system (default:
#                         a temp dir under /dev/shm if that is a different FS).
#   CPR_TEST_REFLINK_DIR  Scratch directory on a reflink-capable file system.
#                         'make test-loopback' sets this up with loopback mounts.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
cleanup=""

trap 'rm -rf $cleanup' EXIT

if [ -z "${CPR_TEST_DIR:-}" ]; then
  CPR_TEST_DIR=$(mktemp -d "$here/tmp.XXXXXX")
  cleanup="$cleanup $CPR_TEST_DIR"
fi

if [ -z "${CPR_TEST_XDEV_DIR+set}" ] && [ -d /dev/shm ] &&
   [ "$(stat -c %d /dev/shm)" != "$(stat -c %d "$CPR_TEST_DIR")" ]; then
  CPR_TEST_XDEV_DIR=$(mktemp -d /dev/shm/cpr-test.XXXXXX)
  cleanup="$cleanup $CPR_TEST_XDEV_DIR"
fi

export CPR_TEST_DIR CPR_TEST_XDEV_DIR="${CPR_TEST_XDEV_DIR:-}"
export CPR_TEST_REFLINK_DIR="${CPR_TEST_REFLINK_DIR:-}"

echo "CPR_TEST_DIR=$CPR_TEST_DIR ($(stat -f -c %T "$CPR_TEST_DIR"))"
[ -n "$CPR_TEST_XDEV_DIR" ] &&
  echo "CPR_TEST_XDEV_DIR=$CPR_TEST_XDEV_DIR ($(stat -f -c %T "$CPR_TEST_XDEV_DIR"))"
[ -n "$CPR_TEST_REFLINK_DIR" ] &&
  echo "CPR_TEST_REFLINK_DIR=$CPR_TEST_REFLINK_DIR ($(stat -f -c %T "$CPR_TEST_REFLINK_DIR"))"

"$here/test_tiers"
"$here/test_cfr_faults"

# CLI: -T / -v.
failures=0
cpr="$top/cpr"
src="$CPR_TEST_DIR/cli_src"
dst="$CPR_TEST_DIR/cli_dst"
head -c 1000000 /dev/urandom > "$src"

expect_tier () {
  want=$1
  shift
  rm -f "$dst"
  got=$("$cpr" -v "$@" "$src" "$dst" 2>&1) || true
  if [ "$got" = "tier: $want" ] && cmp -s "$src" "$dst"; then
    echo "  ok: cpr $* -> $want"
  else
    echo "  FAIL: cpr $* -> expected 'tier: $want', got '$got'"
    failures=$((failures + 1))
  fi
}

expect_tier rw  -T rw
expect_tier cfr -T cfr
expect_tier cfr -T cfr,rw
expect_tier rw  -s 0 -T rw

if "$cpr" -T bogus "$src" "$dst.x" 2>/dev/null; then
  echo "  FAIL: cpr -T bogus succeeded"
  failures=$((failures + 1))
else
  echo "  ok: cpr -T bogus rejected"
fi

if [ -n "$CPR_TEST_REFLINK_DIR" ]; then
  src="$CPR_TEST_REFLINK_DIR/cli_src"
  dst="$CPR_TEST_REFLINK_DIR/cli_dst"
  head -c 1000000 /dev/urandom > "$src"
  expect_tier clone -c
  expect_tier cfr   -T cfr,rw
fi

echo "cli: $failures failures"
[ "$failures" -eq 0 ]
