# extstore → RDMA 원격 메모리 백엔드 포팅 범위 검토

> 작성 2026-07-23, 성능 기준으로 개정.
> 대상: memcached-1.6.42. 코드 독해 노트는 별도 문서 `EXTSTORE_READING.md` (독립 관리).
>
> **목표**: extstore의 저장 위치를 로컬 flash → 원격 서버 메모리(RDMA)로 교체.
> **판단 기준: 성능 최우선.** diff 크기나 구현 난이도는 tie-breaker로만 쓴다.

## 근거 등급 표기

이 문서의 주장은 근거 강도가 제각각이다. **섞어서 인용하지 말 것.**

| 태그 | 의미 |
|---|---|
| `[확인]` | 이 저장소 코드에서 직접 확인. 파일·라인 명시 |
| `[산술]` | `[확인]`된 값에서 단순 계산 |
| `[추정]` | 일반적으로 알려진 수치 기반. **이 하드웨어에서 미측정** |
| `[가설]` | `[추정]`에서 유도한 판단. 측정 전까지 결론 아님 |

**`[추정]`·`[가설]`은 논문·보고서에 그대로 쓰면 안 된다.**
§13의 계측 단계를 거쳐 실측치로 대체할 것.

## 0. 전제: 현재 구조

- **`extstore.c` / `extstore.h`** — memcached를 모르는 범용 페이지 기반 저장 엔진.
  include가 `config.h` + libc뿐이다(`extstore.c:3-19`). 층 분리는 실재한다
- **`storage.c` / `storage.h`** — memcached와 엔진을 잇는 glue.
  flush 정책, GET 처리, recache, compaction 정책, 설정 파싱

읽기 경로: GET이 `ITEM_HDR`를 만나면 `storage_get_item`(`storage.c:251`)이 `obj_io`를
만들어 per-thread IO 큐에 넣고, IO 스레드가 pread 후 콜백. 상세는 독해 문서 참조.

---

## 1. `[가설]` 병목은 저장 매체가 아니라 핸드오프 경로로 이동한다

> **이 절 전체가 가설이다.** 경로는 코드로 확인했으나 **비용은 이 하드웨어에서
> 측정하지 않았다.** §13의 계측을 통과하기 전까지 결론으로 쓰지 말 것.

### 1-1. 경로 `[확인]`

읽기 1회가 거치는 단계. 모두 코드에서 확인했다.

| # | 단계 | 위치 |
|---|---|---|
| 1 | io_pending 구성, 큐 삽입 | `storage.c:251-352` |
| 2 | `pthread_cond_signal`로 IO 스레드 깨움 | `extstore.c:703` |
| 3 | IO 스레드가 `cond_wait`에서 깨어남 | `extstore.c:839` |
| 4 | 실제 IO (`pread` → RDMA READ로 교체 대상) | `extstore.c:915` |
| 5 | 콜백 → `return_io_pending` | `thread.c:822` |
| 6 | eventfd write — **큐가 비어있을 때만** | `thread.c:833-837` |
| 7 | 워커 스레드 libevent 깨어나 큐 드레인 | `thread.c:565-581` |
| 8 | conn 상태머신 재개 | `memcached.c:606`, `:3334` |

`[확인]` 6번의 조건부 syscall은 실재한다 — `STAILQ_EMPTY` 검사 후에만 write한다
(`thread.c:826-834`). 즉 **고부하에서는 syscall이 상각되고, 무부하 단일 요청에서는
전 비용을 낸다.** 무부하 p50/p99가 헤드라인 수치인 실험이라면 이 차이가 중요하다.

### 1-2. 비용 `[추정]` — 미측정

아래는 일반적으로 알려진 크기이며 **이 하드웨어에서 확인한 값이 아니다.**

| 구간 | 추정 비용 | 근거 |
|---|---|---|
| 3, 7 (컨텍스트 스위치 ×2) | ~1-3µs each | 일반적 리눅스 스레드 wakeup |
| 6 (eventfd write) | ~0.5-1µs | syscall 왕복 |
| 2, 5 (mutex) | ~100ns each | 무경합 기준 |
| 4 (RDMA READ) | ~2µs | 일반적 IB/RoCE one-sided read |
| 4 (기존 flash pread) | ~100µs | NVMe 일반값 |

### 1-3. 유도되는 가설

`[가설]` 위 추정이 맞다면, 매체를 flash→RDMA로 바꿨을 때 실제 데이터 전송(4번)이
~2µs인 반면 핸드오프(2,3,5,6,7)가 그와 같거나 더 크다.
즉 **비동기 기계가 100µs를 숨기려고 설계된 것이라, 50배 빠른 매체에서는
기계 자체가 지배적 비용이 된다.**

`[확인]` 이 가설의 *구조적* 근거는 확실하다: IO 스레드 모델은 블로킹 pread를
전제로 존재하며(`extstore.c:839`), 그 전제가 RDMA에서 사라진다.
**불확실한 것은 방향이 아니라 크기다.**

---

## 2. `[가설]` 성능 영향 순위 (큰 것부터)

> **순위 자체가 §1의 추정에 의존한다.** 계측 후 재정렬될 수 있다.
> 특히 Tier 1과 Tier 4의 상대 순서는 실측 없이는 확정 불가.

| Tier | 항목 | 근거 강도 | 수정 위치 |
|---|---|---|---|
| **1** | 비동기 핸드오프 제거 (인라인 폴링) | `[가설]` 방향은 확실, 크기 미측정 | storage.c, thread.c, memcached.c |
| **2** | `io_depth`/`io_threadcount` + 배치 post | `[확인]` 현재 값이 1로 고정됨 | storage.c, extstore.c |
| **3** | compaction 제거 (페이지 내 free list) | `[확인]` 네트워크 읽기 증폭 존재<br>`[가설]` 그 크기는 워크로드 의존 | extstore.c, storage.c |
| **4** | zero-copy 목적지 (MR 등록 방식) | `[확인]` 복사 유무는 확정<br>`[가설]` 상대 비중은 값 크기 의존 | slabs.c, storage.c |
| **5** | hugepage 기반 MR | `[산술]` MTT 엔트리 수<br>`[가설]` 실제 지연 영향 | slabs.c 확인 |

**이전 판(2026-07-23 초판)에서 MR 등록을 헤드라인으로 올린 것은 순위 오류였다.**
그 판단은 diff 크기를 기준으로 삼았고, 성능 기준으로는 Tier 4에 해당한다.
다만 **Tier 1이 더 크다는 것 역시 아직 가설**이라는 점을 함께 유지할 것.

---

## 3. Tier 1 — 비동기 핸드오프 제거

### 문제 `[확인]`

IO 스레드 모델(`extstore.c:839` `extstore_io_thread`)은 **블로킹 pread를 전제**한다.
워커 스레드를 오래 묶어두지 않으려고 별도 스레드로 넘기는 구조다.
RDMA에서는 그 전제가 사라진다 — 이건 구조적 사실이다.

`[가설]` 핸드오프 비용이 대기 비용보다 비싸진다. **크기는 미측정.**

### 방향

워커 스레드에서 직접 `ibv_post_send`(RDMA READ) 후 CQ를 인라인 폴링하고 리턴.
성립하면 §1-1 표의 2,3,5,6,7번이 통째로 사라진다.

`[가설]` 워커를 READ 지연만큼 묶지만 그게 컨텍스트 스위치 2회보다 싸다.
**이 부등식이 이 방향 전체의 전제이며, §13-2 계측의 핵심 검증 대상이다.**
부등식이 뒤집히면 Tier 1은 통째로 폐기된다.

### 대가와 미해결점

- 워커 스레드가 폴링 중엔 다른 연결을 처리 못 한다.
  `[가설]` 소형 값 단일 GET이면 감내 가능한 수준이겠으나,
  **다중 GET(mget)이나 대형 값에서는 점유 시간이 길어져 재검토 필요.**
  감내 가능 여부의 판단 기준 자체가 미측정이다
- QP를 워커 스레드마다 하나씩 두어야 한다 (QP는 동시 post에 thread-safe하지 않음).
  워커 수 = QP 수 = 원격 노드의 QP 부담
- 하이브리드 가능: 짧은 스핀 폴링 후 미완료면 기존 비동기 경로로 폴백.
  최선의 경우 인라인 지연, 최악의 경우 현행 유지
- **conn 상태머신을 건드려야 한다** (`memcached.c:3279` `conn_io_queue` 진입 자체를
  건너뛰는 경로). 4파일 범위 밖이며, MR 문제보다 훨씬 침습적

### 측정 계획

인라인 폴링 없이 먼저 포팅해 baseline을 잡고, 위 표의 2/3/5/6/7 구간을
`rdtsc`로 계측해 실제 핸드오프 비용을 확정한 뒤 착수. 추정치로 설계하지 말 것.

---

## 4. Tier 2 — 파이프라이닝: 구조가 이미 배치를 넘겨준다

**`io_depth`가 1로 하드코딩**(`storage.c:1338`), `io_threadcount` 기본 1(`:1337`).
이대로면 모든 RDMA READ가 스레드 하나의 CQ로 직렬화된다. 처리량 하드 실링.

반면 **공짜로 얻는 것**이 있다. `storage_submit_cb`(`storage.c:381-395`)는
이미 io_pending들을 **obj_io 체인**으로 묶어서 넘기고,
`_extstore_submit`(`extstore.c:678-684`)은 체인 길이를 `depth`로 센다.

→ 이 체인이 **linked WR 리스트에 1:1로 매핑된다.**
`ibv_post_send` 한 번으로 N개 READ를 올리고 **doorbell 한 번**.
기존 구조가 배치 인터페이스를 이미 갖고 있으므로 추가 설계가 필요 없다.

단, Tier 1(인라인 폴링)을 택하면 IO 스레드가 사라져 `io_threadcount`는 무의미해진다.
**Tier 1과 Tier 2는 함께 결정해야 한다.**

---

## 5. Tier 3 — compaction의 존재 이유가 원격 DRAM에서 사라진다

### 사실관계 `[확인]`

compaction은 **페이지를 통째로 네트워크로 읽어와**(`storage.c:932`
`storage_compact_readback`, `settings.ext_wbuf_size` 단위로 반복) 살아있는 객체만
다시 쓴다. flash에서는 필수다 — erase block 때문에 구멍을 재사용할 수 없다.

원격 DRAM엔 그 제약이 없다. 임의 offset에 in-place 덮어쓰기가 된다.

`[가설]` 따라서 원격 DRAM에서 compaction은 불필요한 네트워크 왕복을 만든다.
**다만 그 빈도와 총량은 워크로드(삭제율·TTL 분포)에 전적으로 의존하므로,
"순수 낭비"라고 단정하려면 실측이 필요하다.**

### 유지 vs 제거

| | 유지 | 제거 (페이지 내 free list) |
|---|---|---|
| diff | 최소, 8-1 범위 유지 | extstore.c 자료구조 + storage.c compaction 제거 |
| 얻는 것 | `page_version` ABA 가드를 그대로 물려받음 | compaction 네트워크 증폭 제거, 스레드 1개 감소 |
| 잃는 것 | 원격 DRAM 재기록이라는 불필요 작업 지속 | **객체 단위 무효화 수단을 새로 설계해야 함** |
| 근거 | `[확인]` 현행 동작 | `[가설]` 이득 크기 미측정 |

### 핵심 통찰

`extstore.h:15-20` 주석의 "구멍은 페이지가 evict될 때까지 재사용 불가"는
**flash 제약이지 설계 필연이 아니다.** 이 제약을 걷어내면:

- **페이지 내 free list**를 추가 → 구멍을 즉시 재사용 → **compaction 자체가 불필요**
- 원격 메모리 할당자를 처음부터 만드는 것보다 훨씬 작은 변경.
  페이지/버킷 구조와 `page_version` ABA 가드를 그대로 유지한 채,
  `extstore_delete`(`extstore.c:724`)가 카운터만 줄이는 대신 free list에 넣게 하면 된다
- 얻는 것: compaction의 네트워크 읽기 증폭이 **0이 된다**.
  `storage.c`의 compaction 400여 줄과 전용 스레드도 사라진다

### 남는 문제

- 가변 크기 객체의 free list는 단편화를 낳는다. size-class 방식(memcached 슬랩과 동일한
  발상)으로 완화 가능하지만, 이건 **원격 메모리 할당자를 만드는 것과 경계가 모호해진다**
- `page_version`은 페이지 재활용 시점의 ABA 가드다. in-place 재사용을 도입하면
  **객체 단위 무효화 수단이 따로 필요**하다. 이게 이 방향의 진짜 난제
- → 먼저 free list 없이 포팅해 compaction 빈도를 측정하고,
  실제 네트워크 증폭이 유의미할 때 착수. 다만 **설계 시 이 방향을 막지 말 것**

---

## 6. Tier 4 — MR 등록: (a)가 맞지만 이유가 다르다

`storage.c:350` → `eio->buf = (void *)new_it`.
`new_it`은 `do_item_alloc_pull()`이 준 **슬랩 메모리**(`storage.c:271`)인데
등록돼 있지 않다. RDMA READ 목적지는 등록된 MR이어야 한다.

(쓰기 쪽은 문제없다. wbuf는 `extstore.c:127` `wbuf_new`가 malloc하므로 extstore.c 안에서 끝난다.)

### 성능 기준 비교

| 방식 | 복사 `[확인]` | 추가 비용 `[추정]` | 판정 |
|---|---|---|---|
| **(a) `-L` preallocate + 단일 MR** | 없음 | 없음 | **채택** |
| (d) 슬랩 증가분마다 개별 MR | 없음 | IO마다 addr→lkey 조회, MR 수 증가 | `-L` 불가 시 대안 |
| (b) bounce buffer + memcpy | **1회** | 값 크기 비례 | 회피 |
| (c) ODP (`IBV_ACCESS_ON_DEMAND`) | 없음 | HCA 페이지 폴트 | **기각** |

`[확인]` 복사 유무는 확정적이다 — (b)만 memcpy가 추가된다.
`[가설]` 그 복사가 전체 지연에서 차지하는 비중은 **값 크기에 비례**하므로,
`ext_item_size` 기본값 512B 근방의 소형 값에서는 작고 대형 값에서는 커진다.
정확한 손익분기는 측정 대상이다.

`[추정]` (c) ODP는 첫 접근 시 HCA 페이지 폴트가 발생하며, 이는 RDMA READ 자체보다
수십 배 비싼 것으로 알려져 있다. 꼬리 지연이 중요한 실험에서는 부적합.
**이전 판에서 (c)를 중립적 대안으로 적은 것은 잘못이다** — ODP는 메모리 풋프린트
유연성을 위한 기능이지 성능 기능이 아니다.
다만 이 기각 역시 `[추정]` 근거이므로, ODP를 꼭 써야 할 이유가 생기면 실측할 것.

**(a)의 진짜 근거는 zero-copy와 등록 1회다.** diff 크기가 아니다.
부수 효과로 chunked item의 iovec(`storage.c:305`)도 모든 청크가 같은 lkey를
공유하므로 sge 리스트로 그대로 매핑된다 — (d)였다면 청크마다 lkey 조회가 필요했다.

### 구현 메모

- `slabs.c:214-216`: `prealloc`이면 `mem_base = alloc_large_chunk(mem_limit)` → 연속 단일 영역
- `slabs_init`(`memcached.c:5966`)이 `storage_init`(`:5970`)보다 **먼저** 돌아 순서는 이미 맞다
- slabs.c에 `mem_base` 접근자 추가 필요 (5번째 파일)
- **전제: `-L` 강제.** 없으면 `memory_allocate`(`slabs.c:613`)가 점진 확장하므로 (d)로 강등

---

## 7. Tier 5 — hugepage MR: (a)의 숨은 전제

`alloc_large_chunk`(`slabs.c:135-142`, Linux)는 `posix_memalign` 후
`madvise(MADV_HUGEPAGE)`를 건다. **THP 힌트이지 보장이 아니다.**

이게 왜 성능 문제인가 — MR의 주소 변환 엔트리(MTT) 수 `[산술]`:

| 페이지 크기 | 64GB MR의 엔트리 수 |
|---|---|
| 4KB | 약 16.7M (64GiB ÷ 4KiB) |
| 2MB (THP) | 32,768 (64GiB ÷ 2MiB) |

`[추정]` HCA의 주소변환 캐시가 미스나면 접근마다 PCIe 왕복이 추가된다.
엔트리 수가 3자릿수 차이나므로 캐시 히트율 차이는 크겠지만,
**실제 지연 기여도는 HCA 모델과 접근 패턴에 따라 다르므로 측정 없이 단정 불가.**

→ **기동 후 `/proc/meminfo`의 `AnonHugePages`로 THP가 실제로 붙었는지 먼저 확인할 것.**
`madvise(MADV_HUGEPAGE)`는 힌트라서 조용히 실패할 수 있다.
안 붙으면 명시적 hugetlbfs 할당으로 교체를 검토.

---

## 8. 범위 판정

### 8-1. 원래 가설에 대한 답 `[확인]`

**가설**: extstore.c/.h + storage.c/.h 4개 파일이 수정 범위.
**판정: 기능적으로는 거의 맞다. 단 1건이 범위 밖.**

| 항목 | 어디로 가는가 | 4파일 내? |
|---|---|---|
| QP/CM 연결 수립, CQ 폴링 | `extstore.c` (`extstore_init`, `extstore_io_thread`) | O |
| 원격 데이터 레이아웃 (page → remote addr) | `extstore.c` | O |
| wbuf → RDMA WRITE 소스 버퍼 등록 | `extstore.c:127` `wbuf_new` | O |
| `ext_path=host:port` 파싱 | `storage.c:1235` `storage_conf_parse` | O |
| io_pending ↔ obj_io 변환 | `storage.c:381` `storage_submit_cb` | O |
| **RDMA READ 목적지 버퍼 등록** | **`slabs.c`** (§6) | **X** |

즉 **"돌아가게만 만드는" 최소 포팅은 4파일 + `slabs.c` 소폭 수정으로 가능하다.**

### 8-2. 성능 목표를 넣으면 더 넓어진다 `[가설]`

| 파일 | 무엇 때문에 | 근거 |
|---|---|---|
| `extstore.c` / `.h` | QP/CM 셋업, CQ 폴링, 원격 주소 매핑, wbuf MR, (Tier 3 시) free list | `[확인]` |
| `storage.c` / `.h` | 설정 파싱, obj_io 변환, `io_depth`, (Tier 1 시) 인라인 폴링 진입 | `[확인]` |
| `slabs.c` | Tier 4: `mem_base` 노출 + MR 등록 | `[확인]` |
| `thread.c` | Tier 1: IO 큐 반환 경로 우회 | `[가설]` Tier 1 채택 시에만 |
| `memcached.c` | Tier 1: `conn_io_queue` 상태 진입을 건너뛰는 경로 | `[가설]` Tier 1 채택 시에만 |

→ **Tier 1을 채택하면 범위가 `thread.c`·`memcached.c`까지 넓어진다.**
다만 Tier 1 자체가 계측 대기 중인 가설이므로, **§13-1 baseline 단계에서는
8-1의 좁은 범위로 진행하는 것이 맞다.** 범위 확대는 계측 결과가 정당화한 뒤에.

---

## 9. fork 순서는 안전 (확인함)

ibv 컨텍스트는 fork를 넘어 생존하지 못한다. 순서 확인:
`daemonize()`(`memcached.c:5867`) → `slabs_init()`(`:5966`) → `storage_init()`(`:5970`).
**데몬화가 먼저**라 ibv 리소스는 fork 이후 생성된다. 이후 fork 없음.

---

## 10. 그 밖에 놓치기 쉬운 것

- `_read_from_wbuf`(`extstore.c:815`) — 아직 안 보낸 데이터는 로컬 히트.
  RDMA를 타면 안 된다. **성능상으로도 유지가 이득**
- `storage.c:165` crc32c 검증 — 원격 저장이면 무결성 요구가 강해진다.
  다만 이건 CPU 비용이므로 Tier 1/2 개선분을 갉아먹는다. MAC으로 대체 시
  비용 계측 필요 (Port C/D 경험 연결 지점)
- `extstore_write_request`는 **성공 시 page mutex를 잡은 채 리턴**하고
  `extstore_write`가 푼다(`extstore.c:636`, `:673`). 쓰기 경로 수정 시 이 짝을 깨지 말 것.
  또한 **락 보유 구간에 네트워크 IO를 넣으면 안 된다** — 현재는 memcpy만 하므로 짧다
- `extstore_get_page_data`가 `_free_page`를 겸함(`extstore.c:220`) — 페이지 수명 재설계 시 주의
- wbuf 배칭(4MB)은 flash 산물이지만 RDMA에서도 **대역폭 측면에선 이득**
  (적고 큰 WRITE). 다만 flush 지연이 생기므로 지연 vs 대역폭 트레이드오프로 별도 판단

---

## 11. 기존 포트(`memcached-ported_D`)와의 관계

`memcached-ported_D/rdma.c`(1266줄)는 **다른 축**이다:
`rdma_server_start` / `rdma_conn_init(conn *c)` / `rdma_conn_readable(conn *c)` —
클라이언트↔memcached **전송 계층 교체**(SEND/RECV 양면, MAC 프레이밍, libevent 연동).
등록 대상도 고정 send/recv 풀이지 슬랩이 아니다(`rdma.c:326-328`).

- **재사용 가능**: 디바이스 오픈 / PD / CQ / QP 셋업(`rdma_setup_connection`), CQ 폴링 패턴
- **재사용 불가**: conn 관련 전부, SEND/RECV 프레이밍
- 신규 포트는 **one-sided READ/WRITE**가 맞다. 원격 CPU 개입이 없어 지연이 낮고,
  필요한 주소는 `page_id` + `offset`이 이미 제공한다

---

## 12. 열린 질문

1. Tier 1의 인라인 폴링에서 **워커 점유 시간이 다른 연결에 주는 영향** — mget과
   대형 값에서 특히. 하이브리드(스핀 후 폴백) 임계값은 측정으로 정할 것
2. Tier 3에서 in-place 재사용 도입 시 **객체 단위 무효화 수단** 설계.
   `page_version`은 페이지 단위 가드라 그대로는 못 쓴다
3. 원격 노드 장애 시 동작 — 현재 extstore는 IO 실패를 miss로 강등한다.
   RDMA QP 에러 상태에서의 복구/재연결 정책 필요
4. 원격 서버 형태 — one-sided라면 MR 등록 후 유휴로 충분한가, 아니면
   메모리 할당 협상을 위한 제어 채널이 필요한가
5. `-L` 강제가 운영상 수용 가능한가 (Tier 4의 (a) 전제)

---

## 13. 착수 순서 제안

**이 문서의 `[가설]`은 대부분 2단계에서 확정되거나 폐기된다.**
1단계를 건너뛰고 Tier 1부터 하지 말 것 — baseline이 없으면 이후 개선을 주장할 수 없다.

| # | 단계 | 범위 | 산출물 |
|---|---|---|---|
| 1 | **baseline 포팅** — 기존 구조 유지, `pread`를 RDMA READ로만 교체. MR은 (a) | §8-1 (4파일 + slabs.c) | 동작하는 포트 |
| 2 | **계측** — §1-1 표의 각 구간을 `rdtsc`로 측정 | 계측 코드만 | §1-2 추정치를 **실측치로 대체** |
| 3 | **Tier 2** — `io_depth` 상향 + 체인 배치 post | storage.c, extstore.c | 구조 변경 없는 처리량 개선 |
| 4 | **Tier 1** — 2단계가 §3의 부등식을 지지할 때만 인라인 폴링 | §8-2로 확대 | 최대 이득 (가설) |
| 5 | **Tier 3** — compaction 빈도·네트워크 증폭 실측 후 판단 | extstore.c, storage.c | — |

### 2단계에서 반드시 확정할 것

- §3의 부등식: **인라인 폴링 점유 시간 < 컨텍스트 스위치 2회 + syscall**
  → 뒤집히면 Tier 1 폐기
- 무부하 / 고부하 각각의 핸드오프 비용 (§1-1 6번의 syscall 상각 때문에 다르다)
- RDMA READ 실지연 (값 크기별)
- compaction 발생 빈도와 회당 전송량

각 단계마다 수치를 남기고, **확정된 값은 §1-2 표를 갱신해 `[추정]`을 `[측정]`으로 바꿀 것.**
