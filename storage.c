/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "memcached.h"
#ifdef EXTSTORE

#include "storage.h"
#include "extstore.h"
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
/* ponytail: #include the .c so the crypto TU rides along without a Makefile
 * object-list edit (autotools 1.17 regen is unavailable here). */
#include "ext_crypto.c"

// Single remote size-class for the fixed-size RDMA workload (P-1a).
#define PAGE_BUCKET_DEFAULT 0
#define PAGE_BUCKET_COUNT   1

static bool g_crypto_on = false;
static unsigned int g_read_retries = 3;   // integrity-read retry cap (EXT_READ_RETRIES)
static _Atomic uint64_t g_read_retry_ct = 0;
static _Atomic uint64_t g_badcrc_log_ct = 0;      // rate-limit for the badcrc diagnostic
static _Atomic uint64_t g_flush_log_ct = 0;       // rate-limit for the flush diagnostic

/* Problem B probes (genie). The pre-read reject path had no counter at all: a GET
 * of a stub can be answered as a miss before any RDMA read happens, and nothing
 * recorded it — which is why "permanently unreadable" looked like the same fault
 * as a torn read. These name which path it is. */
static _Atomic uint64_t g_abort_chunked = 0;   /* P-6: item too large */
static _Atomic uint64_t g_abort_alloc = 0;     /* no read destination available */
static _Atomic uint64_t g_plaintext_slab_fallback = 0;
static unsigned int g_plaintext_slot_size;
static _Thread_local cache_t *g_plaintext_cache;

/* 4096 offered requests / 8 workers = 512 nominal; leave burst headroom. */
#define PLAINTEXT_POOL_LIMIT 1024

/* EXT_TRACE_SEAL=1: table indexed by nonce counter (dense, monotonic) recording
 * who sealed each object and at what length, so a badcrc can compare the length
 * it read against the length actually written. No write-path I/O: logging every
 * seal would change the timing of the race being observed. */
#define SEAL_TRACE_MAX (1u << 21)
struct seal_rec { uint32_t page, off, len; char key[24]; };
static struct seal_rec *g_seal_tab;

static uint64_t nonce_counter(const void *obj) {   /* [salt 4][counter 8] */
    uint64_t c; memcpy(&c, (const char *)obj + 4, 8); return c;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int ret;
} store_wait;

static void storage_store_done_cb(void *e, obj_io *io, int ret) {
    (void)e;
    store_wait *w = io->data;
    pthread_mutex_lock(&w->mutex);
    w->ret = ret;
    w->done = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
}

/*
 * API functions
 */
static void storage_finalize_cb(io_pending_t *pending);
static void storage_return_cb(io_pending_t *pending);

// re-cast an io_pending_t into this more descriptive structure.
// the first few items _must_ match the original struct.
typedef struct _io_pending_storage_t {
    uint8_t io_queue_type;
    uint8_t io_sub_type;
    uint8_t payload; // payload offset
    LIBEVENT_THREAD *thread;
    conn *c;
    mc_resp *resp;
    io_queue_cb return_cb;    // called on worker thread.
    io_queue_cb finalize_cb;  // called back on the worker thread.
    STAILQ_ENTRY(io_pending_t) iop_next; // queue chain.
                              /* original struct ends here */
    item *hdr_it;             /* original header item. */
    item *read_it;            /* decrypt destination (RDMA: != io.buf bounce slot) */
    unsigned int read_clsid;  /* slab class of read_it, for freeing */
    cache_t *read_cache;      /* worker-private cache, NULL for slab fallback */
    obj_io io_ctx;            /* embedded extstore IO header */
    unsigned int iovec_data;  /* specific index of data iovec */
    bool noreply;             /* whether the response had noreply set */
    bool miss;                /* signal a miss to unlink hdr_it */
    bool badcrc;              /* signal a crc failure */
    bool active;              /* tells if IO was dispatched or not */
} io_pending_storage_t;

// Only call this if item has ITEM_HDR
bool storage_validate_item(void *e, item *it) {
    item_hdr *hdr = (item_hdr *)ITEM_data(it);
    if (extstore_check(e, hdr->page_id, hdr->page_version) != 0) {
        return false;
    } else {
        return true;
    }
}

void storage_delete(void *e, item *it) {
    if (it->it_flags & ITEM_HDR) {
        item_hdr *hdr = (item_hdr *)ITEM_data(it);
        extstore_delete(e, hdr->page_id, hdr->page_version, 1, hdr->len);
        struct ext_loc loc = { hdr->page_version, hdr->offset, hdr->len, hdr->page_id };
        extstore_free_loc(e, &loc);   /* return the remote slot (P-1) */
    }
}

// Function for the extra stats called from a protocol.
// NOTE: This either needs a name change or a wrapper, perhaps?
// it's defined here to reduce exposure of extstore.h to the rest of memcached
// but feels a little off being defined here.
// At very least maybe "process_storage_stats" in line with making this more
// of a generic wrapper module.
void storage_prof_reset(void) {
    if (ext_storage) extstore_prof_reset(ext_storage);
}

void process_extstore_stats(ADD_STAT add_stats, void *c) {
    int i;
    char key_str[STAT_KEY_LEN];
    char val_str[STAT_VAL_LEN];
    int klen = 0, vlen = 0;
    struct extstore_stats st;

    assert(add_stats);

    void *storage = ext_storage;
    if (storage == NULL) {
        return;
    }
    extstore_get_stats(storage, &st);
    st.page_data = calloc(st.page_count, sizeof(struct extstore_page_data));
    extstore_get_page_data(storage, &st);

    for (i = 0; i < st.page_count; i++) {
        APPEND_NUM_STAT(i, "version", "%llu",
                (unsigned long long) st.page_data[i].version);
        APPEND_NUM_STAT(i, "bytes", "%llu",
                (unsigned long long) st.page_data[i].bytes_used);
        APPEND_NUM_STAT(i, "bucket", "%u",
                st.page_data[i].bucket);
    }

    free(st.page_data);
}

// Additional storage stats for the main stats output.
void storage_stats(ADD_STAT add_stats, void *c) {
    struct extstore_stats st;
    if (ext_storage) {
        extstore_get_stats(ext_storage, &st);
        APPEND_STAT("extstore_page_allocs", "%llu", (unsigned long long)st.page_allocs);
        APPEND_STAT("extstore_pages_free", "%llu", (unsigned long long)st.pages_free);
        APPEND_STAT("extstore_pages_used", "%llu", (unsigned long long)st.pages_used);
        APPEND_STAT("extstore_objects_read", "%llu", (unsigned long long)st.objects_read);
        APPEND_STAT("extstore_objects_written", "%llu", (unsigned long long)st.objects_written);
        APPEND_STAT("extstore_objects_used", "%llu", (unsigned long long)st.objects_used);
        APPEND_STAT("extstore_bytes_written", "%llu", (unsigned long long)st.bytes_written);
        APPEND_STAT("extstore_bytes_read", "%llu", (unsigned long long)st.bytes_read);
        APPEND_STAT("extstore_bytes_used", "%llu", (unsigned long long)st.bytes_used);
        APPEND_STAT("extstore_limit_maxbytes", "%llu", (unsigned long long)(st.page_count * st.page_size));
        APPEND_STAT("extstore_io_queue", "%llu", (unsigned long long)(st.io_queue));
        // RDMA bring-up debug counters (SPEC §8). Nonzero = look here first.
        APPEND_STAT("extstore_engine_dead", "%llu", (unsigned long long)st.engine_dead);
        APPEND_STAT("extstore_write_failures", "%llu", (unsigned long long)st.write_failures);
        APPEND_STAT("extstore_read_failures", "%llu", (unsigned long long)st.read_failures);
        APPEND_STAT("extstore_read_retries", "%llu",
                (unsigned long long)atomic_load(&g_read_retry_ct));
        APPEND_STAT("extstore_get_aborted_chunked", "%llu",
                (unsigned long long)atomic_load(&g_abort_chunked));
        APPEND_STAT("extstore_get_aborted_alloc", "%llu",
                (unsigned long long)atomic_load(&g_abort_alloc));
        APPEND_STAT("extstore_plaintext_slab_fallback", "%llu",
                (unsigned long long)atomic_load(&g_plaintext_slab_fallback));
        APPEND_STAT("extstore_prof_span_ver", "%u", 2);
        // Span v2 distribution (ns); populated only under EXT_RDMA_PROF=1.
        APPEND_STAT("extstore_prof_read_count", "%llu", (unsigned long long)st.prof_read_count);
        APPEND_STAT("extstore_prof_read_avg_ns", "%llu", (unsigned long long)st.prof_read_avg_ns);
        APPEND_STAT("extstore_prof_read_p50_ns", "%llu", (unsigned long long)st.prof_read_p50_ns);
        APPEND_STAT("extstore_prof_read_p99_ns", "%llu", (unsigned long long)st.prof_read_p99_ns);
        APPEND_STAT("extstore_prof_write_count", "%llu", (unsigned long long)st.prof_write_count);
        APPEND_STAT("extstore_prof_write_avg_ns", "%llu", (unsigned long long)st.prof_write_avg_ns);
        APPEND_STAT("extstore_prof_write_p50_ns", "%llu", (unsigned long long)st.prof_write_p50_ns);
        APPEND_STAT("extstore_prof_write_p99_ns", "%llu", (unsigned long long)st.prof_write_p99_ns);
        APPEND_STAT("extstore_prof_read_crypto_avg_ns", "%llu",
                (unsigned long long)st.prof_read_crypto_avg_ns);
        APPEND_STAT("extstore_prof_write_crypto_avg_ns", "%llu",
                (unsigned long long)st.prof_write_crypto_avg_ns);
        // breakout: crypto, sync (SWIOTLB advise), transfer, avg ns.
        APPEND_STAT("extstore_prof_read_sync_avg_ns", "%llu", (unsigned long long)st.prof_read_sync_avg_ns);
        APPEND_STAT("extstore_prof_read_xfer_avg_ns", "%llu", (unsigned long long)st.prof_read_xfer_avg_ns);
        APPEND_STAT("extstore_prof_write_sync_avg_ns", "%llu", (unsigned long long)st.prof_write_sync_avg_ns);
        APPEND_STAT("extstore_prof_write_xfer_avg_ns", "%llu", (unsigned long long)st.prof_write_xfer_avg_ns);
    }

}

// This callback runs in the IO thread.
// TODO: Some or all of this should move to the
// io_pending's callback back in the worker thread.
// It might make sense to keep the crc32c check here though.
static void _storage_get_item_cb(void *e, obj_io *io, int ret) {
    io_pending_storage_t *p = (io_pending_storage_t *)io->data;
    mc_resp *resp = p->resp;
    assert(p->active == true);
    item *read_it = p->read_it;   // decrypt destination (io->buf is the bounce slot)
    bool miss = false;

    if (ret < 1) {
        miss = true;
    } else if (g_crypto_on) {
        // bounce slot holds [nonce|ciphertext|tag]; open into read_it image.
        uint32_t hv = hash(ITEM_key(p->hdr_it), p->hdr_it->nkey);
        struct ext_aad aad = { .hv = hv, .page_id = io->page_id, .pad = 0,
            .offset = io->offset, .page_version = io->page_version };
        uint64_t crypto_start = extstore_prof_stamp();
        int opened = ext_crypto_open(read_it, (uint8_t *)io->buf, io->len, &aad);
        extstore_prof_read_done(e, io, crypto_start, extstore_prof_stamp());
        if (opened < 0) {
            // Retry a transient DMA visibility failure; never serve unverified data.
            if (io->retries++ < g_read_retries) {
                atomic_fetch_add(&g_read_retry_ct, 1);
                io->next = NULL;
                extstore_submit(e, io);   // engine frees this bounce slot, re-posts
                return;
            }
            miss = true;
            p->badcrc = true;
                // Diagnostic (genie's request): a permanently-unreadable key means
                // the stub's AAD no longer matches the object in its slot. Log the
                // expected loc, whether the page version still matches, and the
                // slot's stored nonce (first 12B = boot_salt||counter, identifies
                // which object actually lives there). First N only.
                if (atomic_fetch_add(&g_badcrc_log_ct, 1) < 32) {
                    const unsigned char *n = (const unsigned char *)io->buf;
                    int pv_ok = (extstore_check(e, io->page_id, io->page_version) == 0);
                    fprintf(stderr, "extstore badcrc: key=%.*s stub{page=%u off=%u ver=%u len=%u} "
                        "page_ver_match=%d slot_nonce=%02x%02x%02x%02x.%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        p->hdr_it->nkey, ITEM_key(p->hdr_it),
                        io->page_id, io->offset, io->page_version, io->len, pv_ok,
                        n[0],n[1],n[2],n[3],n[4],n[5],n[6],n[7],n[8],n[9],n[10],n[11]);
                    if (g_seal_tab) {
                        uint64_t ctr = nonce_counter(io->buf);
                        struct seal_rec *r = ctr < SEAL_TRACE_MAX ? &g_seal_tab[ctr] : NULL;
                        if (r && r->key[0])
                            fprintf(stderr, "extstore badcrc: ^ slot holds key=%s sealed at "
                                "page=%u off=%u len=%u; we read len=%u -> %s\n",
                                r->key, r->page, r->off, r->len, io->len,
                                r->len == io->len ? "LENGTH MATCHES" : "LENGTH MISMATCH");
                        else
                            fprintf(stderr, "extstore badcrc: ^ no seal record (counter %llu)\n",
                                (unsigned long long)ctr);
                    }
                }
                // GCM writes the (unverified) plaintext into read_it BEFORE the
                // tag check, so a failed open left read_it's header garbage;
                // reset it_flags so slabs_free doesn't take the chunked path on a
                // bogus header (ASAN SEGV, slabs.c:468).
                read_it->it_flags = 0;
                read_it->slabs_clsid = p->read_clsid;
        }
    } else {
        uint64_t crypto_start = extstore_prof_stamp();
        memcpy(read_it, io->buf, io->len);   // crypto off: io->len == ntotal
        extstore_prof_read_done(e, io, crypto_start, extstore_prof_stamp());
    }

    if (miss) {
        if (p->noreply) {
            // In all GET cases, noreply means we send nothing back.
            resp->skip = true;
        } else {
            // TODO: This should be movable to the worker thread.
            // Convert the binprot response into a miss response.
            // The header requires knowing a bunch of stateful crap, so rather
            // than simply writing out a "new" miss response we mangle what's
            // already there.
            if (resp->binary_prot) {
                protocol_binary_response_header *header =
                    (protocol_binary_response_header *)resp->wbuf;

                // cut the extra nbytes off of the body_len
                uint32_t body_len = ntohl(header->response.bodylen);
                uint8_t hdr_len = header->response.extlen;
                body_len -= resp->iov[p->iovec_data].iov_len + hdr_len;
                resp->tosend -= resp->iov[p->iovec_data].iov_len + hdr_len;
                header->response.extlen = 0;
                header->response.status = (uint16_t)htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
                header->response.bodylen = htonl(body_len);

                // truncate the data response.
                resp->iov[p->iovec_data].iov_len = 0;
                // wipe the extlen iov... wish it was just a flat buffer.
                resp->iov[p->iovec_data-1].iov_len = 0;
                resp->chunked_data_iov = 0;
            } else {
                int i;
                // Meta commands have EN status lines for miss, rather than
                // END as a trailer as per normal ascii.
                if (resp->iov[0].iov_len >= 3
                        && memcmp(resp->iov[0].iov_base, "VA ", 3) == 0) {
                    // TODO: These miss translators should use specific callback
                    // functions attached to the io wrap. This is weird :(
                    resp->iovcnt = 1;
                    resp->iov[0].iov_len = 4;
                    resp->iov[0].iov_base = "EN\r\n";
                    resp->tosend = 4;
                } else {
                    // Wipe the iovecs up through our data injection.
                    // Allows trailers to be returned (END)
                    for (i = 0; i <= p->iovec_data; i++) {
                        resp->tosend -= resp->iov[i].iov_len;
                        resp->iov[i].iov_len = 0;
                        resp->iov[i].iov_base = NULL;
                    }
                }
                resp->chunked_total = 0;
                resp->chunked_data_iov = 0;
            }
        }
        p->miss = true;
    } else {
        assert(read_it->slabs_clsid != 0);
        // TODO: should always use it instead of ITEM_data to kill more
        // chunked special casing.
        if ((read_it->it_flags & ITEM_CHUNKED) == 0) {
            resp->iov[p->iovec_data].iov_base = ITEM_data(read_it);
        }
        p->miss = false;
    }

    p->active = false;
    //assert(c->io_wrapleft >= 0);

    return_io_pending((io_pending_t *)p);
}

int storage_get_item(LIBEVENT_THREAD *t, item *it, mc_resp *resp) {
#ifdef NEED_ALIGN
    item_hdr hdr;
    memcpy(&hdr, ITEM_data(it), sizeof(hdr));
#else
    item_hdr *hdr = (item_hdr *)ITEM_data(it);
#endif
    io_queue_t *q = thread_io_queue_get(t, IO_QUEUE_EXTSTORE);
    size_t ntotal = ITEM_ntotal(it);
    unsigned int clsid = slabs_clsid(ntotal);
    item *new_it;
    cache_t *read_cache = NULL;
    if (ntotal > settings.slab_chunk_size_max) {
        // P-6: chunked items unsupported on the RDMA backend.
        atomic_fetch_add(&g_abort_chunked, 1);
        return -1;
    }
    if (ntotal <= g_plaintext_slot_size) {
        if (g_plaintext_cache == NULL) {
            g_plaintext_cache = cache_create("extstore-plaintext",
                    g_plaintext_slot_size, sizeof(char *));
            if (g_plaintext_cache != NULL)
                cache_set_limit(g_plaintext_cache, PLAINTEXT_POOL_LIMIT);
        }
        if (g_plaintext_cache != NULL) {
            new_it = do_cache_alloc(g_plaintext_cache);
            if (new_it != NULL)
                read_cache = g_plaintext_cache;
        } else {
            new_it = NULL;
        }
    } else {
        new_it = NULL;
    }
    if (new_it == NULL) {
        atomic_fetch_add(&g_plaintext_slab_fallback, 1);
        new_it = do_item_alloc_pull(ntotal, clsid);
    }
    if (new_it == NULL) {
        atomic_fetch_add(&g_abort_alloc, 1);
        return -1;
    }
    // so we can free the chunk on a miss
    new_it->slabs_clsid = clsid;

    io_pending_storage_t *p = do_cache_alloc(t->io_cache);
    // this is a re-cast structure, so assert that we never outsize it.
    assert(sizeof(io_pending_t) >= sizeof(io_pending_storage_t));
    memset(p, 0, sizeof(io_pending_storage_t));
    p->active = true;
    p->miss = false;
    p->badcrc = false;
    p->noreply = resp->noreply;
    p->thread = t;
    p->return_cb = storage_return_cb;
    p->finalize_cb = storage_finalize_cb;
    // io_pending owns the reference for this object now.
    p->hdr_it = it;
    p->read_it = new_it;
    p->read_clsid = clsid;
    p->read_cache = read_cache;
    p->resp = resp;
    p->io_queue_type = IO_QUEUE_EXTSTORE;
    p->payload = offsetof(io_pending_storage_t, io_ctx);
    obj_io *eio = &p->io_ctx;

    // Reserve a response iov (data base filled in by the callback after decrypt).
    p->iovec_data = resp->iovcnt;
    int iovtotal = (resp->binary_prot) ? it->nbytes - 2 : it->nbytes;
    resp_add_iov(resp, "", iovtotal);

    // We can't bail out anymore, so mc_resp owns the IO from here.
    resp->io_pending = (io_pending_t *)p;

    eio->buf = NULL;   // engine assigns a bounce slot at post time

    STAILQ_INSERT_TAIL(&q->stack, (io_pending_t *)p, iop_next);

    // reference ourselves for the callback.
    eio->data = (void *)p;

    // Fill in the remote location from our header.
#ifdef NEED_ALIGN
    eio->page_version = hdr.page_version;
    eio->page_id = hdr.page_id;
    eio->offset = hdr.offset;
    eio->len = hdr.len;
#else
    eio->page_version = hdr->page_version;
    eio->page_id = hdr->page_id;
    eio->offset = hdr->offset;
    eio->len = hdr->len;   // remote object length (ntotal + crypto overhead)
#endif
    eio->retries = 0;
    eio->mode = OBJ_IO_READ;
    eio->cb = _storage_get_item_cb;

    // FIXME: This stat needs to move to reflect # of flash hits vs misses
    // for now it's a good gauge on how often we request out to flash at
    // least.
    pthread_mutex_lock(&t->stats.mutex);
    t->stats.get_extstore++;
    pthread_mutex_unlock(&t->stats.mutex);

    return 0;
}

void storage_submit_cb(io_queue_t *q) {
    // TODO: until we decide to port extstore's internal code to use
    // io_pending objs we "port" the IOP's into an obj_io chain just before
    // submission here.
    void *eio_head = NULL;
    while(!STAILQ_EMPTY(&q->stack)) {
        io_pending_t *p = STAILQ_FIRST(&q->stack);
        STAILQ_REMOVE_HEAD(&q->stack, iop_next);
        // FIXME: re-evaluate this.
        obj_io *io_ctx = (obj_io *) ((char *)p + p->payload);
        io_ctx->next = eio_head;
        eio_head = io_ctx;
    }
    extstore_submit(q->ctx, eio_head);
}

// Runs locally in worker thread.
static void storage_release_pending(io_pending_t *pending) {
    // re-cast to our specific struct.
    io_pending_storage_t *p = (io_pending_storage_t *)pending;

    conn *c = p->c;
    item *it = p->read_it;   // the decrypt destination we allocated
    assert(c != NULL);
    if (p->read_cache != NULL)
        do_cache_free(p->read_cache, it);
    else
        slabs_free(it, p->read_clsid);
    if (p->active) {
        // If request never dispatched, free the read buffer, leave header alone.
        p->resp->suspended = false;
        c->resps_suspended--;
        io_queue_t *q = thread_io_queue_get(p->thread, p->io_queue_type);
        STAILQ_REMOVE(&q->stack, pending, _io_pending_t, iop_next);
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.get_aborted_extstore++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    } else if (p->miss) {
        // Keep the stub on an integrity failure so a transient DMA visibility
        // failure can recover on a later GET. No local value is available.
        if (!p->badcrc)
            item_unlink(p->hdr_it);
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.miss_from_extstore++;
        if (p->badcrc)
            c->thread->stats.badcrc_from_extstore++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    }

    p->io_ctx.buf = NULL;
    p->io_ctx.next = NULL;
    p->active = false;

    item_remove(p->hdr_it);
}

// Called after an IO has been returned to the worker thread.
static void storage_return_cb(io_pending_t *pending) {
    conn_resp_unsuspend(pending->c, pending->resp);
}

// Called after responses have been transmitted. Need to free up related data.
static void storage_finalize_cb(io_pending_t *pending) {
    storage_release_pending(pending);
    // don't need to free the main context, since it's embedded.
}

/*
 * Remote-only SET commit. The input item is a transient request buffer and is
 * never linked into the hash table. A successful RDMA WRITE returns an
 * unlinked ITEM_HDR; the caller publishes that stub and only then replies
 * STORED. Resource exhaustion blocks at the staging pool instead of falling
 * back to a locally serviceable value.
 */
int storage_store_item(void *e, item *it, item **hdr_out, uint32_t hv) {
    size_t ntotal = ITEM_ntotal(it);
    unsigned int rlen = ntotal + (g_crypto_on ? EXT_CRYPTO_OVERHEAD : 0);
    *hdr_out = NULL;
    if (it->it_flags & (ITEM_CHUNKED|ITEM_HDR))
        return -1;

    client_flags_t flags;
    FLAGS_CONV(it, flags);
    item *hdr_it = do_item_alloc(ITEM_key(it), it->nkey, flags, it->exptime,
                                 sizeof(item_hdr));
    if (hdr_it == NULL)
        return -1;

    struct ext_loc loc;
    if (extstore_alloc(e, rlen, PAGE_BUCKET_DEFAULT, &loc) != 0) {
        do_item_remove(hdr_it);
        return -1;
    }

    char *slot = extstore_staging_get(e);
    if (slot == NULL) {
        extstore_free_loc(e, &loc);
        do_item_remove(hdr_it);
        return -1;
    }

    uint64_t prof_start = extstore_prof_stamp();
    int sealed;
    if (g_crypto_on) {
        struct ext_aad aad = { .hv = hv, .page_id = loc.page_id, .pad = 0,
            .offset = loc.offset, .page_version = loc.page_version };
        sealed = ext_crypto_seal((uint8_t *)slot, it, ntotal, &aad);
        if (g_seal_tab) {
            uint64_t ctr = nonce_counter(slot);
            if (ctr < SEAL_TRACE_MAX) {
                struct seal_rec *r = &g_seal_tab[ctr];
                r->page = loc.page_id; r->off = loc.offset; r->len = rlen;
                int kn = it->nkey < (int)sizeof(r->key) - 1 ? it->nkey : (int)sizeof(r->key) - 1;
                memcpy(r->key, ITEM_key(it), kn); r->key[kn] = 0;
            }
        }
    } else {
        memcpy(slot, it, ntotal);
        sealed = (int)ntotal;
    }
    uint64_t prof_crypto_done = extstore_prof_stamp();
    if (sealed != (int)rlen) {
        extstore_staging_put(e, slot);
        extstore_free_loc(e, &loc);
        do_item_remove(hdr_it);
        return -1;
    }

    if (g_crypto_on && atomic_fetch_add(&g_flush_log_ct, 1) < 200) {
        const unsigned char *n = (const unsigned char *)slot;
        fprintf(stderr, "extstore flush: key=%.*s -> {page=%u off=%u ver=%u} "
            "nonce=%02x%02x%02x%02x.%02x%02x%02x%02x%02x%02x%02x%02x\n",
            it->nkey, ITEM_key(it), loc.page_id, loc.offset, loc.page_version,
            n[0],n[1],n[2],n[3],n[4],n[5],n[6],n[7],n[8],n[9],n[10],n[11]);
    }

    store_wait wait = {0};
    pthread_mutex_init(&wait.mutex, NULL);
    pthread_cond_init(&wait.cond, NULL);
    obj_io io = {
        .data = &wait, .next = NULL, .buf = slot,
        .page_version = loc.page_version, .len = rlen, .offset = loc.offset,
        .page_id = loc.page_id, .mode = OBJ_IO_WRITE,
        .cb = storage_store_done_cb,
        .t_start = prof_start, .t_end = prof_crypto_done,
    };
    extstore_submit(e, &io);

    pthread_mutex_lock(&wait.mutex);
    while (!wait.done)
        pthread_cond_wait(&wait.cond, &wait.mutex);
    pthread_mutex_unlock(&wait.mutex);
    pthread_cond_destroy(&wait.cond);
    pthread_mutex_destroy(&wait.mutex);
    extstore_staging_put(e, slot);

    if (wait.ret != (int)rlen) {
        extstore_free_loc(e, &loc);
        do_item_remove(hdr_it);
        return -1;
    }

    item_hdr *hdr = (item_hdr *)ITEM_data(hdr_it);
    hdr->page_version = loc.page_version;
    hdr->offset = loc.offset;
    hdr->len = loc.len;
    hdr->page_id = loc.page_id;
    hdr_it->nbytes = it->nbytes;
    hdr_it->it_flags |= ITEM_HDR |
        (it->it_flags & (ITEM_TOKEN_SENT|ITEM_STALE|ITEM_KEY_BINARY));
    *hdr_out = hdr_it;
    return 0;
}

/*** UTILITY ***/
// ext_path=<genie_host>:<port>:<size>   size unit m|g|t|p (RDMA port)
// FIXME: Modifies argument. copy instead?
struct extstore_conf_file *storage_conf_parse(char *arg) {
    struct extstore_conf_file *cf = NULL;
    char *b = NULL;
    char unit = 0;
    uint64_t multiplier = 0;
    char *host = strtok_r(arg, ":", &b);
    char *port = strtok_r(NULL, ":", &b);
    char *size = strtok_r(NULL, ":", &b);
    if (!host || !port || !size) {
        fprintf(stderr, "ext_path must be host:port:size, ie: ext_path=10.99.0.2:11212:64g\n");
        goto error;
    }
    cf = calloc(1, sizeof(struct extstore_conf_file));
    cf->file = strdup(host);
    cf->cport = atoi(port);

    unit = tolower(size[strlen(size)-1]);
    size[strlen(size)-1] = '\0';
    switch (unit) {
        case 'm': multiplier = 1024ULL * 1024; break;
        case 'g': multiplier = 1024ULL * 1024 * 1024; break;
        case 't': multiplier = 1024ULL * 1024 * 1024 * 1024; break;
        case 'p': multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024; break;
        default:
            fprintf(stderr, "ext_path size needs a unit (m|g|t|p)\n");
            goto error;
    }
    cf->total_size = multiplier * (uint64_t)atoi(size);
    return cf;
error:
    if (cf) { free(cf->file); free(cf); }
    return NULL;
}

struct storage_settings {
    struct extstore_conf_file *storage_file;
    struct extstore_conf ext_cf;
};

void *storage_init_config(struct settings *s) {
    (void)s;
    struct storage_settings *cf = calloc(1, sizeof(struct storage_settings));

    cf->ext_cf.page_size = 64 * 1024 * 1024;
    cf->ext_cf.io_threadcount = 1;
    cf->ext_cf.io_depth = 64;
    cf->ext_cf.page_buckets = PAGE_BUCKET_COUNT;
    char *v;
    cf->ext_cf.slot_size   = (v = getenv("EXT_SLOT_SIZE"))   ? atoi(v) : 2048;
    g_plaintext_slot_size = cf->ext_cf.slot_size;
    cf->ext_cf.read_slots  = (v = getenv("EXT_READ_SLOTS"))  ? atoi(v) : 32;
    cf->ext_cf.write_slots = (v = getenv("EXT_WRITE_SLOTS")) ? atoi(v) : 256;
    if ((v = getenv("EXT_READ_RETRIES"))) g_read_retries = atoi(v);
    if (getenv("EXT_TRACE_SEAL")) {
        g_seal_tab = calloc(SEAL_TRACE_MAX, sizeof(*g_seal_tab));
        fprintf(stderr, "extstore: seal trace %s\n", g_seal_tab ? "on" : "alloc failed");
    }

    return cf;
}

// TODO: pass settings struct?
int storage_read_config(void *conf, char **subopt) {
    struct storage_settings *cf = conf;
    struct extstore_conf *ext_cf = &cf->ext_cf;
    char *subopts_value;

    enum {
        EXT_PAGE_SIZE,
        EXT_THREADS,
        EXT_IO_DEPTH,
        EXT_PATH,
    };

    char *const subopts_tokens[] = {
        [EXT_PAGE_SIZE] = "ext_page_size",
        [EXT_THREADS] = "ext_threads",
        [EXT_IO_DEPTH] = "ext_io_depth",
        [EXT_PATH] = "ext_path",
        NULL
    };

    switch (getsubopt(subopt, subopts_tokens, &subopts_value)) {
        case EXT_PAGE_SIZE:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_page_size argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &ext_cf->page_size)) {
                fprintf(stderr, "could not parse argument to ext_page_size\n");
                return 1;
            }
            ext_cf->page_size *= 1024 * 1024; /* megabytes */
            break;
        case EXT_THREADS:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_threads argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &ext_cf->io_threadcount)) {
                fprintf(stderr, "could not parse argument to ext_threads\n");
                return 1;
            }
            break;
        case EXT_IO_DEPTH:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_io_depth argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &ext_cf->io_depth)) {
                fprintf(stderr, "could not parse argument to ext_io_depth\n");
                return 1;
            }
            break;
        case EXT_PATH:
            if (subopts_value) {
                struct extstore_conf_file *tmp = storage_conf_parse(subopts_value);
                if (tmp == NULL) {
                    fprintf(stderr, "failed to parse ext_path argument\n");
                    return 1;
                }
                if (cf->storage_file != NULL) {
                    tmp->next = cf->storage_file;
                }
                cf->storage_file = tmp;
            } else {
                fprintf(stderr, "missing argument to ext_path, ie: ext_path=10.99.0.2:11212:4g\n");
                return 1;
            }
            break;
        default:
            fprintf(stderr, "Illegal suboption \"%s\"\n", subopts_value);
            return 1;
    }

    return 0;
}

int storage_check_config(void *conf) {
    struct storage_settings *cf = conf;

    if (cf->storage_file) {
        if (settings.udpport) {
            fprintf(stderr, "Cannot use UDP with extstore enabled (-U 0 to disable)\n");
            return 1;
        }

        struct extstore_conf_file *fh = cf->storage_file;
        while (fh != NULL) {
            // page_count is nearest-but-not-larger-than pages * psize
            fh->page_count = fh->total_size / cf->ext_cf.page_size;
            assert(cf->ext_cf.page_size * fh->page_count <= fh->total_size);
            if (fh->page_count == 0) {
                fprintf(stderr, "supplied ext_path has zero size, cannot use\n");
                return 1;
            }
            fh = fh->next;
        }

        return 0;
    }

    return 2;
}

void *storage_init(void *conf) {
    struct storage_settings *cf = conf;
    struct extstore_conf *ext_cf = &cf->ext_cf;

    enum extstore_res eres;
    void *storage = NULL;
    crc32c_init();

    // Remote values are always AES-256-GCM protected.
    const char *keypath = getenv("EXT_CRYPTO_KEY");
    if (!keypath) {
        fprintf(stderr, "ext crypto: EXT_CRYPTO_KEY is required for remote storage\n");
        return NULL;
    }
    FILE *kf = fopen(keypath, "rb");
    uint8_t key[32];
    if (!kf || fread(key, 1, 32, kf) != 32) {
        fprintf(stderr, "ext crypto: cannot read 32-byte key from %s\n", keypath);
        if (kf) fclose(kf);
        return NULL;
    }
    fclose(kf);
    ext_crypto_init(key);
    g_crypto_on = true;

    storage = extstore_init(cf->storage_file, ext_cf, &eres);
    if (storage == NULL) {
        fprintf(stderr, "Failed to initialize external storage: %s\n",
                extstore_err(eres));
        if (eres == EXTSTORE_INIT_OPEN_FAIL) {
            perror("extstore open");
        }
        return NULL;
    }

    return storage;
}

#endif
