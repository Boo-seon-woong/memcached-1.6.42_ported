# memcached-1.6.42-port 실험 실행 가이드

이 문서는 Ariel host에서 SEV-SNP guest에 접속해 다음 두 실험을 직접 수행하는
절차다.

1. RDMA QP 하나의 `ext_io_depth` 포화점 측정
2. 선택한 depth에서 QP 수를 늘리는 scaling 측정

명령은 위에서 아래로 순서대로 실행한다. 별도 설명이 없는 명령은 **guest 안에서**
실행한다.

## 0. 실험 계약

이 실험은 다음 조건을 고정한다.

| 항목 | 값 |
|---|---|
| workload | GET-only, value 64 B |
| remote memory | Genie `10.99.0.2:11212`, 4 GiB |
| encryption | AES-256-GCM ON |
| RDMA 전송 단위 | WR 하나당 logical operation 하나 |
| linked-WR posting batch | 금지 (`EXT_WRITE_BATCH=1`) |
| 같은 QP의 independent outstanding operation | 허용 (`ext_io_depth`) |
| 완료 후 `SYNC_FOR_CPU` batching | 허용 |
| memcached / memtier CPU | `0-15` / `16-23` |
| latency gate | in-server p50 ≤ 30,000 ns |
| throughput 목표 | 10,000,000 Ops/s |

`EXT_WRITE_BATCH=1`은 한 번의 `ibv_post_send()`에 WR 하나만 넘기게 한다.
`ext_io_depth>1`이면 이전 WR의 완료 전에 다음 WR을 **별도의 post 호출로** 올릴 수
있다. 따라서 depth 실험은 명령 coalescing이나 linked-WR batching이 아니다.

현재 profiler의 READ span은 `RDMA READ post → CQE → SYNC_FOR_CPU`다.
AES-256-GCM decrypt는 실행되어 throughput에는 반영되지만
`extstore_prof_read_*` latency에는 포함되지 않는다.

저장 의미론은 remote-only다.

- SET은 AES-256-GCM sealing과 원격 WRITE 완료 후에만 `STORED`를 반환한다.
- hash table에는 key와 `ITEM_HDR` 원격 위치 metadata만 남는다.
- staging/decrypt request buffer는 일시적 작업 공간이며 local value cache가 아니다.
- 원격 공간·staging·WRITE에 실패하면 local value를 남기지 않고 SET이 실패한다.
- `EXT_CRYPTO_KEY`가 없거나 32 B가 아니면 extstore 서버가 시작되지 않는다.

## 1. Ariel host에서 guest 진입

```bash
ssh -t \
  -o UserKnownHostsFile=/dev/null \
  -o StrictHostKeyChecking=no \
  -i /home/seonung/.ssh/snp_guest \
  -p 2222 ubuntu@localhost
```

성공하면 prompt가 다음 형태로 바뀐다.

```text
ubuntu@sev-guest:~$
```

## 2. Guest와 RDMA 선행 상태 확인

```bash
cd "$HOME/kvs-port"

nproc
ip -br addr show ibp1s0
ibv_devices
ping -c 2 10.99.0.2

test -x "$HOME/kvs-port/memcached"
test -x "$HOME/memtier/memtier_benchmark"
test "$(stat -c %s "$HOME/kvs-port/ext.key")" -eq 32

pgrep -a memcached || true
```

정상 기준:

- `nproc`가 `24`
- `ibp1s0`가 `UP`, 주소가 `10.99.0.3/24`
- RDMA device가 하나 이상 표시됨
- `ext.key`가 정확히 32 B

`ping` 성공은 IP 연결만 확인한다. 실제 RDMA CM과 SWIOTLB sync는 뒤의
`EXT_SELFTEST=1`이 검증한다.

> 다음 `start_server`는 기존 memcached를 종료한다. 기존 preload 상태가 필요하면
> 여기서 중단한다.

## 3. 실험 변수 설정

먼저 빠른 동작 확인용으로 1M keys, 15초를 사용한다.

```bash
REPO="$HOME/kvs-port"
MEMTIER="$HOME/memtier/memtier_benchmark"
GENIE="10.99.0.2:11212"

KEYS=1000000
MEMORY_MB=512
TEST_SECONDS=15

MEMTIER_THREADS=8
MEMTIER_CLIENTS=24

RESULT_DIR="$HOME/rdma-results/memcached-port-qd-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULT_DIR"
echo "result directory: $RESULT_DIR"
```

예상 출력은 결과 디렉터리 경로 한 줄뿐이다.

```text
result directory: /home/ubuntu/rdma-results/memcached-port-qd-YYYYMMDD-HHMMSS
```

이 단계는 변수와 빈 디렉터리만 만든다. **실험은 아직 시작되지 않았고 기다릴
작업도 없다.**

## 4. 실행 helper 등록

아래 블록 전체를 한 번에 붙여 넣는다.

```bash
mc_stats() {
    printf 'stats\r\nquit\r\n' |
        nc -q 1 127.0.0.1 11211 |
        tr -d '\r'
}

mc_stat() {
    mc_stats | awk -v key="$1" '$2 == key { print $3; exit }'
}

start_server() {
    local depth="$1"
    local qps="$2"
    local log="$RESULT_DIR/memcached-q${qps}-d${depth}.log"
    local ready=0

    pkill -x memcached 2>/dev/null || true
    tmux kill-session -t mcqd 2>/dev/null || true
    sleep 1

    tmux new-session -d -s mcqd \
      "cd '$REPO' && exec env \
        LD_LIBRARY_PATH='$HOME/covlib:$REPO' \
        MLX5_COHERENT_QP=1 \
        MLX5_COHERENT_CQ=1 \
        EXT_CRYPTO_KEY='$REPO/ext.key' \
        EXT_SLOT_SIZE=256 \
        EXT_READ_SLOTS=64 \
        EXT_RDMA_PROF=1 \
        EXT_SELFTEST=1 \
        EXT_WRITE_BATCH=1 \
        taskset -c 0-15 ./memcached \
          -p 11211 -U 0 -t 16 -m '$MEMORY_MB' -c 8192 -R 1024 \
          -o ext_path='$GENIE':4g,ext_threads='$qps',ext_io_depth='$depth' \
          >'$log' 2>&1"

    for _ in $(seq 1 30); do
        if mc_stats >/dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 1
    done

    if [ "$ready" -ne 1 ]; then
        echo "ERROR: memcached did not start: $log"
        tail -n 30 "$log"
        return 1
    fi

    grep -E 'genie_connect OK|selftest: OK|FAILED|failed' "$log"
    pgrep -a memcached

    local pid
    pid=$(pgrep -x memcached | head -1)
    tr '\0' '\n' <"/proc/$pid/environ" |
        grep -E '^(EXT_|MLX5_COHERENT_|LD_LIBRARY_PATH)' |
        sort
    return 0
}

preload() {
    local depth="$1"
    local qps="$2"
    local requests=$((KEYS / (MEMTIER_THREADS * MEMTIER_CLIENTS) + 1))
    local out="$RESULT_DIR/preload-q${qps}-d${depth}.txt"
    local sf="$RESULT_DIR/preload-stats-q${qps}-d${depth}.txt"

    if ! taskset -c 16-23 env \
      LD_LIBRARY_PATH="$HOME/memtier:$REPO" \
      "$MEMTIER" \
        -s 127.0.0.1 -p 11211 -P memcache_text \
        -t "$MEMTIER_THREADS" -c "$MEMTIER_CLIENTS" --pipeline=1 \
        -d 64 \
        --key-prefix=memtier- \
        --key-minimum=1 --key-maximum="$KEYS" \
        --key-pattern=P:P --ratio=1:0 \
        -n "$requests" --hide-histogram \
        >"$out" 2>&1; then
        echo "ERROR: preload failed: $out"
        tail -n 30 "$out"
        return 1
    fi

    sleep 2
    mc_stats >"$sf"

    local items sets written used bad dead failures
    items=$(awk '$2=="curr_items" {print $3}' "$sf")
    sets=$(awk '$2=="cmd_set" {print $3}' "$sf")
    written=$(awk '$2=="extstore_objects_written" {print $3}' "$sf")
    used=$(awk '$2=="extstore_objects_used" {print $3}' "$sf")
    bad=$(awk '$2=="badcrc_from_extstore" {print $3}' "$sf")
    dead=$(awk '$2=="extstore_engine_dead" {print $3}' "$sf")
    failures=$(awk '$2=="extstore_write_failures" {print $3}' "$sf")

    printf 'PRELOAD qps=%s depth=%s curr_items=%s cmd_set=%s remote_writes=%s remote_live=%s badcrc=%s dead=%s write_failures=%s\n' \
      "$qps" "$depth" "$items" "$sets" "$written" "$used" "$bad" "$dead" "$failures"

    if [ "${items:-0}" -ne "$KEYS" ] ||
       [ "${sets:-0}" -ne "${written:--1}" ] ||
       [ "${items:-0}" -ne "${used:--1}" ] ||
       [ "${bad:-1}" -ne 0 ] ||
       [ "${dead:-1}" -ne 0 ] ||
       [ "${failures:-1}" -ne 0 ]; then
        echo "ERROR: invalid preload; do not measure this point"
        return 1
    fi
}

run_get() {
    local depth="$1"
    local qps="$2"
    local pipeline="$3"
    local out="$RESULT_DIR/get-q${qps}-d${depth}-p${pipeline}.txt"
    local sf="$RESULT_DIR/stats-q${qps}-d${depth}-p${pipeline}.txt"

    printf 'stats reset\r\nquit\r\n' |
        nc -q 1 127.0.0.1 11211 >/dev/null

    taskset -c 16-23 env \
      LD_LIBRARY_PATH="$HOME/memtier:$REPO" \
      "$MEMTIER" \
        -s 127.0.0.1 -p 11211 -P memcache_text \
        -t "$MEMTIER_THREADS" -c "$MEMTIER_CLIENTS" \
        --pipeline="$pipeline" \
        -d 64 \
        --key-prefix=memtier- \
        --key-minimum=1 --key-maximum="$KEYS" \
        --key-pattern=R:R --ratio=0:1 \
        --test-time="$TEST_SECONDS" --hide-histogram |
        tee "$out"

    local memtier_rc=${PIPESTATUS[0]}
    if [ "$memtier_rc" -ne 0 ]; then
        echo "ERROR: memtier failed with status $memtier_rc: $out"
        return 1
    fi

    sleep 1
    mc_stats >"$sf"

    local ops reads gets hits prof avg p50 p99 sync bad dead failures
    ops=$(awk '/Totals/ {v=$2} END {print v}' "$out")
    reads=$(awk '$2=="extstore_objects_read" {print $3}' "$sf")
    gets=$(awk '$2=="cmd_get" {print $3}' "$sf")
    hits=$(awk '$2=="get_hits" {print $3}' "$sf")
    prof=$(awk '$2=="extstore_prof_read_count" {print $3}' "$sf")
    avg=$(awk '$2=="extstore_prof_read_avg_ns" {print $3}' "$sf")
    p50=$(awk '$2=="extstore_prof_read_p50_ns" {print $3}' "$sf")
    p99=$(awk '$2=="extstore_prof_read_p99_ns" {print $3}' "$sf")
    sync=$(awk '$2=="extstore_prof_read_sync_avg_ns" {print $3}' "$sf")
    bad=$(awk '$2=="badcrc_from_extstore" {print $3}' "$sf")
    dead=$(awk '$2=="extstore_engine_dead" {print $3}' "$sf")
    failures=$(awk '$2=="extstore_read_failures" {print $3}' "$sf")

    local result
    result="RESULT qps=$qps depth=$depth pipeline=$pipeline offered=$((MEMTIER_THREADS * MEMTIER_CLIENTS * pipeline)) ops_per_sec=$ops cmd_get=$gets hits=$hits rdma_reads=$reads prof_count=$prof avg_ns=$avg p50_ns=$p50 p99_ns=$p99 sync_ns=$sync badcrc=$bad dead=$dead read_failures=$failures"
    printf '%s\n' "$result" | tee -a "$RESULT_DIR/results.txt"

    if [ "${bad:-1}" -ne 0 ] ||
       [ "${dead:-1}" -ne 0 ] ||
       [ "${failures:-1}" -ne 0 ] ||
       [ "${gets:-0}" -ne "${hits:--1}" ] ||
       [ "${gets:-0}" -ne "${reads:--1}" ] ||
       [ "${reads:-0}" -ne "${prof:--1}" ]; then
        echo "ERROR: invalid GET result; stop this sweep"
        return 1
    fi
}
```

함수 정의가 성공하면 출력 없이 prompt가 돌아온다. 이것도 정상이며 아직 실험은
실행되지 않았다.

등록 여부는 다음으로 확인한다.

```bash
type start_server preload run_get
```

세 함수가 `is a function`으로 표시되어야 한다.

## 5. 한 점 smoke test

전체 sweep 전에 QP 1개, depth 1, client pipeline 1인 한 점만 실행한다.

```bash
start_server 1 1
```

반드시 다음 두 줄이 표시되어야 한다.

```text
extstore: genie_connect OK (...)
extstore selftest: OK (256 bytes written and read back)
```

환경 출력에 다음 값도 있어야 한다.

```text
EXT_CRYPTO_KEY=/home/ubuntu/kvs-port/ext.key
EXT_READ_SLOTS=64
EXT_RDMA_PROF=1
EXT_SELFTEST=1
EXT_SLOT_SIZE=256
EXT_WRITE_BATCH=1
```

그다음 preload한다.

```bash
preload 1 1
```

빠른 설정의 정상 출력:

```text
PRELOAD qps=1 depth=1 curr_items=1000000 cmd_set=... remote_writes=... remote_live=1000000 badcrc=0 dead=0 write_failures=0
```

마지막으로 15초 GET을 실행한다.

```bash
run_get 1 1 1
```

memtier 표 다음에 한 줄짜리 `RESULT`가 출력된다. 결과 파일 확인:

```bash
ls -lh "$RESULT_DIR"
cat "$RESULT_DIR/results.txt"
```

최소한 다음 파일들이 있어야 한다.

```text
memcached-q1-d1.log
preload-q1-d1.txt
preload-stats-q1-d1.txt
get-q1-d1-p1.txt
stats-q1-d1-p1.txt
results.txt
```

## 6. 단일 QP queue-depth sweep

Smoke test가 유효할 때만 실행한다. 각 depth마다 memcached를 재시작하므로 preload도
다시 수행한다.

```bash
{
    for depth in 1 2 4 8 16 32 64; do
        echo "===== qps=1 depth=$depth ====="
        start_server "$depth" 1 || break
        preload "$depth" 1 || break

        for pipeline in 1 4 16 64; do
            run_get "$depth" 1 "$pipeline" || break 2
        done
    done
} 2>&1 | tee "$RESULT_DIR/qd-console.txt"
```

이 단계는 foreground에서 실행된다. `preload` 중에는 출력이 없다가 완료 후
`PRELOAD` 한 줄이 나오며, 각 GET은 `TEST_SECONDS` 동안 memtier 출력을 표시한다.

진행 상황을 다른 SSH 창에서 보려면:

```bash
tail -f "$RESULT_DIR/qd-console.txt"
```

결과 요약:

```bash
column -t "$RESULT_DIR/results.txt"
```

단일 QP 포화 처리율은 `qps=1` 결과 중 pipeline을 높여도 `ops_per_sec`가 더 이상
증가하지 않는 지점이다. latency 조건도 동시에 만족해야 한다.

```text
p50_ns <= 30000
badcrc=0
dead=0
read_failures=0
cmd_get == rdma_reads == prof_count
cmd_get == hits
```

## 7. QP 수 scaling

§6에서 latency gate를 지키며 가장 높은 처리량을 낸 depth와 pipeline을 선택한다.
아래 예시의 `16`은 자동 결론이 아니므로 실제 결과로 바꾼다.

```bash
BEST_DEPTH=16
BEST_PIPELINE=16
```

새 결과 디렉터리를 사용하면 depth sweep과 섞이지 않는다.

```bash
RESULT_DIR="$HOME/rdma-results/memcached-port-qp-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULT_DIR"
echo "result directory: $RESULT_DIR"
```

QP 수 sweep:

```bash
{
    for qps in 1 2 4 8 16 32; do
        echo "===== qps=$qps depth=$BEST_DEPTH ====="
        start_server "$BEST_DEPTH" "$qps" || break
        preload "$BEST_DEPTH" "$qps" || break
        run_get "$BEST_DEPTH" "$qps" "$BEST_PIPELINE" || break
    done
} 2>&1 | tee "$RESULT_DIR/qp-console.txt"
```

```bash
column -t "$RESULT_DIR/results.txt"
```

QP 수를 늘렸을 때 처리량이 선형으로 증가하는지, 어느 지점에서 CPU·client·sync
병목으로 평평해지는지를 확인한다.

## 8. 정식 10M-key 재측정

빠른 sweep으로 후보 depth/pipeline/QP 범위를 줄인 뒤에만 정식 조건을 실행한다.

```bash
KEYS=10000000
MEMORY_MB=2048
TEST_SECONDS=60

RESULT_DIR="$HOME/rdma-results/memcached-port-final-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULT_DIR"
echo "result directory: $RESULT_DIR"
```

후보 한 점의 예:

```bash
FINAL_DEPTH=16
FINAL_QPS=16
FINAL_PIPELINE=16

start_server "$FINAL_DEPTH" "$FINAL_QPS"
preload "$FINAL_DEPTH" "$FINAL_QPS"
run_get "$FINAL_DEPTH" "$FINAL_QPS" "$FINAL_PIPELINE"
```

위 세 후보 값은 quick sweep 결과로 교체한다.

## 9. 10M Ops/s에 필요한 QP 수 역산

단일 QP의 latency gate 내 포화 처리율을 `X`에 넣는다.

```bash
X=750000

awk -v x="$X" '
BEGIN {
    printf "theoretical_minimum_qps=%d\n", int(10000000 / x + 0.999999)
}'
```

이 값은 이론적 최소치다. 최종 수량은 §7의 실제 QP scaling 효율을 적용해 결정한다.
QP 증가에 따라 per-QP 처리율이 감소한다면 단순 나눗셈보다 더 많은 QP가 필요하다.

## 10. 종료와 결과 보존

현재 서버 로그:

```bash
tail -n 50 "$RESULT_DIR"/memcached-*.log
```

실험 종료:

```bash
pkill -x memcached 2>/dev/null || true
tmux kill-session -t mcqd 2>/dev/null || true
```

결과 디렉터리는 삭제되지 않는다.

Host에서 결과를 가져오려면 guest SSH를 종료한 뒤 Ariel host에서 실행한다.

```bash
scp -r \
  -o UserKnownHostsFile=/dev/null \
  -o StrictHostKeyChecking=no \
  -i /home/seonung/.ssh/snp_guest \
  -P 2222 \
  ubuntu@localhost:/home/ubuntu/rdma-results/<result-directory> \
  /home/seonung/2026/rdma-results/
```

`<result-directory>`를 실제 디렉터리 이름으로 바꾼다.

## 11. 실패 판정과 복구

### 변수 설정 또는 함수 등록 후 아무 출력도 없음

정상이다. 변수 대입과 함수 정의는 작업을 실행하지 않는다. §5의
`start_server 1 1`부터 실제 실행이 시작된다.

### `start_server`에서 `Connection refused`

```bash
tail -n 50 "$RESULT_DIR"/memcached-*.log
ping -c 2 10.99.0.2
```

Genie의 `genie_memd`가 `10.99.0.2:11212`에서 RDMA CM 요청을 받고 있는지
확인한다. TCP `nc -z 10.99.0.2 11212`는 RDMA CM 검증이 아니므로 판단 근거로
쓰지 않는다.

### `selftest: FAILED`

측정을 진행하지 않는다. RDMA 연결은 됐지만 `SYNC_FOR_DEVICE` 또는
`SYNC_FOR_CPU`를 포함한 payload 경로가 올바르지 않다는 뜻이다.

### preload가 유효하지 않음

다음 remote-only 불변식 중 하나가 깨진 것이다.

```text
curr_items == KEYS
cmd_set == extstore_objects_written
curr_items == extstore_objects_used
badcrc == engine_dead == extstore_write_failures == 0
```

`cmd_set`은 key 범위를 나누는 과정의 중복 SET 때문에 `KEYS`보다 조금 클 수 있다.
그러므로 `written >= KEYS`가 아니라 `cmd_set == extstore_objects_written`을
검사해야 한다. 해당 점의 성능 수치는 폐기한다.

### RESULT가 유효하지 않음

다음 중 하나라도 발생하면 해당 점을 성능 결과로 사용하지 않는다.

```text
badcrc != 0
dead != 0
read_failures != 0
cmd_get != rdma_reads
rdma_reads != prof_count
cmd_get != get_hits
```

### depth 16 이후 처리량이 증가하지 않음

현재 RDMA CM 연결은 `initiator_depth=16`, `responder_resources=16`을 요청한다.
따라서 READ의 실제 on-wire concurrency는 16-credit에서 제한될 수 있다.
`ext_io_depth=32/64`는 plateau 확인용이며, credit 16을 넘는 실험은 CM 양쪽
설정 변경과 별도 검증이 필요하다.

## 12. 기존 도구 사용 주의

`tools/d6-throughput.sh`는 `EXT_WRITE_BATCH`를 설정하지 않아 기본 linked-WR posting
batch를 허용한다. 이 문서의 “WR 하나씩 독립 post” 실험에는 그대로 사용하지 않는다.

`tools/d6-sweep.sh`는 `EXT_WRITE_BATCH=1`이지만 QP당 `ext_io_depth` sweep을 수행하지
않는다. 기존 D6 재현용이며 이 문서의 depth 실험을 대체하지 않는다.
