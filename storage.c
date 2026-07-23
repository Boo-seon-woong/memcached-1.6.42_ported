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
static unsigned int g_read_retries = 3;   // torn-read retry cap (EXT_READ_RETRIES)
static _Atomic uint64_t g_read_retry_ct = 0;  // torn-read retries observed (debug)
static _Atomic uint64_t g_read_reresolve_ct = 0;  // reads recovered from RAM after retry exhaustion
static _Atomic uint64_t g_badcrc_log_ct = 0;      // rate-limit for the badcrc diagnostic
static _Atomic uint64_t g_flush_log_ct = 0;       // rate-limit for the flush diagnostic

/* Problem B probes (genie). The pre-read reject path had no counter at all: a GET
 * of a stub can be answered as a miss before any RDMA read happens, and nothing
 * recorded it — which is why "permanently unreadable" looked like the same fault
 * as a torn read. These name which path it is. */
static _Atomic uint64_t g_abort_chunked = 0;   /* P-6: item too large */
static _Atomic uint64_t g_abort_alloc = 0;     /* no read destination available */

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

// Write-path context carried across the async RDMA WRITE completion (P-2).
typedef struct {
    item *it;
    uint32_t hv;
    struct ext_loc loc;
    char *slot;
    obj_io io;
} flush_ctx;

static void storage_write_done_cb(void *e, obj_io *io, int ret);

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
        APPEND_NUM_STAT(i, "free_bucket", "%u",
                st.page_data[i].free_bucket);
    }

    free(st.page_data);
}

// Additional storage stats for the main stats output.
void storage_stats(ADD_STAT add_stats, void *c) {
    struct extstore_stats st;
    if (ext_storage) {
        STATS_LOCK();
        APPEND_STAT("extstore_memory_pressure", "%.2f", stats_state.extstore_memory_pressure);
        APPEND_STAT("extstore_compact_lost", "%llu", (unsigned long long)stats.extstore_compact_lost);
        APPEND_STAT("extstore_compact_rescues", "%llu", (unsigned long long)stats.extstore_compact_rescues);
        APPEND_STAT("extstore_compact_resc_cold", "%llu", (unsigned long long)stats.extstore_compact_resc_cold);
        APPEND_STAT("extstore_compact_resc_old", "%llu", (unsigned long long)stats.extstore_compact_resc_old);
        APPEND_STAT("extstore_compact_skipped", "%llu", (unsigned long long)stats.extstore_compact_skipped);
        STATS_UNLOCK();
        extstore_get_stats(ext_storage, &st);
        APPEND_STAT("extstore_page_allocs", "%llu", (unsigned long long)st.page_allocs);
        APPEND_STAT("extstore_page_evictions", "%llu", (unsigned long long)st.page_evictions);
        APPEND_STAT("extstore_page_reclaims", "%llu", (unsigned long long)st.page_reclaims);
        APPEND_STAT("extstore_pages_free", "%llu", (unsigned long long)st.pages_free);
        APPEND_STAT("extstore_pages_used", "%llu", (unsigned long long)st.pages_used);
        APPEND_STAT("extstore_objects_evicted", "%llu", (unsigned long long)st.objects_evicted);
        APPEND_STAT("extstore_objects_read", "%llu", (unsigned long long)st.objects_read);
        APPEND_STAT("extstore_objects_written", "%llu", (unsigned long long)st.objects_written);
        APPEND_STAT("extstore_objects_used", "%llu", (unsigned long long)st.objects_used);
        APPEND_STAT("extstore_bytes_evicted", "%llu", (unsigned long long)st.bytes_evicted);
        APPEND_STAT("extstore_bytes_written", "%llu", (unsigned long long)st.bytes_written);
        APPEND_STAT("extstore_bytes_read", "%llu", (unsigned long long)st.bytes_read);
        APPEND_STAT("extstore_bytes_used", "%llu", (unsigned long long)st.bytes_used);
        APPEND_STAT("extstore_bytes_fragmented", "%llu", (unsigned long long)st.bytes_fragmented);
        APPEND_STAT("extstore_limit_maxbytes", "%llu", (unsigned long long)(st.page_count * st.page_size));
        APPEND_STAT("extstore_io_queue", "%llu", (unsigned long long)(st.io_queue));
        // RDMA bring-up debug counters (SPEC §8). Nonzero = look here first.
        APPEND_STAT("extstore_engine_dead", "%llu", (unsigned long long)st.engine_dead);
        APPEND_STAT("extstore_write_failures", "%llu", (unsigned long long)st.write_failures);
        APPEND_STAT("extstore_read_failures", "%llu", (unsigned long long)st.read_failures);
        APPEND_STAT("extstore_read_retries", "%llu",
                (unsigned long long)atomic_load(&g_read_retry_ct));
        APPEND_STAT("extstore_read_reresolved", "%llu",
                (unsigned long long)atomic_load(&g_read_reresolve_ct));
        APPEND_STAT("extstore_get_aborted_chunked", "%llu",
                (unsigned long long)atomic_load(&g_abort_chunked));
        APPEND_STAT("extstore_get_aborted_alloc", "%llu",
                (unsigned long long)atomic_load(&g_abort_alloc));
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
        if (ext_crypto_open(read_it, (uint8_t *)io->buf, io->len, &aad) < 0) {
            // tag fail = torn read (concurrent in-place overwrite) or tamper.
            if (io->retries++ < g_read_retries) {
                atomic_fetch_add(&g_read_retry_ct, 1);
                io->next = NULL;
                extstore_submit(e, io);   // engine frees this bounce slot, re-posts
                return;
            }
            // Retries exhausted: an in-place overwrite (P-1a) keeps racing this
            // read, so re-reading the slot can never win. Don't lose data — the
            // overwrite came from a SET that had the value in RAM, so re-resolve
            // the key and serve the live RAM copy if it's still there and the
            // same size. (genie's finding: torn reads under write load returned
            // miss for data that exists.)
            uint32_t rhv = hash(ITEM_key(p->hdr_it), p->hdr_it->nkey);
            item_lock(rhv);
            item *cur = assoc_find(ITEM_key(p->hdr_it), p->hdr_it->nkey, rhv);
            if (cur && !(cur->it_flags & ITEM_HDR) &&
                    ITEM_ntotal(cur) == ITEM_ntotal(p->hdr_it)) {
                memcpy(read_it, cur, ITEM_ntotal(cur));  // serve live value (copy under lock)
                item_unlock(rhv);
                atomic_fetch_add(&g_read_reresolve_ct, 1);
                // fall through to the hit path (miss stays false).
            } else {
                item_unlock(rhv);
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
        }
    } else {
        memcpy(read_it, io->buf, io->len);   // crypto off: io->len == ntotal
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
    if (ntotal > settings.slab_chunk_size_max) {
        // P-6: chunked items unsupported on the RDMA backend.
        atomic_fetch_add(&g_abort_chunked, 1);
        return -1;
    }
    new_it = do_item_alloc_pull(ntotal, clsid);
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
static void recache_or_free(io_pending_t *pending) {
    // re-cast to our specific struct.
    io_pending_storage_t *p = (io_pending_storage_t *)pending;

    conn *c = p->c;
    item *it = p->read_it;   // the decrypt destination we allocated
    assert(c != NULL);
    // D7: no recache — every GET is a real remote read. Just free the buffer.
    if (p->active) {
        // If request never dispatched, free the read buffer, leave header alone.
        slabs_free(it, p->read_clsid);
        p->resp->suspended = false;
        c->resps_suspended--;
        io_queue_t *q = thread_io_queue_get(p->thread, p->io_queue_type);
        STAILQ_REMOVE(&q->stack, pending, _io_pending_t, iop_next);
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.get_aborted_extstore++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    } else if (p->miss) {
        // A badcrc "miss" is a torn read (in-place overwrite raced us) — the data
        // still exists in remote memory, so DON'T unlink the key. Unlinking here
        // permanently deletes a live key and leaks its remote slot (genie's
        // finding). Only a genuine miss (transport failure / real absence) drops
        // the stub.
        if (!p->badcrc)
            item_unlink(p->hdr_it);
        slabs_free(it, p->read_clsid);
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.miss_from_extstore++;
        if (p->badcrc)
            c->thread->stats.badcrc_from_extstore++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    } else {
        slabs_free(it, p->read_clsid);
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
    recache_or_free(pending);
    // don't need to free the main context, since it's embedded.
}

/*
 * WRITE FLUSH — per-item RDMA WRITE at SET time (SPEC §5; D7 immediate flush,
 * D7/P-1 in-place overwrite, P-2 verify-before-swap + one-in-flight-per-key).
 */

// Seal `it` into a staging slot and submit a remote WRITE. Caller MUST hold
// item_lock(hv). inherit != NULL reuses the old slot for same-size overwrite.
static void storage_flush_item(void *e, item *it, uint32_t hv,
                               const struct ext_loc *inherit) {
    size_t ntotal = ITEM_ntotal(it);
    unsigned int rlen = ntotal + (g_crypto_on ? EXT_CRYPTO_OVERHEAD : 0);
    if ((it->it_flags & (ITEM_CHUNKED|ITEM_HDR)) || it->nbytes <= 2) return;
    if (it->it_flags & ITEM_RFLUSH) return;   // one in-flight write per key (P-2)

    struct ext_loc loc;
    bool in_place = (inherit && inherit->len == rlen);
    if (in_place) {
        loc = *inherit;
    } else {
        if (inherit) extstore_free_loc(e, inherit);   // size changed: release old
        if (extstore_alloc(e, rlen, PAGE_BUCKET_DEFAULT, &loc) != 0)
            return;                                    // remote full: leave in RAM
    }

    char *slot = extstore_staging_get(e);
    if (!slot) { if (!in_place) extstore_free_loc(e, &loc); return; }  // backpressure

    if (g_crypto_on) {
        struct ext_aad aad = { .hv = hv, .page_id = loc.page_id, .pad = 0,
            .offset = loc.offset, .page_version = loc.page_version };
        ext_crypto_seal((uint8_t *)slot, it, ntotal, &aad);
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
    }
    // Diagnostic (genie's ask): log which key seals which slot + the nonce it
    // wrote, so a badcrc slot_nonce can be matched back to its true owner.
    if (g_crypto_on && atomic_fetch_add(&g_flush_log_ct, 1) < 200) {
        const unsigned char *n = (const unsigned char *)slot;
        fprintf(stderr, "extstore flush: key=%.*s -> {page=%u off=%u ver=%u} %s "
            "nonce=%02x%02x%02x%02x.%02x%02x%02x%02x%02x%02x%02x%02x\n",
            it->nkey, ITEM_key(it), loc.page_id, loc.offset, loc.page_version,
            in_place ? "in_place" : "alloc",
            n[0],n[1],n[2],n[3],n[4],n[5],n[6],n[7],n[8],n[9],n[10],n[11]);
    }

    // ponytail: per-flush malloc; pool if the SET rate makes it show up.
    flush_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { extstore_staging_put(e, slot); if (!in_place) extstore_free_loc(e, &loc); return; }
    it->it_flags |= ITEM_RFLUSH;
    refcount_incr(it);              // own a reference for the flight (lock held)
    ctx->it = it; ctx->hv = hv; ctx->loc = loc; ctx->slot = slot;
    ctx->io.mode = OBJ_IO_WRITE;
    ctx->io.buf = slot;
    ctx->io.page_id = loc.page_id;
    ctx->io.page_version = loc.page_version;
    ctx->io.offset = loc.offset;
    ctx->io.len = rlen;
    ctx->io.cb = storage_write_done_cb;
    ctx->io.data = ctx;
    ctx->io.next = NULL;
    extstore_submit(e, &ctx->io);
}

// IO-thread context: verify the flushed version is still live, then swap the
// RAM item for a stub. Handles supersede (chain a re-flush) and delete (P-2).
static void storage_write_done_cb(void *e, obj_io *io, int ret) {
    flush_ctx *ctx = io->data;
    extstore_staging_put(e, ctx->slot);
    item_lock(ctx->hv);
    if (ret < 0) {
        ctx->it->it_flags &= ~ITEM_RFLUSH;      // leave in RAM; next SET retries
        item_unlock(ctx->hv);
        item_remove(ctx->it);
        free(ctx);
        return;
    }
    item *cur = assoc_find(ITEM_key(ctx->it), ctx->it->nkey, ctx->hv);
    if (cur == ctx->it && (cur->it_flags & ITEM_LINKED)) {
        client_flags_t flags; FLAGS_CONV(ctx->it, flags);
        item *hdr_it = do_item_alloc(ITEM_key(ctx->it), ctx->it->nkey, flags,
                                     ctx->it->exptime, sizeof(item_hdr));
        if (hdr_it != NULL) {
            item_hdr *hdr = (item_hdr *)ITEM_data(hdr_it);
            hdr->page_version = ctx->loc.page_version;
            hdr->offset = ctx->loc.offset;
            hdr->len = ctx->loc.len;
            hdr->page_id = ctx->loc.page_id;
            // overload nbytes onto the stub so GET reconstructs the original
            // size (ITEM_ntotal(stub) == original ntotal). Mirrors stock.
            hdr_it->nbytes = ctx->it->nbytes;
            hdr_it->it_flags |= ITEM_HDR | (ctx->it->it_flags & ITEM_PRESERVE_FLAGS);
            do_item_replace(ctx->it, hdr_it, ctx->hv, ITEM_get_cas(ctx->it));
            do_item_remove(hdr_it);
        }   // else: alloc failed -> full item stays in RAM, next SET retries
        ctx->it->it_flags &= ~ITEM_RFLUSH;
        item_unlock(ctx->hv);
        item_remove(ctx->it);
    } else if (cur && !(cur->it_flags & (ITEM_HDR|ITEM_RFLUSH)) &&
               (unsigned int)(ITEM_ntotal(cur) +
                   (g_crypto_on ? EXT_CRYPTO_OVERHEAD : 0)) == ctx->loc.len) {
        // superseded by a same-size SET: chain a re-flush into the same slot.
        ctx->it->it_flags &= ~ITEM_RFLUSH;
        storage_flush_item(e, cur, ctx->hv, &ctx->loc);   // takes a ref under lock
        item_unlock(ctx->hv);
        item_remove(ctx->it);
        free(ctx);
        return;
    } else {
        extstore_free_loc(e, &ctx->loc);        // deleted / different size: reclaim
        ctx->it->it_flags &= ~ITEM_RFLUSH;
        item_unlock(ctx->hv);
        item_remove(ctx->it);
    }
    free(ctx);
}

// Called from the store path (memcached.c) with item_lock(hv) held. Extracts
// the old item's remote slot for in-place reuse, then flushes.
void storage_flush_on_store(void *e, item *it, item *old_it, uint32_t hv) {
    struct ext_loc inh, *pinh = NULL;
    if (old_it && (old_it->it_flags & ITEM_HDR)) {
        item_hdr *oh = (item_hdr *)ITEM_data(old_it);
        inh = (struct ext_loc){ oh->page_version, oh->offset, oh->len, oh->page_id };
        pinh = &inh;
    }
    storage_flush_item(e, it, hv, pinh);
}

// LRU-driven background flush and compaction are gone (D7). No-op stubs for
// the callers in thread.c / memcached.c.
void storage_write_pause(void) { }
void storage_write_resume(void) { }
int start_storage_write_thread(void *arg) { (void)arg; return 0; }
void storage_compact_pause(void) { }
void storage_compact_resume(void) { }
int start_storage_compact_thread(void *arg) { (void)arg; return 0; }

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
    cf->free_bucket = PAGE_BUCKET_DEFAULT;
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
    struct storage_settings *cf = calloc(1, sizeof(struct storage_settings));

    s->ext_item_size = 512;
    s->ext_item_age = UINT_MAX;
    s->ext_low_ttl = 0;
    s->ext_recache_rate = 2000;
    s->ext_max_frag = 0.8;
    s->ext_drop_unread = false;
    s->ext_wbuf_size = 1024 * 1024 * 4;
    s->ext_compact_under = 0;
    s->ext_drop_under = 0;
    s->ext_max_sleep = 1000000;
    s->slab_automove_freeratio = 0.01;
    s->ext_page_size = 1024 * 1024 * 64;
    s->ext_io_threadcount = 1;
    cf->ext_cf.page_size = settings.ext_page_size;
    cf->ext_cf.wbuf_size = settings.ext_wbuf_size;
    cf->ext_cf.io_threadcount = settings.ext_io_threadcount;
    cf->ext_cf.io_depth = 64;
    cf->ext_cf.page_buckets = PAGE_BUCKET_COUNT;
    cf->ext_cf.wbuf_count = cf->ext_cf.page_buckets;
    // RDMA slot/staging sizing. ponytail: env knobs; promote to -o if needed.
    char *v;
    cf->ext_cf.slot_size   = (v = getenv("EXT_SLOT_SIZE"))   ? atoi(v) : 2048;
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
        EXT_WBUF_SIZE,
        EXT_THREADS,
        EXT_IO_DEPTH,
        EXT_PATH,
        EXT_ITEM_SIZE,
        EXT_ITEM_AGE,
        EXT_LOW_TTL,
        EXT_RECACHE_RATE,
        EXT_COMPACT_UNDER,
        EXT_DROP_UNDER,
        EXT_MAX_SLEEP,
        EXT_MAX_FRAG,
        EXT_DROP_UNREAD,
        SLAB_AUTOMOVE_FREERATIO, // FIXME: move this back?
    };

    char *const subopts_tokens[] = {
        [EXT_PAGE_SIZE] = "ext_page_size",
        [EXT_WBUF_SIZE] = "ext_wbuf_size",
        [EXT_THREADS] = "ext_threads",
        [EXT_IO_DEPTH] = "ext_io_depth",
        [EXT_PATH] = "ext_path",
        [EXT_ITEM_SIZE] = "ext_item_size",
        [EXT_ITEM_AGE] = "ext_item_age",
        [EXT_LOW_TTL] = "ext_low_ttl",
        [EXT_RECACHE_RATE] = "ext_recache_rate",
        [EXT_COMPACT_UNDER] = "ext_compact_under",
        [EXT_DROP_UNDER] = "ext_drop_under",
        [EXT_MAX_SLEEP] = "ext_max_sleep",
        [EXT_MAX_FRAG] = "ext_max_frag",
        [EXT_DROP_UNREAD] = "ext_drop_unread",
        [SLAB_AUTOMOVE_FREERATIO] = "slab_automove_freeratio",
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
        case EXT_WBUF_SIZE:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_wbuf_size argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &ext_cf->wbuf_size)) {
                fprintf(stderr, "could not parse argument to ext_wbuf_size\n");
                return 1;
            }
            ext_cf->wbuf_size *= 1024 * 1024; /* megabytes */
            settings.ext_wbuf_size = ext_cf->wbuf_size;
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
        case EXT_ITEM_SIZE:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_item_size argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &settings.ext_item_size)) {
                fprintf(stderr, "could not parse argument to ext_item_size\n");
                return 1;
            }
            break;
        case EXT_ITEM_AGE:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_item_age argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &settings.ext_item_age)) {
                fprintf(stderr, "could not parse argument to ext_item_age\n");
                return 1;
            }
            break;
        case EXT_LOW_TTL:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_low_ttl argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &settings.ext_low_ttl)) {
                fprintf(stderr, "could not parse argument to ext_low_ttl\n");
                return 1;
            }
            break;
        case EXT_RECACHE_RATE:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_recache_rate argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &settings.ext_recache_rate)) {
                fprintf(stderr, "could not parse argument to ext_recache_rate\n");
                return 1;
            }
            break;
        case EXT_COMPACT_UNDER:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_compact_under argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &settings.ext_compact_under)) {
                fprintf(stderr, "could not parse argument to ext_compact_under\n");
                return 1;
            }
            break;
        case EXT_DROP_UNDER:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_drop_under argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &settings.ext_drop_under)) {
                fprintf(stderr, "could not parse argument to ext_drop_under\n");
                return 1;
            }
            break;
        case EXT_MAX_SLEEP:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_max_sleep argument\n");
                return 1;
            }
            if (!safe_strtoul(subopts_value, &settings.ext_max_sleep)) {
                fprintf(stderr, "could not parse argument to ext_max_sleep\n");
                return 1;
            }
            break;
        case EXT_MAX_FRAG:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing ext_max_frag argument\n");
                return 1;
            }
            if (!safe_strtod(subopts_value, &settings.ext_max_frag)) {
                fprintf(stderr, "could not parse argument to ext_max_frag\n");
                return 1;
            }
            break;
        case SLAB_AUTOMOVE_FREERATIO:
            if (subopts_value == NULL) {
                fprintf(stderr, "Missing slab_automove_freeratio argument\n");
                return 1;
            }
            if (!safe_strtod(subopts_value, &settings.slab_automove_freeratio)) {
                fprintf(stderr, "could not parse argument to slab_automove_freeratio\n");
                return 1;
            }
            break;
        case EXT_DROP_UNREAD:
            settings.ext_drop_unread = true;
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
                fprintf(stderr, "missing argument to ext_path, ie: ext_path=/d/file:5G\n");
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
    struct extstore_conf *ext_cf = &cf->ext_cf;

    if (cf->storage_file) {
        if (settings.item_size_max > ext_cf->wbuf_size) {
            fprintf(stderr, "-I (item_size_max: %d) cannot be larger than ext_wbuf_size: %d\n",
                settings.item_size_max, ext_cf->wbuf_size);
            return 1;
        }

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

    // Load AES-256-GCM key if configured (crypto on). ponytail: env path knob.
    const char *keypath = getenv("EXT_CRYPTO_KEY");
    if (keypath) {
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
    }

    settings.ext_global_pool_min = 0;
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
