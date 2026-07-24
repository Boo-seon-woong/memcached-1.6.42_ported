#!/bin/bash
# D6 in-server span + throughput sweep, run inside the SEV guest.
# Per sweep point: start memcached (ext_threads=$1, workers=$2), preload $KEYS,
# then SET / GET / 1:9 at 60s each; capture the prof span stats + memtier ops/s.
# Fixed per admin: 64B values, crypto ON, EXT_SLOT_SIZE=256, pipeline=1.
# Adapted for the 3GB guest: KEYS=4M, -m 1024 (see conversation.md).
set -u
cd "$HOME/kvs-port"
GENIE=10.99.0.2:11212
KEYS=${KEYS:-4000000}
MC_M=${MC_M:-1024}
MT="$HOME/memtier/memtier_benchmark"
MTENV="LD_LIBRARY_PATH=$HOME/memtier:$HOME/kvs-port"
ENV="LD_LIBRARY_PATH=$HOME/covlib:$PWD MLX5_COHERENT_QP=1 MLX5_COHERENT_CQ=1 EXT_CRYPTO_KEY=$PWD/ext.key EXT_SLOT_SIZE=256 EXT_RDMA_PROF=1 EXT_WRITE_BATCH=1"

# memtier client shape: threads x connections (default 16x12=192)
TEST_TIME=${TEST_TIME:-60}
MT_T=${MT_T:-16}; MT_C=${MT_C:-12}

mc_cmd(){ exec 9<>/dev/tcp/127.0.0.1/11211; printf "%b" "$1" >&9; timeout 5 cat <&9; exec 9<&- 9>&-; }
prof_row(){ mc_cmd "stats\r\nquit\r\n" | tr -d '\r' | awk '
  /extstore_prof_read_avg_ns/{ra=$3} /extstore_prof_read_p50_ns/{r5=$3} /extstore_prof_read_p99_ns/{r9=$3}
  /extstore_prof_read_sync_avg_ns/{rs=$3} /extstore_prof_read_xfer_avg_ns/{rx=$3} /extstore_prof_read_count/{rc=$3}
  /extstore_prof_write_avg_ns/{wa=$3} /extstore_prof_write_p50_ns/{w5=$3} /extstore_prof_write_p99_ns/{w9=$3}
  /extstore_prof_write_sync_avg_ns/{ws=$3} /extstore_prof_write_xfer_avg_ns/{wx=$3} /extstore_prof_write_count/{wc=$3}
  /badcrc_from_extstore/{bc=$3} /curr_items/{ci=$3}
  END{printf "  READ  n=%s avg=%sns p50=%s p99=%s [sync=%s xfer=%s]\n",rc,ra,r5,r9,rs,rx;
      printf "  WRITE n=%s avg=%sns p50=%s p99=%s [sync=%s xfer=%s]\n",wc,wa,w5,w9,ws,wx;
      printf "  badcrc=%s curr_items=%s\n",bc,ci}'; }

run_point(){
  local ext=$1 workers=$2
  echo "############ POINT ext_threads=$ext workers=$workers mt=${MT_T}x${MT_C} keys=$KEYS ############"
  pkill -x memcached 2>/dev/null; sleep 1; tmux kill-session -t d6 2>/dev/null
  tmux new-session -d -s d6 "$ENV ./memcached -p 11211 -U 0 -t $workers -m $MC_M -o ext_path=$GENIE:4g,ext_threads=$ext 2>&1 | tee mc.d6.log"
  sleep 4
  if ! pgrep -x memcached >/dev/null; then echo "  ABORTED: memcached did not start"; grep -i "fail\|refuse\|selftest" mc.d6.log|head -2; return; fi
  echo "  preload $KEYS keys..."
  eval $MTENV $MT -s 127.0.0.1 -p 11211 -P memcache_text -t $MT_T -c $MT_C --pipeline=1 -d 64 \
    --key-prefix=memtier- --key-minimum=1 --key-maximum=$KEYS --key-pattern=P:P --ratio=1:0 \
    -n $((KEYS/(MT_T*MT_C)+1)) --hide-histogram >/tmp/preload.out 2>&1
  local ci=$(mc_cmd "stats\r\nquit\r\n"|tr -d '\r'|awk '/curr_items/{print $3}')
  echo "  preloaded, curr_items=$ci"
  for phase in "SET 1:0" "GET 0:1" "1:9 1:9"; do
    local name=${phase% *} ratio=${phase#* }
    mc_cmd "stats reset\r\nquit\r\n" >/dev/null
    echo "  --- phase $name (ratio $ratio, 60s) ---"
    local ops=$(eval $MTENV $MT -s 127.0.0.1 -p 11211 -P memcache_text -t $MT_T -c $MT_C --pipeline=1 -d 64 \
      --key-prefix=memtier- --key-minimum=1 --key-maximum=$KEYS --key-pattern=R:R --ratio=$ratio \
      --test-time=$TEST_TIME --hide-histogram 2>/dev/null | awk '/Totals/{print $2}')
    echo "  memtier ops/s: $ops"
    prof_row
  done
}

# points: "ext_threads workers" pairs from args, or a default coarse grid
if [ $# -gt 0 ]; then
  for p in "$@"; do run_point ${p/:/ }; done
else
  for ext in 4 16 64 128; do run_point $ext 16; done
fi
pkill -x memcached 2>/dev/null
echo "SWEEP DONE"
