#!/usr/bin/env bash
#
# Report gcov branch coverage of libcpr.c and fail if below a threshold.
#
# Usage: coverage.sh OBJ_DIR MIN_PERCENT

set -eu

OBJ_DIR=${1:?usage: coverage.sh OBJ_DIR MIN_PERCENT}
MIN=${2:?usage: coverage.sh OBJ_DIR MIN_PERCENT}
GCOV=${GCOV:-gcov}

cd "$(dirname "$0")/.."

out=$($GCOV -b -n -o "$OBJ_DIR" libcpr.c)
echo "$out"

pct=$(echo "$out" |
      awk -F'[:%]' '/^File .*libcpr\.c/ { f = 1 }
                    f && /^Taken at least once/ { print $2; exit }')

if [ -z "$pct" ]; then
  echo "coverage: could not parse branch coverage from $GCOV output" >&2
  exit 1
fi

if awk -v p="$pct" -v m="$MIN" 'BEGIN { exit !(p + 0 >= m + 0) }'; then
  echo "coverage: libcpr.c branches taken ${pct}% (>= ${MIN}%)"
else
  echo "coverage: libcpr.c branches taken ${pct}% (< ${MIN}%)" >&2
  exit 1
fi
