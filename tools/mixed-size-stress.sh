#!/usr/bin/env bash
# Stress the free-list reuse rule with MIXED object sizes.
#
# torn-repro.sh uses one value size, where every freed slot exactly fits the next
# request — the easy case for the LIFO top-only reuse in extstore_alloc. This one
# cycles sizes so freed slots are often the wrong size for the next request, which
# is where "only reuse if the top entry is big enough, and shrink its recorded
# len" can quietly stop recycling and consume fresh page space instead.
#
#   ./tools/mixed-size-stress.sh [host] [port] [rounds]
#
# Watch pages_used: it should plateau. Continuous growth across rounds with a
# constant key count means freed slots are not being reclaimed.
set -u
H=${1:-127.0.0.1}; P=${2:-11311}; ROUNDS=${3:-6}
KEYS=${KEYS:-5000}; N=${N:-5000}

stat() { printf 'stats\r\nquit\r\n' | nc -q1 "$H" "$P" | awk -v k="$1" '$2==k{print $3}' | tr -d '\r'; }
command -v memtier_benchmark >/dev/null || { echo "memtier_benchmark not found"; exit 1; }

echo "round  value_size  pages_used  bytes_used  badcrc  curr_items"
for r in $(seq 1 "$ROUNDS"); do
  # alternate sizes so each rewrite of a key changes its slot size requirement
  case $((r % 3)) in
    0) D=200 ;;
    1) D=400 ;;
    2) D=800 ;;
  esac
  memtier_benchmark -s "$H" -p "$P" -P memcache_text --hide-histogram \
    -c 4 -t 2 -n "$N" --ratio=1:0 -d "$D" --key-maximum="$KEYS" --key-pattern=S:S \
    >/dev/null 2>&1
  sleep 2
  printf "%5d  %10d  %10s  %10s  %6s  %10s\n" "$r" "$D" \
    "$(stat extstore_pages_used)" "$(stat extstore_bytes_used)" \
    "$(stat badcrc_from_extstore)" "$(stat curr_items)"
done

echo
echo "PASS if pages_used plateaus and badcrc stays 0 with curr_items == $KEYS."
echo "Growing pages_used at a constant key count = freed slots are not being reused."
