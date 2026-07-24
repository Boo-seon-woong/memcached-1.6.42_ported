#!/bin/bash
# 10-second GET matrix. In this port ext_threads == QP count == IO thread count.
set -uo pipefail

REPO=${REPO:-"$HOME/kvs-port"}
PORT_BIN=${PORT_BIN:-"$REPO/spanv2-pool-crypto-qpaff-iolock/memcached"}
STOCK_BIN=${STOCK_BIN:-"$REPO/memcached.stock"}
MT=${MT:-"$HOME/memtier/memtier_benchmark"}
OUT=${OUT:-"$HOME/rdma-results/config-matrix-10s-$(date +%Y%m%d-%H%M%S)"}
TEST_SECONDS=${TEST_SECONDS:-10}
KEYS=${KEYS:-1000000}
MT_CLIENTS=16
PHASES=${PHASES:-all}
SERVER_CPUS=
CLIENT_CPUS=

mkdir -p "$OUT"
CSV="$OUT/results.csv"
printf '%s\n' \
    'phase,label,mode,mt_threads,mt_clients,mc_threads,qp,ext_threads,pipeline,depth,remote_get_s,remote_avg_us,remote_p50_us,remote_p99_us,xfer_us,sync_us,crypto_us,memtier_get_s,memtier_avg_us,memtier_p50_us,memtier_p99_us,cmd_get,misses,badcrc,read_failures,engine_dead,status' \
    > "$CSV"

stats() {
    exec 9<>/dev/tcp/127.0.0.1/11211 || return 1
    printf 'stats\r\nquit\r\n' >&9
    timeout 5 cat <&9 | tr -d '\r'
    exec 9<&- 9>&-
}

value() {
    awk -v key="$2" '$1 == "STAT" && $2 == key { print $3; exit }' "$1"
}

stop_server() {
    tmux kill-session -t matrix-mc 2>/dev/null || true
    pkill -f "^$PORT_BIN " 2>/dev/null || true
    pkill -f "^$STOCK_BIN " 2>/dev/null || true
    sleep 1
}

run_memtier() {
    local mt_threads=$1 pipeline=$2 seconds=$3 pattern=$4 ratio=$5 out=$6
    shift 6
    taskset -c "$CLIENT_CPUS" env LD_LIBRARY_PATH="$HOME/memtier:$REPO" "$MT" \
        -s 127.0.0.1 -p 11211 -P memcache_text \
        --threads="$mt_threads" --clients="$MT_CLIENTS" --pipeline="$pipeline" \
        -d 64 --key-prefix=m- --key-minimum=1 --key-maximum="$KEYS" \
        --key-pattern="$pattern" --ratio="$ratio" "$@" \
        --hide-histogram > "$out" 2>&1
}

run_point() {
    local phase=$1 label=$2 mode=$3 mt_threads=$4 mc_threads=$5 ext=$6 pipe=$7 depth=$8
    local dir="$OUT/$label" bin command pid preload_n curr warm stats_out
    local mt_line mt_ops mt_avg mt_p50 mt_p99
    local remote_count remote_s ravg rp50 rp99 xfer sync crypto cmd misses badcrc rf dead
    local status=ok

    CLIENT_CPUS="$((24 - mt_threads))-23"
    SERVER_CPUS="0-$((23 - mt_threads))"
    mkdir -p "$dir"
    stop_server
    if [[ "$mode" == port ]]; then
        bin=$PORT_BIN
        command="cd $REPO && exec taskset -c $SERVER_CPUS env LD_LIBRARY_PATH=$HOME/covlib:$REPO MLX5_COHERENT_QP=1 MLX5_COHERENT_CQ=1 EXT_RDMA_PROF=1 EXT_CRYPTO_KEY=$REPO/ext.key EXT_SLOT_SIZE=256 EXT_READ_SLOTS=64 $bin -p 11211 -U 0 -t $mc_threads -m 2048 -c 8192 -R 1024 -o ext_path=10.99.0.2:11212:4g,ext_threads=$ext,ext_io_depth=$depth"
    else
        bin=$STOCK_BIN
        command="cd $REPO && exec taskset -c $SERVER_CPUS env LD_LIBRARY_PATH=$REPO $bin -p 11211 -U 0 -t $mc_threads -m 2048 -c 8192 -R 1024"
    fi
    printf '%s\n' "$command" > "$dir/server-command.txt"
    tmux new-session -d -s matrix-mc "$command >$dir/server.txt 2>&1"

    pid=
    for _ in $(seq 1 30); do
        pid=$(pgrep -f "^$bin " | head -1)
        [[ -n "$pid" ]] && stats > "$dir/stats-start.txt" 2>/dev/null && break
        sleep 1
    done
    if [[ -z "$pid" ]]; then
        printf '%s\n' "$phase,$label,$mode,$mt_threads,$MT_CLIENTS,$mc_threads,$ext,$ext,$pipe,$depth,,,,,,,,,,,,,,,,,server_failed" >> "$CSV"
        echo "FAILED server: $label"
        return
    fi
    sha256sum "/proc/$pid/exe" > "$dir/server-sha256.txt"

    preload_n=$(( (KEYS + mt_threads * MT_CLIENTS - 1) /
        (mt_threads * MT_CLIENTS) ))
    run_memtier "$mt_threads" 8 0 P:P 1:0 "$dir/preload.txt" -n "$preload_n"
    stats > "$dir/stats-after-preload.txt"
    curr=$(value "$dir/stats-after-preload.txt" curr_items)
    if [[ "$curr" != "$KEYS" ]]; then
        printf '%s\n' "$phase,$label,$mode,$mt_threads,$MT_CLIENTS,$mc_threads,$ext,$ext,$pipe,$depth,,,,,,,,,,,,,,,,,preload_$curr" >> "$CSV"
        echo "FAILED preload: $label curr_items=$curr"
        stop_server
        return
    fi

    run_memtier "$mt_threads" "$pipe" 2 R:R 0:1 "$dir/warmup.txt" --test-time=2
    exec 9<>/dev/tcp/127.0.0.1/11211
    printf 'stats reset\r\nquit\r\n' >&9
    timeout 5 cat <&9 > /dev/null || true
    exec 9<&- 9>&-

    run_memtier "$mt_threads" "$pipe" "$TEST_SECONDS" R:R 0:1 \
        "$dir/load.txt" --test-time="$TEST_SECONDS"
    stats_out="$dir/stats-final.txt"
    stats > "$stats_out"

    mt_line=$(awk '$1 == "Gets" { line=$0 } END { print line }' "$dir/load.txt")
    read -r _ mt_ops _ _ mt_avg mt_p50 mt_p99 _ _ <<< "$mt_line"
    mt_avg=$(awk -v n="${mt_avg:-0}" 'BEGIN { printf "%.3f", n * 1000 }')
    mt_p50=$(awk -v n="${mt_p50:-0}" 'BEGIN { printf "%.3f", n * 1000 }')
    mt_p99=$(awk -v n="${mt_p99:-0}" 'BEGIN { printf "%.3f", n * 1000 }')

    cmd=$(value "$stats_out" cmd_get); misses=$(value "$stats_out" get_misses)
    if [[ "$mode" == port ]]; then
        remote_count=$(value "$stats_out" extstore_prof_read_count)
        remote_s=$(awk -v n="$remote_count" -v s="$TEST_SECONDS" \
            'BEGIN { printf "%.2f", n / s }')
        ravg=$(awk -v n="$(value "$stats_out" extstore_prof_read_avg_ns)" 'BEGIN { printf "%.3f", n/1000 }')
        rp50=$(awk -v n="$(value "$stats_out" extstore_prof_read_p50_ns)" 'BEGIN { printf "%.3f", n/1000 }')
        rp99=$(awk -v n="$(value "$stats_out" extstore_prof_read_p99_ns)" 'BEGIN { printf "%.3f", n/1000 }')
        xfer=$(awk -v n="$(value "$stats_out" extstore_prof_read_xfer_avg_ns)" 'BEGIN { printf "%.3f", n/1000 }')
        sync=$(awk -v n="$(value "$stats_out" extstore_prof_read_sync_avg_ns)" 'BEGIN { printf "%.3f", n/1000 }')
        crypto=$(awk -v n="$(value "$stats_out" extstore_prof_read_crypto_avg_ns)" 'BEGIN { printf "%.3f", n/1000 }')
        badcrc=$(value "$stats_out" badcrc_from_extstore)
        rf=$(value "$stats_out" extstore_read_failures)
        dead=$(value "$stats_out" extstore_engine_dead)
        [[ "$misses" == 0 && "$badcrc" == 0 && "$rf" == 0 &&
           "$dead" == 0 && "$remote_count" == "$cmd" ]] ||
            status=correctness_failed
    else
        remote_s= ravg= rp50= rp99= xfer= sync= crypto=
        badcrc= rf= dead=0
        ext=0
        [[ "$misses" == 0 ]] || status=correctness_failed
    fi
    printf '%s\n' "$phase,$label,$mode,$mt_threads,$MT_CLIENTS,$mc_threads,$ext,$ext,$pipe,$depth,$remote_s,$ravg,$rp50,$rp99,$xfer,$sync,$crypto,${mt_ops:-0},$mt_avg,$mt_p50,$mt_p99,$cmd,$misses,$badcrc,$rf,$dead,$status" >> "$CSV"
    echo "DONE $label remote=${remote_s:-na} avg=${ravg:-na} p99=${rp99:-na} memtier=${mt_ops:-0}"
    stop_server
}

sha256sum "$PORT_BIN" "$STOCK_BIN" "$MT" > "$OUT/binaries.sha256"
{
    echo "QP and ext_threads are one 1:1 knob in this implementation."
    echo "Port latency is span-v2 READ post through CQE/SYNC and decrypt completion."
    echo "Stock latency is memtier end-to-end only."
    echo "CPU split is dynamic: client gets the top mt_threads CPUs; server gets the rest of CPUs 0-23."
    echo "Canonical throughput is cmd_get/TEST_SECONDS; memtier_get_s preserves memtier's reported auxiliary value."
} > "$OUT/README.txt"

if [[ "$PHASES" == all || "$PHASES" == qp ]]; then
    for ext in 1 2 4 8 16; do
        run_point qp "qp-ext$ext" port 8 8 "$ext" 4 64
    done
fi

if [[ "$PHASES" == all || "$PHASES" == pipeline ]]; then
    for pipe in 1 2 4 8 16 32; do
        run_point pipeline "pipeline-$pipe" port 8 8 8 "$pipe" 64
    done
fi

if [[ "$PHASES" == all || "$PHASES" == threads ]]; then
    for mt_threads in 4 8 16; do
        for mc_threads in 4 8 16; do
            for ext in 4 8 16; do
                ((mt_threads + mc_threads + ext <= 24)) || continue
                run_point threads "threads-mt${mt_threads}-mc${mc_threads}-ext${ext}" \
                    port "$mt_threads" "$mc_threads" "$ext" 4 64
            done
        done
    done
fi

if [[ "$PHASES" == all || "$PHASES" == depth ]]; then
    for depth in 16 32 64 128; do
        run_point depth "depth-$depth" port 8 8 8 4 "$depth"
    done
fi

if [[ "$PHASES" == all || "$PHASES" == stock ]]; then
    run_point stock stock-local native 8 8 0 4 0
fi

if [[ "$PHASES" == frontier ]]; then
    run_point frontier candidate-q4-d32-p4 port 8 8 4 4 32
    run_point frontier candidate-q8-d16-p8 port 8 8 8 8 16
    run_point frontier candidate-q8-d16-p6 port 8 8 8 6 16
    run_point frontier candidate-q6-d16-p4 port 8 8 6 4 16
    for pipe in 4 6 8; do
        run_point stock "stock-p$pipe" native 8 8 0 "$pipe" 0
    done
fi
stop_server
echo "RESULT_DIR=$OUT"
