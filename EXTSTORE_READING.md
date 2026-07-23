# extstore 코드 독해 가이드 (memcached-1.6.42)

> line-by-line 독해용 로드맵. 읽은 항목은 체크박스로 진행 상황 기록.
> 라인 번호는 1.6.42 소스 기준 (이 디렉토리).
>
> **이 문서는 순수 코드 독해 전용.** RDMA 포팅 검토는 별도 문서
> `EXTSTORE_RDMA_PORTING.md`에 있으며 서로 독립적으로 관리한다.

## 전체 구조

extstore는 **두 층**이다:

- **`extstore.c`** — memcached를 모르는 범용 페이지 기반 스토리지 엔진.
  페이지 할당, write buffer(wbuf), IO 스레드, 페이지 수명 관리.
- **`storage.c`** — memcached와 엔진을 잇는 glue.
  flush 정책, GET 처리, recache, compaction 정책.

관통하는 질문 3가지 (읽는 내내 유지):

1. **정합성**: 페이지가 재활용되는 동안 날아가던 read는 어떻게 무효화되나?
   → `page_version` 비교 (`extstore_check`, `_storage_get_item_cb`)
2. **RAM에 남는 것**: item 헤더 + 12바이트 `item_hdr` 뿐 — 키는 RAM, 값은 디스크.
3. **비동기 배관**: 워커 스레드 → per-thread IO queue → extstore IO 스레드
   → 콜백 → conn 재개. 이 한 사이클을 `storage_get_item`부터 끝까지
   한 번 손으로 따라가면 전체가 잡힌다.

---

## 0. 공통 기반 — extstore가 올라타는 memcached 기계

extstore 코드를 읽다 보면 설명 없이 튀어나오는 것들. 여기를 먼저 알아야
§3 이후가 읽힌다.

- [x] **전역 `settings`** — 타입 `memcached.h:454`, 실체 `memcached.c:107`,
  공개 `memcached.h:564` `extern`. BSS라 `main()` 전에 0으로 초기화됨
- [x] **슬랩 클래스와 `slabs_clsid`** (`slabs.c:77`) — 크기 → 클래스 id 선형 스캔
- [ ] `slabs_available_chunks` (`slabs.c`) — 클래스별 여유 청크 수
- [ ] `global_page_pool_size` (`slabs.c:548`) — 비어 있는 슬랩 페이지 수
- [ ] **`slab_automove_extstore.c`** — 메모리 압력을 계산해
  `settings.ext_global_pool_min` 수위를 정하는 곳. **§3의 목표치가 여기서 온다**
- [ ] `items.c` LRU 구조 (HOT/WARM/COLD/TEMP) — §3의 `lru_pull_tail` 전제

## 1. 자료구조 (계약부터)

- [x] `extstore.h` 전체 (125줄) — **읽음, 아래 노트 참조**
  - `obj_io` 구조체 (`extstore.h:79`) — **모든 IO의 계약.**
    `mode`(OBJ_IO_READ/WRITE), `page_id`/`page_version`/`offset`/`len`, 완료 콜백 `cb`
  - API 선언 (`extstore.h:107-110`): `extstore_write_request` / `extstore_write` /
    `extstore_submit` / `extstore_submit_bg`
- [ ] `memcached.h:580` — `ITEM_HDR` 플래그: "값은 디스크, RAM엔 헤더만" 표시
- [ ] `memcached.h:677-681` — `item_hdr` 구조체: RAM에 남는 것은
  `page_id / page_version / offset` 단 12바이트
- [ ] `storage.h` 전체 (45줄) — glue 레이어 공개 API

## 2. 초기화

- [ ] `memcached.c:4873` `storage_init_config` →
  `memcached.c:5970` `storage_init` →
  `memcached.c:6037,6041` compact/write 스레드 시작
- [ ] `storage.c:1235` `storage_conf_parse` — `-o ext_path=...` 파싱
- [ ] `storage.c:1583` `storage_init` — `extstore_init` 호출
- [ ] `extstore.c:257` `extstore_init` — 파일 열기, 페이지 배열/버킷 구성,
  IO 스레드 생성. **페이지·버킷·free list 구조가 여기서 다 정해지므로 정독 필수**
- [ ] `thread.c:486-489` — 워커 스레드마다 `IO_QUEUE_EXTSTORE` 큐를
  `storage_submit_cb`와 함께 등록. 비동기 IO 배관의 시작점

## 3. 쓰기 경로 (RAM → 디스크 flush)

- [x] `storage.c:598` `storage_write_thread` — **읽음, 아래 §3 노트 참조.**
  "아이템을 옮기는 스레드"가 아니라 **빈 슬랩 페이지 수를 목표 수위로 유지하는
  피드백 제어기**. 목표치는 이 파일이 아니라 `slab_automove_extstore.c:245`에서 온다
- [ ] `storage.c:498` `storage_write` — **이 파일에서 가장 중요한 함수**
  - `:503` `lru_pull_tail(COLD_LRU, LRU_PULL_RETURN_ITEM)`으로 tail item 획득
  - `:522-526` 버킷 선택: DEFAULT / CHUNKED / LOWTTL
  - `:537-540` `extstore_write_request`로 wbuf 공간 예약 후
    **`buf_it->time = it_info.hv`** — 해시값을 time 필드에 숨김.
    compaction 때 재계산 없이 쓰기 위한 트릭
  - `:560~` 원본 item을 `item_hdr`만 든 작은 item으로 교체
- [ ] `items.c:1072` `lru_pull_tail` — `LRU_PULL_RETURN_ITEM` 분기만 따라가면 됨
- [ ] `extstore.c:591` `extstore_write_request` — 활성 페이지 wbuf에 공간 예약
- [ ] `extstore.c:652` `extstore_write` — 쓰기 커밋
- [ ] `extstore.c:559` `_submit_wbuf` — wbuf 가득 차면 IO 스레드로 제출
- [ ] `extstore.c:467` `_allocate_page`, `extstore.c:500` `_allocate_wbuf`

## 4. 읽기 경로 (GET이 ITEM_HDR를 만났을 때)

- [ ] `proto_text.c:445,1564` — `storage_get_item`이 함수 포인터로 주입됨
  (바이너리 프로토콜은 `proto_bin.c:530`)
- [ ] `storage.c:251` `storage_get_item` — 읽기용 item 할당, `io_pending_t` 구성,
  hdr의 page_id/version/offset을 `obj_io`에 채워 conn의 IO 큐에 적재
- [ ] `thread.c:528` `thread_io_queue_submit` →
  `storage.c:381` `storage_submit_cb` →
  `extstore.c:709` `extstore_submit` / `:677` `_extstore_submit`
- [ ] `extstore.c:839` `extstore_io_thread` — 실제 pread 수행 후 콜백
  - **주의: `extstore.c:815` `_read_from_wbuf`** — 아직 디스크에 안 내려간
    데이터는 write buffer에서 직접 읽는 경로가 따로 있음
- [ ] `storage.c:147` `_storage_get_item_cb` — 읽어온 버퍼의 키/버전 검증
  (페이지가 그새 재활용됐으면 miss 처리), resp에 연결
- [ ] `memcached.c:3279` → `conn_io_queue` 상태 (`:548`, `:3334`),
  `memcached.c:606` `conn_io_queue_return` — IO 완료 후 conn 재개 상태머신
- [ ] `storage.c:398` `recache_or_free` — `:436-447` `ext_recache_rate`
  (기본 1/2000) 확률로 값을 RAM에 되살림

## 5. 삭제·무효화

- [ ] `storage.c:56` `storage_validate_item` / `storage.c:65` `storage_delete`
- [ ] `extstore.c:724` `extstore_delete` — 페이지의 obj/byte 카운트만 감소.
  실제로 지우지 않음 (로그 구조)
- [ ] `memcached.c:1627` — SET으로 덮어쓸 때 기존 ITEM_HDR의 디스크 사본 삭제

## 6. Compaction / 페이지 회수

- [ ] `storage.c:1101` `storage_compact_thread` — 루프
- [ ] `storage.c:798` `storage_compact_check` — `ext_max_frag`(기본 0.8, `:816`)
  기준으로 victim 페이지 선정
- [ ] `storage.c:932` `storage_compact_readback` — **핵심.**
  페이지를 통째로 읽어 각 객체의 숨겨둔 hv(§3에서 time에 넣은 것)로
  해시 테이블을 조회해 살아있는 객체만 재기록.
  extstore가 아닌 storage 레이어가 유효성을 판단하는 구조
- [ ] `storage.c:1086` `_storage_compact_cb`
- [ ] `extstore.c:772` `extstore_close_page`, `:787` `extstore_evict_page`,
  `:429` `_evict_page`, `:952` `_free_page` — 페이지 수명 종료.
  `page_version` 증가가 진행 중인 read를 어떻게 무효화하는지 볼 것

---

## 부록: 주요 튜닝 노브 (기본값은 `storage.c:1319` `storage_init_config`)

| 노브 | 기본값 | 의미 |
|---|---|---|
| `ext_item_age` | UINT_MAX | 이 나이 이상인 item만 flush 대상 |
| `ext_recache_rate` | 2000 | 디스크 hit 1/N 확률로 RAM 복귀 |
| `ext_max_frag` | 0.8 | 페이지 유효 데이터 비율이 이 미만이면 compaction |
| `ext_low_ttl` | - | 곧 만료될 item을 LOWTTL 버킷 페이지로 분리 |

---

# 독해 노트

> 노트 작성 규칙: **각 구조체/함수의 목적을 high level로 먼저 서술한 뒤**
> line별 detail로 내려간다. "이게 왜 존재하는가"가 없으면 필드 나열은 의미가 없다.

## §0 공통 기반

### 전역 `settings` — 어디서 정의되고 언제 채워지나

세 곳에 나뉘어 있다.

| 역할 | 위치 |
|---|---|
| 타입 선언 | `memcached.h:454` `struct settings { ... }` |
| **실체 (전역 객체)** | `memcached.c:107` `struct settings settings;` |
| 외부 공개 | `memcached.h:564` `extern struct settings settings;` |

파일 스코프 전역 → BSS → **`main()` 진입 전에 전부 0**. 그래서 "정의"는 항상 되어
있고, 진짜 문제는 "언제 의미 있는 값이 들어오나"다.

초기화 순서 (전부 워커 스레드 시작 전):

1. BSS 0 초기화 — `main()` 이전
2. `settings_init()` — 호출 `memcached.c:4869`, 본문 `:216`.
   **`ext_*` 필드는 하나도 안 건드린다**
3. `storage_init_config(&settings)` — 호출 `memcached.c:4873`, 본문 `storage.c:1319`.
   여기서 `ext_*` 기본값 전부
4. getopt 루프 → `storage_read_config()` — 호출 `memcached.c:5615`, 본문 `storage.c:1346`
5. `storage_check_config()` — `memcached.c:5679`
6. (한참 뒤) `storage_init()` `:5970` → 스레드 시작 `:6037`, `:6041`

**2단계와 3단계의 분리가 두 층 분리의 증거다.** memcached 코어는 extstore 정책 노브의
존재를 모르고, storage.c가 자기 것만 채운다.

`storage_init_config`는 이름과 달리 **두 가지 일을 한다**:
정책 노브(`ext_item_size` 등)는 인자로 받은 전역 `settings`에 쓰고,
엔진 기하(`page_size` 등)는 자기가 calloc한 `storage_settings.ext_cf`에 채워 반환한다
(`storage.c:1335-1340`). §1에서 정리한 "엔진이 아는 값 vs 정책만 아는 값"의 경계가
이 함수 하나 안에서 물리적으로 갈린다.

예: `ext_item_size` — 필드 선언 `memcached.h:519`,
기본값 512 `storage.c:1322`, 커맨드라인 덮어쓰기 `storage.c:1437`.

### `slabs_clsid` (`slabs.c:77-86`)

*목적: 객체 크기 → 슬랩 클래스 id.* memcached는 임의 크기 malloc을 하지 않고
미리 정해진 크기 계급 중 **요청 크기 이상인 가장 작은 것**을 고른다. 그 고르는 함수.

```c
unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST;                    // 1
    if (size == 0 || size > settings.item_size_max)
        return 0;                                // 0 = 저장 불가
    while (size > slabclass[res].size)           // 선형 스캔
        if (res++ == power_largest)
            return power_largest;
    return res;
}
```

- 클래스 크기는 `slabs_init`이 `factor`(기본 1.25) 등비수열로 생성
  (`slabs.c:255-258`), `slab_chunk_size_max`까지. 최대 64개
  (`MAX_NUMBER_OF_SLAB_CLASSES` = 63+1, `memcached.h:120`)
- **선형 스캔이다.** 그래서 `storage.c:620` 주석이
  "cache per-loop to avoid calls to the slabs_clsid() search loop" — 호출자가 hoisting한다
- **반환 0의 의미가 주석과 어긋난다.** `slabs.c:74` 주석은 "0 means error"인데,
  실제 0은 `size == 0`이거나 `item_size_max` 초과일 때뿐.
  `item_size_max` 이하인데 최대 클래스에도 안 들어가면 `power_largest`를 반환한다 —
  에러가 아니라 chunked item 경로

## §1 `extstore.h` (완료)

### 1-0. 이 헤더가 정의하는 것: 6개 타입 = 3개 접점

extstore.h 전체는 **호출자와 엔진 사이의 접점 3개**만 정의한다.

| 접점 | 언제 | 타입 |
|---|---|---|
| **켤 때** (1회) | init | `extstore_conf` (엔진의 형태) + `extstore_conf_file` (저장 매체) → 실패 시 `extstore_res` |
| **쓰고 읽을 때** (매번) | 런타임 | `obj_io` + `obj_io_mode` + `obj_io_cb` |
| **들여다볼 때** (주기적) | 관찰 | `extstore_stats` (집계) + `extstore_page_data` (페이지별) |

각 구조체의 존재 이유 한 줄 요약:

- **`extstore_conf`** — 엔진의 *물리적 형태*를 init에 한 번 못 박는다. 런타임 불변.
  (페이지 기하 + 버킷 수 + 버퍼 + 동시성)
- **`extstore_conf_file`** — 어느 파일/디바이스에 몇 바이트를 쓸지. `next`로 연결 리스트라
  여러 디바이스 가능. 버킷을 특정 디바이스에 고정하는 통로이기도 함
- **`obj_io`** — 한 번의 IO 요청이자 응답. read는 입력, write는 출력이 되는 필드가 섞여 있음
- **`extstore_stats`** — 엔진 전체 집계. fragmentation을 관찰해 compaction 시점을 정하는 근거
- **`extstore_page_data`** — 페이지 하나하나의 상태. 어느 페이지를 compaction할지 고르는 근거
- **`extstore_res`** — init 실패 사유. 런타임 에러는 여기 없음 (int 0/-1로만 표현)

**핵심 대비**: `extstore_conf`에 들어가는 값(엔진이 아는 것)과 `settings.ext_*`에
남는 값(storage.c만 아는 것)의 경계가 곧 두 층 분리의 실체다.
엔진은 "언제 쓸지"를 모르고, 정책은 "어떻게 저장되는지"를 모른다.

| 엔진이 아는 것 (`extstore_conf`) | 정책만 아는 것 (`settings.ext_*`) |
|---|---|
| page_size, wbuf_size, 버킷 수, IO 스레드 수 | ext_item_age (언제 flush) |
| = **물리 레이아웃 + 동시성** | ext_recache_rate (얼마나 RAM 복귀) |
| | ext_max_frag (언제 compaction) |
| | ext_low_ttl (어느 버킷에 넣을지) |

### 1-1. line별 detail

**L1-2 헤더 가드** — `#include`가 하나도 없다. `uint64_t`/`bool`/`struct iovec`를
쓰면서도 self-contained가 아니라, 포함하는 쪽이 `stdint.h`/`stdbool.h`/`sys/uio.h`를
먼저 include해야 한다 (`extstore.c:5-13` → `:19` 순서가 그래서 강제됨).

**L4-13 `extstore_page_data`** — compaction 판단에 필요한 최소 집합.
내부 `store_page`(`extstore.c:55-73`)는 20개 필드인데 여기 노출은 5개뿐.
- id 필드 없음 → 배열 인덱스가 곧 page_id
- `version`은 여기서 uint64_t지만 내부는 `unsigned int`(`extstore.c:60`). 넓히기만 함
- L12 `active` = 쓰기 중인 페이지는 compaction 대상 불가, 통계 합산만
- **함정**: 이걸 채우는 `extstore_get_page_data`(`extstore.c:195`)는 순수 읽기가 아님.
  `:220`에서 `obj_count==0 && refcount==0`인 페이지를 그 자리에서 `_free_page()` 한다.
  통계 조회 함수가 GC를 겸함. `bucket`은 살아있고 안 닫힌 페이지에만 채워짐(`:217`)

**L15-40 `extstore_stats`** — L15-20 주석이 extstore 설계 핵심의 요약:
구멍은 페이지가 evict되거나 전부 삭제될 때까지 재사용 불가 =
**페이지 내부에 free list가 없다**. 로그 구조. compaction이 필수인 이유.
- `bytes_fragmented` = (사용 페이지 크기 합) − (유효 바이트). 유도값
- L35 `bytes_read`는 실제 디스크 읽기만 카운트. wbuf 히트 제외 (→ `extstore.c:815`)
- L38 `io_queue` = 현재 큐 깊이, 백프레셔 신호

**L42-54 `extstore_conf`** — *목적: 엔진의 물리적 형태를 init에 한 번 못 박기.*
extstore.c는 memcached를 모르므로 전역 `settings`를 볼 수 없다.
그래서 엔진이 알아야 할 값만 뽑아 담은 전용 구조체가 필요하다.
L42-44 TODO에서 저자 스스로 "임시 구조체"라고 인정 — 그리고 정리되지 않은 채 남았다.

실제로 채워지는 값 (`storage.c:1335-1340` `storage_init_config`):

| 필드 | 실제 값 | 출처 | 엔진에서 읽는 곳 |
|---|---|---|---|
| `page_size` | 64MB | `settings.ext_page_size` | `extstore.c:290` |
| `wbuf_size` | 4MB | `settings.ext_wbuf_size` | `extstore.c:390` |
| `page_buckets` | 6 | `PAGE_BUCKET_COUNT` | `extstore.c:384-385` |
| `wbuf_count` | 6 (= page_buckets) | `storage.c:1340` | `extstore.c:389` |
| `io_threadcount` | 1 | `settings.ext_io_threadcount` | `extstore.c:406-415` |
| `io_depth` | 1 | 하드코딩 | `extstore.c:403` |
| `page_count` | **설정 안 됨** | — | **읽히지 않음 (dead)** |
| `free_page_buckets` | **설정 안 됨** | — | **읽히지 않음 (dead)** |

- **8개 필드 중 2개가 죽어 있다.** `page_count`는 실제로는 파일별로 계산되고
  (`storage.c:1568`: `fh->page_count = fh->total_size / page_size`),
  free page 버킷 배열은 `cf->page_buckets`를 재사용한다(`extstore.c:364-365`).
  구조체 필드만 봐서 목적을 알기 어려운 게 정상 — 실제로 정리가 안 된 상태다
- L46 `page_size` 64-256M: eviction 단위가 페이지 통째. 크면 메타데이터↓ 손실폭↑
- **L48 `page_buckets` = 동시에 열려 있는 쓰기 페이지 수 = 6**
  (`storage.c:14-21`: DEFAULT/COMPACT/CHUNKED/LOWTTL/COLDCOMPACT/OLD).
  성격이 다른 데이터를 물리적으로 다른 페이지에 모아, 페이지 통째 evict 시
  버려지는 유효 데이터를 최소화하는 것이 목적
- L49 `free_page_buckets` 주석이 "(see code)" — 저자도 설명 포기. 실제로 dead field.
  다만 *개념*은 살아있다: `extstore_conf_file.free_bucket`(L63)으로 파일별 지정 가능
  (`storage.c:1284-1295`, `-o ext_path=/f/e:64m:lowttl` 형태)
- L50 wbuf_size 정합 위반 → L98 `EXTSTORE_INIT_PAGE_WBUF_ALIGNMENT`.
  **헤더에 안 적힌 추가 제약**: page_size와 wbuf_size 둘 다 2MB 배수여야 한다
  (`extstore.c:278`, flash erase block 고려 TODO)
- L51 `wbuf_count` 주석 "might get locked to 2 per active page"는 **실현되지 않았다.**
  storage.c는 `wbuf_count = page_buckets` = 버킷당 정확히 1개로 설정하고,
  `extstore_init`은 `page_buckets <= wbuf_count`만 검증한다(`extstore.c:268`).
  더블 버퍼링이 아님 → §3에서 wbuf 고갈 시 동작 확인할 것
- L53 `io_depth` = 배치 크기, 락 경합 완화가 주목적. 기본 1 (하드코딩)

**L56-65 `extstore_conf_file`** — `next`로 연결 리스트 = 여러 파일/디바이스.
per-file `bucket`/`free_bucket`으로 "이 버킷은 이 디바이스로" 고정 가능.
`total_size`는 page_count 슬라이싱 *전* 크기 (나머지는 버려짐).

**L67-70 `obj_io_mode`** — READ/WRITE뿐. **DELETE 없음** =
삭제는 IO가 아니라 순수 메타데이터 연산(L116). 로그 구조의 직접적 귀결.

**L75-92 `struct _obj_io` (이 파일의 심장)**
- L76-77 **소유권 규칙**: 제출 후 `->next`는 IO 스레드 소유.
  링크드 리스트로 배치 제출하는 구조라 필요한 규칙
- L80 `data` = 사용자 포인터. storage.c가 `io_pending_t`를 넣어둠.
  엔진이 memcached를 모르면서 왕복하게 하는 유일한 통로
- L82-84 `buf` **또는** `iov`. chunked item은 메모리가 흩어져 iovec 필요
  (`storage.c:305`에서 IOV_MAX개 malloc)
- **L85-88 "for read mode" 주석을 믿지 말 것 — write에서는 출력 파라미터다**

  | 필드 | write에서 채우는 곳 |
  |---|---|
  | `buf`, `page_id` | `extstore_write_request` (`extstore.c:637-638`) |
  | `offset`, `page_version` | `extstore_write` (`extstore.c:660-661`) |

  이 중 page_id/page_version/offset이 그대로 `item_hdr`(`memcached.h:677`)가 된다.
  읽기의 입력 = 쓰기의 출력인 대칭 구조
- L85 `page_version`이 여기선 `unsigned int`, L115/L116 API는 `uint64_t`, 내부도
  `unsigned int` → API만 넓힘. 실질 ABA 방어 폭은 32비트
- **L88 `page_id`가 `unsigned short` → 페이지 최대 65536개.**
  L99 `TOO_MANY_PAGES`의 근거. 64M 페이지 기준 총 용량 상한 4TB

**L94-103 `extstore_res`** — 8개 전부 init 에러.
런타임 에러 enum이 없음 = 런타임 실패는 int 0/-1로만 표현.

**L105-123 API 표면** — `extstore_init`이 `void *` 리턴, 모든 API 첫 인자가
`void *ptr` = C에서의 불투명 핸들.
- **L107-108 2단계 쓰기가 가장 위험한 계약.** 호출자가 커밋 전에 (a) memcpy할
  버퍼와 (b) item_hdr에 넣을 page_id를 알아야 해서 나눔. 그런데 헤더에 없는 사실:
  `extstore_write_request`는 **성공 시 page mutex를 잡은 채 리턴**하고
  (`extstore.c:636` 주석 "leaves p locked"), 그 락은 `extstore_write`가 푼다(`:673`).
  request 성공 후 write를 안 부르면 그 페이지는 영구 데드락
- L109 `submit` = 라운드로빈 IO 스레드(전경 GET) vs L110 `submit_bg` = 전용
  bg 스레드(`store_engine.bg_thread`, `extstore.c:92`)로 wbuf flush/compaction.
  백그라운드 작업이 전경 읽기 지연을 밀어내지 않게 하는 분리
- **L111-113 주석 위치 오류**: `extstore_check`(L115) 위에 붙어있지만 실제로는
  `extstore_delete`(L116)의 count/bytes 설명
- L122 `close_page`("더 이상 안 씀, drain 대기") vs L123 `evict_page`("강제 회수")

**없는 것도 신호**: `extstore_read()` 없음 → 읽기는 예외 없이 비동기 submit.
shutdown/destroy 없음 → 엔진은 프로세스 수명 내내 생존.

### §1에서 뒤로 넘긴 질문
1. `extstore_write_request` 성공 후 `extstore_write`가 항상 호출되는가?
   (→ §3 `storage.c:498`)
2. `page_data` 배열을 호출자가 zero-fill 하는가? 안 하면 `bucket`에 stale 값
   (→ §6 `storage.c:798`)
3. `page_version` 32비트 랩어라운드가 실제 도달 가능한가? (→ §6 `_free_page`)
4. `wbuf_count == page_buckets`(버킷당 1개)인데, 한 버킷이 wbuf를 flush 중일 때
   같은 버킷에 계속 쓰기가 들어오면? wbuf freelist 고갈 시 동작 (→ §3 `_allocate_wbuf`)

---

## §3 쓰기 경로 — `storage_write_thread` (`storage.c:598-733`)

### 3-0. 목적: 아이템을 옮기는 게 아니라 빈 페이지 수를 유지하는 것

이름 때문에 "아이템을 디스크로 쓰는 스레드"로 읽히지만, 실제로는
**빈 슬랩 페이지 수를 목표 수위로 유지하는 피드백 제어기**다.
아이템을 밀어내는 것은 수위를 올리는 수단일 뿐.

| 제어 요소 | 코드 | 값 |
|---|---|---|
| 목표(setpoint) | `settings.ext_global_pool_min` | **이 파일 밖에서 결정** |
| 측정(PV) | `global_page_pool_size()` (`:622`) | 현재 빈 슬랩 페이지 수 |
| 오차 → 작업량 | `target_pages` (`:625-630`) | 부족분 |

### 3-1. 목표치가 밖에서 온다 — 최대 혼란 지점

`settings.ext_global_pool_min`을 storage.c에서 찾으면 `:1602`의 `= 0`밖에 안 나온다.
실제 값은 **다른 파일**에서 온다:

- `slab_automove_extstore.c:122` — `global_pool_watermark = total_pages * free_ratio`
  (`free_ratio` = `slab_automove_freeratio` = 0.01, 최소 2)
- `slab_automove_extstore.c:245` — 그 값을 `settings.ext_global_pool_min`에 대입
- 실행 주체는 **LRU maintainer 스레드**

→ **메모리 압력을 판단하는 주체와 밀어내는 주체가 분리되어 있다.**

기동 초기 동작도 여기서 나온다: `storage_init`이 0으로 두고(`storage.c:1602`),
automove는 관측 window가 찰 때까지 갱신하지 않는다
(`slab_automove_extstore.c:241-243`). **초기에는 이 스레드가 거의 논다.**

### 3-2. 실제 게이트는 `target`이 아니라 `item_age`다

`target`을 계산해놓고 "몇 개 옮길지"를 세지 않는다. **모드 스위치로만 쓴다** (`:681-685`):

```c
if (chunks_free < target) {
    item_age = 0;                      // 나이 무시, 무조건 밀어냄
} else {
    item_age = settings.ext_item_age;  // 기본 UINT_MAX
}
```

그리고 `storage_write`(`:514`)에서:

```c
if ((it->it_flags & ITEM_HDR) == 0 &&
        (item_age == 0 || current_time - it->time > item_age))
```

`item_age == 0` → 무조건 통과. `UINT_MAX` → `current_time - it->time > UINT_MAX`는
사실상 절대 참이 아님 → **아무것도 안 옮긴다.**

→ **기본 설정에서 이 스레드는 메모리 압력이 있을 때만 일한다.**
`-o ext_item_age=N`을 주면 "압력이 없어도 오래된 건 미리 내려보내기"가 켜진다.
이 두 줄이 extstore의 동작 성격을 통째로 바꾼다.

### 3-3. 되먹임 셋이 겹쳐 있다 — 코드가 복잡해 보이는 진짜 이유

각각 다른 축을 조절한다.

| # | 축 | 코드 | 동작 |
|---|---|---|---|
| 1 | **양** — 얼마나 | `target` (`:672-676`) | 글로벌 풀 부족분 기반 모드 스위치 |
| 2 | **클래스별 빈도** — 어느 클래스를 얼마나 자주 | `backoff[x]` | 실패 시 `++`, 성공 시 `1` (`:703-707`)<br>`counter % backoff[x] != 0`이면 스킵 (`:642`) |
| 3 | **시간** — 전체 주기 | `to_sleep` | 성공 시 `/= 2` (`:696-697`), 실패 시 `++` (`:728`)<br>범위 200µs(`:596`) ~ `ext_max_sleep`(기본 1초) |

- 2번: 계속 실패하는 클래스는 1/2, 1/3, 1/4... 로 점점 드물게 본다.
  빈 클래스를 매번 뒤지는 낭비 제거
- 3번: **감소는 곱셈, 증가는 덧셈** → 압력에 빠르게 반응하고 천천히 잠든다

### 3-4. 루프 3중 구조

| 루프 | 위치 | 역할 |
|---|---|---|
| `while(1)` | `:619` | 전체 주기 |
| `for x` | `:636` | 슬랩 클래스를 **큰 것부터** 순회 |
| `while(1)` | `:679` | 한 클래스에서 `storage_write` 실패할 때까지 반복 |

### 3-5. 나머지 세부

- **큰 클래스부터** (`:635-636`) — 주석 "the largest items have the least overhead
  from going to disk". 큰 값일수록 헤더 오버헤드 대비 RAM 절약이 크다
- **작은 클래스 보호** (`:657-676`) — 저자가 "stupid heuristic"이라 자평.
  `chunk_size`가 500/1000/2000 미만이면 `max_pages`를 3/4/5로 제한.
  작은 아이템은 밀어내도 RAM에 item 헤더 + `item_hdr` 12바이트가 남아 절약폭이 작고,
  과하게 밀면 계속 밀어야 하는 악순환(runaway)이 생긴다
- **compaction 체커 킥** (`:688-693`, `:721-725`) — 쓴 바이트가 `slab_page_size`만큼
  쌓이면 `storage_compact_cond` signal. `:612-616` 주석대로 매 루프 호출은 너무 잦아서
  바이트 기준으로 바꿨다. 잠들 때도 `to_sleep * 10`만큼 차감해 가끔 깨움
- **락 춤** (`:611`, `:711`, `:730`) — `storage_write_plock`을 작업 중엔 쥐고
  sleep 직전에 놓는다. `storage_write_pause()`(`:751`)가 이 락을 잡으면 다음 sleep
  지점에서 멈춘다. 슬랩 재배치(`thread.c:164`) 중 안전 정지용

### 3-6. 한 줄 요약

LRU maintainer가 메모리 압력을 보고 수위를 정한다 → 이 스레드가 현재 빈 페이지와
비교해 부족하면 `item_age=0` 모드로 전환한다 → 큰 클래스부터 실패할 때까지 밀어낸다
→ 실패한 클래스는 backoff로 뜸하게 보고, 아무것도 못 하면 sleep을 늘려 잠든다.

### §3에서 뒤로 넘긴 질문

1. `storage_write`(`:498`) 본문은 아직 안 읽음 — §1 질문 1
   (`extstore_write_request` ↔ `extstore_write` 짝)을 여기서 확인할 것
2. `lru_pull_tail`의 `LRU_PULL_RETURN_ITEM` 분기 (`items.c:1072`) 미독
3. `slab_automove_extstore.c`의 압력 계산 전체 (`:88-124`) 미독 — §0 항목

