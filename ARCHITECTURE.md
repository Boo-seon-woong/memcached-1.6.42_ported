# memcached 1.6.42 — 아키텍처 & 워크플로우

스톡 소스가 어떻게 구성되어 있고 요청이 어떤 경로로 흐르는지 정리한 지도입니다.
`file:line` 참조는 이 트리 기준이므로 바로 코드로 점프할 수 있습니다.

- C 약 40 K LOC, 93개 `.c`/`.h` 파일, autotools 빌드.
- 단일 프로세스, 다중 스레드, 이벤트 기반(libevent), 워커별 샤딩.
- 실질적인 로직은 전부 `memcached.c` + `thread.c`에 있고, `daemon.c`는 `daemonize()` 하나뿐.

---

## 1. 만 미터 상공 뷰

```
              ┌─────────────────────────────────────────────────────────┐
   clients ──▶│  Listener 소켓  ──  main/dispatcher 스레드               │
              │        (main_base, 리스너 + clock 타이머 소유)           │
              └───────────────┬─────────────────────────────────────────┘
                              │ accept() + dispatch_conn_new()
             워커에 라운드로빈 │ (CQ_ITEM을 워커 큐에 push,
                              ▼  eventfd로 워커 깨움)
      ┌───────────┬───────────┬───────────┬───────────┐
      │ worker 0  │ worker 1  │ worker 2  │  ...  N-1  │  각자: 전용 event_base,
      │ 이벤트루프│ 이벤트루프│ 이벤트루프│           │  conn마다 drive_machine()
      └─────┬─────┴─────┬─────┴─────┬─────┴───────────┘
            │ 커맨드는 프로토콜 + 스토리지 레이어를 통과
            ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │ STORAGE (공유, 락 보호)                                            │
   │   해시 테이블(assoc.c) ── 아이템(items.c) ── 슬랩 할당기          │
   │   (slabs.c) ── 세그먼트 LRU(items.c) ── [extstore 플래시]          │
   └──────────────────────────────────────────────────────────────────┘
            ▲
            │ 백그라운드 스레드가 스토리지를 건강하게 유지
   LRU maintainer · LRU crawler · slab rebalancer · assoc rehash ·
   logger · idle-timeout · (extstore IO/compaction)
```

핵심 구조적 아이디어: **dispatcher는 클라이언트 I/O를 처리하지 않는다**. 연결을
accept해서 워커에게 넘겨주기만 하고, 워커가 그 연결의 소켓을 수명 내내 소유한다.
공유되는 유일한 대상은 스토리지이며, 세분화된 락(버킷별 item 락, LRU 락, 슬랩 락)으로
보호된다.

---

## 2. 스레딩 모델

| 스레드 | 역할 | 진입점 |
|---|---|---|
| **Main / dispatcher** | 리스닝 소켓 + clock 타이머 소유; accept 후 hand-off. `main_base` 실행. | `main()` `memcached.c:4695` → `event_base_loop` `memcached.c:6186` |
| **Worker × `-t N`** | 각자 `event_base` 하나; 자신의 conn에 대해 `drive_machine()` 실행. | `memcached_thread_init` `thread.c:1083` → `worker_libevent` `thread.c:507` |
| **LRU maintainer** | 아이템을 HOT↔WARM↔COLD로 이동, eviction/reclaim 트리거. | `lru_maintainer_thread` `items.c:1542` |
| **LRU crawler** | LRU tail을 훑으며 만료 키 회수; metadump 구동. | `item_crawler_thread` `crawler.c:593` |
| **Slab rebalancer** | automove 결정에 따라 페이지 단위로 슬랩 클래스 간 이동. | `slab_rebalance_thread` `slabs_mover.c:650` |
| **Assoc maintenance** | 해시 테이블을 점진적으로 확장/rehash. | `assoc_maintenance_thread` `assoc.c:197` |
| **Logger** | conn별 링 버퍼를 비워 비동기 log/watch 출력. | `logger_thread` `logger.c:902` |
| **Idle timeout** | `settings.idle_timeout` 초과 유휴 연결 종료(옵트인). | `conn_timeout_thread` `memcached.c:292` |
| **Extstore IO / compaction** | 비동기 플래시 read/write + 페이지 compaction (EXTSTORE 빌드). | `extstore_io_thread` `extstore.c:412` |

모든 백그라운드 스레드는 워커 풀 직후 `main()`에서 시작된다
(`memcached.c:~6016`–`6063`).

### 연결 hand-off (listener → worker)

1. 리스너 fd가 `main_base`에서 발화 → `event_handler` → `drive_machine`의
   `conn_listening` 케이스(`memcached.c:2980`)에서 클라이언트 fd를 accept.
2. `dispatch_conn_new()`(`thread.c:772`)가 `select_thread_round_robin()`
   (`thread.c:699`)으로 워커를 고르고, `CQ_ITEM`(모드 `queue_new_conn`)을 할당해
   워커 큐에 push한 뒤, 그 워커의 **eventfd**에 1바이트를 써서 깨운다
   (`notify_worker`, `thread.c:336`).
3. 워커의 libevent가 깨어남 → `thread_libevent_process()`(`thread.c:588`)가 아이템을
   pop하고 `conn_new()`(`thread.c:621`)를 호출해, 클라이언트 fd의 이벤트를 *이 워커의*
   base에 바인딩하고 `c->thread = me`를 설정한다.

나머지 큐 모드도 같은 채널을 재사용한다: `queue_pause`, `queue_timeout`,
`queue_redispatch`, `queue_stop`, proxy reload.

---

## 3. 연결 상태 머신

모든 클라이언트 fd는 하나의 `conn`(`memcached.h:820`)이다. 그 생명주기는
`while(!stop) switch(c->state)` 루프인 **`drive_machine()`**(`memcached.c:2961`)이며,
단일 libevent 콜백 `event_handler()`(`memcached.c:3357`)에서 호출된다.

상태(`enum conn_states` `memcached.h:202`; `conn_set_state` `memcached.c:944`로 전환):

| 상태 | 하는 일 |
|---|---|
| `conn_listening` | 새 fd accept, `dispatch_conn_new`, 정지. (dispatcher 전용) |
| `conn_waiting` | `EV_READ` 무장, 소켓이 읽기 가능해질 때까지 대기. |
| `conn_read` | `try_read_network()`로 `rbuf` 채움. → `conn_parse_cmd` / 대기 복귀 / close. |
| `conn_parse_cmd` | `c->try_read_command(c)` 호출 — **프로토콜 레이어로의 hand-off 지점**. |
| `conn_new_cmd` | 커맨드별 상태 리셋; `reqs_per_event` 이후 양보해 다른 conn 기아 방지. |
| `conn_nread` | 고정 `rlbytes` 페이로드를 아이템 본문(`ritem`)에 직접 읽고 `complete_nread()`. |
| `conn_swallow` | `sbytes`를 읽어 버림(초과/에러 본문). |
| `conn_write` / `conn_mwrite` | 응답 iovec 체인을 `transmit()`; 완료 시 아이템 해제 → `conn_new_cmd`. |
| `conn_io_queue`/`_pending`/`_resume` | 비동기 IO 핸드셰이크(extstore/proxy): 대기 중 detach, `conn_mwrite`로 재개. |
| `conn_closing` / `conn_closed` | `conn_close()` / 절대 실행되면 안 되는 `abort()`. |
| `conn_watch` | 연결을 logger 스레드로 이관(`watch` 명령). |

### 버퍼링

- **읽기:** conn당 `rbuf` 하나; `rcurr` = 파싱 커서, `rbytes` = 미파싱 바이트.
  아이템 본문은 복사를 피하려고 할당된 아이템에 *직접*(`ritem`, `rlbytes`) 읽는다.
  읽기 버퍼는 스레드별 캐시에서 온다(`rbuf_alloc`/`rbuf_release`, `thread.c:454`).
- **쓰기:** 옛 `msglist`/`msgcurr` 모델은 사라졌다. 응답은 **`mc_resp`** 객체의
  링크드 리스트(`memcached.h:771`)이며, 각각 자체 `wbuf`와 인라인 `iovec[]`를 가진다.
  전송 시 `_transmit_pre()`(`memcached.c:2555`)가 체인 전체의 iovec을 임시 배열로 모으고
  `transmit()`(`memcached.c:2683`)이 패스당 **단일 scatter-gather `sendmsg`**를 수행한다.
  GET은 `ITEM_data`를 직접 참조하고(zero-copy), 바이트가 전송될 때까지 아이템 refcount를
  `resp->item`에 잡아둔다.

---

## 4. 프로토콜 레이어

각 연결은 `c->try_read_command` 함수 포인터를 가지며, `conn_new`(`memcached.c:743`)에서
`c->protocol`에 따라 선택된다:

- `ascii_prot` → `try_read_command_ascii`(`proto_text.c:350`)
- `binary_prot` → `try_read_command_binary`(`proto_bin.c:49`)
- `negotiating_prot` → `try_read_command_negotiate`(`memcached.c:2345`) — 첫 바이트를
  엿봄; `0x80`이면 binary, 아니면 ASCII로 재지정 후 재디스패치.

공유 파서는 `process_request()`(`proto_parser.c:163`): **제자리(in-place)** 파싱으로
복사가 없어(proxy가 무수정 포워딩 가능) 커맨드를 식별하고, 벤더 토크나이저
(`vendor/mcmc/mcmc.h`)로 shape별 서브파서를 돌린다. **meta** 명령(`mg`/`ms`/`md`/`ma`/`mn`/`me`)은
별도 연결 프로토콜이 아니라 ASCII 연결로 들어와 여기서 인식된다; 실행부도
`proto_parser.c`에 있다.

- `proto_text.c` — 클래식 ASCII 프론트엔드(`get`/`set`/`add`/`incr`/`delete`…).
- `proto_bin.c` — 바이너리 프로토콜(`dispatch_bin_command` `proto_bin.c:892`,
  와이어 구조체는 `protocol_binary.h`).
- `proto_parser.c` — 공유 파서 + meta + 공유 커맨드 실행부.

---

## 5. 워크플로우: SET과 GET 종단간(ASCII)

### SET `key flags exptime vlen\r\n<value>\r\n`

```
conn_parse_cmd → try_read_command_ascii (proto_text.c:350)
  → process_command_ascii (proto_text.c:1517)   # 파서가 CMD_SET + vlen 설정
  → process_update_command (proto_text.c:756)
      → process_update_cmd_start (proto_parser.c:744)
          → item_alloc(key, nkey, flags, exptime, vlen)   # -> do_item_alloc items.c:249
      → c->item / c->ritem = ITEM_data / c->rlbytes = nbytes
      → conn_set_state(conn_nread)                # value 바이트는 상태 머신이 읽음
--- 상태 머신이 value를 아이템에 읽어들임 ---
conn_nread → complete_nread (memcached.c:1422)
  → complete_nread_ascii (proto_text.c:117)       # 후행 \r\n 검증
      → store_item (thread.c:962)                 # item_lock(hv)
          → do_store_item (memcached.c:1547)
              → do_item_get/assoc_find, CAS 검사
              → item_replace / do_item_link → assoc_insert + LRU link
      → out_string("STORED") (memcached.c:1217)   # c->resp->wbuf에 기록
      → item_remove(it)                           # store 경로 ref 해제
```

### GET `key [key2 …]\r\n`

```
conn_parse_cmd → process_command_ascii (proto_text.c:1517)   # 파서가 CMD_GET 설정
  → process_get_command (proto_text.c:421)                   # 키들에 대해 루프
      → process_get_cmd (proto_parser.c:655)
          → item_get / assoc_find (assoc.c:70)
          → hit: resp_add_iov("VALUE k f len\r\n") + resp_add_iov(ITEM_data, nbytes)
                 resp->item = it     # 전송까지 refcount 유지(zero-copy)
  → resp_add_iov("END\r\n"); conn_set_state(conn_new_cmd)
--- conn_new_cmd → conn_mwrite → transmit()이 resp 체인을 sendmsg 한 번으로 ---
```

바이너리와 meta 경로도 동일한 코어로 수렴한다: 쓰기는 `store_item` → `do_store_item`
→ `assoc`/`items`/`slabs`, 읽기는 `item_get`/`assoc_find`.

---

## 6. 스토리지 엔진

### 슬랩 할당기 (`slabs.c`)

메모리는 **슬랩 클래스**(`slabclass[]` `slabs.c:37`)로 나뉘고, 각각 고정 **청크 크기**를
가진다. `slabs_init()`(`slabs.c:202`)이 클래스를 기하적으로 구성한다: 클래스마다
`size *= factor`(기본 **1.25**), 정렬 올림, `slab_chunk_size_max`까지. **페이지**(기본 1 MB)는
`perslab`개의 균등 청크로 나뉘어 freelist에 올라간다. `slabs_clsid()`(`slabs.c:77`)가 요청
크기를 맞는 최소 클래스로 매핑한다. `do_slabs_alloc/free`(`slabs.c:411`/`501`)가 freelist를
pop/push하며, 새 페이지는 글로벌 페이지 풀 또는 `memory_allocate()`에서 온다. 클래스 0
(`SLAB_GLOBAL_PAGE_POOL`)은 재할당을 위해 해제된 페이지 전체를 보관한다.

### 아이템 (`memcached.h:597`)

`struct _stritem`: LRU 링크(`next`/`prev`), 해시 체인(`h_next`), `time`, `exptime`,
`nbytes`, `refcount`, `it_flags`, `slabs_clsid`(클래스 id **+** 상위 2비트에 LRU 세그먼트),
`nkey`, 그리고 가변 페이로드가 **[8바이트 CAS?] → key → [client flags?] → data`\r\n`**
순으로 배치된다. 접근 매크로(`ITEM_key`/`ITEM_data`/`ITEM_get_cas`/`ITEM_clsid`…)는
`memcached.h:127`. 청크보다 큰 아이템은 `ITEM_CHUNKED`이며 링크된 `item_chunk`들에 걸쳐 있다.
`do_item_alloc()`(`items.c:249`)이 크기를 계산하고 메모리를 당기며, 클래스가 꽉 차면
`lru_pull_tail`로 회수/eviction한다.

### 해시 테이블 (`assoc.c`)

`primary_hashtable`(`assoc.c:38`), `hashsize(hashpower)`개 버킷, 충돌은 `h_next`로 체이닝.
`assoc_find`/`assoc_insert`/`assoc_delete`(`assoc.c:70`/`153`/`172`). `curr_items > 1.5 ×
hashsize`이면 `assoc_expand()`(`assoc.c:122`)가 2배 크기 테이블을 할당하고 maintenance
스레드가 버킷을 점진 이관하므로, 조회는 `expand_bucket`을 확인해 키가 현재 어느 테이블에
있는지 판단한다. 해시 함수(`jenkins` / `murmur3` / `xxh3`)는 init 시 `hash_init()`
(`hash.c:15`)이 선택한다.

### 세그먼트 LRU (`items.c`)

클래스마다 4개 서브리스트 — **HOT / WARM / COLD / TEMP** — `slabs_clsid` 상위 2비트로
구분되며 각각 전용 락을 가진다. `do_item_link/unlink/update`(`items.c:485`/`507`/`549`)가
리스트 + 바이트 회계를 유지한다. `do_item_bump()`(`items.c:1032`)는 `FETCHED` 후 `ACTIVE`를
표시하고, COLD-active 히트는 읽기 경로 경합을 피하려 lock-free bump 버퍼로 비동기 승격된다.
**`lru_pull_tail()`**(`items.c:1072`)이 핵심이다: 만료 아이템을 제자리 회수하고, 크기/나이
비율에 따라 HOT→WARM→COLD로 흘려보내며, 메모리 압박 시 COLD tail에서 evict한다. **crawler**
(`crawler.c:593`)는 접근된 적 없는 만료 키를 회수하려 독립적으로 tail을 훑는다.

### 슬랩 리밸런싱 (`slab_automove.c` + `slabs_mover.c`)

`slab_automove_run()`(`slab_automove.c:74`)이 슬라이딩 윈도우로 클래스별 eviction 비율과
아이템 나이를 샘플링해, 가장 오래되고 압박 낮은 클래스에서 가장 젊고 활발히 evict하는
클래스로 옮길 페이지를 고른다; `slab_rebalance_thread`(`slabs_mover.c:650`)가 물리적 페이지
이동을 수행한다(살아있는 아이템 rescue 포함).

### extstore (선택, `extstore.c`)

Write-through 플래시 티어: 아이템 **값**을 고정 크기 페이지로 구성된 SSD 파일에 저장하고,
page/offset/version을 가리키는 작은 RAM 헤더(`ITEM_HDR`)만 남긴다. 비동기 IO 스레드가
read/write를 처리하고, compaction 스레드가 단편화된 페이지를 회수한다. 노드가 RAM보다 훨씬
많은 데이터를 담게 해주며, 읽기 지연을 용량과 맞바꾼다. `EXTSTORE` 정의 시에만 빌드(기본 on).

---

## 7. 보조 서브시스템

- **Proxy** (`proto_proxy.c`, `proxy_*.c`) — 선택(`--enable-proxy`, `#ifdef PROXY`, 기본
  off). memcached를 다른 memcached 백엔드 앞단의 **Lua 설정** 라우팅/샤딩 프록시로 바꾼다:
  풀, 라우팅 함수, 요청 핸들러를 Lua 설정으로 정의(`proxy_init` `proto_proxy.c:350`).
  각 프록시 워커는 자체 libevent 루프를 돌린다. 해싱 헬퍼(`proxy_ring_hash`/
  `proxy_jump_hash`/`proxy_xxhash`), rate limiting, TLS 백엔드가 함께 있다. 실험적.
- **Auth** — 바이너리 프로토콜용 SASL(`sasl_defs.c`, `--enable-sasl`); ASCII 프로토콜
  user/pass 인증용 `authfile.c`(항상 컴파일, `authfile_load`/`authfile_check`).
- **Warm restart** (`restart.c`) — 슬랩 메모리를 재시작 간에 mmap 백엔드 파일
  (hugepage/pmem)에 유지; `restart_fixup`(`restart.c:356`)이 옛 mmap 주소에서 포인터를
  재기준화해 기존 아이템을 살린다.
- **Stats & settings** — 프로세스 전역 `struct settings settings`(`memcached.c:107`,
  정의 `memcached.h:454`)를 CLI 인자로 채운다. 핫패스 stat 카운터는 락 경합을 피하려
  **스레드별**(`thread_stats`, `memcached.h:368`)이고 필요 시 합산한다; 전역
  `stats`/`stats_state`는 `STATS_LOCK()` 하에. 텍스트 `stats` 명령은
  `append_stat`/`ADD_STAT`(`server_stats` `memcached.c:1747`)로 보고한다.

---

## 8. 빌드 & 테스트

표준 autotools: `./autogen.sh && ./configure && make`.

- 메인 설정: `configure.ac`, `Makefile.am`. 산출물: `bin_PROGRAMS = memcached`;
  `memcached-debug`(같은 소스, 디버그 플래그)가 테스트 스위트가 돌리는 대상.
- 선택 기능(`AC_ARG_ENABLE`): `--disable-extstore`(기본 on), `--enable-proxy`(+`-uring`,
  `-tls`), `--enable-tls`, `--enable-sasl`(+`-pwdb`), `--enable-seccomp`,
  `--enable-unix-socket`, `--enable-dtrace`. 플랫폼 권한 드롭 파일(`linux_priv.c` 등)은
  빌드 조건으로 선택된다.
- 테스트: C 유닛 테스트는 `testapp.c`; 메인 스위트는 `t/`의 **Perl `prove` 스크립트**
  (~114개 `.t` 파일, 예: `t/getset.t`, `t/binary.t`, `t/extstore*.t`). `make test`는
  `memcached-debug`를 빌드하고 `testapp`를 돌린 뒤 `prove t/*.t`를 실행한다. `sizes.c`는
  ABI / warm-restart 점검용으로 구조체 크기를 출력한다.

---

## 빠른 파일 색인

| 관심사 | 파일 |
|---|---|
| 시작, conn 상태 머신, transmit | `memcached.c`, `daemon.c` |
| 스레드, 연결 hand-off | `thread.c` |
| 프로토콜 | `proto_parser.c`, `proto_text.c`, `proto_bin.c`, `protocol_binary.h`, `vendor/mcmc/` |
| 스토리지 | `slabs.c`, `items.c`, `assoc.c`, `hash.c` (+`jenkins_/murmur3_/*_hash.c`) |
| LRU / 회수 | `items.c`, `crawler.c`, `slab_automove*.c`, `slabs_mover.c` |
| 플래시 티어 | `extstore.c` |
| 프록시 | `proto_proxy.c`, `proxy_*.c` |
| 보조 | `logger.c`, `restart.c`, `authfile.c`, `sasl_defs.c`, `cache.c`, `bipbuffer.c` |
