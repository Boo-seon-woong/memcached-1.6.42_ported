# RDMA extstore CPU 최적화 점진 적용과 측정

측정일: 2026-07-24

## 목적

`md/CPU_COST_ACCOUNTING.md`에서 확인한 port server 증가분
6.42 CPU-µs/op를 큰 항목부터 하나씩 제거하고, 각 패치 직후 동일 workload의
throughput과 latency를 다시 측정한다. 여러 최적화를 한 번에 적용하지 않는다.

## 고정 비교 조건

| 항목 | 값 |
|---|---|
| operation | GET-only |
| value / key set | 64 B / 1,000,000 preloaded keys |
| memcached | worker 8, CPU 0–15 |
| extstore | QP/io thread 8, depth 64, AES-256-GCM ON |
| memtier | 8 threads × 16 clients, pipeline 4, CPU 16–23 |
| runtime | warmup 12초, 총 55초 |
| correctness | 100% hit, badcrc/read/write failure 모두 0 |

Port의 headline throughput은 두 steady-state control window의
`Δextstore_prof_read_count/Δt` 평균이다. 모든 GET이 remote hit인 경우
`Δcmd_get/Δt`와 같아야 한다. Headline latency는 내부 span v2다. GET은 RDMA
post 직전부터 AES-GCM open 완료까지, SET은 AES-GCM seal 직전부터 CQE까지며
`extstore_prof_span_ver=2`로 결과에 명시한다. 따라서 encrypt/decrypt는
latency에 포함되고 v1 수치와 직접 비교하지 않는다.

memtier는 부하 생성기다. memtier의 ops/s와 end-to-end latency는 offered load,
hit/miss, client-side queueing 확인용 보조값이며 Port의 remote-memory
headline으로 사용하지 않는다. Stock에는 remote span이 없으므로 stock
baseline만 memtier end-to-end throughput/latency로 평가한다.

CPU는 기존과 같이 두 개의 10초 control window에서 `/proc`의 thread별
`utime/stime` 차분을 완료 GET 수로 나눠 CPU-µs/op로 계산한다. 함수별
분해가 필요할 때만 software CPU-clock bpftrace를 사용한다.

기본 운영점은 실측 latency-qualified 최대점인 `mcT=8, mtT=8×c16,
pipeline=4, QP/io=8, depth=64`로 고정한다. 단계 0–2의 기존 표는 당시
고정점(QP=4, pipeline=32)에서 얻은 패치 효과 기록이며 소급 변경하지 않는다.

## 단계

| 단계 | 변경 | 예상 회수 | 다음 단계 진입 조건 |
|---|---|---:|---|
| 0 | 기존 binary 재측정 | 기준선 | workload와 binary hash 보존 |
| 1 | GET plaintext buffer를 per-worker lockless cache로 전환 | 약 3.82 CPU-µs/op | fallback=0, badcrc=0, worker CPU/futex 감소 |
| 2 | AES-GCM context/cipher 재사용 | 최대 약 1 CPU-µs/op | crypto test 통과, badcrc=0, io CPU 감소 |
| 3a | worker→QP affinity 고정 | queue 경합 감소 | 동일 조건에서 CPU/latency 감소 |
| 3b | IO owner-only 상태의 mutex 제거 | lock CPU 감소 | correctness gate와 throughput 유지 |
| 3c | worker/I/O thread CPU 고정 배치 | 실측 후 폐기 | throughput 유지 시에만 채택 |
| 4 | worker-direct/coherent-MR 재검토 | 미정 | 작은 패치 후 남은 CPU/profile로 재판단 |

단계 1의 cache는 기존 `cache_t`와 lockless `do_cache_alloc/free()`를
재사용한다. worker마다 별도 cache를 쓰고, 응답 전송 완료 후
`storage_finalize_cb()`에서 반환한다. cache가 소진되거나 item이 slot보다
크면 기존 slab 경로로 fallback해 정합성을 보존한다.

## 결과

| 단계 | binary SHA-256 | remote GET/s | span-v2 avg | p50 | p99 | worker CPU | io CPU | server CPU |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 0 baseline + span-v2 | `038daa69…75c8b` | **845,572** | **115.717** | **113.500** | **231.100** | **5.813** | **2.588** | **8.400** |
| 1 plaintext pool | `291a9913…24504` | **1,239,314** | **112.662** | **114.000** | **248.500** | **2.323** | **3.065** | **5.388** |
| 2 crypto reuse | `6bcf2186…96d43f` | **2,379,384** | **63.386** | **63.000** | **129.600** | **1.918** | **1.536** | **3.454** |

단위는 span-v2 latency와 CPU 모두 `µs`/op다. 각 행은 독립 실행 결과이며
raw log 디렉터리와 실행 binary hash를 함께 보존한다. 1단계 변화는 remote
throughput **+46.6%**, avg span **-2.6%**, p50 **+0.4%**, p99 **+7.5%**,
server CPU **-35.9%**다. worker CPU가 5.813→2.323으로 줄고 io CPU가
2.588→3.065로 늘어 다음 병목이 io/crypto 쪽으로 이동했다.

2단계는 1단계 대비 remote throughput **+92.0%**, avg/p50/p99 span
**-43.7%/-44.7%/-47.8%**, server CPU **-35.9%**다. 최초 baseline 대비로는
remote throughput **+181.4%**, server CPU **-58.9%**다.

| 단계 | memtier load ops/s | end-to-end avg | p50 | p99 |
|---|---:|---:|---:|---:|
| 0 baseline + span-v2 | 842,573 | 4.925 | 4.863 | 7.423 |
| 1 plaintext pool | 1,230,083 | 3.255 | 0.423 | 14.399 |
| 2 crypto reuse | 2,397,173 | 1.693 | 0.455 | 7.231 |

이 표의 latency 단위는 `ms`이며 보조값이다. raw:

- `rdma-results/cpu-optimization-20260724/cpu-opt-spanv2-baseline-r2-20260724/`
- `rdma-results/cpu-optimization-20260724/cpu-opt-spanv2-pool-r2-20260724/`
- `rdma-results/cpu-optimization-20260724/cpu-opt-spanv2-pool-crypto-20260724/`

## 단계 1 패치 기록

- 변경 파일: `storage.c`
- 핵심 allocation diff: `+37/-5` (`storage.c`; span-v2 계측 제외)
- binary SHA-256:
  `291a9913e2b8359e1040f6ba355e9b0ccd78aa53c7067ae9aae6caea4de24504`
- 기존 `cache_t`와 worker 전용 `do_cache_alloc/free()`를 재사용한다.
- cache는 worker TLS에 최초 GET 시 생성되고 최대 1,024개 slot을 보유한다.
  고정 workload의 nominal in-flight 512/worker에 2배 burst headroom을 둔다.
- `io_pending_storage_t.read_cache`가 실제 allocator를 기억하므로 callback은
  IO thread에서 실행돼도 반환은 기존처럼 worker의 finalize에서 수행된다.
- pool miss 또는 oversized item은 기존 slab allocator로 fallback하며
  `extstore_plaintext_slab_fallback`에 누적된다.

로컬 gate:

| 검사 | 결과 |
|---|---|
| production build | 통과 |
| `testapp` 56개 | 통과 |
| AES-GCM tamper/torn-read test | 통과 |
| `io_pending_storage_t` ABI 크기 | 176 B, `io_pending_t`와 동일 |
| `git diff --check` | 통과 |
| 1M preload / remote GET hit | 통과 / 68,887,232 of 68,887,232 |
| slab fallback | 0 |
| badcrc / RDMA failure / engine dead | 모두 0 |

## 단계 2 패치 기록

- 변경 파일: `ext_crypto.c`, `test_ext_crypto.c`
- diff: `+24/-8`, test `+2`
- binary SHA-256:
  `6bcf2186659a51d74266182347f13fc20dc8330113dc4dff82f234857196d43f`
- encrypt/decrypt별 TLS `EVP_CIPHER_CTX`를 최초 op에서 만들고 정상 op 사이에
  재사용한다. nonce, key, AAD, tag는 매 op 다시 설정한다.
- 인증 실패나 OpenSSL API 오류가 발생하면 해당 context를 즉시 폐기한다.
  다음 op는 새 context에서 시작하므로 실패 상태가 이어지지 않는다.
- thread 종료 destructor는 두지 않는다. 장수 memcached thread당 context
  최대 2개인 process-lifetime allocation으로 제한된다.

gate:

| 검사 | 결과 |
|---|---|
| good → bad AAD → good 재사용 | 통과 |
| torn ciphertext reject / nonce uniqueness | 통과 |
| `testapp` 56개 | 통과 |
| 4-thread open microbench | 약 2.2µs → 0.28µs/op |
| 1M preload / remote GET hit | 통과 / 131,848,608 of 131,848,608 |
| slab fallback / badcrc / retry | 모두 0 |
| RDMA read/write failure / engine dead | 모두 0 |

하드웨어 내부 decrypt 계측은 1.872→0.707µs/op(-62.2%)였다. 나머지 span은
xfer 41.198µs, sync 4.381µs이며 총 avg 63.386µs와 같은 wall-clock
범주의 값이다. CPU-µs/op와 합산하지 않는다.

## Remote latency 경계와 `<30µs` operating point

GET 시점의 remote object는 SET 때 이미 암호화돼 있다. 따라서 서로 다른
object lifetime을 합치지 않고 방향별로 측정한다.

- SET: AES-GCM seal 시작 → RDMA WRITE CQE
- GET: RDMA READ post 직전 → CQE → `SYNC_FOR_CPU`/private buffer →
  AES-GCM open 완료

GET span은 worker→io enqueue 전 대기와 decrypt 후 worker notify/TCP 응답을
포함하지 않는다. 반면 post된 WQE가 앞선 WQE 뒤에서 기다리는 시간과 CQE
batch에서 자기 decrypt 순서를 기다리는 시간은 실제 완료 지연이므로 포함한다.
이 때문에 throughput-max `pipeline=32`를 intrinsic latency로 해석하면 안 된다.

Phase 2 binary, QP/io=4, depth=64에서 pipeline만 바꾼 결과:

| pipeline | remote GET/s | GET avg | p50 | p99 | post→CQE avg | sync/private | decrypt |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 584,149 | **12.960** | **11.600** | 35.400 | 7.378 | 2.179 | 0.886 |
| 2 | 1,094,250 | **18.649** | **16.100** | 62.300 | 10.393 | 2.659 | 0.840 |
| 4 | 1,836,090 | 33.410 | **27.800** | 98.300 | 19.984 | 3.368 | 0.798 |
| 32 | 2,379,384 | 63.386 | 63.000 | 129.600 | 41.198 | 4.381 | 0.707 |

latency 단위는 `µs`다. `<30µs`는 avg 기준 pipeline≤2, p50 기준
pipeline≤4에서 충족한다. p99 기준은 pipeline=1도 35.4µs이므로 미충족이다.
예전 `<15µs` 기억은 pipeline=1의 avg 12.96µs 및 post→CQE 7.38µs와
일치한다.

추가 raw:

- `rdma-results/cpu-optimization-20260724/cpu-opt-spanv2-pool-crypto-pipe1-20260724/`
- `rdma-results/cpu-optimization-20260724/cpu-opt-spanv2-pool-crypto-pipe2-20260724/`
- `rdma-results/cpu-optimization-20260724/cpu-opt-spanv2-pool-crypto-pipe4-20260724/`

### Pipeline–QP 동시 스케일링

`128 connections × pipeline ÷ QP`를 일정하게 유지하면 QP당 queueing을
제한할 수 있다. 다만 현재 구현은 `QP 1개 = busy-poll io thread 1개`이므로
QP 증가가 server CPU 경쟁도 동시에 늘린다.

| pipeline | QP/io threads | remote GET/s | GET avg | p50 | p99 | server CPU-µs/op |
|---:|---:|---:|---:|---:|---:|---:|
| 2 | 4 | 1,094,250 | 18.649 | 16.100 | 62.300 | 7.721 |
| 2 | 8 | 1,011,985 | 18.212 | 15.300 | 66.600 | 9.302 |
| 4 | 4 | **1,836,090** | 33.410 | 27.800 | 98.300 | 5.037 |
| 4 | 8 | **1,702,230** | **28.715** | **22.800** | 112.600 | 5.996 |
| 4 | 16 | 1,637,705 | 37.751 | 28.200 | 153.500 | 6.362 |

latency 단위는 `µs`다. QP=8은 pipeline=4의 avg를 30µs 아래로 낮추며,
기존 avg-qualified 최대점 `(pipeline=2, QP=4)`보다 remote throughput이
55.6% 높다. QP=16은 8 worker + 16 busy-poll io thread가 16 server vCPU를
초과해 throughput과 latency가 모두 악화된다. 따라서 현 구조와 CPU
partition에서 avg `<30µs` 최대 실측점은 **pipeline=4, QP=8,
1.702M remote GET/s, avg 28.715µs**다. p99 `<30µs`는 여전히 미충족이다.

추가 raw:

- `rdma-results/cpu-optimization-20260724/cpu-opt-balanced-pipe2-qp8-20260724/`
- `rdma-results/cpu-optimization-20260724/cpu-opt-balanced-pipe4-qp8-20260724/`
- `rdma-results/cpu-optimization-20260724/cpu-opt-balanced-pipe4-qp16-20260724/`

### 최적점 최소 패치

고정 운영점 `(mcT=8, mtT=8×c16, pipeline=4, QP=8, depth=64)`에서만
단계별로 겹쳐 측정했다.

| 단계 | remote GET/s | GET avg | p50 | p99 | worker CPU | io CPU | server CPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| 최적점 기준선 | 1,702,230 | 28.715 | 22.800 | 112.600 | - | - | 5.996 |
| 3a worker→QP affinity | 1,703,345 | 27.836 | 22.600 | 103.300 | 3.520 | 2.313 | 5.833 |
| 3b + IO owner lock 제거 | **1,724,687** | **26.768** | **21.800** | **97.900** | **3.492** | **2.299** | **5.791** |

latency와 CPU 단위는 `µs/op`다. 3a는 각 worker가 최초 선택한 QP를 계속
사용해 모든 worker가 모든 QP queue mutex를 건드리던 round-robin을 없앴다.
3b는 IO thread만 접근하는 `outstanding`과 `bounce_free`의 self-lock만
제거했고 producer/consumer queue mutex와 condition variable은 유지했다.
기준선 대비 remote throughput은 1.3%, avg/p50/p99는 각각
6.8%/4.4%/13.1%, server CPU는 3.4% 개선됐다.

binary와 raw:

- 3a: `70081df0…bbcba`,
  `rdma-results/cpu-optimization-20260724/cpu-opt-pipe4-qp8-qpaff-20260724/`
- 3b: `564505f4…cf33`,
  `rdma-results/cpu-optimization-20260724/cpu-opt-pipe4-qp8-qpaff-iolock-20260724/`

두 단계 모두 1M preload, 100% remote hit, span version 2,
plaintext fallback/badcrc/RDMA failure 0을 통과했다.

3c에서 worker를 CPU 0–7, IO thread를 CPU 8–15에 1:1 고정했지만 remote
throughput이 1.725M→1.567M(-9.1%)으로 감소했다. avg가 22.085µs로 낮아진
것은 처리량 감소로 queueing이 줄어든 결과다. Linux scheduler의 동적 배치가
더 나으므로 thread별 pinning 코드는 폐기하고 process taskset 0–15만 유지한다.
raw:
`rdma-results/cpu-optimization-20260724/cpu-opt-pipe4-qp8-qpaff-iolock-pinned-20260724/`

`ext_io_depth=64`에 맞춰 post/CQ batch cap도 32→64로 늘린 실험은 같은
binary의 cap32 대조와 remote throughput이 1.72844M 대 1.72877M으로
동일했고 server CPU는 5.828 대 5.717µs/op로 더 컸다. 유지할 이득이 없어
코드는 32로 되돌렸다. raw:

- `rdma-results/cpu-optimization-20260724/cpu-opt-pipe4-qp8-qpaff-iolock-batch64-20260724/`
- `rdma-results/cpu-optimization-20260724/cpu-opt-pipe4-qp8-qpaff-iolock-batch32-control-20260724/`

CQ batch별 shared stats mutex를 per-IO counter로 바꾼 실험도 remote
1.716M/s, server CPU 5.808µs/op, avg/p99 27.495/101.2µs로 기준보다
개선되지 않아 되돌렸다. raw:
`rdma-results/cpu-optimization-20260724/cpu-opt-pipe4-qp8-qpaff-iolock-pstats-20260724/`

### Client vCPU를 server로 재배치

기존 `(pipeline=4, QP=8)`에서 memtier 8 threads를 CPU 16–23, server를
CPU 0–15에 배치했다. memtier를 4 threads × 32 clients로 바꿔 총
128 connections를 유지하고 CPU 20–23에 배치한 뒤, CPU 16–19를 server에
넘겨 CPU 0–19를 사용했다.

| server CPUs | mc workers | io/QP | memtier | remote GET/s | GET avg | p50 | p99 | server CPU-µs/op |
|---|---:|---:|---|---:|---:|---:|---:|---:|
| 0–15 | 8 | 8 | 8×16 | **1,702,230** | 28.715 | 22.800 | 112.600 | 5.996 |
| 0–19 | 8 | 8 | 4×32 | 992,015 | **16.204** | 14.700 | 43.000 | 8.391 |
| 0–19 | 12 | 8 | 4×32 | 881,475 | 17.527 | 16.100 | 48.300 | 10.163 |

4-thread memtier는 connection과 pipeline이 같아도 약 1M ops/s에서 먼저
포화됐다. 낮아진 remote latency는 server 개선이 아니라 offered load 감소의
결과다. 이 상태에서 worker를 8→12로 늘리면 throughput은 11.1% 감소하고
CPU-µs/op도 증가한다. 따라서 이 실험으로 server 20-vCPU의 최대 성능을
판정할 수 없으며, off-box client 또는 더 효율적인 load generator가 필요하다.

추가 raw:

- `rdma-results/cpu-optimization-20260724/cpu-opt-pipe4-qp8-mct8-mtt4-c32-s20-20260724/`
- `rdma-results/cpu-optimization-20260724/cpu-opt-pipe4-qp8-mct12-mtt4-c32-s20-20260724/`

### 10초 전체 구성 스윕

같은 `564505f4…cf33` binary로 QP/ext, pipeline, thread 조합, depth와 stock
local control을 각각 10초 측정했다. Port headline은 remote completion
count와 span-v2이며 decrypt를 포함한다.

avg `<30µs`에서 thread 조합의 최대점은 기존과 같은
`mtT=8, mcT=8, QP/ext=8, pipeline=4`의 1.740M/s, avg 26.940µs였다.
이 조합의 depth만 바꾸면 depth=16이 1.737M/s, avg 21.048µs로
depth=64의 1.632M/s, avg 27.603µs보다 좋았다. 따라서 10초 quick-run의
후속 후보는 depth=16이다. 전체 표, 그래프, raw 선택 기준은
`md/CONFIG_MATRIX_10S_20260724.md`에 기록했다.

후속 7-point frontier에서 depth=16을 유지하고 pipeline을 8로 올린
`mtT=8, mcT=8, QP/ext=8`이 remote **2.445M/s**, avg **22.252µs**,
p99 **59.600µs**를 기록했다. 기존 optimal 반복 중앙값보다 throughput
41.4% 증가, avg/p99 18.4%/41.0% 감소다. 같은 pipeline=8 stock local은
3.371M/s였고 Port는 그 72.54%에 도달했다. 상세 비교와 10M 목표의 적용
경계는 `md/FRONTIER_7POINT_20260724.md`에 기록했다.

하드웨어 gate와 성능값은 Genie `10.99.0.2:11212`가 재기동된 뒤 기록한다.
단계 1 측정 전에는 단계 2 코드를 겹치지 않는다.

Genie 응답 gate: virgin 4 GiB `genie_memd`가 PID 311542로 재기동됐고
fabric token이 Ariel에 전달됐다. TCP `/dev/tcp` 결과는 RDMA-CM readiness
판정에 쓰지 않고 실제 `rdma_connect`와 1M preload 성공으로 검증한다.

## 중단 조건

- preload가 1,000,000 item에 도달하지 않음
- hit rate가 100%가 아님
- `badcrc`, RDMA read/write failure, engine dead 중 하나라도 증가
- latency가 악화됐는데 CPU 감소로 설명되지 않음

중단 조건이 발생하면 다음 패치를 겹치지 않고 해당 단계만 되돌려 원인을
확인한다.
