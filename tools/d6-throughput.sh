#!/bin/bash
# Remote-memory throughput upper-bound: raise offered concurrency via memtier
# pipeline depth (each request is still ONE op = ONE RDMA transfer -> publication=1
# holds; pipeline is client-side request depth, not RDMA-frame batching).
# For each pipeline depth: GET-only 30s, capture ops/s + in-server span p50/p99 +
# guest load. Goal: max ops/s where p50 < 30us.
# One preload; GET-only doesn't mutate, so pipeline values reuse the same data.
set -u
cd "$HOME/kvs-port"
GENIE=10.99.0.2:11212
KEYS=${KEYS:-10000000}
EXT=${EXT:-16}          # QP count
WORKERS=${WORKERS:-16}
MT_T=${MT_T:-16}; MT_C=${MT_C:-12}
TT=${TT:-30}
MT="$HOME/memtier/memtier_benchmark"; MTENV="LD_LIBRARY_PATH=$HOME/memtier:$HOME/kvs-port"
# NOTE: no EXT_WRITE_BATCH -> posting batches (still 1 op/transfer). prof on.
ENV="LD_LIBRARY_PATH=$HOME/covlib:$PWD MLX5_COHERENT_QP=1 MLX5_COHERENT_CQ=1 EXT_CRYPTO_KEY=$PWD/ext.key EXT_SLOT_SIZE=256 EXT_RDMA_PROF=1"
mc(){ exec 9<>/dev/tcp/127.0.0.1/11211; printf "%b" "$1" >&9; timeout 5 cat <&9; exec 9<&- 9>&-; }
prof(){ mc "stats\r\nquit\r\n" | tr -d '\r' | awk '
  /extstore_prof_read_avg_ns/{a=$3}/extstore_prof_read_p50_ns/{p5=$3}/extstore_prof_read_p99_ns/{p9=$3}
  /extstore_prof_read_sync_avg_ns/{s=$3}/extstore_prof_read_xfer_avg_ns/{x=$3}/extstore_prof_read_count/{n=$3}
  /badcrc_from_extstore/{b=$3}
  END{printf "    READ n=%s p50=%sns p99=%sns avg=%sns [sync=%s xfer=%s] badcrc=%s\n",n,p5,p9,a,s,x,b}'; }

echo "### throughput sweep: ext_threads=$EXT workers=$WORKERS mt=${MT_T}x${MT_C} keys=$KEYS TT=${TT}s"
pkill -x memcached 2>/dev/null; sleep 1; tmux kill-session -t tp 2>/dev/null
tmux new-session -d -s tp "$ENV ./memcached -p 11211 -U 0 -t $WORKERS -m 2048 -o ext_path=$GENIE:4g,ext_threads=$EXT 2>&1 | tee mc.tp.log"
sleep 4
pgrep -x memcached >/dev/null || { echo "  ABORTED: memcached did not start"; grep -i "fail\|refus\|selftest" mc.tp.log|head; exit 1; }
echo "  preload $KEYS..."
eval $MTENV $MT -s 127.0.0.1 -p 11211 -P memcache_text -t $MT_T -c $MT_C --pipeline=1 -d 64 \
  --key-prefix=memtier- --key-minimum=1 --key-maximum=$KEYS --key-pattern=P:P --ratio=1:0 \
  -n $((KEYS/(MT_T*MT_C)+1)) --hide-histogram >/tmp/pl.out 2>&1
echo "  preloaded curr_items=$(mc "stats\r\nquit\r\n"|tr -d '\r'|awk '/curr_items/{print $3}')"
for P in ${PIPES:-1 2 4 8 16 32}; do
  mc "stats reset\r\nquit\r\n" >/dev/null
  ops=$(eval $MTENV $MT -s 127.0.0.1 -p 11211 -P memcache_text -t $MT_T -c $MT_C --pipeline=$P -d 64 \
    --key-prefix=memtier- --key-minimum=1 --key-maximum=$KEYS --key-pattern=R:R --ratio=0:1 \
    --test-time=$TT --hide-histogram 2>/dev/null | awk '/Totals/{print $2}')
  load=$(uptime | grep -oE "load average: [0-9.]+" | grep -oE "[0-9.]+$")
  printf "  pipeline=%-3s offered=%-6s GET ops/s=%-10s load=%s/24cores\n" "$P" "$((MT_T*MT_C*P))" "$ops" "$load"
  prof
done
pkill -x memcached 2>/dev/null
echo "### done"
