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
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include "extstore.h"
#include <time.h>

/* EXT_RDMA_PROF (D6): runtime-gated in-server span profiling. EXT_WRITE_BATCH
 * caps the posting round so each SYNC advise is attributable to one op. */
#define PROF_BUCKETS 8192        /* x100ns => 0..819us range, overflow clamps */
#define PROF_BUCKET_NS 100
static int g_prof_on = 0;
static unsigned int g_batch_limit = 32;      /* posting-round cap (<= wrs[] size) */
static double g_ns_per_cycle = 0.0;          /* rdtsc cycle -> ns */

static inline uint64_t prof_rdtsc(void) { return __builtin_ia32_rdtsc(); }

/* Calibrate rdtsc against CLOCK_MONOTONIC over ~50ms (invariant TSC assumed). */
static void prof_calibrate(void) {
    struct timespec t0, t1;
    uint64_t c0 = prof_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct timespec s = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    nanosleep(&s, NULL);
    uint64_t c1 = prof_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    uint64_t cyc = c1 - c0;
    g_ns_per_cycle = cyc ? ns / (double)cyc : 0.0;
    fprintf(stderr, "extstore prof: TSC %.4f ns/cycle (%.0f MHz)\n",
            g_ns_per_cycle, g_ns_per_cycle > 0 ? 1000.0 / g_ns_per_cycle : 0.0);
}

static inline void prof_record(uint32_t *hist, uint64_t *count, uint64_t *sum,
                               uint64_t cycles) {
    uint64_t ns = (uint64_t)(cycles * g_ns_per_cycle);
    unsigned int b = ns / PROF_BUCKET_NS;
    if (b >= PROF_BUCKETS) b = PROF_BUCKETS - 1;
    hist[b]++; (*count)++; *sum += ns;
}

/* DMA-registerable buffer. On SEV-SNP the passthrough NIC can only DMA SHARED
 * (unencrypted) memory, so the read-bounce and write-staging pools must come
 * from /dev/snp_shared. On non-TEE hosts that device is absent, so fall back to
 * anonymous mmap (keeps the genie loopback path working). §9 / P-3(a). */
static char *dma_alloc(size_t sz) {
    int fd = open("/dev/snp_shared", O_RDWR);
    if (fd >= 0) {
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        /* keep fd open: the shared region is tied to it. one-time init leak. */
        if (p != MAP_FAILED) {
            fprintf(stderr, "extstore: dma_alloc %zuB from /dev/snp_shared\n", sz);
            return p;
        }
        fprintf(stderr, "extstore: snp_shared mmap(%zuB) failed: %s; using anon\n",
                sz, strerror(errno));
        close(fd);
    }
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* Custom advice added by the SEV SWIOTLB-sync kernel patch (rdma-porting-refs/).
 * Builds against stock headers; unpatched libibverbs returns an error we tolerate. */
#ifndef IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU
#define IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU 3
#endif
/* Write-direction counterpart: staging must be pushed to the device before the
 * NIC reads it, or the WRITE transmits pre-DMA contents (proved on 2026-07-23:
 * genie received a 496-byte run of 0x00 for a sealed object). The kernel patch
 * must use this same numeric value — if it picks a different one, change this
 * define, do not assume 4. A patched header wins over this fallback. */
#ifndef IBV_ADVISE_MR_ADVICE_SYNC_FOR_DEVICE
#define IBV_ADVISE_MR_ADVICE_SYNC_FOR_DEVICE 4
#endif

#define CM_TIMEOUT_MS 2000

/* Remote MR info handed to each client connection in the accept private_data. */
struct xrd_mr_info { uint64_t raddr; uint32_t rkey; uint64_t size; } __attribute__((packed));

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
    struct rdma_cm_id *cm_id;     /* owns the QP after rdma_connect */
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    char *bounce_base;            /* read_slots * slot_size */
    uint64_t bounce_free;         /* bit=1 -> slot free (read_slots <= 64) */
    unsigned int outstanding;
    store_engine *e;
    pthread_t tid;
    /* EXT_RDMA_PROF: per-thread span histogram (lock-free; aggregated on read).
     * PROF_BUCKETS x PROF_BUCKET_NS covers 0..~102us; bucket = ns/100 clamped. */
    uint64_t prof_r_count, prof_w_count, prof_r_sum_ns, prof_w_sum_ns;
    uint32_t prof_r_hist[8192], prof_w_hist[8192];
    /* admin's breakout: sync (advise) vs transfer (post..CQE) portion, ns sums. */
    uint64_t prof_r_sync_ns, prof_r_xfer_ns, prof_w_sync_ns, prof_w_xfer_ns;
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

const char *extstore_err(enum extstore_res res) {
    const char *rv = "unknown error";
    switch (res) {
        case EXTSTORE_INIT_OOM: rv = "failed to allocate engine"; break;
        case EXTSTORE_INIT_OPEN_FAIL: rv = "failed to open RDMA device / connect genie"; break;
        case EXTSTORE_INIT_THREAD_FAIL: rv = "failed to spawn IO thread"; break;
        case EXTSTORE_INIT_SELFTEST_FAIL:
            rv = "remote memory self-test failed (see extstore selftest lines above)"; break;
        default: break;
    }
    return rv;
}

/* ---- RDMA connection via rdma_cm (IP-based; CM resolves GID/route/MTU) ----
 * One RC connection per IO thread. Synchronous CM (one-shot at init). The genie
 * server hands back the remote MR (addr,rkey,size) in the accept private_data.
 */
static int cm_wait(struct rdma_event_channel *ch, enum rdma_cm_event_type want,
                   struct rdma_cm_event **out) {
    struct rdma_cm_event *ev;
    if (rdma_get_cm_event(ch, &ev)) return -1;
    if (ev->event != want) {
        fprintf(stderr, "extstore rdma_cm: expected %s but got %s (status %d)\n",
                rdma_event_str(want), rdma_event_str(ev->event), ev->status);
        rdma_ack_cm_event(ev); return -1;
    }
    if (out) *out = ev; else rdma_ack_cm_event(ev);
    return 0;
}

/* Connect one QP for thread `ti`. On the first connection, learns the remote MR.
 * Uses/sets e->pd (shared across all QPs, from the resolved device context). */
static int cm_connect_one(store_engine *e, struct sockaddr *dst, unsigned int ti,
                          bool first, uint64_t *size_out) {
    store_iothr *t = &e->io_threads[ti];
#define CM_FAIL(step) do { fprintf(stderr, "extstore rdma_cm: %s failed (thread %u): %s\n", \
        step, ti, strerror(errno)); return -1; } while (0)
    struct rdma_event_channel *ch = rdma_create_event_channel();
    if (!ch) CM_FAIL("create_event_channel");
    if (rdma_create_id(ch, &t->cm_id, NULL, RDMA_PS_TCP)) CM_FAIL("create_id");
    if (rdma_resolve_addr(t->cm_id, NULL, dst, CM_TIMEOUT_MS)) CM_FAIL("resolve_addr");
    if (cm_wait(ch, RDMA_CM_EVENT_ADDR_RESOLVED, NULL)) CM_FAIL("ADDR_RESOLVED event");
    if (rdma_resolve_route(t->cm_id, CM_TIMEOUT_MS)) CM_FAIL("resolve_route");
    if (cm_wait(ch, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL)) CM_FAIL("ROUTE_RESOLVED event");

    if (first) {
        e->pd = ibv_alloc_pd(t->cm_id->verbs);
        if (!e->pd) CM_FAIL("alloc_pd");
    }
    t->cq = ibv_create_cq(t->cm_id->verbs, e->io_depth * 2, NULL, NULL, 0);
    if (!t->cq) CM_FAIL("create_cq");
    struct ibv_qp_init_attr ia = { .send_cq = t->cq, .recv_cq = t->cq,
        .qp_type = IBV_QPT_RC, .cap = { .max_send_wr = e->io_depth + 1,
            .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 } };
    if (rdma_create_qp(t->cm_id, e->pd, &ia)) CM_FAIL("create_qp");
    t->qp = t->cm_id->qp;

    struct rdma_conn_param cp = { .responder_resources = 16, .initiator_depth = 16,
        .retry_count = 7, .rnr_retry_count = 7 };
    struct rdma_cm_event *ev;
    if (rdma_connect(t->cm_id, &cp)) CM_FAIL("connect");
    if (cm_wait(ch, RDMA_CM_EVENT_ESTABLISHED, &ev)) CM_FAIL("ESTABLISHED event");
    if (first) {
        if (ev->param.conn.private_data_len < sizeof(struct xrd_mr_info)) {
            rdma_ack_cm_event(ev); return -1;
        }
        struct xrd_mr_info mi;
        memcpy(&mi, ev->param.conn.private_data, sizeof(mi));
        e->raddr = mi.raddr; e->rkey = mi.rkey; *size_out = mi.size;
    }
    rdma_ack_cm_event(ev);
    /* keep the event channel; not polled after connect */
    return 0;
}

static int genie_connect(store_engine *e, const char *host, int port,
                         uint64_t *size_out) {
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) return -1;
    for (unsigned int i = 0; i < e->io_threadcount; i++)
        if (cm_connect_one(e, (struct sockaddr *)&sa, i, i == 0, size_out))
            return -1;
    return 0;
}

/* ---- IO thread: post batch -> poll -> batched sync -> cb (SPEC §6) ---- */

static void *extstore_io_thread(void *arg) {
    store_iothr *t = arg;
    store_engine *e = t->e;
    unsigned int depth = e->io_depth;
    struct ibv_wc wc[32];
    struct ibv_send_wr wrs[32], *bad;
    struct ibv_sge sg[32], sync_sg[32], dev_sg[32];

    while (1) {
        pthread_mutex_lock(&t->mutex);
        while (!t->queue && !t->outstanding)
            pthread_cond_wait(&t->cond, &t->mutex);

        if (e->local) {   /* memcpy transport (no RDMA); remote == local_mem */
            obj_io *batch = t->queue; t->queue = t->queue_tail = NULL;
            pthread_mutex_unlock(&t->mutex);
            unsigned int reads = 0, writes = 0; uint64_t rb = 0, wb = 0;
            while (batch) {
                obj_io *io = batch; batch = io->next;
                char *rem = e->local_mem + e->pages[io->page_id].remote_off + io->offset;
                if (io->mode == OBJ_IO_READ) { io->buf = rem; reads++; rb += io->len; }
                else { memcpy(rem, io->buf, io->len); writes++; wb += io->len; }
                io->cb(e, io, (int)io->len);
            }
            STAT_L(e);
            e->stats.objects_read += reads;   e->stats.bytes_read += rb;
            e->stats.objects_written += writes; e->stats.bytes_written += wb;
            STAT_UL(e);
            continue;
        }

        int n = 0;
        while (t->queue && t->outstanding + n < depth && n < (int)g_batch_limit) {
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
            n++;
        }
        pthread_mutex_unlock(&t->mutex);

        if (n) {
            /* push staging to the device before the NIC reads it (mirror of the
             * SYNC_FOR_CPU on the read side). Only the WRITE sges need it. */
            int nd = 0;
            for (int i = 0; i < n; i++)
                if (wrs[i].opcode == IBV_WR_RDMA_WRITE) dev_sg[nd++] = sg[i];
            /* WRITE span opens here — before the SYNC_FOR_DEVICE advise (D6). */
            if (g_prof_on) {
                uint64_t ts = prof_rdtsc();
                for (int i = 0; i < n; i++)
                    if (wrs[i].opcode == IBV_WR_RDMA_WRITE)
                        ((obj_io *)(uintptr_t)wrs[i].wr_id)->t_start = ts;
            }
            if (nd) {
                int adv = ibv_advise_mr(e->pd, IBV_ADVISE_MR_ADVICE_SYNC_FOR_DEVICE,
                                        IBV_ADVISE_MR_FLAG_FLUSH, dev_sg, nd);
                static _Atomic int dev_warned;
                if (adv && !atomic_exchange(&dev_warned, 1))
                    fprintf(stderr, "extstore: ibv_advise_mr(SYNC_FOR_DEVICE) failed: %s"
                            " — writes may transmit pre-DMA contents\n", strerror(adv));
            }
            /* per-write t_end = SYNC_FOR_DEVICE done: splits WRITE span into
             * sync (t_end - t_start) and transfer (CQE - t_end). */
            if (g_prof_on && nd) {
                uint64_t ts = prof_rdtsc();
                for (int i = 0; i < n; i++)
                    if (wrs[i].opcode == IBV_WR_RDMA_WRITE)
                        ((obj_io *)(uintptr_t)wrs[i].wr_id)->t_end = ts;
            }
            /* READ span opens here — immediately before ibv_post_send (D6). */
            if (g_prof_on) {
                uint64_t ts = prof_rdtsc();
                for (int i = 0; i < n; i++)
                    if (wrs[i].opcode == IBV_WR_RDMA_READ)
                        ((obj_io *)(uintptr_t)wrs[i].wr_id)->t_start = ts;
            }
            if (ibv_post_send(t->qp, &wrs[0], &bad)) {
                atomic_store(&e->dead, 1);
                STAT_L(e); e->stats.engine_dead = 1; STAT_UL(e);
                for (int i = 0; i < n; i++) {
                    obj_io *io = (obj_io *)(uintptr_t)wrs[i].wr_id;
                    STAT_L(e);
                    if (io->mode == OBJ_IO_READ) e->stats.read_failures++;
                    else e->stats.write_failures++;
                    STAT_UL(e);
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
        /* WRITE span closes at the CQE reap; READ span closes after SYNC_FOR_CPU. */
        uint64_t t_poll = g_prof_on ? prof_rdtsc() : 0;

        /* batched SWIOTLB->private sync for READs in this batch */
        int nsync = 0;
        for (int i = 0; i < c; i++) {
            obj_io *io = (obj_io *)(uintptr_t)wc[i].wr_id;
            if (io->mode == OBJ_IO_READ && wc[i].status == IBV_WC_SUCCESS)
                sync_sg[nsync++] = (struct ibv_sge){ .addr = (uintptr_t)io->buf,
                    .length = io->len, .lkey = e->bounce_mr->lkey };
        }
        if (nsync) {
            /* Return was ignored: if the driver does not implement ADVISE (some
             * do return EOPNOTSUPP), reads see whatever is in the bounce buffer
             * and the failure only shows up later as GCM tag mismatches. Warn
             * once so that is diagnosable instead of silent. */
            int adv = ibv_advise_mr(e->pd, IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU,
                                    IBV_ADVISE_MR_FLAG_FLUSH, sync_sg, nsync);
            static _Atomic int advise_warned;
            if (adv && !atomic_exchange(&advise_warned, 1))
                fprintf(stderr, "extstore: ibv_advise_mr(SYNC_FOR_CPU) failed: %s"
                        " — bounce reads are not being synced\n", strerror(adv));
        }
        uint64_t t_sync_done = g_prof_on ? prof_rdtsc() : 0;

        for (int i = 0; i < c; i++) {
            obj_io *io = (obj_io *)(uintptr_t)wc[i].wr_id;
            if (g_prof_on && wc[i].status == IBV_WC_SUCCESS && io->t_start) {
                if (io->mode == OBJ_IO_READ) {
                    prof_record(t->prof_r_hist, &t->prof_r_count, &t->prof_r_sum_ns,
                                t_sync_done - io->t_start);
                    t->prof_r_xfer_ns += (uint64_t)((t_poll - io->t_start) * g_ns_per_cycle);
                    t->prof_r_sync_ns += (uint64_t)((t_sync_done - t_poll) * g_ns_per_cycle);
                } else {
                    prof_record(t->prof_w_hist, &t->prof_w_count, &t->prof_w_sum_ns,
                                t_poll - io->t_start);
                    t->prof_w_sync_ns += (uint64_t)((io->t_end - io->t_start) * g_ns_per_cycle);
                    t->prof_w_xfer_ns += (uint64_t)((t_poll - io->t_end) * g_ns_per_cycle);
                }
            }
            int ok = (wc[i].status == IBV_WC_SUCCESS);
            /* The callback owns io and may free it (storage_write_done_cb frees
             * the enclosing flush_ctx), so snapshot everything we still need. */
            int is_read = (io->mode == OBJ_IO_READ);
            unsigned int len = io->len;
            char *buf = io->buf;
            if (!ok) {
                atomic_store(&e->dead, 1);
                STAT_L(e);
                e->stats.engine_dead = 1;
                if (is_read) e->stats.read_failures++;
                else e->stats.write_failures++;
                STAT_UL(e);
            }
            io->cb(e, io, ok ? (int)len : -1);
            if (is_read) {
                int s = (buf - t->bounce_base) / e->slot_size;
                pthread_mutex_lock(&t->mutex); t->bounce_free |= 1ULL << s; pthread_mutex_unlock(&t->mutex);
            } else if (ok) {
                STAT_L(e);
                e->stats.objects_written++; e->stats.bytes_written += len;
                STAT_UL(e);
            }
        }
        pthread_mutex_lock(&t->mutex); t->outstanding -= c; pthread_mutex_unlock(&t->mutex);
        STAT_L(e);
        e->stats.objects_read += nsync;
        for (int i = 0; i < nsync; i++) e->stats.bytes_read += sync_sg[i].length;
        STAT_UL(e);
    }
    return NULL;
}

/* ---- init ---- */

/* EXT_SELFTEST=1: write a known pattern to remote memory and RDMA READ it back
 * before serving traffic. Exists because a write can succeed at every layer that
 * reports status — clean CQE, objects_written++, no engine_dead — and still
 * deposit zeros in the remote MR when the local buffer is not synced to the
 * device (SEV SWIOTLB). Nothing else in this engine can see that; the data is
 * only discovered to be garbage on read-back, long after the benchmark ran.
 * Opt-in, so the client can still be brought up for connect/latency work while
 * the kernel-side sync is missing. */
static int selftest(store_engine *e) {
    store_iothr *t = &e->io_threads[0];
    unsigned int len = e->slot_size < 256 ? e->slot_size : 256;
    unsigned char *src = (unsigned char *)e->staging_base;
    unsigned char *dst = (unsigned char *)t->bounce_base;
    for (unsigned int i = 0; i < len; i++) src[i] = (unsigned char)(0x5A ^ (i * 31));
    memset(dst, 0, len);

    /* page 0 offset 0: no object lives there yet (pages are handed out top-down) */
    for (int pass = 0; pass < 2; pass++) {          /* 0 = WRITE out, 1 = READ back */
        struct ibv_sge sg = { .addr = (uintptr_t)(pass ? dst : src), .length = len,
            .lkey = pass ? e->bounce_mr->lkey : e->staging_mr->lkey };
        struct ibv_send_wr *bad, wr = { .wr_id = (uint64_t)pass, .sg_list = &sg,
            .num_sge = 1, .send_flags = IBV_SEND_SIGNALED,
            .opcode = pass ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE };
        wr.wr.rdma.remote_addr = e->raddr;
        wr.wr.rdma.rkey = e->rkey;
        if (!pass) {
            /* push staging to the device before the NIC reads it (same sync the
             * real WRITE path uses) — else the NIC transmits pre-DMA zeros */
            int adv = ibv_advise_mr(e->pd, IBV_ADVISE_MR_ADVICE_SYNC_FOR_DEVICE,
                                    IBV_ADVISE_MR_FLAG_FLUSH, &sg, 1);
            if (adv)
                fprintf(stderr, "extstore selftest: SYNC_FOR_DEVICE advise failed: %s\n",
                        strerror(adv));
        }
        if (ibv_post_send(t->qp, &wr, &bad)) {
            fprintf(stderr, "extstore selftest: post_send(%s) failed: %s\n",
                    pass ? "READ" : "WRITE", strerror(errno));
            return -1;
        }
        struct ibv_wc wc;
        int c = 0;
        for (long spin = 0; spin < 500000000L && c == 0; spin++)
            c = ibv_poll_cq(t->cq, 1, &wc);
        if (c <= 0) {
            fprintf(stderr, "extstore selftest: no completion for %s\n",
                    pass ? "READ" : "WRITE");
            return -1;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "extstore selftest: %s completed with status %s (%d)\n",
                    pass ? "READ" : "WRITE", ibv_wc_status_str(wc.status), wc.status);
            return -1;
        }
        if (pass) {
            /* sync the bounce SWIOTLB->private before the CPU reads it back */
            int adv = ibv_advise_mr(e->pd, IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU,
                                    IBV_ADVISE_MR_FLAG_FLUSH, &sg, 1);
            if (adv)
                fprintf(stderr, "extstore selftest: SYNC_FOR_CPU advise failed: %s\n",
                        strerror(adv));
        }
    }

    if (memcmp(src, dst, len) == 0) {
        fprintf(stderr, "extstore selftest: OK (%u bytes written and read back)\n", len);
        return 0;
    }
    unsigned int i = 0;
    while (i < len && src[i] == dst[i]) i++;
    fprintf(stderr, "extstore selftest: FAILED — remote memory does not hold what "
            "we wrote. First mismatch at byte %u: sent 0x%02x, read back 0x%02x.\n",
            i, src[i], dst[i]);
    fprintf(stderr, "extstore selftest: read-back is %s. Both transfers reported "
            "success, so the transport works and the payload does not — on SEV this "
            "is the SWIOTLB sync (SYNC_FOR_DEVICE on staging, SYNC_FOR_CPU on "
            "bounce) missing from mlx5_ib.\n",
            dst[i] == 0 ? "all zero (pre-DMA contents)" : "different data");
    return -1;
}

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

    if (getenv("EXT_RDMA_PROF")) { g_prof_on = 1; prof_calibrate(); }
    { const char *b = getenv("EXT_WRITE_BATCH");
      if (b) { unsigned v = (unsigned)strtoul(b, NULL, 10);
               if (v >= 1 && v <= 32) g_batch_limit = v; } }

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

    /* read-bounce + write-staging pools (SEV: shared memory; §9). Registration
     * happens after we have a pd from the connection. */
    size_t bsz = (size_t)e->io_threadcount * e->read_slots * e->slot_size;
    char *bbase = dma_alloc(bsz);
    if (!bbase) { *res = EXTSTORE_INIT_OOM; goto fail; }

    e->staging_count = cf->write_slots ? cf->write_slots : 256;
    size_t ssz = (size_t)e->staging_count * e->slot_size;
    e->staging_base = dma_alloc(ssz);
    if (!e->staging_base) { *res = EXTSTORE_INIT_OOM; goto fail; }

    /* per-thread state, then connect all QPs via rdma_cm (sets e->pd, e->raddr) */
    e->io_threads = calloc(e->io_threadcount, sizeof(store_iothr));
    for (unsigned int i = 0; i < e->io_threadcount; i++) {
        store_iothr *t = &e->io_threads[i];
        t->e = e;
        pthread_mutex_init(&t->mutex, NULL);
        pthread_cond_init(&t->cond, NULL);
        t->bounce_base = bbase + (size_t)i * e->read_slots * e->slot_size;
        t->bounce_free = (e->read_slots >= 64) ? ~0ULL : ((1ULL << e->read_slots) - 1);
    }
    uint64_t rsize = 0;
    if (genie_connect(e, fh->file, fh->cport, &rsize)) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
    fprintf(stderr, "extstore: genie_connect OK (raddr=0x%lx rkey=0x%x size=%lu)\n",
            (unsigned long)e->raddr, e->rkey, (unsigned long)rsize);

    /* register MRs on the connection's pd */
    e->bounce_mr = ibv_reg_mr(e->pd, bbase, bsz, IBV_ACCESS_LOCAL_WRITE);
    if (!e->bounce_mr) fprintf(stderr, "extstore: reg_mr(bounce %zuB) failed: %s\n", bsz, strerror(errno));
    e->staging_mr = ibv_reg_mr(e->pd, e->staging_base, ssz, IBV_ACCESS_LOCAL_WRITE);
    if (!e->staging_mr) fprintf(stderr, "extstore: reg_mr(staging %zuB) failed: %s\n", ssz, strerror(errno));
    if (!e->bounce_mr || !e->staging_mr) { *res = EXTSTORE_INIT_OPEN_FAIL; goto fail; }
    e->staging_free = malloc(sizeof(char *) * e->staging_count);
    for (unsigned int i = 0; i < e->staging_count; i++)
        e->staging_free[i] = e->staging_base + (size_t)i * e->slot_size;
    e->staging_top = e->staging_count;

    if (getenv("EXT_SELFTEST") && selftest(e) != 0) {
        *res = EXTSTORE_INIT_SELFTEST_FAIL; goto fail;
    }

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
    /* A freed loc carries the *previous* object's len. Reuse its physical slot
     * only if that slot is at least as large as this request (else a bigger
     * object would overrun the neighbour), and stamp the caller's real len so
     * the stub and the sealed object agree — otherwise a 500-byte slot reused
     * for a 499-byte object leaves the stub claiming 500 while the seal wrote
     * 499, and every GET RDMA-READs one byte too many and fails GCM forever.
     * ponytail: LIFO top-only check + conservative shrink (recorded len can
     * only decrease); for the fixed-size workload every len matches so this is
     * exact recycling. A size-class free-list would reclaim more under mixed
     * sizes — add if fragmentation shows up. */
    if (fs->top > 0 && fs->arr[fs->top-1].len >= len) {
        *out = fs->arr[--fs->top];
        out->len = len;
        pthread_mutex_unlock(&e->mutex);
        return 0;
    }
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

/* Sum per-thread histograms and pull avg / p50 / p99 (ns) out of the merged one. */
static void prof_summarize(store_engine *e, int read,
        uint64_t *count, uint64_t *avg, uint64_t *p50, uint64_t *p99) {
    uint64_t merged[PROF_BUCKETS]; memset(merged, 0, sizeof(merged));
    uint64_t total = 0, sum = 0;
    for (unsigned int i = 0; i < e->io_threadcount; i++) {
        store_iothr *t = &e->io_threads[i];
        uint32_t *h = read ? t->prof_r_hist : t->prof_w_hist;
        total += read ? t->prof_r_count : t->prof_w_count;
        sum   += read ? t->prof_r_sum_ns : t->prof_w_sum_ns;
        for (int b = 0; b < PROF_BUCKETS; b++) merged[b] += h[b];
    }
    *count = total; *avg = total ? sum / total : 0;
    *p50 = *p99 = 0;
    if (!total) return;
    uint64_t c = 0, need50 = (total + 1) / 2, need99 = (total * 99 + 99) / 100;
    int f50 = 0, f99 = 0;
    for (int b = 0; b < PROF_BUCKETS && !(f50 && f99); b++) {
        c += merged[b];
        if (!f50 && c >= need50) { *p50 = (uint64_t)b * PROF_BUCKET_NS; f50 = 1; }
        if (!f99 && c >= need99) { *p99 = (uint64_t)b * PROF_BUCKET_NS; f99 = 1; }
    }
}

void extstore_get_stats(void *ptr, struct extstore_stats *st) {
    store_engine *e = ptr;
    STAT_L(e);
    struct extstore_page_data *pd = st->page_data;
    *st = e->stats;
    st->page_data = pd;
    STAT_UL(e);
    if (g_prof_on) {
        prof_summarize(e, 1, &st->prof_read_count, &st->prof_read_avg_ns,
                       &st->prof_read_p50_ns, &st->prof_read_p99_ns);
        prof_summarize(e, 0, &st->prof_write_count, &st->prof_write_avg_ns,
                       &st->prof_write_p50_ns, &st->prof_write_p99_ns);
        uint64_t rs = 0, rx = 0, ws = 0, wx = 0;
        for (unsigned int i = 0; i < e->io_threadcount; i++) {
            store_iothr *t = &e->io_threads[i];
            rs += t->prof_r_sync_ns; rx += t->prof_r_xfer_ns;
            ws += t->prof_w_sync_ns; wx += t->prof_w_xfer_ns;
        }
        st->prof_read_sync_avg_ns  = st->prof_read_count  ? rs / st->prof_read_count  : 0;
        st->prof_read_xfer_avg_ns  = st->prof_read_count  ? rx / st->prof_read_count  : 0;
        st->prof_write_sync_avg_ns = st->prof_write_count ? ws / st->prof_write_count : 0;
        st->prof_write_xfer_avg_ns = st->prof_write_count ? wx / st->prof_write_count : 0;
    }
}

void extstore_prof_reset(void *ptr) {
    store_engine *e = ptr;
    for (unsigned int i = 0; i < e->io_threadcount; i++) {
        store_iothr *t = &e->io_threads[i];
        t->prof_r_count = t->prof_w_count = t->prof_r_sum_ns = t->prof_w_sum_ns = 0;
        t->prof_r_sync_ns = t->prof_r_xfer_ns = t->prof_w_sync_ns = t->prof_w_xfer_ns = 0;
        memset(t->prof_r_hist, 0, sizeof(t->prof_r_hist));
        memset(t->prof_w_hist, 0, sizeof(t->prof_w_hist));
    }
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
