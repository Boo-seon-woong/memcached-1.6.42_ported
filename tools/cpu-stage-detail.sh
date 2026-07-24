#!/bin/bash
# Reproduce CPU-us/op accounting and split syscall CPU by worker/io thread group.
set -euo pipefail

MODE=${MODE:-port}
REPO=${REPO:-"$HOME/kvs-port"}
MT=${MT:-"$HOME/memtier/memtier_benchmark"}
PORT_BIN=${PORT_BIN:-"$REPO/memcached"}
NATIVE_BIN=${NATIVE_BIN:-"$REPO/memcached.stock"}
CRYPTO_BIN=${CRYPTO_BIN:-"$REPO/ext-crypto-cost"}
PROFILE_PROGRAM=${PROFILE_PROGRAM:-"$REPO/cpu-stage-profile.bt"}
OUT=${OUT:-"$HOME/rdma-results/cpu-stage-$MODE-$(date +%Y%m%d-%H%M%S)"}
TRACE_SECONDS=${TRACE_SECONDS:-5}
PROFILE_SECONDS=${PROFILE_SECONDS:-10}
CONTROL_SECONDS=${CONTROL_SECONDS:-10}
LOAD_SECONDS=${LOAD_SECONDS:-55}
PIPELINE=${PIPELINE:-4}
EXT_THREADS=${EXT_THREADS:-8}
EXT_IO_DEPTH=${EXT_IO_DEPTH:-64}
MT_THREADS=${MT_THREADS:-8}
MT_CLIENTS=${MT_CLIENTS:-16}
MC_THREADS=${MC_THREADS:-8}
SERVER_CPUS=${SERVER_CPUS:-0-15}
CLIENT_CPUS=${CLIENT_CPUS:-16-23}
RUN_STRACE=${RUN_STRACE:-0}
RATE_LIMIT=${RATE_LIMIT:-}
CLK_TCK=$(getconf CLK_TCK)
PRELOAD_PER_CLIENT=$(( (1000000 + MT_THREADS * MT_CLIENTS - 1) /
    (MT_THREADS * MT_CLIENTS) ))

mkdir -p "$OUT"
exec > >(tee "$OUT/runner.txt") 2>&1

mc_stats() {
    local out=$1
    exec 9<>/dev/tcp/127.0.0.1/11211
    printf "stats\r\nquit\r\n" >&9
    timeout 5 cat <&9 | tr -d "\r" > "$out" || true
    exec 9<&- 9>&-
}

stat_value() {
    awk -v key="$2" '$1 == "STAT" && $2 == key { print $3; exit }' "$1"
}

snap() {
    local pid=$1 out=$2 t tid
    : > "$out"
    for t in /proc/"$pid"/task/*/stat; do
        tid=${t%/stat}
        tid=${tid##*/}
        awk -v tid="$tid" '{ print tid, $2, $14, $15 }' "$t" >> "$out"
    done
}

delta() {
    local before=$1 after=$2 ops=$3 scope=$4 label=$5
    awk -v ops="$ops" -v hz="$CLK_TCK" -v scope="$scope" -v label="$label" '
        NR == FNR { comm[$1] = $2; u[$1] = $3; s[$1] = $4; next }
        $1 in u {
            du[$2] += $3 - u[$1]
            ds[$2] += $4 - s[$1]
        }
        END {
            for (c in du) {
                us = du[c] * 1000000.0 / hz / ops
                ss = ds[c] * 1000000.0 / hz / ops
                printf "%s %s %-18s %.6f %.6f %.6f\n",
                    label, scope, c, us, ss, us + ss
            }
        }
    ' "$before" "$after"
}

group_tids() {
    local snapfile=$1 group=$2 pid=$3
    case "$group" in
        worker) awk '$2 == "(mc-worker)" { printf "%s ", $1 }' "$snapfile" ;;
        io) awk -v pid="$pid" '$1 != pid && $2 == "(memcached)" { printf "%s ", $1 }' "$snapfile" ;;
        *) return 1 ;;
    esac
}

run_window() {
    local label=$1 seconds=$2 trace_group=${3:-}
    local sb="$OUT/$label-server-before.txt" sa="$OUT/$label-server-after.txt"
    local cb="$OUT/$label-client-before.txt" ca="$OUT/$label-client-after.txt"
    local gb="$OUT/$label-stats-before.txt" ga="$OUT/$label-stats-after.txt"
    local g1 g2 r1 r2 ops remote_ops tids rc=0

    mc_stats "$gb"
    g1=$(stat_value "$gb" cmd_get)
    r1=$(stat_value "$gb" extstore_prof_read_count)
    snap "$MCPID" "$sb"
    snap "$MTPID" "$cb"

    if [[ -n "$trace_group" ]]; then
        tids=$(group_tids "$sb" "$trace_group" "$MCPID")
        [[ -n "$tids" ]] || { echo "no tids for $trace_group"; return 1; }
        local args=() tid
        for tid in $tids; do args+=(-p "$tid"); done
        printf "%s tids: %s\n" "$trace_group" "$tids" | tee "$OUT/$label-tids.txt"
        set +e
        timeout -s INT "$seconds" sudo -n strace -qq -c -S time \
            -U total-time,min-time,max-time,avg-time,calls,errors,name \
            -o "$OUT/$label-strace.txt" "${args[@]}"
        rc=$?
        set -e
        [[ $rc == 0 || $rc == 124 || $rc == 130 ]] ||
            { echo "strace failed: rc=$rc"; return "$rc"; }
    else
        sleep "$seconds"
    fi

    snap "$MCPID" "$sa"
    snap "$MTPID" "$ca"
    mc_stats "$ga"
    g2=$(stat_value "$ga" cmd_get)
    ops=$((g2 - g1))
    r2=$(stat_value "$ga" extstore_prof_read_count)
    remote_ops=$(( ${r2:-$g2} - ${r1:-$g1} ))
    printf "label=%s seconds=%s ops=%s ops_per_s=%.2f\n" \
        "$label" "$seconds" "$ops" "$(awk -v n="$ops" -v s="$seconds" 'BEGIN { print n/s }')" \
        | tee "$OUT/$label-window.txt"
    printf "remote_reads=%s remote_reads_per_s=%.2f\n" \
        "$remote_ops" "$(awk -v n="$remote_ops" -v s="$seconds" 'BEGIN { print n/s }')" \
        | tee -a "$OUT/$label-window.txt"
    delta "$sb" "$sa" "$ops" server "$label" | tee "$OUT/$label-cpu-us-op.txt"
    delta "$cb" "$ca" "$ops" client "$label" | tee -a "$OUT/$label-cpu-us-op.txt"
}

run_profile_window() {
    local label=cpu-profile
    local sb="$OUT/$label-server-before.txt" sa="$OUT/$label-server-after.txt"
    local cb="$OUT/$label-client-before.txt" ca="$OUT/$label-client-after.txt"
    local gb="$OUT/$label-stats-before.txt" ga="$OUT/$label-stats-after.txt"
    local g1 g2 r1 r2 ops remote_ops rc=0
    local -a workers ios clients args

    mc_stats "$gb"
    g1=$(stat_value "$gb" cmd_get)
    r1=$(stat_value "$gb" extstore_prof_read_count)
    snap "$MCPID" "$sb"
    snap "$MTPID" "$cb"
    read -ra workers <<< "$(group_tids "$sb" worker "$MCPID")"
    read -ra ios <<< "$(group_tids "$sb" io "$MCPID")"
    read -ra clients <<< "$(awk '$2 == "(memtier_benchma)" { printf "%s ", $1 }' "$cb")"
    while ((${#workers[@]} < 8)); do workers+=(0); done
    while ((${#ios[@]} < 4)); do ios+=(0); done
    while ((${#clients[@]} < 8)); do clients+=(0); done
    args=("${workers[@]:0:8}" "${ios[@]:0:4}" "${clients[@]:0:8}")
    printf "groups: worker=%s io=%s client=%s\n" \
        "${workers[*]:0:8}" "${ios[*]:0:4}" "${clients[*]:0:8}" \
        | tee "$OUT/$label-tids.txt"

    set +e
    timeout -s INT "$PROFILE_SECONDS" sudo -n env BPFTRACE_MAX_MAP_KEYS=32768 \
        bpftrace -q "$PROFILE_PROGRAM" "${args[@]}" \
        > "$OUT/$label.txt" 2> "$OUT/$label.stderr.txt"
    rc=$?
    set -e
    [[ $rc == 0 || $rc == 124 || $rc == 130 ]] ||
        { echo "bpftrace failed: rc=$rc"; cat "$OUT/$label.stderr.txt"; return "$rc"; }

    snap "$MCPID" "$sa"
    snap "$MTPID" "$ca"
    mc_stats "$ga"
    g2=$(stat_value "$ga" cmd_get)
    ops=$((g2 - g1))
    r2=$(stat_value "$ga" extstore_prof_read_count)
    remote_ops=$(( ${r2:-$g2} - ${r1:-$g1} ))
    printf "label=%s seconds=%s ops=%s ops_per_s=%.2f\n" \
        "$label" "$PROFILE_SECONDS" "$ops" \
        "$(awk -v n="$ops" -v s="$PROFILE_SECONDS" 'BEGIN { print n/s }')" \
        | tee "$OUT/$label-window.txt"
    printf "remote_reads=%s remote_reads_per_s=%.2f\n" \
        "$remote_ops" \
        "$(awk -v n="$remote_ops" -v s="$PROFILE_SECONDS" 'BEGIN { print n/s }')" \
        | tee -a "$OUT/$label-window.txt"
    delta "$sb" "$sa" "$ops" server "$label" | tee "$OUT/$label-cpu-us-op.txt"
    delta "$cb" "$ca" "$ops" client "$label" | tee -a "$OUT/$label-cpu-us-op.txt"
}

pkill -x memcached 2>/dev/null || true
tmux kill-session -t mc 2>/dev/null || true
sleep 1

if [[ "$MODE" == port ]]; then
    RUN_BIN=$PORT_BIN
    SERVER_CMD="cd $REPO && exec taskset -c $SERVER_CPUS env LD_LIBRARY_PATH=$HOME/covlib:$REPO MLX5_COHERENT_QP=1 MLX5_COHERENT_CQ=1 EXT_RDMA_PROF=1 EXT_CRYPTO_KEY=$REPO/ext.key EXT_SLOT_SIZE=256 EXT_READ_SLOTS=64 $PORT_BIN -p 11211 -U 0 -t $MC_THREADS -m 2048 -c 8192 -R 1024 -o ext_path=10.99.0.2:11212:4g,ext_threads=$EXT_THREADS,ext_io_depth=$EXT_IO_DEPTH"
elif [[ "$MODE" == native ]]; then
    RUN_BIN=$NATIVE_BIN
    SERVER_CMD="cd $REPO && exec taskset -c 0-15 env LD_LIBRARY_PATH=$REPO $NATIVE_BIN -p 11211 -U 0 -t 8 -m 2048 -c 8192 -R 1024"
else
    echo "MODE must be port or native" >&2
    exit 2
fi

pkill -f "^$RUN_BIN " 2>/dev/null || true
tmux new-session -d -s mc "$SERVER_CMD >$OUT/server.txt 2>&1"
sleep 6
MCPID=$(pgrep -f "^$RUN_BIN " | head -1)
[[ -n "$MCPID" ]] || { echo "memcached did not start"; exit 1; }
tr "\0" " " < /proc/"$MCPID"/cmdline | tee "$OUT/server-command.txt"
printf "\n" | tee -a "$OUT/server-command.txt"
sha256sum "/proc/$MCPID/exe" | tee "$OUT/server-sha256.txt"

taskset -c "$CLIENT_CPUS" env LD_LIBRARY_PATH="$HOME/memtier:$REPO" "$MT" \
    -s 127.0.0.1 -p 11211 -P memcache_text --threads="$MT_THREADS" --clients="$MT_CLIENTS" \
    --pipeline=8 -d 64 --key-prefix=m- --key-minimum=1 --key-maximum=1000000 \
    --key-pattern=P:P --ratio=1:0 -n "$PRELOAD_PER_CLIENT" --hide-histogram \
    > "$OUT/preload.txt" 2>&1
mc_stats "$OUT/stats-after-preload.txt"
[[ $(stat_value "$OUT/stats-after-preload.txt" curr_items) == 1000000 ]] ||
    { echo "preload did not create 1M items"; exit 1; }

RATE_ARGS=()
[[ -n "$RATE_LIMIT" ]] && RATE_ARGS=(--rate-limiting="$RATE_LIMIT")
taskset -c "$CLIENT_CPUS" env LD_LIBRARY_PATH="$HOME/memtier:$REPO" "$MT" \
    -s 127.0.0.1 -p 11211 -P memcache_text --threads="$MT_THREADS" --clients="$MT_CLIENTS" \
    --pipeline="$PIPELINE" -d 64 --key-prefix=m- --key-minimum=1 --key-maximum=1000000 \
    --key-pattern=R:R --ratio=0:1 --test-time="$LOAD_SECONDS" --hide-histogram \
    "${RATE_ARGS[@]}" \
    > "$OUT/load.txt" 2>&1 &
MTPID=$!
sleep 12

snap "$MCPID" "$OUT/thread-map.txt"
ps -L -p "$MCPID" -o pid,tid,psr,pcpu,comm,wchan:28 > "$OUT/thread-map-ps.txt"
run_window control-before "$CONTROL_SECONDS"
run_profile_window
if [[ "$RUN_STRACE" == 1 ]]; then
    run_window worker-strace "$TRACE_SECONDS" worker
    if [[ "$MODE" == port ]]; then
        run_window io-strace "$TRACE_SECONDS" io
    fi
fi
run_window control-after "$CONTROL_SECONDS"

wait "$MTPID"
mc_stats "$OUT/stats-final.txt"
tmux kill-session -t mc 2>/dev/null || true
pkill -f "^$RUN_BIN " 2>/dev/null || true

if [[ "$MODE" == port && -x "$CRYPTO_BIN" ]]; then
    reads=$(stat_value "$OUT/stats-final.txt" extstore_objects_read)
    bytes=$(stat_value "$OUT/stats-final.txt" extstore_bytes_read)
    ptlen=$(awk -v b="$bytes" -v n="$reads" 'BEGIN { print int((b/n)+0.5)-28 }')
    printf "objects=%s bytes=%s representative_plaintext_len=%s\n" \
        "$reads" "$bytes" "$ptlen" | tee "$OUT/crypto-input.txt"
    for run in 1 2 3 4 5; do
        taskset -c 0-3 "$CRYPTO_BIN" "$ptlen" 250000 4
    done | tee "$OUT/crypto-cost.txt"
fi

sudo -n chown -R "$(id -u):$(id -g)" "$OUT"
echo "RESULT_DIR=$OUT"
