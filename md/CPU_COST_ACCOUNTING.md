# RDMA extstore CPU 비용 회계와 최적화 우선순위

측정일: 2026-07-24

## Workload와 비교 축

### 공통 workload

| 항목 | 조건 |
|---|---|
| operation | GET-only |
| value / key set | 64 B / 1,000,000 preloaded keys |
| memcached worker | `-t 8` |
| memtier | 8 threads × 16 clients/thread = 128 connections |
| pipeline | 32 |
| CPU 배치 | server CPU 0–15, client CPU 16–23 |
| CPU control | profile 전·후 각각 10초 steady-state `/proc` snapshot |

port와 stock은 같은 client workload, worker 수, CPU 배치로 측정했다. 다만
stock은 RAM memcached이므로 extstore, RDMA QP/depth, AES-GCM 조건 자체가
없다. 따라서 이 비교는 동일한 요청 부하에서 **remote encrypted backend를
추가한 최종 port**와 **stock RAM backend**의 차이다.

### Port 전용 조건

| 항목 | 조건 |
|---|---|
| extstore QP / io thread | 4 / 4, 1:1 |
| `ext_io_depth` | 64 |
| security | AES-256-GCM ON |
| data placement | 전 데이터 remote |
| publication | 1 operation = 1 RDMA transfer |

### 두 비교의 목적

| 비교 | Port | Stock | 목적 |
|---|---:|---:|---|
| 동일 shape, 각자 포화 | 평균 0.828M ops/s | 평균 7.775M ops/s | 최종 throughput 격차 측정 |
| throughput-matched | 0.822–0.834M ops/s | 0.837–0.838M ops/s | 동일 처리율에서 per-op CPU 증가분 측정 |

stock rate-matched run은 connection당 6,400 req/s로 제한했다. 포화 stock의
낮은 CPU-µs/op를 그대로 port의 추가 비용으로 해석하면 batching 효과까지
port 비용으로 잘못 귀속되므로, 원인별 CPU 차분은 rate-matched 결과만 쓴다.

## 결론

동일 shape에서 포화 throughput은 port 평균 **0.828M ops/s**, stock 평균
**7.775M ops/s**로 stock이 약 **9.4배** 높다. throughput을 약 830k ops/s로
맞추면 port server는 **8.48 CPU-µs/op**, stock memcached는
**2.06 CPU-µs/op**를 쓴다. port의 직접 CPU 증가분은
**6.42 CPU-µs/op(4.12배)**다.

증가분의 우선순위는 다음과 같다.

| port 증가 원인 | CPU-µs/op | 증가분 비율 | 근거 |
|---|---:|---:|---|
| GET 임시 plaintext item의 slab alloc/free | **3.82** | **59.5%** | worker futex 및 allocator sample; futex call site 98.8%가 `slabs_alloc`, `slabs_free`, `storage_finalize_cb` |
| AES-256-GCM open | **1.67** | **25.9%** | production CPU-clock profile; exact crypto microbench 범위 0.76(single)–2.05(4-thread contention) |
| 나머지 RDMA/io engine | **0.89** | **13.8%** | io total 2.55에서 GCM 1.67을 제외 |
| 나머지 worker 차분 | 0.05 | 0.8% | 위 항목 외 throughput-matched native 차분 |
| **합계** | **6.42** | **100%** | port 8.48 - native 2.06 |

따라서 첫 패치는 io-thread 전체 삭제가 아니라 **GET plaintext buffer를
global slab allocator에서 분리**하는 것이다. 두 번째는 per-io-thread
`EVP_CIPHER_CTX` 재사용이다. worker-direct와 coherent-MR은 그 뒤다.

## Throughput 9.4배 차이의 구성

동일 shape에서 각 구현이 도달한 operating point를 비교하면 다음 관계가
성립한다.

| 항목 | 계산 | 배수 |
|---|---:|---:|
| 실제 throughput 격차 | 7.775M / 0.828M | **9.39배** |
| port의 직접 per-op CPU 증가 | 8.48 / 2.06 | **4.12배** |
| stock의 고부하 batching/amortization | 2.06 / 0.90 | **2.29배** |
| CPU 회계상 합성 | 4.12 × 2.29 | **9.43배** |

즉 약 9.4배를 전부 하나의 고정된 port CPU overhead로 보면 안 된다.
동일 처리율에서 확인되는 직접 CPU 회귀는 4.12배이고, stock은 0.84M에서
7.78M으로 부하가 올라가면서 syscall, packet, event 처리 등을 더 잘
상각해 CPU-µs/op가 2.06에서 0.90으로 추가 하락한다. 반대로 port는 높은
per-op 비용과 stage 직렬화·경합 때문에 그 batching operating point에
진입하지 못한다.

이 분해는
`8.48 / 0.90 = (8.48 / 2.06) × (2.06 / 0.90)`이라는 **operating-point
회계**다. 두 배수가 서로 독립된 원인이라는 실험적 증명은 아니다. batching
효과 자체도 처리율, 큐잉, 경합 상태에 의해 달라진다.

### 사용 코어 환산

| 대상 | throughput | server CPU-µs/op | 환산 server core |
|---|---:|---:|---:|
| port | 0.828M | 8.48 | **7.02** |
| stock rate-matched | 0.838M | 2.06 | **1.73** |
| stock saturated | 7.775M | 0.90 | **7.00** |

port와 포화 stock 모두 서버 전체로는 약 7 core-seconds/s를 소비하지만,
같은 CPU량으로 처리하는 요청 수가 약 9.4배 다르다. 동시에 port가 배정된
16개 server CPU를 모두 소진한 것은 아니다. 따라서 현 ceiling은 단순한
총 코어 부족만이 아니라 특정 stage의 높은 per-op 비용, lock/futex 경합,
io stage 분리와 batching 부족이 결합된 결과다. 특히 futex에서 잠들어
기다린 wall time은 `/proc` CPU 시간에 포함되지 않으므로, 해당 경로의
throughput 영향은 CPU-µs/op만으로 보이는 것보다 클 수 있다.

## 측정 방법

CPU 총량은 PMU나 wall-clock span이 아니라 `/proc/<pid>/task/<tid>/stat`의
`utime`, `stime` 차분을 완료 GET 수로 나눴다.

함수·syscall 내부 비율은 vPMU를 쓰지 않는 software CPU-clock
`bpftrace profile:hz:249` stack sample로 구했다. profile 자체가 늘린 CPU는
총량에 사용하지 않고, profile 앞뒤 10초 control의 `/proc` CPU를 기준으로
sample 비율만 곱했다. syscall 횟수는 `raw_syscalls:sys_enter` tracepoint로
셌다. stack의 `perf_trace_run_bpf*` sample은 계측 오버헤드로 제외했다.

`strace -c`도 보조로 실행했지만 ptrace가 throughput을 worker trace에서
약 321k, io trace에서 약 111k까지 떨어뜨렸다. 따라서 strace 시간 비율은
비용 산정에서 제외하고 원시 파일만 보존했다.

## 실행물과 재현성

| 구분 | 실행 파일 SHA-256 | control throughput |
|---|---|---:|
| port | `3c83d70e14d380ef9bfcb2291f8627e98aec7b15db8827da49139c2db50d2fc1` | 822–834k ops/s |
| stock, rate-matched | `97ceee04532b8bc9ad0e86400ccfaa7d1b7d711866efd83b78696bee24269d93` | 837–838k ops/s |
| stock, saturated same shape | 위와 동일 | 7.62–7.93M ops/s |

원시 결과:

- port authoritative:
  `/home/seonung/2026/rdma-results/cpu-stage-detail-20260724/cpu-stage-port-20260724-detail5/`
- stock rate-matched:
  `/home/seonung/2026/rdma-results/cpu-stage-detail-20260724/cpu-stage-native-20260724-rate820k/`
- stock saturated:
  `/home/seonung/2026/rdma-results/cpu-stage-detail-20260724/cpu-stage-native-20260724-detail3/`
- ptrace 보조:
  `/home/seonung/2026/rdma-results/cpu-stage-detail-20260724/cpu-stage-port-20260724-detail2/`

재현 도구:

- `tools/cpu-stage-detail.sh`
- `tools/cpu-stage-profile.bt`
- `tools/ext-crypto-cost.c`
- `tools/summarize-cpu-profile.py`

## stage 총량

control 앞뒤 두 구간의 CPU-µs/op 평균이다.

| 대상 | stage | usr | sys | 합계 |
|---|---|---:|---:|---:|
| port | worker ×8 | 2.15 | 3.78 | **5.93** |
| port | io thread ×4 | 2.34 | 0.22 | **2.55** |
| port | server 합계 | 4.49 | 3.99 | **8.48** |
| port | memtier ×8 | 1.22 | 0.77 | 1.99 |
| stock rate-matched | worker ×8 | 0.98 | 1.08 | **2.06** |
| stock rate-matched | memtier ×8 | 1.51 | 0.85 | 2.36 |
| stock saturated | worker ×8 | 0.52 | 0.37 | **0.90** |
| stock saturated | memtier ×8 | 0.49 | 0.33 | 0.82 |

stock의 0.90과 2.06 차이는 batching/idle 상태 차이다. port 증가분은
throughput까지 맞춘 2.06을 기준으로 계산했다. client CPU는 server port
증가분에서 제외한다.

## worker 상세

### Port worker CPU

| 영역 | CPU-µs/op | worker 내 비율 |
|---|---:|---:|
| futex | **2.93** | **49.4%** |
| item/slab/hash user | 0.97 | 16.4% |
| sendmsg/TCP kernel | 0.64 | 10.8% |
| allocator/locks user | 0.49 | 8.3% |
| 기타 user | 0.35 | 5.9% |
| response/TCP user | 0.31 | 5.2% |
| 기타 kernel | 0.13 | 2.2% |
| read/TCP + epoll | 0.08 | 1.3% |
| protocol/event loop | 0.03 | 0.5% |

futex CPU sample의 호출 위치:

| 호출 위치 | futex sample | 비율 |
|---|---:|---:|
| `slabs_alloc` | 3,890 | 57.1% |
| `storage_finalize_cb` → `storage_release_pending` → `slabs_free` | 1,475 | 21.7% |
| `slabs_free` | 1,363 | 20.0% |
| 기타 | 80 | 1.2% |

즉 worker sys 3.78의 주원인은 TCP read/send 횟수가 아니라, GET마다
`storage_get_item()`이 `do_item_alloc_pull()`로 임시 `read_it`을 만들고
응답 후 `storage_finalize_cb()`가 이를 `slabs_free()`하는 경로다.

### Worker syscall 횟수

| syscall | port calls/op | stock rate-matched calls/op |
|---|---:|---:|
| futex | **1.9854** | **0.1023** |
| read | 0.0326 | 0.0276 |
| sendmsg | 0.0275 | 0.0276 |
| epoll_wait | 0.0053 | 0.0131 |

read/sendmsg 횟수는 거의 같지만 futex는 port가 약 19.4배다.

## io thread 상세

| 영역 | CPU-µs/op | io 내 비율 |
|---|---:|---:|
| AES-GCM open | **1.67** | **65.3%** |
| 기타 io user | 0.31 | 12.3% |
| completion callback | 0.17 | 6.8% |
| `ioctl`/DMA sync | **0.16** | 6.4% |
| allocator/locks | 0.13 | 5.0% |
| RDMA post/poll에서 식별된 sample | 0.06 | 2.2% |
| yield/futex/write/기타 kernel | 0.05 | 2.0% |

`ioctl`은 0.0323 calls/op, 즉 약 31 ops마다 한 번으로 이미 batch되어 있다.
coherent-MR이 제거할 수 있는 CPU 상한은 현재 약 **0.16µs/op**다.

production `ext_crypto_open()`과 동일 소스를 131 B plaintext/159 B remote
object로 측정한 결과:

| 조건 | open CPU-µs/op | seal CPU-µs/op |
|---|---:|---:|
| 1 thread median | 0.764 | 0.746 |
| 4 threads tight-loop median | 2.048 | 2.315 |
| production profile 추정 | **1.666** | GET-only라 미측정 |

production stack에는 `EVP_CIPHER_fetch`, `CRYPTO_THREAD_read_lock`,
`EVP_CIPHER_CTX_new/free`가 반복해서 나타났다. 현 구현이 op마다 context를
만들고 cipher/provider를 다시 초기화해 4개 io thread가 OpenSSL 내부 lock을
경합한다.

## 패치 우선순위

### 1. Per-worker plaintext pool

`storage_get_item()`의 `do_item_alloc_pull()`과
`storage_release_pending()`의 `slabs_free()`를 hot path에서 제거한다.

최소 설계:

1. worker마다 private plaintext cache를 둔다. 고정 workload에서는
   nominal 512 in-flight/worker의 2배인 최대 1,024 slot을 허용한다.
   각 slot은 `EXT_SLOT_SIZE`이고 MR/shared memory일 필요가 없다.
2. `io_pending_storage_t`가 slot pointer와 index를 보유한다.
3. `storage_get_item()`은 자기 worker freelist에서 slot을 얻어 `read_it`으로
   사용한다.
4. 응답 전송이 끝난 `storage_finalize_cb()`에서 같은 worker freelist로
   반환한다.
5. pool 소진 시 첫 패치는 기존 slab 경로로 fallback하고 fallback count를
   stats에 남긴다. 측정에서 fallback이 보이면 slot 수만 조정한다.

실측 결과: fallback=0, badcrc/RDMA failure=0, remote throughput
845.6k→1,239.3k ops/s(+46.6%), worker CPU 5.813→2.323 CPU-µs/op,
server CPU 8.400→5.388 CPU-µs/op. remote span-v2 avg는
115.717→112.662µs였고 p99는 231.1→248.5µs로 증가했으므로 다음 단계에서도
tail을 함께 추적한다.

### 2. Per-io-thread `EVP_CIPHER_CTX` 재사용

`store_iothr`마다 seal/open context를 한 번 만들고 cipher를 한 번 fetch한다.
각 op는 nonce, AAD, tag와 key/IV 상태를 반드시 새로 설정한다. tag mismatch는
기존처럼 fail-closed이며 context 상태가 다음 op에 누출되지 않아야 한다.

수용 기준: 기존 crypto self-test와 torn/AAD mismatch gate 통과,
4-thread open CPU <1.0µs/op 목표.

실측 결과: TLS context 재사용 diff는 `ext_crypto.c +24/-8`,
`test_ext_crypto.c +2`다. 4-thread microbench open은 약
2.2→0.28 CPU-µs/op, 하드웨어 내부 decrypt span은
1.872→0.707µs/op로 감소했다. plaintext pool 단계 대비 remote throughput은
1.239M→2.379M ops/s, server CPU는 5.388→3.454 CPU-µs/op,
remote span-v2 avg/p50/p99는 112.662/114.0/248.5µs에서
63.386/63.0/129.6µs로 감소했다. 131,848,608 GET 전부 hit였고
badcrc/retry/RDMA failure는 0이었다.

### 3. Worker-direct/io-thread 축소

최적 운영점에서 worker→QP affinity와 IO-owner self-lock 제거까지 적용한
결과 remote throughput은 1.702M→1.725M, server CPU는
5.996→5.791 CPU-µs/op, span-v2 avg/p99는 28.715/112.6→26.768/97.9µs로
개선됐다. queue mutex와 callback/notify는 아직 남아 있다.

worker-direct는 worker 8개만으로 현재 worker+io가 사용하는 약 10개 server
core를 감당해야 하므로 즉시 IO thread 전체를 삭제하지 않는다. worker와 IO를
전용 CPU에 1:1 고정한 실험도 throughput을 9.1% 낮춰 폐기했다. 남은
queue/callback 비용이 실제로 큰 경우에만 구조를 바꾼다.

### 4. coherent-MR

CPU 절감 상한이 약 0.16µs/op이므로 throughput 최적화 우선순위는 낮다.
DMA bounce와 latency span을 줄여야 할 때 별도 kernel track으로 진행한다.

## Latency 계약

CPU 최적화 근거는 이 문서의 CPU-µs/op다. latency span v2는 별도 목표이며,
사용자 결정대로 GET decrypt와 SET encrypt를 포함한다. CPU 비용과 wall-clock
span을 서로 대체해 해석하지 않는다.

구체적인 경계는 SET `seal 시작→WRITE CQE`, GET
`READ post→CQE→SYNC/private copy→open 완료`다. pipeline=32의 GET avg
63.386µs는 post된 WQE 및 completion/decrypt batch queueing을 포함한 포화
지연이다. pipeline=1에서는 post→CQE 7.378µs, 전체 GET avg 12.960µs로
기존 `<15µs` 관측과 일치한다. `<30µs` avg를 유지하는 최대 실측점은
QP=4 고정 시 pipeline=2의 1.094M remote GET/s, avg 18.649µs다.
pipeline과 QP를 함께 조절하면 `(pipeline=4, QP=8)`에서
1.702M remote GET/s와 avg 28.715µs를 달성한다. QP=16은 QP당 busy-poll
thread가 worker와 16 server vCPU를 초과해 avg 37.751µs로 악화됐다.

memtier를 8→4 threads로 줄여 CPU 16–19를 server에 넘긴 실험은 client가
약 1M ops/s에서 먼저 포화돼 server 확장 효과를 판정하지 못했다. 같은
4-thread client에서 worker를 8→12로 늘리자 remote throughput은
0.992M→0.881M으로 감소했다. server 확장 검증에는 off-box client 또는
2M+ ops/s를 공급하면서 별도 CPU를 쓰는 load generator가 필요하다.
