# memcached 1.6.42 → RDMA port 소스 변경 명세

> 비교 시점: 2026-07-24
> 기준 트리: `../memcached-1.6.42/`
> 포트 트리: 이 디렉터리 (`memcached-1.6.42-port/`, Git `178e16d` 이후
> 2026-07-24 remote-only invariant 수정 포함)
> 목적: 현재 소스에서 실제로 바뀐 함수, 자료구조, 호출 흐름과 추가 파일을
> 원본까지 역추적할 수 있게 기록한다.

## 1. 결론

이 포트는 memcached의 네트워크 프로토콜이나 인메모리 캐시 코어를 RDMA로 바꾼
것이 아니다. 변경 범위는 `EXTSTORE` 백엔드다.

| stock 1.6.42 | 현재 port |
|---|---|
| RAM item을 LRU 기반 background thread가 flash page로 내림 | SET 응답 전에 item별 RDMA WRITE를 완료하고 metadata stub만 publish |
| flash file + `pread`/`preadv`/`pwrite` | 원격 서버가 공개한 단일 MR + one-sided RDMA READ/WRITE |
| write buffer(wbuf), page eviction, compaction, recache | staging/bounce pool, remote slot allocator, copy-on-write overwrite |
| CRC32C로 읽은 item 검사 | 필수 AES-256-GCM 암호화와 무결성 검사 |
| file path 기반 `ext_path=/path:size[:bucket]` | IPv4 RDMA peer 기반 `ext_path=host:port:size` |
| 일반 heap buffer | SEV-SNP에서는 `/dev/snp_shared` mmap을 우선 사용 |

변경의 중심은 다음 7개 기존 소스 파일이다.

```text
Makefile.am
memcached.c
memcached.h
storage.c
storage.h
extstore.c
extstore.h
```

새 핵심 소스는 `ext_crypto.c`, `ext_crypto.h`,
`genie-server/genie_memd.c`, `test_ext_crypto.c`다. `tools/` 아래 파일은 실험과
장애 재현용이며 memcached 실행 파일에는 링크되지 않는다.

### 1.1 2026-07-24 remote-only 불변식 수정

아래 본문 중 `storage_flush_on_store()`, `storage_flush_item()`,
`storage_write_done_cb()`, `ITEM_RFLUSH`, in-place overwrite, RAM fallback을
설명하는 부분은 수정 전 구현의 역추적 기록이다. 현재 authoritative 경로는 다음과
같다.

```text
do_store_item()                           memcached.c
  -> storage_store_item()                 storage.c
       -> ITEM_HDR를 publish 전에 미리 할당
       -> extstore_alloc()로 새 remote loc 할당
       -> extstore_staging_get()           slot 소진 시 condition wait
       -> ext_crypto_seal()                AES-256-GCM, 반환 길이 검사
       -> extstore_submit(OBJ_IO_WRITE)
       -> completion condition wait
       -> 성공: loc을 담은 unlinked ITEM_HDR 반환
       -> 실패: staging/loc/header 반환, local value publish 금지
  -> 성공: do_item_link()/item_replace()로 ITEM_HDR만 hash에 publish
  -> overwrite: 새 stub publish 후 이전 remote loc 반환
  -> 실패: NOT_STORED
```

명세상 필수 조건:

- `STORED`는 해당 remote WRITE completion 이후에만 반환한다.
- storage-enabled hash table에는 full value item이 존재하면 안 된다.
- staging slot 부족은 local fallback이 아니라 backpressure다.
- GET은 `ITEM_HDR`를 통해 매번 remote READ하며 RAM full-item 복구 경로가 없다.
- `EXT_CRYPTO_KEY`는 필수이며 정확한 32-byte key를 읽지 못하면 시작을 실패한다.
- overwrite는 이전 published slot을 덮지 않고 새 slot에 기록한 뒤 stub을 교체한다.

검증은 `tools/remote-only-smoke.sh`로 수행한다. 저장·overwrite 후
`curr_items == extstore_objects_used`, 모든 성공 SET에 대해
`cmd_set == extstore_objects_written`, GET에 대해
`cmd_get == get_hits == extstore_objects_read`를 요구한다.

## 2. 비교 기준과 재현 방법

### 2.1 트리 식별

`memcached-1.6.42`는 Git 저장소가 아니다. 따라서 stock 쪽 식별은 아래 SHA-256이
기준이고, port 쪽은 Git commit과 SHA-256을 함께 쓴다.

| 파일 | stock SHA-256 | port SHA-256 |
|---|---|---|
| `Makefile.am` | `3c21dee8cafdba1763c3d3824f6e447d4f91a3cfc36d59b6465b6a4a92cdf482` | `acfda78ebbe4c6ac8fff757cfe6442ac2a3165a8b31a8505d898707645736270` |
| `extstore.c` | `ea87e8729e2286b250db354f52a8d1ca590f2f8a4e874d89ab0db56b59697dd1` | `59945457aba52b4264936767450afe273e65f7eb3e4dbf4598f5d5f7488d6335` |
| `extstore.h` | `2320a96ebc5cf6acdb145e9a90c7081cf6443fcea5420c2d3eec534dbd430e6c` | `c6be9258571436da67683baa1447f1135e167b4133f69a82b76c6569d6a53337` |
| `storage.c` | `7a79b806f0e40d9c58c0afef9a51241d2a9748f796186b2d17a5d56af6da7401` | `c0ca981df27a7f6e6c671f3bf8c457e4dbc8960dd34b88efeaa7d65ad7740eed` |
| `storage.h` | `9d1f4eb94f7f286ec26d9cff21b4804bdd46bb07c27a250a0ccd8e172f2313e5` | `5e5ab9dea883f8b541a35f665a04522f6016a0161088cc2ed724c8f9f32b15e3` |
| `memcached.c` | `3d175909ab9671dc6cca41f400c548500371a1d4cce4c60ce241ed30e3a129ce` | `53c4a988f36729987f0d9557dddba345761c8f72e764555d595732e0fbb77af3` |
| `memcached.h` | `61787c57d001528f191a07c35dc380fa8cf8e19930433745388a0818d3794762` | `be3dd02866448c71ec663f4ce792e95e77e8993a9dee0507dd1de42c6c46a6a2` |

현재 diff를 다시 만드는 명령:

```bash
cd /home/seonung/2026

for f in Makefile.am extstore.c extstore.h storage.c storage.h memcached.c memcached.h; do
    git diff --no-index --function-context \
        "memcached-1.6.42/$f" "memcached-1.6.42-port/$f" || true
done

sha256sum \
    memcached-1.6.42/{Makefile.am,extstore.c,extstore.h,storage.c,storage.h,memcached.c,memcached.h} \
    memcached-1.6.42-port/{Makefile.am,extstore.c,extstore.h,storage.c,storage.h,memcached.c,memcached.h}
```

### 2.2 소스 diff 크기

`git diff --no-index --numstat` 기준이다. 앞 숫자가 추가, 뒤 숫자가 삭제다.

| 파일 | 추가 | 삭제 | 판정 |
|---|---:|---:|---|
| `extstore.c` | 728 | 881 | flash engine을 RDMA engine으로 재작성 |
| `storage.c` | 347 | 947 | background flush/compaction 삭제, 즉시 flush 신설 |
| `extstore.h` | 51 | 28 | API와 IO 계약 교체 |
| `memcached.c` | 13 | 1 | store hook과 profile reset 추가 |
| `memcached.h` | 6 | 0 | in-flight flag와 remote length 추가 |
| `storage.h` | 3 | 0 | flush/profile 공개 함수 추가 |
| `Makefile.am` | 4 | 1 | RDMA/OpenSSL 링크 추가 |

### 2.3 비교에서 제외한 것

- `.deps/*.Po`, `*.o`, 실행 파일, coverage 파일(`*.gcda`, `*.gcno`,
  `*.gcov`)은 생성물이다.
- stock에만 있는 `EXTSTORE_RDMA_SPEC.md`, `EXTSTORE_RDMA_DECISIONS.md`,
  `LOCAL_THROUGHPUT_TEST.md`, `rdma-porting-refs/`는 설계/실험 자료다.
- port의 `conversation.md`, `.monitor/`는 소스 변경이 아니다.
- 생성된 `Makefile`의 링크 변경은 §11에서 별도로 다룬다.

## 3. 전체 런타임 구조

### 3.1 SET

```text
protocol SET
  -> store_item()
  -> do_store_item()                         memcached.c:1550
       -> item_replace() 또는 do_item_link()
       -> storage_flush_on_store()            memcached.c:1671,1713
            -> storage_flush_item()           storage.c:523
                 -> old stub slot 상속 또는 extstore_alloc()
                 -> staging slot 획득
                 -> [선택] ext_crypto_seal()
                 -> extstore_submit(WRITE)
  <- 클라이언트에는 STORED 반환

RDMA IO thread
  -> SYNC_FOR_DEVICE
  -> ibv_post_send(RDMA WRITE)
  -> ibv_poll_cq()
  -> storage_write_done_cb()                  storage.c:590
       -> 아직 같은 RAM item이면 ITEM_HDR stub으로 교체
       -> 같은 크기의 후속 RAM item이면 같은 slot으로 재-flush
       -> 삭제/크기 변경이면 기존 slot 반환
```

중요한 실제 계약:

- `STORED`는 RDMA WRITE 완료를 기다리지 않는다.
- WRITE 실패 시 클라이언트의 SET 성공 응답은 취소되지 않는다. full item은 RAM에
  남고 callback이 `ITEM_RFLUSH`를 지운다.
- 같은 크기의 기존 `ITEM_HDR`를 덮어쓸 때 remote address를 상속한다.
- “한 key당 한 WRITE”라는 주석과 달리 `ITEM_RFLUSH`는 **item 객체의 flag**다.
  교체로 새 item 객체가 생기면 key 전역 직렬화 장치는 아니다.

### 3.2 GET

```text
ASCII: process_get_cmd(..., storage_get_item, ...)
Binary: process_bin_get_or_touch()
  -> ITEM_HDR이면 storage_get_item()           storage.c:360
       -> 평문 item용 slab buffer 할당
       -> obj_io에 remote loc/len 기록
       -> worker IO queue에 적재
  -> storage_submit_cb()                       storage.c:445
       -> extstore_submit(READ)

RDMA IO thread
  -> bounce slot 배정
  -> ibv_post_send(RDMA READ)
  -> ibv_poll_cq()
  -> SYNC_FOR_CPU
  -> _storage_get_item_cb()                    storage.c:211
       -> [선택] GCM open into slab item
       -> tag fail이면 제한 횟수 재제출
       -> 소진 시 현재 hash item을 다시 찾아 RAM full item이면 복사
       -> hit/miss response iovec 확정
       -> return_io_pending()

worker thread
  -> storage_return_cb()                       storage.c:506
  -> response 전송
  -> storage_finalize_cb()
       -> recache 없이 임시 slab buffer 해제
```

ASCII 호출자는 `proto_text.c:445`, meta/ASCII 전달 지점은
`proto_text.c:1544,1564-1573`, binary 직접 호출은 `proto_bin.c:533`이다.
이 파일들은 두 트리에서 바뀌지 않았다. 기존 `storage_get_item` callback 경계를
그대로 이용했기 때문이다.

### 3.3 DELETE와 eviction

기존 `STORAGE_delete()` 호출자는 그대로다. 예:

```text
items.c:617,985,994,1121,1137,1204
slabs_mover.c:415,465
proto_parser.c:808,935,1386,1619
crawler.c:242
proto_bin.c:1158,1326
```

port의 `storage_delete()`는 stub의 `(page_id, page_version, offset, len)`으로:

1. `extstore_delete()`에서 page의 object/byte 회계를 내리고
2. `extstore_free_loc()`으로 remote slot을 LIFO free stack에 반환한다.

## 4. `memcached.c` 변경 함수

### 4.1 `stats_reset()` — `memcached.c:207`

stock:

```c
threadlocal_stats_reset();
item_stats_reset();
```

port:

```c
threadlocal_stats_reset();
item_stats_reset();
#ifdef EXTSTORE
storage_prof_reset();
#endif
```

`stats reset` 때 RDMA profile histogram만 함께 지운다. `extstore` 누적
read/write/failure counter나 `g_read_retry_ct` 같은 storage 전역 atomic counter는
지우지 않는다.

### 4.2 `do_store_item()` — `memcached.c:1550`

기존 item을 교체하는 분기:

```diff
- STORAGE_delete(t->storage, old_it);
  item_replace(old_it, it, hv, cas_in);
  stored = STORED;
+ if (t->storage)
+     storage_flush_on_store(t->storage, it, old_it, hv);
```

신규 item을 link하는 분기:

```diff
  do_item_link(it, hv, cas_in);
  stored = STORED;
+ if (t->storage)
+     storage_flush_on_store(t->storage, it, NULL, hv);
```

함수 진입 전 `store_item()`이 `item_lock(hv)`를 잡으므로 새 flush hook도 같은
key lock 안에서 호출된다(`thread.c:962-970`).

변경 이유:

- 기존 stub의 remote slot을 즉시 delete하면 같은 크기 overwrite가 그 주소를
  상속할 수 없다.
- background LRU flush가 제거됐으므로 성공한 store 지점에서 직접 WRITE를
  제출해야 한다.

## 5. `memcached.h` 변경 자료구조

### 5.1 `ITEM_RFLUSH` — `memcached.h:593`

```c
#define ITEM_RFLUSH 8192
```

한 item 객체의 remote WRITE가 비행 중임을 표시한다. `ITEM_PRESERVE_FLAGS`에는
포함되지 않으므로 생성된 `ITEM_HDR` stub으로 복사되지 않는다.

### 5.2 `item_hdr` — `memcached.h:680`

stock:

```c
typedef struct {
    unsigned int page_version;
    unsigned int offset;
    unsigned short page_id;
} item_hdr;
```

port:

```c
typedef struct {
    unsigned int page_version;
    unsigned int offset;
    unsigned int len;
    unsigned short page_id;
    unsigned short pad;
} item_hdr;
```

`len`은 remote object 전체 길이다. crypto on이면 `ITEM_ntotal(item) + 28`,
off이면 `ITEM_ntotal(item)`이다. 다음 두 곳에서 필수다.

- 같은 크기 overwrite인지 판정
- GET RDMA READ 길이와 slot 반환 길이 결정

`pad`는 구조체를 16 byte로 맞춘다.

## 6. `storage.c` 함수별 변경 명세

### 6.1 새 전역 상태와 타입 — `storage.c:17-56`

| 심볼 | 계약 |
|---|---|
| `PAGE_BUCKET_COUNT=1` | stock의 6개 flash/compaction bucket을 단일 remote size class로 축소 |
| `g_crypto_on` | `EXT_CRYPTO_KEY`를 읽었을 때만 true |
| `g_read_retries` | GCM tag fail 재시도 상한, 기본 3 |
| `g_read_retry_ct` | 실제 재제출 횟수 |
| `g_read_reresolve_ct` | retry 소진 뒤 RAM item으로 복구한 횟수 |
| `g_badcrc_log_ct` | badcrc 진단 로그 32회 제한 |
| `g_flush_log_ct` | crypto flush 진단 로그 200회 제한 |
| `g_abort_chunked` | chunked stub GET 거부 횟수 |
| `g_abort_alloc` | GET용 slab buffer 할당 실패 횟수 |
| `g_seal_tab` | `EXT_TRACE_SEAL`용 nonce→key/loc/len 진단 표 |
| `flush_ctx` | 비동기 WRITE 동안 item ref, hash, loc, staging slot, `obj_io` 소유 |

### 6.2 변경된 기존 함수

| 함수 | stock | port |
|---|---|---|
| `storage_delete()` | page 회계만 감소, 크기는 `ITEM_ntotal(stub)` 사용 | `hdr->len`으로 page 회계 후 `extstore_free_loc()` |
| `storage_stats()` | stock extstore 통계만 출력 | RDMA failure/retry/abort/profile 통계 19개 추가 |
| `_storage_get_item_cb()` | flash image의 CRC32C 검증 | bounce에서 GCM open 또는 memcpy; torn-read retry와 RAM 재조회 |
| `storage_get_item()` | chunked item/iovec 지원, destination이 `eio->buf` | chunked 거부, 별도 `read_it`, engine이 bounce 배정, `hdr->len` 사용 |
| `recache_or_free()` | hit를 확률적으로 RAM에 recache 가능 | recache 완전 제거; 읽기 buffer는 항상 해제 |
| `storage_finalize_cb()` | chunked read용 동적 iovec도 해제 | iovec 경로가 없어 `recache_or_free()`만 호출 |
| `storage_conf_parse()` | `/file:size[:bucket]` | `IPv4-or-local:port:size` |
| `storage_init_config()` | IO depth 1, bucket 6 | IO depth 64, bucket 1, slot 환경변수 기본값 추가 |
| `storage_init()` | compaction threshold 설정 후 extstore init | threshold 자동설정 제거, optional crypto key init 추가 |

`storage_validate_item()`, `process_extstore_stats()`, `storage_submit_cb()`,
`storage_return_cb()`, `storage_read_config()`, `storage_check_config()`는 본체 로직이
그대로다. 이 때문에 flash용 옵션과 검사 일부가 현재도 남아 있다.

### 6.3 `_storage_get_item_cb()` 상세 — `storage.c:211`

입력:

- `io->buf`: engine이 배정한 bounce slot
- `p->read_it`: 평문 item image를 받을 slab chunk
- `io->{page_id,page_version,offset,len}`: stub에서 복원한 remote 위치

crypto on:

```text
AAD = { hash(key), page_id, 0, offset, page_version }
ext_crypto_open(read_it, io->buf, io->len, &aad)
```

tag failure 처리:

1. `io->retries++ < g_read_retries`면 같은 `obj_io`를 `extstore_submit()`에
   다시 넣고 callback을 끝낸다.
2. 재시도 소진 시 item hash lock을 잡고 같은 key를 다시 찾는다.
3. 현재 item이 `ITEM_HDR`가 아니고 원 stub과 `ITEM_ntotal`이 같으면 그 RAM
   image를 `read_it`으로 복사해 hit 처리한다.
4. 아니면 `miss=true`, `badcrc=true`로 응답을 miss로 바꾼다.
5. 실패한 GCM open이 destination에 미검증 평문을 썼을 수 있으므로
   `read_it->it_flags=0`, 원래 slab class를 복원한다.

`recache_or_free()`는 `badcrc` miss일 때 기존 stub을 unlink하지 않는다. stock처럼
unlink하면 overwrite와 경합한 정상 key를 영구 삭제하고 remote slot도 누수시키기
때문이다.

### 6.4 `storage_get_item()` 상세 — `storage.c:360`

- `ITEM_ntotal(stub)` 크기의 일반 slab item을 `do_item_alloc_pull()`로 할당한다.
- `ntotal > settings.slab_chunk_size_max`면 명시적으로 `-1`을 반환한다.
- response data iovec은 먼저 빈 base로 예약한다.
- `eio->buf=NULL`: READ를 post할 때 `extstore_io_thread()`가 bounce slot을 배정한다.
- `eio->len=hdr->len`: remote crypto overhead까지 읽는다.
- callback에서 복호화가 끝나면 iovec base를 `ITEM_data(read_it)`으로 채운다.

### 6.5 새 WRITE 함수

#### `storage_flush_item()` — `storage.c:523`

내부 함수이며 caller는 `item_lock(hv)`를 보유해야 한다.

```text
1. chunked, ITEM_HDR, 빈 value(nbytes<=2), ITEM_RFLUSH면 skip
2. remote_len = ITEM_ntotal + (crypto ? 28 : 0)
3. old loc 길이가 같으면 in-place; 다르면 old loc 반환 후 새 loc 할당
4. staging slot 획득. 없으면 full item을 RAM에 둔 채 return
5. crypto on: item 전체 이미지를 AES-GCM seal
   crypto off: item 전체 이미지를 memcpy
6. flush_ctx 할당, ITEM_RFLUSH set, item refcount 증가
7. OBJ_IO_WRITE를 extstore_submit()
```

`ext_crypto_seal()`의 반환값은 현재 검사하지 않는다.

#### `storage_write_done_cb()` — `storage.c:590`

IO thread에서 실행된다.

| 완료 시 현재 hash 상태 | 동작 |
|---|---|
| WRITE 실패 | flag 제거, full item RAM 유지 |
| `cur == flushed item`이고 linked | `item_hdr` 할당 후 stub으로 교체 |
| 다른 full item, `ITEM_RFLUSH` 없음, remote length 같음 | 같은 loc으로 후속 flush chain |
| 삭제됨, 이미 stub, 다른 크기, 또는 다른 item이 자체 flush 중 | 이전 loc 반환 |

모든 경로에서 staging slot과 비행 중 보유한 item ref를 반환한다.

#### `storage_flush_on_store()` — `storage.c:643`

`memcached.c`에 노출된 wrapper다. `old_it`이 `ITEM_HDR`면 remote loc을 추출해
`storage_flush_item()`에 상속한다.

#### `storage_prof_reset()` — `storage.c:113`

전역 `ext_storage`가 있으면 `extstore_prof_reset()`을 호출한다.

### 6.6 삭제된 background 경로

다음 stock 구현은 port에서 삭제됐다.

| stock 함수/타입 | stock 위치 | 삭제 효과 |
|---|---:|---|
| `storage_write()` | `storage.c:498` | COLD LRU item을 골라 flash로 내리는 로직 제거 |
| `storage_write_thread()` | `storage.c:598` | memory pressure/age 기반 flush loop 제거 |
| `_storage_buk`, `_compact_flags` | `storage.c:775` | compaction 판단 자료구조 제거 |
| `storage_compact_check()` | `storage.c:798` | fragmented/old page 선택 제거 |
| `storage_compact_readback()` | `storage.c:932` | page readback 후 live item rewrite 제거 |
| `_storage_compact_cb()` | `storage.c:1086` | compaction read callback 제거 |
| `storage_compact_thread()` | `storage.c:1101` | compactor thread 제거 |

기존 caller를 바꾸지 않기 위해 아래 6개 public 함수는 삭제하지 않고 no-op으로
남겼다(`storage.c:655-660`).

```c
void storage_write_pause(void) { }
void storage_write_resume(void) { }
int start_storage_write_thread(void *arg) { (void)arg; return 0; }
void storage_compact_pause(void) { }
void storage_compact_resume(void) { }
int start_storage_compact_thread(void *arg) { (void)arg; return 0; }
```

따라서 `thread.c:164-177`, `memcached.c:6049-6053`의 기존 호출은 컴파일되지만
thread를 만들거나 pause하지 않는다.

## 7. `extstore.h` 계약 변경

### 7.1 `extstore_stats`

기존 필드에 다음을 추가했다.

```c
write_failures
read_failures
engine_dead

prof_read_count
prof_read_avg_ns
prof_read_p50_ns
prof_read_p99_ns
prof_write_count
prof_write_avg_ns
prof_write_p50_ns
prof_write_p99_ns

prof_read_sync_avg_ns
prof_read_xfer_avg_ns
prof_write_sync_avg_ns
prof_write_xfer_avg_ns
```

### 7.2 `extstore_conf`

기존 `page_size`, `page_buckets`, `io_threadcount`, `io_depth`는 재사용한다.
`free_page_buckets`, `wbuf_size`, `wbuf_count`는 ABI/파서 호환용으로 남았지만
engine에서 쓰지 않는다.

추가:

```c
unsigned int slot_size;
unsigned int read_slots;
unsigned int write_slots;
```

### 7.3 `extstore_conf_file`

`file`의 의미가 file path에서 RDMA peer host 문자열로 바뀌었다.

```c
char *file;  /* host 또는 "local" */
int cport;   /* RDMA CM port */
```

기존 `fd`, `offset`, bucket 필드는 port engine에서 사용하지 않는다.

### 7.4 `obj_io`

삭제:

```c
struct iovec *iov;
unsigned int iovcnt;
```

추가:

```c
unsigned char retries;
uint64_t t_start;
uint64_t t_end;
```

`buf`의 의미:

- READ: submit 시 NULL, post 시 engine이 bounce slot 주소를 기록
- WRITE: caller가 채운 staging slot

### 7.5 `ext_loc`

새 remote allocation 결과:

```c
struct ext_loc {
    unsigned int page_version;
    unsigned int offset;
    unsigned int len;
    unsigned short page_id;
};
```

### 7.6 public API 교체

삭제:

```c
extstore_write_request()
extstore_write()
extstore_submit_bg()
extstore_close_page()
extstore_evict_page()
```

추가:

```c
int extstore_alloc(void *, unsigned int len, unsigned int bucket,
                   struct ext_loc *out);
void extstore_free_loc(void *, const struct ext_loc *);
char *extstore_staging_get(void *);
void extstore_staging_put(void *, char *);
void extstore_prof_reset(void *);
```

유지됐지만 의미가 바뀐 API:

- `extstore_init()`
- `extstore_submit()`
- `extstore_delete()`
- `extstore_check()`
- `extstore_get_stats()`
- `extstore_get_page_data()`
- `extstore_err()`

`EXTSTORE_INIT_SELFTEST_FAIL` error code가 추가됐다.

## 8. `extstore.c` 함수별 변경 명세

### 8.1 삭제된 flash/wbuf 함수

| 함수 | stock 역할 |
|---|---|
| `thread_setname()` | extstore thread 이름 지정 |
| `wbuf_new()` | flash write buffer 할당 |
| `_get_io_thread()` | 가장 짧은 file IO queue 선택 |
| `_next_version()` | page 재활용 version 증가 |
| `_evict_page()` | bucket page 강제 회수 |
| `_allocate_page()` | free page를 bucket에 연결 |
| `_allocate_wbuf()` | page에 wbuf 연결 |
| `_wbuf_cb()` | `pwrite` 완료 뒤 wbuf 회수 |
| `_submit_wbuf()` | full wbuf를 background IO에 제출 |
| `extstore_write_request()` | wbuf 공간 예약 |
| `extstore_write()` | wbuf cursor와 page 회계 갱신 |
| `_extstore_submit()` | 지정 file IO thread queue에 연결 |
| `extstore_submit_bg()` | write/compact 전용 queue 제출 |
| `extstore_close_page()` | compaction 완료 page close |
| `extstore_evict_page()` | page eviction 통계/close |
| `_read_from_wbuf()` | 아직 disk에 안 내려간 wbuf에서 read |
| `_free_page()` | page reset 후 free bucket 반환 |

file descriptor, `pread`/`preadv`, `pwrite`, wbuf stack, background IO thread,
page close/eviction/refcount 기계가 함께 없어졌다.

### 8.2 새 내부 함수

| 함수 | 위치 | 명세 |
|---|---:|---|
| `prof_rdtsc()` | 37 | x86 TSC read |
| `prof_calibrate()` | 40 | 50 ms monotonic sleep으로 cycle→ns 보정 |
| `prof_record()` | 55 | 100 ns histogram bucket 기록 |
| `dma_alloc()` | 67 | `/dev/snp_shared` 우선, 실패 시 anonymous mmap |
| `cm_wait()` | 189 | 원하는 RDMA CM event 확인/ack |
| `cm_connect_one()` | 204 | IO thread 하나의 RC QP/CQ 생성과 connect |
| `genie_connect()` | 247 | 모든 IO thread QP 연결 |
| `selftest()` | 459 | remote offset 0에 pattern WRITE 후 READ-back |
| `grab_active()` | 647 | bucket active page 또는 새 free page 선택 |
| `prof_summarize()` | 771 | thread histogram merge와 avg/p50/p99 계산 |

### 8.3 재작성된 `extstore_init()` — `extstore.c:531`

RDMA backend:

```text
1. config/env를 engine에 복사
2. bounce/staging shared buffer 할당
3. IO thread state와 bounce bitmap 초기화
4. IO thread마다 rdma_cm connect
5. 첫 ESTABLISHED private_data에서 remote raddr/rkey/size 수신
6. bounce/staging을 같은 PD에 ibv_reg_mr(LOCAL_WRITE)
7. staging free stack 생성
8. [선택] EXT_SELFTEST
9. remote size를 page_size로 나눠 page array/free stack 생성
10. RDMA IO thread 시작
```

`local:0:size` backend는 RDMA 연결과 MR 등록 대신 `local_mem`에 `memcpy`한다.

RDMA 모드에서 실제 page count는 `ext_path`에 쓴 size가 아니라 genie가
private data로 보낸 `rsize / page_size`다. `ext_path` size는 기존
`storage_check_config()`의 사전 검사에는 쓰이지만 RDMA engine capacity를
결정하지 않는다.

### 8.4 연결 프로토콜

client와 server가 공유하는 packed 구조:

```c
struct xrd_mr_info {
    uint64_t raddr;
    uint32_t rkey;
    uint64_t size;
} __attribute__((packed));
```

첫 연결이 remote MR 정보를 정하고, IO thread 수만큼 RC connection/QP를 만든다.
주소 해석 순서:

```text
rdma_create_event_channel
rdma_create_id
rdma_resolve_addr
RDMA_CM_EVENT_ADDR_RESOLVED
rdma_resolve_route
RDMA_CM_EVENT_ROUTE_RESOLVED
ibv_alloc_pd(first only)
ibv_create_cq
rdma_create_qp
rdma_connect
RDMA_CM_EVENT_ESTABLISHED
```

현재 client parser는 `inet_pton(AF_INET)`만 쓰므로 numeric IPv4만 지원한다.
DNS 이름과 IPv6는 지원하지 않는다.

### 8.5 재작성된 `extstore_io_thread()` — `extstore.c:259`

#### queue와 backpressure

- `extstore_submit()`은 atomic round-robin으로 IO thread를 고른다.
- READ는 해당 thread의 `bounce_free` u64 bitmap에서 slot을 얻는다.
- bounce slot이 없거나 `outstanding == io_depth`면 queue에 남긴다.
- 한 posting round는 `EXT_WRITE_BATCH`와 32 중 작은 수까지 처리한다.
- 각 WR은 `IBV_SEND_SIGNALED`, SGE 1개, 독립 CQE다.

#### WRITE

```text
staging SGE 모음
-> ibv_advise_mr(SYNC_FOR_DEVICE, FLUSH)
-> linked WR list로 ibv_post_send(RDMA_WRITE)
-> ibv_poll_cq()
-> callback
-> staging slot은 storage callback이 반환
```

#### READ

```text
bounce slot 배정
-> ibv_post_send(RDMA_READ)
-> ibv_poll_cq()
-> 완료 READ SGE를 한 번에 ibv_advise_mr(SYNC_FOR_CPU, FLUSH)
-> callback에서 decrypt/copy
-> callback 반환 뒤 bounce slot 반환
```

custom advice 숫자는 stock header에 없을 때 다음 fallback을 쓴다.

```c
SYNC_FOR_CPU    = 3
SYNC_FOR_DEVICE = 4
```

driver가 지원하지 않으면 warning을 한 번 출력하지만 IO 자체는 계속한다.

#### fail-fast

`ibv_post_send` 실패나 error CQE:

1. `e->dead=1`
2. `engine_dead=1`
3. 방향별 failure counter 증가
4. callback에 `ret=-1`

이후 `extstore_submit()`은 verbs를 호출하지 않고 요청 chain 전체의 callback을
즉시 `-1`로 호출한다. 자동 reconnect는 없다.

### 8.6 remote slot allocator

#### `grab_active()` — `extstore.c:647`

bucket의 active page에 `allocated + len <= page_size` 공간이 있으면 재사용한다.
없으면 global free page stack에서 하나를 꺼내 bump cursor를 0으로 시작한다.

#### `extstore_alloc()` — `extstore.c:658`

1. `len > slot_size`면 실패
2. bucket free loc stack의 **top 하나만** 검사
3. top의 과거 길이가 요청 이상이면 pop하고 결과 `len`을 새 길이로 덮음
4. 아니면 active page에서 bump allocation

혼합 크기에서 top이 너무 작으면 stack 아래의 적합한 slot을 찾지 않는다.
고정 크기 workload를 위한 의도적 단순화다.

#### `extstore_free_loc()` — `extstore.c:690`

loc을 원 page bucket의 동적 LIFO stack에 push하고 global
`bytes_used`/`objects_used`를 감소시킨다.

주의: size-change overwrite의 old loc 반환은 `extstore_free_loc()`만 호출하고
`extstore_delete()`를 호출하지 않는다. 따라서 현재 코드에서 page별
`obj_count`/`bytes_used`와 global 회계가 서로 다를 수 있다. 재사용 pop 경로도
global 사용량을 다시 증가시키지 않는다.

현재 WRITE 실패/skip 경로의 loc 처리도 서로 다르다.

- 신규 loc을 잡은 뒤 staging 또는 `flush_ctx` 할당이 실패하면 신규 loc을 반환한다.
- in-place 상속 loc에서 같은 실패가 나면 loc을 반환하지 않는다. 이미 old stub은
  hash에서 교체된 뒤이므로 그 loc을 다시 가리키는 stub이 없어질 수 있다.
- RDMA WRITE가 실패한 callback은 full item을 RAM에 남기지만 loc을 반환하지 않는다.
- WRITE는 성공했으나 callback의 `item_hdr` 할당이 실패해도 loc을 반환하지 않는다.
- old stub을 chunked/빈 item으로 교체해 `storage_flush_item()`의 첫 guard에서
  return하면 상속 loc 정리 경로를 타지 않는다.

즉 “remote full이면 RAM fallback”은 데이터 가용성 동작이고, 모든 실패에서 remote
capacity까지 회수된다는 뜻은 아니다.

### 8.7 유지된 page API의 실제 의미

- `extstore_check()`: page id 범위와 version 일치만 확인
- `extstore_delete()`: page별 object/byte 회계만 감소
- `extstore_get_page_data()`: version/bytes/bucket/active snapshot

port에는 page reclaim이나 version 증가 경로가 없다. page version은 init 때 1로
설정되고, 할당된 page의 `active`도 다시 false가 되지 않는다.

`page_id`는 `uint16_t`지만 port `extstore_init()`은
`remote_size / page_size < UINT16_MAX`를 검사하지 않는다. page 수가 65,535 이상인
설정은 `p->id = i`에서 잘린 id를 만들 수 있다.

## 9. 암호화 소스

### 9.1 새 파일

- `ext_crypto.h`
- `ext_crypto.c`
- `test_ext_crypto.c`

`storage.c:15`가 `#include "ext_crypto.c"`로 구현 파일을 직접 포함한다.
따라서 `ext_crypto.c`는 별도 object로 `Makefile.am`에 등록되지 않는다.

### 9.2 remote object format

crypto on:

```text
offset + 0                 12 B nonce
offset + 12                ITEM_ntotal(item) B ciphertext
offset + 12 + ntotal       16 B GCM tag

remote_len = ntotal + 28
```

crypto off:

```text
ITEM 전체 image 그대로
remote_len = ntotal
```

nonce:

```text
4 B boot salt from getrandom()
8 B process-global atomic counter
```

AAD:

```c
struct ext_aad {
    uint32_t hv;
    uint16_t page_id;
    uint16_t pad;
    uint32_t offset;
    uint32_t page_version;
};
```

AES-256-GCM OpenSSL EVP를 쓴다.

### 9.3 함수 계약

| 함수 | 계약 |
|---|---|
| `ext_crypto_init(key[32])` | key 복사, 4-byte random salt 생성, counter 0 초기화; random 실패 시 abort |
| `make_nonce()` | salt와 `atomic_fetch_add` counter로 12-byte nonce 생성 |
| `ext_crypto_seal()` | nonce/ciphertext/tag 작성, 성공 시 `ptlen+28`, 실패 시 -1 |
| `ext_crypto_open()` | tag까지 검증, 성공 시 plaintext 길이, 실패 시 -1 |

`EXT_CRYPTO_KEY=/path`가 있을 때만 crypto가 켜진다. 파일에서 처음 32 byte를
읽으며, 추가 byte와 파일 permission은 검사하지 않는다. 환경변수가 없으면
평문 remote object가 정상 동작 경로다.

### 9.4 self-check

`test_ext_crypto.c`는 다음을 assert한다.

1. round trip
2. AAD mismatch reject
3. ciphertext 1-byte 변조 reject
4. 연속 seal의 nonce 비중복

독립 실행:

```bash
cd /home/seonung/2026/memcached-1.6.42-port
cc -o /tmp/test_ext_crypto test_ext_crypto.c ext_crypto.c -lcrypto
/tmp/test_ext_crypto
```

## 10. 원격 서버 추가

### 10.1 `genie-server/genie_memd.c`

memcached process와 별도인 passive one-sided RDMA server다.

함수:

| 함수 | 위치 | 역할 |
|---|---:|---|
| `dief()` | 27 | 메시지 출력 후 exit 1 |
| `peer()` | 31 | 연결 peer IPv4:port 문자열 |
| `parse_size()` | 42 | k/m/g/t suffix를 byte로 변환 |
| `on_usr1()` | 58 | MR dump flag 설정 |
| `dump_mr()` | 66 | fill byte와 다른 구간/총량 출력 |
| `main()` | 85 | listen, MR 등록, QP accept, disconnect 처리 |

실행 계약:

```bash
cc -O2 -o genie_memd genie_memd.c -lrdmacm -libverbs
./genie_memd <port> <size> [--hugepages] [--prefill]
```

- 첫 `CONNECT_REQUEST`에서 anonymous 또는 `MAP_HUGETLB` memory를 할당한다.
- 단일 PD/MR을 등록하고 모든 client connection에 공유한다.
- remote READ/WRITE 권한을 준다.
- `rdma_accept()` private data로 `(raddr,rkey,size)`를 전달한다.
- `--prefill`은 MR을 `0xAA`로 채워 “WRITE 미도착”과 “0 전송”을 구분한다.
- `SIGUSR1`은 MR에서 fill과 달라진 첫 구간과 전체 byte 수를 출력한다.
- disconnect event는 먼저 ack한 뒤 QP/ID를 destroy한다.
- listen backlog은 1024다.
- process lifetime 동안 MR은 유지되며 정상 종료/해제 경로는 없다.

이 server는 top-level `Makefile.am` target이 아니다. 수동으로 별도 빌드해야 한다.

## 11. 빌드 변경

`Makefile.am`:

```diff
-memcached_LDADD =
+memcached_LDADD = -lrdmacm -libverbs -lcrypto
+memcached_debug_LDADD += -lrdmacm -libverbs -lcrypto
```

생성된 현재 `Makefile`도 같은 세 library가 직접 추가돼 있다.

주의:

- `Makefile.in`은 두 트리에서 동일하며 링크 변경이 반영되지 않았다.
- 현재 tree의 `Makefile`로 `make`하면 세 library를 링크한다.
- 현재 `Makefile.in`을 그대로 두고 `./configure`만 다시 실행하면 링크 변경이
  사라질 수 있다.
- `Makefile.am`에서 재생성하려면 적합한 autotools로 `Makefile.in`까지 갱신해야 한다.
- RDMA/OpenSSL library는 `EXTSTORE` 사용 여부와 무관하게 memcached target에
  unconditional link된다.
- profile 구현은 `__builtin_ia32_rdtsc()`를 무조건 포함하므로 현재
  `extstore.c`는 x86 전제다.

## 12. 설정과 환경변수

### 12.1 `-o` 옵션

실제로 의미가 바뀐 옵션:

| 옵션 | port 의미 |
|---|---|
| `ext_path=host:port:size` | RDMA peer와 사전검사용 size |
| `ext_threads=N` | IO thread 수 = RC QP 수 |
| `ext_io_depth=N` | thread당 최대 outstanding |
| `ext_page_size=MiB` | remote MR을 나누는 page 크기 |

local memcpy backend는 삭제되었다. `ext_path`는 반드시 Genie endpoint여야 하며
`ext_path=local:...`은 RDMA 연결 실패로 시작되지 않는다.

### 12.2 환경변수

| 변수 | 기본 | 코드 위치 | 의미 |
|---|---:|---:|---|
| `EXT_SLOT_SIZE` | 2048 | `storage.c:729` | bounce/staging slot byte |
| `EXT_READ_SLOTS` | 32 | `storage.c:730` | IO thread당 READ slot, engine에서 최대 64 |
| `EXT_WRITE_SLOTS` | 256 | `storage.c:731` | process 전체 staging slot |
| `EXT_READ_RETRIES` | 3 | `storage.c:732` | GCM tag fail 재시도 |
| `EXT_CRYPTO_KEY` | unset | `storage.c:988` | 32-byte AES key file |
| `EXT_TRACE_SEAL` | unset | `storage.c:733` | nonce 기반 seal 진단 표 |
| `EXT_RDMA_PROF` | unset | `extstore.c:546` | in-server span profile |
| `EXT_WRITE_BATCH` | 32 | `extstore.c:547` | 1..32 posting round cap |
| `EXT_SELFTEST` | unset | `extstore.c:611` | startup remote write/read-back |

환경변수 숫자는 `atoi`/`strtoul` 결과에 대한 범위 검사가 제한적이다.
예를 들어 `EXT_READ_SLOTS=0`이면 READ용 slot이 없어 queue가 진행하지 않는다.

### 12.3 제거된 stock local-cache 옵션

background flush, recache, compaction, local memcpy backend와 그 설정/parser/stat은
소스에서 삭제되었다. 따라서 `ext_item_age`, `ext_item_size`,
`ext_recache_rate`, `ext_wbuf_size`, `ext_low_ttl`, `ext_compact_under`,
`ext_drop_under`, `ext_max_sleep`, `ext_max_frag`, `ext_drop_unread`,
`slab_automove_freeratio`를 지정하면 `Illegal suboption`으로 시작이 실패한다.

실제 remote WRITE 가능 여부를 결정하는
`ITEM_ntotal + AES-256-GCM overhead <= EXT_SLOT_SIZE`는 요청별로 검사한다.

## 13. 통계와 계측

### 13.1 장애/정합성 통계

```text
extstore_engine_dead
extstore_write_failures
extstore_read_failures
extstore_read_retries
extstore_read_reresolved
extstore_get_aborted_chunked
extstore_get_aborted_alloc
```

### 13.2 profile span

`EXT_RDMA_PROF=1`일 때:

WRITE:

```text
total = SYNC_FOR_DEVICE 직전 -> WRITE CQE poll
sync  = SYNC_FOR_DEVICE 직전 -> advice 반환
xfer  = advice 반환 -> WRITE CQE poll
```

READ:

```text
total = ibv_post_send 직전 -> SYNC_FOR_CPU 반환
xfer  = ibv_post_send 직전 -> READ CQE poll
sync  = READ CQE poll -> SYNC_FOR_CPU 반환
```

출력:

```text
extstore_prof_read_count/avg_ns/p50_ns/p99_ns
extstore_prof_write_count/avg_ns/p50_ns/p99_ns
extstore_prof_read_sync_avg_ns/read_xfer_avg_ns
extstore_prof_write_sync_avg_ns/write_xfer_avg_ns
```

histogram은 100 ns × 32,768 bucket이며 3.2767 ms 이상은 마지막 bucket으로
clamp된다. `storage.c`의 `stats reset`이 histogram과 profile sum을 지운다.

현재 source comment `extstore.c:135`의 “~102us”는 8,192 bucket 시절의 흔적으로,
현재 상수/배열의 실제 상한과 다르다.

### 13.3 제거된 legacy stat

recache, compaction, page eviction/reclaim, local-value memory-pressure 통계는 해당
기능과 함께 삭제되었다. `extstore_io_queue`는 호환용으로 남아 있으나 현재 port가
갱신하지 않으므로 처리율 판단에 사용하지 않는다.

## 14. 추가 도구

| 파일 | 목적 | memcached build 포함 |
|---|---|---|
| `tools/advise-cost.c` | 일반/snp_shared MR에서 `ibv_advise_mr` 호출 비용과 SGE batch sweep | 아니오 |
| `tools/torn-repro.sh` | 같은 크기 SET/GET 경합으로 torn-read 재현 | 아니오 |
| `tools/mixed-size-stress.sh` | 혼합 크기 free-list 용량/재사용 관찰 | 아니오 |
| `tools/d6-sweep.sh` | guest에서 SET/GET/1:9 profile+throughput sweep | 아니오 |
| `tools/remote-only-smoke.sh` | SET/overwrite/GET의 remote counter 일치 불변식 검사 | 아니오 |
| `tools/loopback-mirror.sh` | 비TEE mirror sweep | 아니오 |
| `tools/commit-monitor.sh` | 협업 Git channel poller | 아니오 |

경로 전제:

- `d6-sweep.sh`: `$HOME/kvs-port`
- `loopback-mirror.sh`: `/home/seonung/2026/memcached-1.6.42_ported`
- 현재 checkout: `/home/seonung/2026/memcached-1.6.42-port`

따라서 두 sweep script는 현재 디렉터리 이름에서 그대로 실행되는 일반화된
runbook이 아니다.

## 15. 기존 설계 문서와 현재 코드의 차이

port source comment는 `EXTSTORE_RDMA_SPEC.md`를 참조하지만 그 파일은 stock
디렉터리에만 있고 port Git에는 없다. 그 문서는 구현 전 명세이므로 현재 코드와
다음이 다르다.

| 옛 명세 | 현재 코드 |
|---|---|
| `items.c`에 local serve counter 추가 | `items.c`는 두 트리에서 동일 |
| `ext_threads` 기본 8 | `storage_init_config()` 기본 1 |
| slot/key/retry를 `-o` suboption으로 제공 | 환경변수만 사용 |
| key file 0600 및 정확한 format 검증 | 처음 32 byte read만 확인 |
| 최대 item과 slot을 startup에서 검증 | 요청 시 `extstore_alloc()`에서만 길이 거부 |
| `slab_automove_extstore.c` build 제외 | 소스 파일과 build reference 모두 삭제 |
| key당 정확히 한 in-flight WRITE | flag가 item 객체에만 붙음 |
| 여러 local-serve/flush-skip/remote-full counter | 현재 구현에는 없음 |
| `Makefile.am` 기반 완전한 build 재생성 | `Makefile.am`/`Makefile.in` 모두 현재 source list와 일치 |

현재 동작을 설명할 때는 이 문서를 기준으로 하고, 옛 spec은 결정 배경을 확인할
때만 사용해야 한다.

## 16. Git 이력으로 역추적

port의 초기 commit `ac97727`에 이미 stock 대비 RDMA rewrite가 들어 있다.
즉 stock→최초 port 변화는 Git commit 하나의 patch로 복원하기보다
§2의 sibling-tree `--no-index` diff가 정확한 기준이다.

그 이후 안정화 이력:

| commit | 변경 |
|---|---|
| `ac97727` | 최초 RDMA/crypto/server 구현 |
| `194172d` | rdma_cm 기반 `genie_memd` 배포와 client 연결 수정 |
| `1012a8e` | 생성 Makefile RDMA link와 byte/object counter |
| `221ccb0` | completion callback 이후 UAF, server disconnect deadlock 수정 |
| `68f9e4e` | unsupported `SYNC_FOR_CPU` warning |
| `212a146` | decrypt-fail slab header 복구, `/dev/snp_shared`, 상세 init log |
| `41af207` | `EXT_SELFTEST` |
| `b110ab0` | WRITE 전 `SYNC_FOR_DEVICE` |
| `bc516e0`, `c1c408e` | torn-read retry 소진 시 RAM 재조회, badcrc key unlink 방지 |
| `ed5ef15`, `cedfca8`, `ad0074a` | nonce/loc/length 진단과 pre-read reject counter |
| `04e2f3f` | selftest 양방향 sync |
| `c739953` | freed slot stale length/성장 overrun 수정 |
| `c87c2ea` | total span histogram과 batch cap |
| `e49469f` | sync/xfer breakdown |
| `f06356d` | histogram 상한 3.27 ms로 확대 |

함수의 현재 line을 만든 commit 확인:

```bash
cd /home/seonung/2026/memcached-1.6.42-port

git blame -L 523,650 storage.c
git blame -L 259,447 extstore.c
git log -L :storage_flush_item:storage.c
git log -L :extstore_io_thread:extstore.c
git show c739953 -- extstore.c tools/lentest-sizechange.sh
git show c87c2ea -- extstore.c extstore.h storage.c memcached.c storage.h
```

경로별 전체 변경 commit:

```bash
git log --reverse --stat -- \
    Makefile Makefile.am \
    memcached.c memcached.h storage.c storage.h extstore.c extstore.h \
    ext_crypto.c ext_crypto.h test_ext_crypto.c \
    genie-server/genie_memd.c tools
```

## 17. 변경되지 않은 경계

다음 핵심 경계는 stock과 동일하다.

- ASCII/binary/meta parser
- client socket과 libevent state machine
- hash table, item lock, slab allocator, segmented LRU
- worker의 `io_pending` suspend/return 메커니즘
- `ITEM_HDR`를 만났을 때 `storage_get_item`으로 넘기는 protocol callback
- `STORAGE_delete`를 호출하는 delete/expiry/crawler/slab-move 지점

즉 port를 디버깅할 때 우선순위는 다음이다.

```text
SET 실패 또는 metadata stub 미생성
  -> storage_store_item의 alloc/staging/seal/WRITE completion 확인

stub은 생겼지만 GET miss/badcrc
  -> hdr loc/len -> RDMA READ -> sync -> GCM AAD/tag 순서 확인

engine_dead
  -> 최초 post_send/error CQE와 rdma_cm/QP 상태 확인

capacity 증가/회계 이상
  -> extstore_alloc/free_loc의 LIFO top-only 재사용과 page/global stat 분리 확인
```

## 18. 이 문서 작성 시 검증한 범위

2026-07-24 현재 checkout에서 다음을 실행해 통과했다.

```bash
cd /home/seonung/2026/memcached-1.6.42-port
make -j24 memcached
./testapp
# 56/56 ok

cc -Wall -Wextra -o /tmp/memcached-port-test-ext-crypto \
    test_ext_crypto.c ext_crypto.c -lcrypto
/tmp/memcached-port-test-ext-crypto
# ext_crypto: ok

cc -Wall -Wextra -O2 -o /tmp/memcached-port-genie-memd \
    genie-server/genie_memd.c -lrdmacm -libverbs

cc -Wall -Wextra -O2 -o /tmp/memcached-port-advise-cost \
    tools/advise-cost.c -libverbs

strings memcached | grep -Ei \
  'ext_item_(size|age)|ext_recache|recache_from_extstore|extstore_compact'
# 출력 없음
```

같은 바이너리를 SEV-SNP guest에 배포해 1 QP/depth 1로 startup selftest와
`tools/remote-only-smoke.sh`를 통과했다. smoke의 100 SET + 100 overwrite +
100 GET에서 `cmd_set=extstore_objects_written=200`,
`cmd_get=extstore_objects_read=100`, `badcrc=engine_dead=read/write_failures=0`을
확인했다. 결과는 guest의
`/home/ubuntu/rdma-results/no-local-cache-20260724-060638/`에 보존했다.
