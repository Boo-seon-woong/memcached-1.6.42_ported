/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* extstore RDMA backend: one-sided READ/WRITE to a remote (genie) MR.
 * Replaces the flash/wbuf engine. See EXTSTORE_RDMA_SPEC.md.
 * Storage model: remote memory sliced into pages; each page bump-allocates
 * fixed-max slots. In-place overwrite (P-1a) means alloc is cold after preload.
 */
#include "config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <infiniband/verbs.h>
#include "extstore.h"

/* Custom advice added by the SEV SWIOTLB-sync kernel patch (rdma-porting-refs/).
 * Builds against stock headers; unpatched libibverbs returns an error we tolerate. */
#ifndef IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU
#define IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU 3
#endif

#define XRD_MAGIC "XRD1"
#define IB_PORT 1
#define GID_IDX 0
#define PSN 0

#define STAT_L(e)   pthread_mutex_lock(&e->stats_mutex)
#define STAT_UL(e)  pthread_mutex_unlock(&e->stats_mutex)

typedef struct _store_page {
    pthread_mutex_t mutex;
    uint64_t obj_count, bytes_used;
    uint64_t remote_off;      /* byte offset of this page within the remote MR */
    uint32_t version;
    uint32_t allocated;       /* bump cursor within page */
    uint16_t id, bucket;
    bool active, free;
    struct _store_page *next; /* free-page stack link */
} store_page;

/* per-bucket freed-slot stack (holes from delete/resize; cold path) */
struct loc_stack { struct ext_loc *arr; int top, cap; };

typedef struct store_engine store_engine;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    obj_io *queue, *queue_tail;
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    char *bounce_base;            /* read_slots * slot_size */
    uint64_t bounce_free;         /* bit=1 -> slot free (read_slots <= 64) */
    unsigned int outstanding;
    store_engine *e;
    pthread_t tid;
} store_iothr;

struct store_engine {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *bounce_mr, *staging_mr;
    uint64_t raddr; uint32_t rkey;       /* genie MR */
    /* ponytail: local test backend — remote memory is a local buffer, RDMA is
     * memcpy. Lets the full flush/read/crypto path run under memtier without
     * genie/RDMA (ext_path=local:0:<size>). Not a shipping path. */
    bool local;
    char *local_mem;
    store_page *pages;
    store_page **page_buckets;           /* active page per bucket */
    store_page *free_pages;              /* stack of unused pages */
    struct loc_stack *freeloc;           /* [page_bucketcount] */
    size_t page_size;
    unsigned int page_count, page_bucketcount;
    unsigned int slot_size, read_slots;
    store_iothr *io_threads;
    unsigned int io_threadcount, io_depth;
    unsigned int last_io_thread;
    /* staging pool (write path) */
    char *staging_base;
    char **staging_free; int staging_top; unsigned int staging_count;
    pthread_mutex_t staging_mutex;
    atomic_uint_fast64_t dead;           /* QP error -> fail-fast */
    pthread_mutex_t mutex;               /* pages / buckets / freeloc */
    struct extstore_stats stats;
    pthread_mutex_t stats_mutex;
};

/* ---- small helpers ---- */

static int read_full(int fd, void *b, size_t n) {
    uint8_t *p = b; while (n) { ssize_t r = read(fd, p, n); if (r <= 0) return -1; p += r; n -= r; } return 0;
}
static int write_full(int fd, const void *b, size_t n) {
    const uint8_t *p = b; while (n) { ssize_t r = write(fd, p, n); if (r <= 0) return -1; p += r; n -= r; } return 0;
}

const char *extstore_err(enum extstore_res res) {
    const char *rv = "unknown error";
    switch (res) {
        case EXTSTORE_INIT_OOM: rv = "failed to allocate engine"; break;
        case EXTSTORE_INIT_OPEN_FAIL: rv = "failed to open RDMA device / connect genie"; break;
        case EXTSTORE_INIT_THREAD_FAIL: rv = "failed to spawn IO thread"; break;
        default: break;
    }
    return rv;
}

/* ---- RDMA connection bootstrap (client side of SPEC §2.7) ---- */

static int qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr a = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = IB_PORT,
        .qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE };
    return ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
            IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static int qp_to_rts(struct ibv_qp *qp, uint32_t dqpn, uint32_t dpsn,
                     const union ibv_gid *dgid) {
    struct ibv_qp_attr r = { .qp_state = IBV_QPS_RTR, .path_mtu = IBV_MTU_1024,
        .dest_qp_num = dqpn, .rq_psn = dpsn, .max_dest_rd_atomic = 16,
        .min_rnr_timer = 12,
        .ah_attr = { .is_global = 1, .port_num = IB_PORT,
            .grh = { .hop_limit = 1, .sgid_index = GID_IDX } } };
    memcpy(&r.ah_attr.grh.dgid, dgid, 16);
    if (ibv_modify_qp(qp, &r, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
            IBV_QP_MIN_RNR_TIMER)) return -1;
    struct ibv_qp_attr t = { .qp_state = IBV_QPS_RTS, .sq_psn = PSN,
        .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 16 };
    return ibv_modify_qp(qp, &t, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
            IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
}

static int genie_connect(store_engine *e, const char *host, int port,
                         uint64_t *size_out) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *ai; char pbuf[16]; snprintf(pbuf, sizeof(pbuf), "%d", port);
    if (getaddrinfo(host, pbuf, &hints, &ai)) return -1;
    int s = socket(ai->ai_family, ai->ai_socktype, 0);
    if (s < 0 || connect(s, ai->ai_addr, ai->ai_addrlen)) { freeaddrinfo(ai); return -1; }
    freeaddrinfo(ai);

    union ibv_gid gid;
    if (ibv_query_gid(e->ctx, IB_PORT, GID_IDX, &gid)) { close(s); return -1; }
    uint32_t nqp = e->io_threadcount;

    /* client -> genie: [magic][nqp][nqp*{qpn,psn}][gid] */
    if (write_full(s, XRD_MAGIC, 4) || write_full(s, &nqp, 4)) { close(s); return -1; }
    for (uint32_t i = 0; i < nqp; i++) {
        uint32_t qpn = e->io_threads[i].qp->qp_num, psn = PSN;
        if (write_full(s, &qpn, 4) || write_full(s, &psn, 4)) { close(s); return -1; }
    }
    if (write_full(s, &gid, 16)) { close(s); return -1; }

    /* genie -> client: [magic][raddr][rkey][size][nqp*{qpn,psn}][gid] */
    char magic[4];
    if (read_full(s, magic, 4) || memcmp(magic, XRD_MAGIC, 4)) { close(s); return -1; }
    if (read_full(s, &e->raddr, 8) || read_full(s, &e->rkey, 4) ||
        read_full(s, size_out, 8)) { close(s); return -1; }
    struct { uint32_t qpn, psn; } srv[64];
    if (read_full(s, srv, nqp * 8)) { close(s); return -1; }
    union ibv_gid sgid;
    if (read_full(s, &sgid, 16)) { close(s); return -1; }
    close(s);

    for (uint32_t i = 0; i < nqp; i++)
        if (qp_to_rts(e->io_threads[i].qp, srv[i].qpn, srv[i].psn, &sgid)) return -1;
    return 0;
}

/* ---- IO thread: post batch -> poll -> batched sync -> cb (SPEC §6) ---- */

static void *extstore_io_thread(void *arg) {
    store_iothr *t = arg;
    store_engine *e = t->e;
    unsigned int depth = e->io_depth;
    struct ibv_wc wc[32];
    struct ibv_send_wr wrs[32], *bad;
    struct ibv_sge sg[32], sync_sg[32];

    while (1) {
        pthread_mutex_lock(&t->mutex);
        while (!t->queue && !t->outstanding)
            pthread_cond_wait(&t->cond, &t->mutex);

        if (e->local) {   /* memcpy transport (no RDMA); remote == local_mem */
            obj_io *batch = t->queue; t->queue = t->queue_tail = NULL;
            pthread_mutex_unlock(&t->mutex);
            unsigned int reads = 0;
            while (batch) {
                obj_io *io = batch; batch = io->next;
                char *rem = e->local_mem + e->pages[io->page_id].remote_off + io->offset;
                if (io->mode == OBJ_IO_READ) { io->buf = rem; reads++; }
                else memcpy(rem, io->buf, io->len);
                io->cb(e, io, (int)io->len);
            }
            STAT_L(e); e->stats.objects_read += reads; STAT_UL(e);
            continue;
        }

        int n = 0;
        while (t->queue && t->outstanding + n < depth && n < 32) {
            obj_io *io = t->queue;
            int slot = -1;
            if (io->mode == OBJ_IO_READ) {
                if (t->bounce_free == 0) break;         /* no bounce slot: backpressure */
                slot = __builtin_ffsll((long long)t->bounce_free) - 1;
            }
            t->queue = io->next;
            if (!t->queue) t->queue_tail = NULL;
            if (io->mode == OBJ_IO_READ) {
                t->bounce_free &= ~(1ULL << slot);
                io->buf = t->bounce_base + (size_t)slot * e->slot_size;
            }
            sg[n] = (struct ibv_sge){ .addr = (uintptr_t)io->buf, .length = io->len,
                .lkey = (io->mode == OBJ_IO_READ) ? e->bounce_mr->lkey
                                                  : e->staging_mr->lkey };
            wrs[n] = (struct ibv_send_wr){ .wr_id = (uintptr_t)io, .sg_list = &sg[n],
                .num_sge = 1, .send_flags = IBV_SEND_SIGNALED,
                .opcode = (io->mode == OBJ_IO_READ) ? IBV_WR_RDMA_READ
                                                    : IBV_WR_RDMA_WRITE };
            wrs[n].wr.rdma.remote_addr = e->raddr +
                    e->pages[io->page_id].remote_off + io->offset;
            wrs[n].wr.rdma.rkey = e->rkey;
            wrs[n].next = NULL;
            if (n) wrs[n-1].next = &wrs[n];
#ifdef EXT_RDMA_PROF
            io->t_post = __builtin_ia32_rdtsc();
#endif
            n++;
        }
        pthread_mutex_unlock(&t->mutex);

        if (n) {
            if (ibv_post_send(t->qp, &wrs[0], &bad)) {
                atomic_store(&e->dead, 1);
                for (int i = 0; i < n; i++) {
                    obj_io *io = (obj_io *)(uintptr_t)wrs[i].wr_id;
                    if (io->mode == OBJ_IO_READ)
                        t->bounce_free |= 1ULL << ((io->buf - t->bounce_base) / e->slot_size);
                    io->cb(e, io, -1);
                }
            } else {
                pthread_mutex_lock(&t->mutex); t->outstanding += n; pthread_mutex_unlock(&t->mutex);
            }
        }

        int c = ibv_poll_cq(t->cq, 32, wc);
        if (c <= 0) { if (t->outstanding) sched_yield(); continue; }

        /* batched SWIOTLB->private sync for READs in this batch */
        int nsync = 0;
        for (int i = 0; i < c; i++) {
            obj_io *io = (obj_io *)(uintptr_t)wc[i].wr_id;
            if (io->mode == OBJ_IO_READ && wc[i].status == IBV_WC_SUCCESS)
                sync_sg[nsync++] = (struct ibv_sge){ .addr = (uintptr_t)io->buf,
                    .length = io->len, .lkey = e->bounce_mr->lkey };
        }
        if (nsync)
            ibv_advise_mr(e->pd, IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU,
                          IBV_ADVISE_MR_FLAG_FLUSH, sync_sg, nsync);

        for (int i = 0; i < c; i++) {
            obj_io *io = (obj_io *)(uintptr_t)wc[i].wr_id;
#ifdef EXT_RDMA_PROF
            io->t_cqe = __builtin_ia32_rdtsc();
#endif
            int ok = (wc[i].status == IBV_WC_SUCCESS);
            if (!ok) atomic_store(&e->dead, 1);
            io->cb(e, io, ok ? (int)io->len : -1);
            if (io->mode == OBJ_IO_READ) {
                int s = (io->buf - t->bounce_base) / e->slot_size;
                pthread_mutex_lock(&t->mutex); t->bounce_free |= 1ULL << s; pthread_mutex_unlock(&t->mutex);
            }
        }
        pthread_mutex_lock(&t->mutex); t->outstanding -= c; pthread_mutex_unlock(&t->mutex);
        STAT_L(e); e->stats.objects_read += nsync; STAT_UL(e);
    }
    return NULL;
}

/* ---- init ---- */

void *extstore_init(struct extstore_conf_file *fh, struct extstore_conf *cf,
        enum extstore_res *res) {
    store_engine *e = calloc(1, sizeof(*e));
    if (!e) { *res = EXTSTORE_INIT_OOM; return NULL; }
    e->page_size = cf->page_size;
    e->slot_size = cf->slot_size;
    e->read_slots = cf->read_slots > 64 ? 64 : cf->read_slots;
    e->io_threadcount = cf->io_threadcount;
    e->io_depth = cf->io_depth ? cf->io_depth : 64;
    pthread_mutex_init(&e->mutex, NULL);
    pthread_mutex_init(&e->stats_mutex, NULL);
    pthread_mutex_init(&e->staging_mutex, NULL);
    atomic_store(&e->dead, 0);
    e->local = (fh->file && strcmp(fh->file, "local") == 0);

    if (e->local) {
        /* local test backend: remote memory + staging are plain buffers */
        uint64_t rsize = fh->total_size;
        e->local_mem = calloc(1, rsize);
        e->staging_count = cf->write_slots ? cf->write_slots : 256;
        e->staging_base = calloc(e->staging_count, e->slot_size);
        if (!e->local_mem || !e->staging_base) { *res = EXTSTORE_INIT_OOM; goto fail; }
        e->staging_free = malloc(sizeof(char *) * e->staging_count);
        for (unsigned int i = 0; i < e->staging_count; i++)
            e->staging_free[i] = e->staging_base + (size_t)i * e->slot_size;
        e->staging_top = e->staging_count;
        e->page_count = rsize / e->page_size;
        if (e->page_count == 0) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
        e->io_threads = calloc(e->io_threadcount, sizeof(store_iothr));
        for (unsigned int i = 0; i < e->io_threadcount; i++) {
            store_iothr *t = &e->io_threads[i];
            t->e = e;
            pthread_mutex_init(&t->mutex, NULL);
            pthread_cond_init(&t->cond, NULL);
        }
        goto pages_setup;
    }

    int ndev; struct ibv_device **devs = ibv_get_device_list(&ndev);
    if (!devs || ndev == 0) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
    e->ctx = ibv_open_device(devs[0]);
    ibv_free_device_list(devs);
    if (!e->ctx) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
    e->pd = ibv_alloc_pd(e->ctx);
    if (!e->pd) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }

    /* bounce pool (read dest) */
    size_t bsz = (size_t)e->io_threadcount * e->read_slots * e->slot_size;
    char *bbase; if (posix_memalign((void **)&bbase, 4096, bsz)) { *res = EXTSTORE_INIT_OOM; goto fail; }
    e->bounce_mr = ibv_reg_mr(e->pd, bbase, bsz, IBV_ACCESS_LOCAL_WRITE);
    if (!e->bounce_mr) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }

    /* staging pool (write source). ponytail: plain mmap here; on the SEV guest
     * this must be snp_shared (WC/shared) memory — see rdma-porting-refs. */
    e->staging_count = cf->write_slots ? cf->write_slots : 256;
    size_t ssz = (size_t)e->staging_count * e->slot_size;
    e->staging_base = mmap(NULL, ssz, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (e->staging_base == MAP_FAILED) { *res = EXTSTORE_INIT_OOM; goto fail; }
    e->staging_mr = ibv_reg_mr(e->pd, e->staging_base, ssz, IBV_ACCESS_LOCAL_WRITE);
    if (!e->staging_mr) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
    e->staging_free = malloc(sizeof(char *) * e->staging_count);
    for (unsigned int i = 0; i < e->staging_count; i++)
        e->staging_free[i] = e->staging_base + (size_t)i * e->slot_size;
    e->staging_top = e->staging_count;

    /* IO threads: QP + CQ + bounce window each */
    e->io_threads = calloc(e->io_threadcount, sizeof(store_iothr));
    for (unsigned int i = 0; i < e->io_threadcount; i++) {
        store_iothr *t = &e->io_threads[i];
        t->e = e;
        pthread_mutex_init(&t->mutex, NULL);
        pthread_cond_init(&t->cond, NULL);
        t->bounce_base = bbase + (size_t)i * e->read_slots * e->slot_size;
        t->bounce_free = (e->read_slots >= 64) ? ~0ULL : ((1ULL << e->read_slots) - 1);
        t->cq = ibv_create_cq(e->ctx, e->io_depth * 2, NULL, NULL, 0);
        if (!t->cq) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
        struct ibv_qp_init_attr ia = { .send_cq = t->cq, .recv_cq = t->cq,
            .qp_type = IBV_QPT_RC, .cap = { .max_send_wr = e->io_depth + 1,
                .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 } };
        t->qp = ibv_create_qp(e->pd, &ia);
        if (!t->qp || qp_to_init(t->qp)) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
    }

    /* connect genie, learn remote MR, RTS all QPs */
    uint64_t rsize = 0;
    if (genie_connect(e, fh->file, fh->cport, &rsize)) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }

    /* pages carve the remote MR */
    e->page_count = rsize / e->page_size;
    if (e->page_count == 0) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
pages_setup:
    e->page_bucketcount = cf->page_buckets ? cf->page_buckets : 1;
    e->pages = calloc(e->page_count, sizeof(store_page));
    for (unsigned int i = 0; i < e->page_count; i++) {
        store_page *p = &e->pages[i];
        pthread_mutex_init(&p->mutex, NULL);
        p->id = i; p->version = 1; p->remote_off = (uint64_t)i * e->page_size;
        p->free = true; p->next = e->free_pages; e->free_pages = p;
    }
    e->page_buckets = calloc(e->page_bucketcount, sizeof(store_page *));
    e->freeloc = calloc(e->page_bucketcount, sizeof(struct loc_stack));

    e->stats.page_count = e->page_count;
    e->stats.page_size = e->page_size;
    e->stats.pages_free = e->page_count;

    for (unsigned int i = 0; i < e->io_threadcount; i++) {
        if (pthread_create(&e->io_threads[i].tid, NULL, extstore_io_thread,
                &e->io_threads[i])) { *res = EXTSTORE_INIT_THREAD_FAIL; goto fail; }
    }
    return e;
fail:
    /* leak on init failure; process exits anyway (stock behaviour) */
    return NULL;
}

/* ---- allocation (SPEC §2.4 / P-1) ---- */

/* caller holds e->mutex */
static store_page *grab_active(store_engine *e, unsigned int bucket, unsigned int len) {
    store_page *p = e->page_buckets[bucket];
    if (p && p->allocated + len <= e->page_size) return p;
    if (!e->free_pages) return NULL;
    p = e->free_pages; e->free_pages = p->next;
    p->free = false; p->active = true; p->bucket = bucket; p->allocated = 0;
    e->page_buckets[bucket] = p;
    e->stats.pages_free--; e->stats.pages_used++; e->stats.page_allocs++;
    return p;
}

int extstore_alloc(void *ptr, unsigned int len, unsigned int bucket, struct ext_loc *out) {
    store_engine *e = ptr;
    if (bucket >= e->page_bucketcount) bucket = 0;
    if (len > e->slot_size) return -1;
    pthread_mutex_lock(&e->mutex);
    struct loc_stack *fs = &e->freeloc[bucket];
    if (fs->top > 0) { *out = fs->arr[--fs->top]; pthread_mutex_unlock(&e->mutex); return 0; }
    store_page *p = grab_active(e, bucket, len);
    if (!p) { pthread_mutex_unlock(&e->mutex); return -1; }
    out->page_id = p->id; out->page_version = p->version;
    out->offset = p->allocated; out->len = len;
    p->allocated += len; p->obj_count++; p->bytes_used += len;
    e->stats.bytes_used += len; e->stats.objects_used++;
    pthread_mutex_unlock(&e->mutex);
    return 0;
}

void extstore_free_loc(void *ptr, const struct ext_loc *loc) {
    store_engine *e = ptr;
    if (loc->page_id >= e->page_count) return;
    unsigned int bucket = e->pages[loc->page_id].bucket;
    pthread_mutex_lock(&e->mutex);
    struct loc_stack *fs = &e->freeloc[bucket];
    if (fs->top == fs->cap) {
        fs->cap = fs->cap ? fs->cap * 2 : 64;
        fs->arr = realloc(fs->arr, fs->cap * sizeof(struct ext_loc));
    }
    fs->arr[fs->top++] = *loc;
    if (e->stats.bytes_used >= loc->len) e->stats.bytes_used -= loc->len;
    if (e->stats.objects_used) e->stats.objects_used--;
    pthread_mutex_unlock(&e->mutex);
}

/* ---- submit ---- */

int extstore_submit(void *ptr, obj_io *io) {
    store_engine *e = ptr;
    if (atomic_load(&e->dead)) {          /* fail-fast (P-5) */
        obj_io *nx; for (obj_io *p = io; p; p = nx) { nx = p->next; p->cb(e, p, -1); }
        return 0;
    }
    unsigned int idx = __atomic_fetch_add(&e->last_io_thread, 1, __ATOMIC_RELAXED)
                       % e->io_threadcount;
    store_iothr *t = &e->io_threads[idx];
    obj_io *tail = io; while (tail->next) tail = tail->next;
    pthread_mutex_lock(&t->mutex);
    if (!t->queue) t->queue = io; else t->queue_tail->next = io;
    t->queue_tail = tail;
    pthread_cond_signal(&t->cond);
    pthread_mutex_unlock(&t->mutex);
    return 0;
}

/* ---- staging pool ---- */

char *extstore_staging_get(void *ptr) {
    store_engine *e = ptr;
    pthread_mutex_lock(&e->staging_mutex);
    char *s = e->staging_top ? e->staging_free[--e->staging_top] : NULL;
    pthread_mutex_unlock(&e->staging_mutex);
    return s;
}
void extstore_staging_put(void *ptr, char *slot) {
    store_engine *e = ptr;
    pthread_mutex_lock(&e->staging_mutex);
    e->staging_free[e->staging_top++] = slot;
    pthread_mutex_unlock(&e->staging_mutex);
}

/* ---- misc API kept for storage.c ---- */

int extstore_check(void *ptr, unsigned int page_id, uint64_t page_version) {
    store_engine *e = ptr;
    if (page_id >= e->page_count) return -1;
    store_page *p = &e->pages[page_id];
    pthread_mutex_lock(&p->mutex);
    int rv = (p->version == page_version) ? 0 : -1;
    pthread_mutex_unlock(&p->mutex);
    return rv;
}

int extstore_delete(void *ptr, unsigned int page_id, uint64_t page_version,
        unsigned int count, unsigned int bytes) {
    store_engine *e = ptr;
    if (page_id >= e->page_count) return -1;
    store_page *p = &e->pages[page_id];
    pthread_mutex_lock(&p->mutex);
    int rv = -1;
    if (p->version == page_version) {
        if (p->obj_count >= count) p->obj_count -= count; else p->obj_count = 0;
        if (p->bytes_used >= bytes) p->bytes_used -= bytes; else p->bytes_used = 0;
        rv = 0;
    }
    pthread_mutex_unlock(&p->mutex);
    return rv;
}

void extstore_get_stats(void *ptr, struct extstore_stats *st) {
    store_engine *e = ptr;
    STAT_L(e);
    struct extstore_page_data *pd = st->page_data;
    *st = e->stats;
    st->page_data = pd;
    STAT_UL(e);
}

void extstore_get_page_data(void *ptr, struct extstore_stats *st) {
    store_engine *e = ptr;
    if (!st->page_data) return;
    for (unsigned int i = 0; i < e->page_count; i++) {
        store_page *p = &e->pages[i];
        st->page_data[i].version = p->version;
        st->page_data[i].bytes_used = p->bytes_used;
        st->page_data[i].bucket = p->bucket;
        st->page_data[i].active = p->active;
    }
}
