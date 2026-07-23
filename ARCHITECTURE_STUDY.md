# memcached-1.6.42 아키텍처 스터디

> **문서 목적** — RDMA 포팅에 앞서 **원본(vanilla) memcached 소스 코드의 구조**를 파악하기 위한 스터디 노트.
> 본 문서는 **RDMA 자체는 전혀 다루지 않는다.** 오직 원본 memcached가 어떻게 구성되어 있고,
> set/get 요청이 코드 안에서 어떤 경로로 흐르는지를 기술한다.
>
> 다루는 것: ① 파일 책임 구조, ② 두 축(데이터부 / 통신부)과 그 접합점, ③ set/get 경로,
> ④ **전송 계층 함수 포인터의 정체 규명**(local 내부 전송인가 remote 전송인가).
>
> 모든 코드 위치는 `파일:라인` 형식이며 memcached-1.6.42 기준이다.

---

## 0. 한눈에 보는 계층 구조

memcached는 **통신부**와 **데이터부**가 분리되어 있고, 그 사이를 **프로토콜 파서 + 응답 조립부**가 잇는다.

```
┌─ 축2: 통신부 (COMM / TRANSPORT) ─────────────────────────────┐
│  thread.c    : 워커 스레드 풀 + libevent + 연결 분배           │
│  memcached.c : drive_machine 연결 상태머신 + 소켓 I/O          │
│                c->read / c->sendmsg / c->write (전송 함수포인터) │
└──────────────────────────┬───────────────────────────────────┘
                           │  ← 접합점(SEAM): 파싱 & 응답 조립
┌─ 프로토콜 계층 (PROTOCOL) ───────────────────────────────────┐
│  proto_text.c   : ASCII 프로토콜 (get/set ...) 파싱·처리       │
│  proto_bin.c    : 바이너리 프로토콜                            │
│  proto_parser.c : 공용 파서 토큰화                             │
│  → mc_resp(iovec)로 응답 조립, c->ritem 으로 item 메모리 지목    │
└──────────────────────────┬───────────────────────────────────┘
                           │
┌─ 축1: 데이터부 (DATA PLANE / STORAGE ENGINE) ────────────────┐
│  assoc.c  : 해시테이블 (key → item 포인터)                     │
│  items.c  : KV 아이템 생성·링크·조회·LRU                        │
│  slabs.c  : 슬랩 메모리 풀 (실제 데이터가 사는 곳, mem_base)     │
└──────────────────────────────────────────────────────────────┘
```

**핵심 통찰**: 통신부는 "클라이언트와 바이트를 주고받는" 책임만, 데이터부는 "KV를 메모리에 저장/조회"하는
책임만 진다. 둘은 프로토콜 계층을 통해서만 만난다. 이 분리가 명확해서 각 축을 독립적으로 학습·수정할 수 있다.

---

## 1. 파일 책임 구조

### 1.1 축1 — 데이터부 (실제 KV가 메모리에 사는 곳)

| 파일 | 책임 | 핵심 심볼 |
|---|---|---|
| `slabs.c` / `slabs.h` | **슬랩 할당자**. 큰 메모리 덩어리(`mem_base`)를 미리 잡아 슬래브→페이지→청크로 쪼개 관리. 실제 값이 저장되는 물리적 풀. | `slabs_init` (`slabs.c:202`), `mem_base` (`:45`), `do_slabs_alloc` (`:411`), `memory_allocate` (`:613`) |
| `items.c` / `items.h` | **아이템 관리**. 슬랩에서 공간을 받아 KV 아이템을 만들고, 해시테이블·LRU에 링크. | `do_item_alloc` (`items.c:249`), `do_item_link` (`:485`), `do_item_get` (`:974`) |
| `assoc.c` / `assoc.h` | **해시테이블**. key의 해시값 → item 포인터 매핑. 조회의 시작점. | `assoc_find` (`assoc.c:70`), `assoc_insert` (`:153`), `assoc_delete` (`:172`) |
| `hash.c` / `jenkins_hash.c` / `murmur3_hash.c` | 키 해시 함수 (기본 xxhash/jenkins/murmur 선택). | `hash` 함수 포인터 |
| `crawler.c` | LRU 크롤러 — 만료 아이템 회수(백그라운드). | `lru_crawler_*` |
| `slabs_mover.c` / `slab_automove.c` | 슬랩 재배치(page mover) / 자동 이동 정책. | `slab_rebalance_*` |

### 1.2 축2 — 통신부 (클라이언트와 바이트를 주고받는 곳)

| 파일 | 책임 | 핵심 심볼 |
|---|---|---|
| `memcached.c` | **메인 + 연결 상태머신**. 프로그램 진입점, 소켓 생성/listen, 연결 객체(`conn`) 생성, `drive_machine` 상태머신으로 수신→파싱→응답→송신 구동. | `main`, `drive_machine` (`memcached.c:2961`), `conn_new` (`:610`), `try_read_network` (`:2431`), `transmit` (`:2683`), `server_socket` (`:3444`) |
| `thread.c` | **멀티스레드 모델**. 워커 스레드 풀 생성, 각 스레드가 자기 libevent 루프를 돌림. accept된 연결을 워커에 분배. | `memcached_thread_init` (`thread.c:1083`), `setup_thread` (`:420`), `worker_libevent` (`:507`), `dispatch_conn_new` (`:772`), `thread_libevent_process` (`:588`) |
| `proto_text.c` | **ASCII 프로토콜**. `get`/`set`/`add`/`delete`/`incr` 등 텍스트 명령 파싱과 처리. | `try_read_command_ascii` (`proto_text.c:350`), `process_get_command` (`:421`), `process_update_command` (`:756`) |
| `proto_bin.c` | 바이너리 프로토콜 처리. | `try_read_command_binary`, `dispatch_bin_command` |
| `proto_parser.c` / `proto_parser_type.h` | 명령 토큰화 공용 파서. | `mcp_parser_t`, `proto_parse_cmd` |

### 1.3 지원 계층 (양 축 공통 인프라)

| 파일 | 책임 |
|---|---|
| `logger.c` | 스레드별 로깅 링버퍼 |
| `util.c` | 문자열/숫자 유틸 (`safe_strtoull` 등) |
| `stats_prefix.c` | prefix 단위 통계 |
| `authfile.c` / `sasl_defs.c` | 인증(ASCII authfile / SASL) |
| `daemon.c`, `*_priv.c` | 데몬화 / OS별 권한 드랍(seccomp, pledge 등) |
| `restart.c` | warm restart (재시작 시 메모리 보존) |
| `bipbuffer.c`, `itoa_ljust.c`, `base64.c` | 소형 자료구조/변환 유틸 |

### 1.4 이번 스터디에서 **제외**해도 되는 파일 (원본 이해 목적상)

| 파일군 | 이유 |
|---|---|
| `proxy_*.c`, `proto_proxy.c` (~13,000줄) | Lua 기반 라우팅 **프록시** 서브시스템. KVS 코어와 독립. 원본 get/set 이해에 불필요. |
| `storage.c`, `extstore.c`, `slab_automove_extstore.c` | 플래시(SSD) 계층 확장(extstore). 옵션 기능. |
| `tls.c` | TLS 전송(옵션). 단, §4의 전송 추상화 이해에는 **선례로서 참고 가치** 있음. |
| `xxhash.h`, `crc32c.c`, `md5.c` | 외부 vendored 라이브러리. |
| `testapp.c`, `sizes.c`, `timedrun.c` | 테스트/도구. |

---

## 2. 두 축과 접합점의 정의

### 축1 — 데이터부 (Data Plane)
"**KV 데이터가 어디에, 어떻게 메모리에 저장되고 조회되는가**"를 담당.
- 데이터의 물리적 거처: `slabs.c`의 `mem_base` 풀.
- 데이터의 논리적 단위: `struct _stritem` (아이템, `memcached.h:597`).
- 데이터의 색인: `assoc.c`의 해시테이블.

### 축2 — 통신부 (Comm Layer)
"**클라이언트와 바이트를 어떻게 주고받는가**"를 담당.
- 연결 수립: `server_socket` → `dispatch_conn_new` → `conn_new`.
- 수신/파싱/송신 구동: `drive_machine` 상태머신(17개 상태, `memcached.h:202` `enum conn_states`).
- 실제 바이트 I/O: `try_read_network`(수신) / `transmit`(송신), 내부적으로 `c->read`/`c->sendmsg` 호출(§4).

### 접합점 (Seam) — 두 축이 만나는 얇은 층
프로토콜 계층이 접합점이다. 여기서 **통신부가 받은 바이트가 데이터부의 연산으로 번역**되고,
**데이터부의 결과(item 메모리)가 통신부의 응답 버퍼로 연결**된다.
- 진입: `try_read_command_ascii` (`proto_text.c:350`) — 수신 바이트를 명령으로 파싱.
- SET 방향: `process_update_command` (`:756`) → `c->ritem = ITEM_data(it)` (`:768`)로 수신 페이로드를 아이템 메모리에 직접 받음.
- GET 방향: `process_get_command` (`:421`) → `resp_add_iov(resp, ITEM_data(it), ...)`로 아이템 메모리를 응답 iovec에 연결.

접합점의 핵심 자료구조는 `struct mc_resp`(`memcached.h:779`)이다. **값 데이터를 복사하지 않고**
`item` 포인터와 `iovec`로 가리켜 전송하도록 설계되어 있다:
```c
struct mc_resp {
    item *item;                       // 참조를 잡아둔 아이템 (memcached.h:779)
    struct iovec iov[MC_RESP_IOVCOUNT]; // 전송할 조각들 (헤더 + 값 포인터)
    uint8_t iovcnt;
    ...
};
```

---

## 3. set / get 경로 (세로 관통)

부팅 후 하나의 연결에서 **set 한 번, get 한 번**이 코드 안에서 흐르는 전체 경로. 축2 → 접합점 → 축1 → 접합점 → 축2 순으로 관통한다.

### 3.0 부팅 (공통 준비)
```
main (memcached.c)
 ├─ slabs_init(...)              slabs.c:202   메모리 풀(mem_base) 확보
 ├─ assoc_init(...)              assoc.c:55    해시테이블 초기화
 ├─ memcached_thread_init(...)   thread.c:1083 워커 스레드 풀 + 스레드별 libevent
 │    └─ setup_thread            thread.c:420
 │    └─ worker_libevent         thread.c:507  각 워커의 이벤트 루프 시작
 └─ server_socket(...)           memcached.c:3444  listen 소켓 생성 + 이벤트 등록
```

### 3.1 연결 수립 (축2)
```
클라이언트 접속
 → (리스너의) drive_machine: case conn_listening   memcached.c:2980   accept()
 → dispatch_conn_new(sfd, ...)                      thread.c:772      워커 스레드에 배정
 → thread_libevent_process                          thread.c:588     워커가 새 연결 수령
 → conn_new(sfd, conn_new_cmd, ...)                 memcached.c:610   conn 객체 생성
      └─ c->read = tcp_read; c->sendmsg = tcp_sendmsg; c->write = tcp_write;  (:734)
```

### 3.2 SET 경로 — `set foo 0 0 3\r\nbar\r\n`
```
[축2] drive_machine: conn_read                      memcached.c:3064
        → try_read_network(c)                        memcached.c:2431
             → res = c->read(c, c->rbuf+..., avail)  memcached.c:2468   ← 소켓에서 명령 바이트 수신
[축2→접합점] drive_machine: conn_parse_cmd           memcached.c:3094
        → try_read_command_ascii(c)                  proto_text.c:350   명령 라인 파싱
        → process_update_command(c, ...)             proto_text.c:756
[접합점→축1] item 공간 확보
        → item_alloc / do_item_alloc                 items.c:249        슬랩 풀에서 아이템 공간 할당
        → c->ritem = ITEM_data(it)                   proto_text.c:768   페이로드 수신 목적지 지정
[축2] drive_machine: conn_nread                      memcached.c:3139
        → res = c->read(c, c->ritem, c->rlbytes)     memcached.c:3169   ← 값 바이트를 아이템 메모리로 직접 수신
[축1] 저장 확정
        → do_store_item                              memcached.h:943 (구현 items.c)
        → do_item_link → assoc_insert                items.c:485 / assoc.c:153   해시테이블·LRU에 등록
[축2] 응답 "STORED\r\n"
        → resp_add_iov(resp, "STORED\r\n", 8)        (proto_text.c)
        → drive_machine: conn_mwrite → transmit      memcached.c:3269 / :2683
             → c->sendmsg(c, &msg, 0)                memcached.c:2704   ← 응답 송신
```

### 3.3 GET 경로 — `get foo\r\n`
```
[축2] drive_machine: conn_read → try_read_network    memcached.c:3064 / :2431
             → c->read(...)                           memcached.c:2468  ← "get foo\r\n" 수신
[축2→접합점] drive_machine: conn_parse_cmd            memcached.c:3094
        → try_read_command_ascii                      proto_text.c:350
        → process_get_command(c, ...)                 proto_text.c:421
[접합점→축1] 조회
        → assoc_find(key, nkey, hv)                   assoc.c:70         해시테이블에서 item 포인터 획득
        → do_item_get(...)                            items.c:974        유효성/만료 확인, 참조 획득
[접합점] 응답 조립 (복사 없음)
        → resp_add_iov(resp, ITEM_data(it), it->nbytes)  proto_text.c    응답 iovec가 아이템 메모리를 가리킴
        → resp_add_iov(resp, "END\r\n", 5)            proto_text.c:478
[축2] 송신
        → drive_machine: conn_mwrite → transmit       memcached.c:3269 / :2683
             → c->sendmsg(c, &msg, 0)                 memcached.c:2704   ← iovec를 그대로 소켓으로 전송
```

**요점**: GET 응답에서 값 데이터는 슬랩 풀 안의 아이템 메모리(`ITEM_data(it)`)를 **복사 없이 iovec로 가리켜**
`sendmsg`의 scatter-gather로 전송된다. SET 수신에서도 값은 아이템 메모리(`c->ritem`)로 **직접 수신**된다.
즉 원본 memcached는 이미 값 데이터에 대해 zero-copy 지향 경로를 갖고 있다.

---

## 4. 전송 계층 함수 포인터의 정체 규명 ★

> 앞선 논의에서 "memcached의 전송 계층이 함수 포인터로 분리되어 있다"고 했다.
> 이것이 **local 머신 내부 전송인지, remote 전송인지, 단일 하드웨어 내부 데이터 이동인지**를
> 코드 근거로 명확히 규명한다.

### 4.1 무엇인가 — 정의
`struct conn`은 전송을 3개의 함수 포인터로 추상화한다 (`memcached.h:76-78`):
```c
ssize_t (*read)(conn *c, void *buf, size_t count);
ssize_t (*sendmsg)(conn *c, struct msghdr *msg, int flags);
ssize_t (*write)(conn *c, void *buf, size_t count);
```
TCP 연결일 때 `conn_new`에서 다음 구현으로 바인딩된다 (`memcached.c:734-736`):
```c
c->read    = tcp_read;
c->sendmsg = tcp_sendmsg;
c->write   = tcp_write;
```
그리고 실제 구현은 (`memcached.c:130-143`):
```c
ssize_t tcp_read(conn *c, void *buf, size_t count)      { return read(c->sfd, buf, count); }
ssize_t tcp_sendmsg(conn *c, struct msghdr *msg, int f) { return sendmsg(c->sfd, msg, f); }
ssize_t tcp_write(conn *c, void *buf, size_t count)     { return write(c->sfd, buf, count); }
```

### 4.2 규명 결과

**① 이것은 "remote 전송(클라이언트↔서버 네트워크 통신)" 계층이다. 서버 내부의 메모리 이동이 아니다.**
- 세 함수는 전부 `c->sfd`에 대한 POSIX 소켓 syscall(`read`/`sendmsg`/`write`)을 감싼 것이다.
- `c->sfd`(`memcached.h` `struct conn`의 `int sfd`)는 **하나의 클라이언트 연결에 대한 소켓 파일 디스크립터**다.
- 따라서 이 계층이 옮기는 바이트는 **memcached 서버 프로세스와 (통상 다른 머신의) 클라이언트 프로세스 사이**를
  오가는 프로토콜 메시지(명령/응답)다. 커널 네트워크 스택(TCP/IP) → NIC → 네트워크를 경유한다.

**② "단일 하드웨어 내부의 내부 데이터 이동"이 아니다.**
- 서버 자기 메모리 안에서 데이터를 옮기는 것과는 무관하다. 데이터부(슬랩 풀, 아이템)와는 별개의 책임이다.
- 다만 클라이언트가 **같은 머신**에 있고 Unix 도메인 소켓/loopback으로 붙는 배치도 가능하다.
  그 경우조차 이 계층은 여전히 **프로세스 간(client↔server) 커널 매개 통신**이지,
  프로세스 내부(intra-process) 메모리 접근이 아니다. 즉 "local이냐 remote냐"의 물리적 거리와 무관하게,
  **역할상 항상 '클라이언트-서버 메시지 전송 채널'**이다.

**③ 이 추상화가 "remote 전송 계층 수정"에 유의미한가 — 그렇다. 단, 절반의 범위에서.**
- 유의미한 이유: 모든 클라이언트 바이트 I/O가 이 3개 함수로 **단일하게 깔때기처럼 모인다**.
  `drive_machine`의 수신/송신 로직은 `c->read`/`c->sendmsg`를 통해서만 바이트를 만지므로,
  전송 방식을 바꾸려면 이 바인딩(`memcached.c:734`)만 다른 구현으로 교체하면 된다.
  실제로 memcached는 **TLS**를 이 자리에 대체 구현(`ssl_*`)으로 끼워 이미 검증했다(§4.3).
- "절반의 범위"인 이유: 이 함수 포인터가 추상화하는 것은 **메시지(명령/응답) 바이트 스트림의 전송**뿐이다.
  **데이터 메모리(슬랩 풀/아이템) 자체를 노출하거나 추상화하지는 않는다.**
  즉 이 계층을 바꾸면 "바이트를 어떻게 나르는가"는 바뀌지만, 데이터는 여전히
  `명령 파싱 → 아이템 메모리 복사/참조 → 응답`이라는 경로를 그대로 탄다.
  데이터 메모리의 원격 노출은 이 계층이 아니라 **축1(데이터부)에서 별도로 다뤄야 할 독립 주제**다.

### 4.3 근거: 이 자리에 실제 사용되는 대체 구현들 (원본에 존재)
| 전송 | read/sendmsg/write 바인딩 | 위치 |
|---|---|---|
| TCP 소켓 | `tcp_read` / `tcp_sendmsg` / `tcp_write` | `memcached.c:130-143`, 바인딩 `:734` |
| TLS 소켓(옵션) | `ssl_*` 계열 | `tls.c` (빌드 옵션 `--enable-tls`) |
| 초기/미설정 | `NULL` | `memcached.c:629-631` |

`c->read`가 호출되는 모든 수신 지점 — `memcached.c:2468`(명령 수신), `:2927`(chunked),
`:3169`(SET 값 수신), `:3240`(swallow) — 과 `c->sendmsg` 송신 지점 `:2704`이
전부 이 추상화를 경유한다. 전송 방식은 이 한 겹에 응집되어 있다.

### 4.4 한 줄 결론
> `c->read`/`c->sendmsg`/`c->write`는 **클라이언트-서버 간 네트워크 메시지 전송(remote 통신)** 을
> 추상화한 계층이다. 서버 내부의 데이터 이동이 아니며, 물리적으로 같은 머신이어도 역할은
> 언제나 "프로세스 간 클라이언트-서버 채널"이다. 전송 방식 교체 시 **단일 삽입점**으로서 유의미하나,
> 이는 **메시지 전송의 교체**일 뿐 **데이터 메모리의 원격 노출과는 별개의 축**임에 유의한다.

---

## 5. 핵심 구조체 요약

### 5.1 `struct conn` — 연결 상태 (통신부의 중심, `memcached.h:836`)
| 필드 | 의미 |
|---|---|
| `int sfd` | 클라이언트 소켓 파일 디스크립터 (§4의 전송 대상) |
| `enum conn_states state` | 현재 상태머신 상태 (`conn_read`, `conn_nread`, `conn_mwrite` 등) |
| `char *rbuf; char *rcurr; int rsize; int rbytes` | 수신 버퍼 / 현재 파싱 위치 / 총 크기 / 미파싱 바이트 |
| `char *ritem; int rlbytes` | SET 페이로드를 직접 받을 아이템 메모리 포인터와 남은 길이 |
| `mc_resp *resp` | 응답 조립 객체 (iovec + item 참조) |
| `(*read)/(*sendmsg)/(*write)` | 전송 함수 포인터 (§4) |

### 5.2 `struct _stritem` (item) — KV 아이템 (데이터부의 단위, `memcached.h:597`)
key, flags, exptime, nbytes 등 메타데이터가 헤더에 오고, 값 데이터는 `ITEM_data(it)`가 가리키는
슬랩 풀 내부의 연속 영역에 저장된다. 해시체인(`h_next`)과 LRU(`next`/`prev`) 링크를 겸한다.

### 5.3 `struct mc_resp` — 응답 조립 (접합점, `memcached.h:779`)
값을 복사하지 않고 `item` 참조 + `iovec[]`로 가리켜 `sendmsg` scatter-gather로 전송하기 위한 구조.

---

## 6. 권장 학습 순서

1. **부팅 골격**: `main` → `memcached_thread_init` → `setup_thread` → `worker_libevent` (스레드/이벤트 뼈대)
2. **연결 수립**: `server_socket` → `dispatch_conn_new` → `conn_new` (전송 함수 바인딩 지점 확인)
3. **상태머신**: `drive_machine`의 17개 case를 훑고 `try_read_network`/`transmit` 정독
4. **SET 세로 관통**: `process_update_command` → `do_item_alloc` → `assoc_insert` (§3.2)
5. **GET 세로 관통**: `process_get_command` → `assoc_find` → `do_item_get` → `resp_add_iov` (§3.3)
6. **데이터부 심화**: `slabs.c`의 `slabs_init`/`do_slabs_alloc`로 메모리 풀 구조 이해

이 순서로 한 번 관통하면, 통신부(축2)와 데이터부(축1)가 코드 위치 단위로 손에 잡힌다.
