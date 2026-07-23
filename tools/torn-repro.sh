#!/usr/bin/env bash
# Reproduce the P-1a torn-read data loss under write load, and measure whether a
# fix closes it. Reads that fail their AAD tag because an in-place overwrite
# replaced the slot are answered to the client as misses, for data that exists.
#
# A bash client cannot hit the window (tried, 4800 reads, nothing) — the race
# needs memtier's request rate, which is why this exists as a tool rather than a
# one-liner in the channel.
#
#   ./tools/torn-repro.sh [host] [port]        # defaults 127.0.0.1 11311
#
# PASS = badcrc_from_extstore stays 0 across the mixed phase.
# Any nonzero delta is a read that was silently answered as a miss.
set -u
H=${1:-127.0.0.1}; P=${2:-11311}
KEYS=${KEYS:-20000}; N=${N:-20000}; D=${D:-400}

stat() { printf 'stats\r\nquit\r\n' | nc -q1 "$H" "$P" | awk -v k="$1" '$2==k{print $3}' | tr -d '\r'; }

command -v memtier_benchmark >/dev/null || { echo "memtier_benchmark not found"; exit 1; }

echo "== preload $KEYS keys x ${D}B =="
memtier_benchmark -s "$H" -p "$P" -P memcache_text --hide-histogram \
  -c 4 -t 2 -n "$N" --ratio=1:0 -d "$D" --key-maximum="$KEYS" --key-pattern=S:S \
  2>&1 | grep -E '^Sets'
sleep 2

b0=$(stat badcrc_from_extstore); r0=$(stat extstore_read_retries)
m0=$(stat get_misses);           g0=$(stat get_extstore)

echo "== mixed 1:1 (reads racing in-place overwrites) =="
memtier_benchmark -s "$H" -p "$P" -P memcache_text --hide-histogram \
  -c 4 -t 2 -n "$N" --ratio=1:1 -d "$D" --key-maximum="$KEYS" --key-pattern=R:R \
  2>&1 | grep -E '^Gets|^Sets'

b1=$(stat badcrc_from_extstore); r1=$(stat extstore_read_retries)
m1=$(stat get_misses);           g1=$(stat get_extstore)

echo
echo "extstore reads      : $((g1-g0))"
echo "read_retries        : $((r1-r0))"
echo "badcrc (LOST READS) : $((b1-b0))"
echo "get_misses counted  : $((m1-m0))"
if [ "$((b1-b0))" -eq 0 ]; then
  echo "RESULT: PASS — no reads lost to torn slots under write load"
else
  echo "RESULT: FAIL — $((b1-b0)) reads answered as misses for data that exists"
  [ "$((m1-m0))" -lt "$((b1-b0))" ] && \
    echo "        (and only $((m1-m0)) of them show up in get_misses)"
fi
