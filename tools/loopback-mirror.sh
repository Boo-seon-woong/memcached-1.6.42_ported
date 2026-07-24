#!/usr/bin/env bash
# Non-TEE loopback mirror of the D6 sweep, so every SEV number has a reference
# taken with the identical invocation and the identical span definition.
#
#   ./tools/loopback-mirror.sh "16:16:16x12" "32:16:16x12" ...
#          point = <ext_threads>:<memcached -t>:<memtier -t>x<memtier -c>
#
# Per point: genie_memd restarted virgin (admin's requirement), preload, then the
# three phases. Prints one table row per phase: in-server span avg/p50/p99 per
# direction plus memtier ops/s.
#
# Fixed by the D6 spec — do not vary these without taking the measurement both
# ways: 64 B values, crypto ON, EXT_SLOT_SIZE=256, pipeline depth 1.
set -u
R=/home/seonung/2026/memcached-1.6.42_ported
SP=${SP:-/tmp/claude-1002/-home-seonung-2026/2c927544-6ab1-43c9-8f7b-8106f4718b5f/scratchpad}
PORT=${PORT:-11311}
GPORT=${GPORT:-11212}
# 4M keys / -m 1024: the guest has 3 GB RAM (10M stubs + -m 2048 would evict),
# so the SEV sweep uses these and the mirror must match or the subtraction is
# invalid. Override via env if ariel's final config differs.
KEYS=${KEYS:-4000000}
MEMLIMIT=${MEMLIMIT:-1024}
PRELOAD_N=${PRELOAD_N:-${KEYS}}
TEST_TIME=${TEST_TIME:-60}
D=64

cd "$R" || exit 1
command -v memtier_benchmark >/dev/null || { echo "memtier_benchmark not found"; exit 1; }
[ -f .ext.key ] || head -c 32 /dev/urandom > .ext.key

stat() { printf 'stats\r\nquit\r\n' | nc -q1 127.0.0.1 "$PORT" | awk -v k="$1" '$2==k{print $3}' | tr -d '\r'; }

restart_server() {   # virgin MR between sweep points
  for p in $(pgrep -x genie_memd); do kill -9 "$p"; done
  sleep 1
  (cd genie-server && setsid bash -c "stdbuf -oL -eL ./genie_memd $GPORT 4g --prefill \
     2>&1 | awk '{ print strftime(\"%F %T\"), \$0; fflush() }' > $SP/gm_mirror.log" &)
  sleep 2
}

start_client() {     # $1 ext_threads  $2 memcached worker threads
  for p in $(pgrep -x memcached); do kill "$p"; done
  sleep 2
  (setsid env EXT_RDMA_PROF=1 EXT_CRYPTO_KEY="$R/.ext.key" EXT_SLOT_SIZE=256 \
     "$R/memcached" -p "$PORT" -U 0 -m "$MEMLIMIT" -t "$2" \
     -o "ext_path=10.99.0.2:$GPORT:4g,ext_item_size=2,ext_threads=$1" \
     > "$SP/mc_mirror.log" 2>&1 &)
  for i in $(seq 1 30); do ss -lnt | grep -q ":$PORT" && return 0; sleep 1; done
  echo "  client failed to start:"; tail -3 "$SP/mc_mirror.log"; return 1
}

row() {              # $1 point  $2 phase  $3 ops/s
  printf "%-16s %-8s  R %8s/%8s/%8s ns   W %8s/%8s/%8s ns   %10s ops/s\n" \
    "$1" "$2" "$(stat extstore_prof_read_avg_ns)" "$(stat extstore_prof_read_p50_ns)" \
    "$(stat extstore_prof_read_p99_ns)" "$(stat extstore_prof_write_avg_ns)" \
    "$(stat extstore_prof_write_p50_ns)" "$(stat extstore_prof_write_p99_ns)" "$3"
}

run_phase() {        # $1 point  $2 label  $3 ratio  $4 mt_threads  $5 mt_conns
  local ops
  ops=$(memtier_benchmark -s 127.0.0.1 -p "$PORT" -P memcache_text --hide-histogram \
        -t "$4" -c "$5" --ratio="$3" -d "$D" --key-maximum="$KEYS" --key-pattern=R:R \
        --test-time="$TEST_TIME" --pipeline=1 2>&1 | awk '/^Totals/{print $2}')
  row "$1" "$2" "${ops:-ERR}"
}

echo "point            phase     in-server span (avg/p50/p99)                              throughput"
for pt in "$@"; do
  IFS=':' read -r EXT_T MC_T MT <<< "$pt"
  MT_T=${MT%x*}; MT_C=${MT#*x}
  restart_server
  if ! start_client "$EXT_T" "$MC_T"; then echo "$pt  SKIPPED (client start failed)"; continue; fi

  memtier_benchmark -s 127.0.0.1 -p "$PORT" -P memcache_text --hide-histogram \
    -t "$MT_T" -c "$MT_C" -n $((PRELOAD_N / (MT_T * MT_C) + 1)) --ratio=1:0 -d "$D" \
    --key-maximum="$KEYS" --key-pattern=S:S --pipeline=1 >/dev/null 2>&1
  sleep 3

  # a failed point is recorded and skipped, not debugged (admin's instruction)
  if [ "$(stat badcrc_from_extstore)" != "0" ]; then
    echo "$pt  ABORTED: badcrc=$(stat badcrc_from_extstore) after preload"; continue
  fi

  run_phase "$pt" "SET"  "1:0" "$MT_T" "$MT_C"
  run_phase "$pt" "GET"  "0:1" "$MT_T" "$MT_C"
  run_phase "$pt" "1:9"  "1:9" "$MT_T" "$MT_C"
  echo "                 (badcrc=$(stat badcrc_from_extstore) curr_items=$(stat curr_items))"
done

for p in $(pgrep -x memcached); do kill "$p"; done
echo "done — mirror complete, client stopped"
