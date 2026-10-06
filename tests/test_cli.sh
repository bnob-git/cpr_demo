#!/usr/bin/env bash
#
# End-to-end tests for the cpr CLI. Output is TAP.
#
# Usage: test_cli.sh CPR_BIN FALLBACK_DIR [REFLINK_DIR] [SUDO] [LABEL]
#
# FALLBACK_DIR must be on a FS without reflink support; REFLINK_DIR (optional)
# on a FICLONE-capable FS. SUDO is a command prefix used for chattr when not
# running as root.

set -u

CPR=${1:?usage: test_cli.sh CPR_BIN FALLBACK_DIR [REFLINK_DIR] [SUDO] [LABEL]}
FB=${2:?usage: test_cli.sh CPR_BIN FALLBACK_DIR [REFLINK_DIR] [SUDO] [LABEL]}
RL=${3:-}
SUDO=${4:-}
LABEL=${5:+$5: }

n=0
failed=0

expect_fail() {
  if "$@"; then
    echo "unexpected success: $*"
    return 1
  fi
  return 0
}

mkdata() {
  head -c "$2" /dev/urandom > "$1"
}

# same_bytes FILE_A OFF_A FILE_B OFF_B LEN
same_bytes() {
  cmp -s <(tail -c +"$(($2 + 1))" "$1" | head -c "$5") \
         <(tail -c +"$(($4 + 1))" "$3" | head -c "$5")
}

is_shared() {
  ! command -v filefrag >/dev/null || filefrag -v "$1" | grep -q shared
}

can_chattr() {
  [ "$(id -u)" -eq 0 ] || [ -n "$SUDO" ]
}

# --- Fallback FS ------------------------------------------------------------

t_no_fallback_fails() {
  mkdata "$T/src" 10000
  expect_fail "$CPR" "$T/src" "$T/dst" || return 1
}

t_fallback_copy() {
  mkdata "$T/src" 1048589
  "$CPR" -c "$T/src" "$T/dst" && cmp "$T/src" "$T/dst"
}

t_fallback_empty_file() {
  : > "$T/src"
  "$CPR" -c "$T/src" "$T/dst" && [ ! -s "$T/dst" ]
}

t_existing_dst_needs_force() {
  mkdata "$T/src" 5000
  mkdata "$T/dst" 20000
  expect_fail "$CPR" -c "$T/src" "$T/dst" 2>/dev/null || return 1
  # -f truncates, so a larger dst must end up identical to src.
  "$CPR" -c -f "$T/src" "$T/dst" && cmp "$T/src" "$T/dst"
}

t_fallback_range() {
  mkdata "$T/src" 10000
  "$CPR" -c -s 3 -d 5 -l 1000 "$T/src" "$T/dst" || return 1
  [ "$(stat -c %s "$T/dst")" -eq 1005 ] || return 1
  [ "$(head -c 5 "$T/dst" | od -An -tx1 | tr -d ' \n')" = 0000000000 ] ||
    return 1
  same_bytes "$T/src" 3 "$T/dst" 5 1000
}

t_preserve_perms_and_times() {
  mkdata "$T/src" 100
  chmod 0640 "$T/src"
  touch -d '2001-02-03 04:05:06' "$T/src"
  "$CPR" -c -p -t "$T/src" "$T/dst" || return 1
  [ "$(stat -c %a "$T/dst")" = 640 ] &&
    [ "$(stat -c %Y "$T/dst")" = "$(stat -c %Y "$T/src")" ]
}

t_bad_args() {
  expect_fail "$CPR" 2>/dev/null || return 1
  expect_fail "$CPR" "$T/only_src" 2>/dev/null || return 1
  expect_fail "$CPR" -s abc "$T/a" "$T/b" 2>/dev/null || return 1
  expect_fail "$CPR" -c "$T/missing" "$T/dst" 2>/dev/null
}

t_immutable_dst() {
  can_chattr || return 77
  mkdata "$T/src" 100
  : > "$T/dst"
  $SUDO chattr +i "$T/dst" 2>/dev/null || return 77
  "$CPR" -c -f "$T/src" "$T/dst" 2> "$T/err"
  local rc=$?
  $SUDO chattr -i "$T/dst"
  [ "$rc" -ne 0 ] && grep -q "Operation not permitted" "$T/err"
}

# --- Reflink FS -------------------------------------------------------------

t_reflink_copy() {
  [ -n "$RL" ] || return 77
  mkdata "$T/src" 1048589
  "$CPR" "$T/src" "$T/dst" && cmp "$T/src" "$T/dst" && is_shared "$T/dst"
}

t_reflink_range() {
  [ -n "$RL" ] || return 77
  mkdata "$T/src" 1048576
  "$CPR" -s 4096 -d 8192 -l 65536 "$T/src" "$T/dst" || return 1
  same_bytes "$T/src" 4096 "$T/dst" 8192 65536
}

t_reflink_unaligned_range() {
  [ -n "$RL" ] || return 77
  mkdata "$T/src" 10000
  expect_fail "$CPR" -s 1 -l 100 "$T/src" "$T/dst" || return 1
  "$CPR" -c -s 1 -l 100 "$T/src" "$T/dst2" &&
    same_bytes "$T/src" 1 "$T/dst2" 0 100
}

t_cross_fs() {
  [ -n "$RL" ] || return 77
  mkdata "$T/src" 100000
  expect_fail "$CPR" "$T/src" "$T_RL/dst" || return 1
  "$CPR" -c "$T/src" "$T_RL/dst2" && cmp "$T/src" "$T_RL/dst2"
}

# run FN DIR: run FN with $T set to a fresh dir under DIR (and $T_RL under
# the reflink dir for cross-FS tests).
run() {
  local fn=$1 base=$2 rc
  [ -n "$base" ] || base=$FB
  T=$(mktemp -d "$base/cli.XXXXXX")
  T_RL=""
  [ -n "$RL" ] && T_RL=$(mktemp -d "$RL/cli.XXXXXX")

  "$fn" > "$T.log" 2>&1
  rc=$?

  n=$((n + 1))
  if [ "$rc" -eq 77 ]; then
    echo "ok $n - ${LABEL}cli_$fn # SKIP needs reflink dir or root"
  elif [ "$rc" -eq 0 ]; then
    echo "ok $n - ${LABEL}cli_$fn"
  else
    echo "not ok $n - ${LABEL}cli_$fn"
    sed 's/^/#   /' "$T.log"
    failed=1
  fi

  rm -rf "$T" "$T.log"
  [ -n "$T_RL" ] && rm -rf "$T_RL"
}

run t_no_fallback_fails "$FB"
run t_fallback_copy "$FB"
run t_fallback_empty_file "$FB"
run t_existing_dst_needs_force "$FB"
run t_fallback_range "$FB"
run t_preserve_perms_and_times "$FB"
run t_bad_args "$FB"
run t_immutable_dst "$FB"
run t_reflink_copy "$RL"
run t_reflink_range "$RL"
run t_reflink_unaligned_range "$RL"
run t_cross_fs "$FB"

echo "1..$n"
exit "$failed"
