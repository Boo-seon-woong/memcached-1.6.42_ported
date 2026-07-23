/* genie_memd — passive one-sided RDMA memory server for the extstore RDMA port.
 * Allocates one big MR and hands (addr,rkey,size) to every client connection in
 * the accept private_data, then idles. Client does all READ/WRITE (one-sided).
 * Connection via rdma_cm (IP-based; CM resolves GID/route/MTU — no hardcoding).
 *
 * Build: cc -O2 -o genie_memd genie_memd.c -lrdmacm -libverbs
 * Run:   ./genie_memd <port> <size> [--hugepages]     size accepts k/m/g/t suffix
 *
 * ponytail: single MR, no reconnect bookkeeping — experiment harness only.
 * Any error = exit (abnormal exit is itself the "experiment invalid" signal).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>

struct xrd_mr_info { uint64_t raddr; uint32_t rkey; uint64_t size; } __attribute__((packed));

static void dief(const char *m) { fprintf(stderr, "%s\n", m); exit(1); }

/* Who is on the other end — needed to tell "the SEV guest reached us" apart from
 * "a local loopback client reached us" when several clients share one server. */
static const char *peer(struct rdma_cm_id *id) {
    static __thread char buf[64];
    struct sockaddr *sa = rdma_get_peer_addr(id);
    if (!sa || sa->sa_family != AF_INET) return "?";
    struct sockaddr_in *in = (struct sockaddr_in *)sa;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
    snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(in->sin_port));
    return buf;
}

static uint64_t parse_size(const char *s) {
    char *end; uint64_t v = strtoull(s, &end, 10);
    switch (*end) {
        case 'k': case 'K': return v << 10;
        case 'm': case 'M': return v << 20;
        case 'g': case 'G': return v << 30;
        case 't': case 'T': return v << 40;
        default: return v;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) dief("usage: genie_memd <port> <size> [--hugepages]");
    int port = atoi(argv[1]);
    uint64_t size = parse_size(argv[2]);
    int huge = (argc > 3 && strcmp(argv[3], "--hugepages") == 0);

    /* listen via rdma_cm */
    struct rdma_event_channel *ch = rdma_create_event_channel();
    if (!ch) dief("rdma_create_event_channel");
    struct rdma_cm_id *lid;
    if (rdma_create_id(ch, &lid, NULL, RDMA_PS_TCP)) dief("rdma_create_id");
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                              .sin_port = htons(port) };
    if (rdma_bind_addr(lid, (struct sockaddr *)&sa)) dief("rdma_bind_addr");
    if (rdma_listen(lid, 64)) dief("rdma_listen");

    /* backing memory: registered once the first connection gives us a verbs ctx.
     * We need a pd, which needs a device context; get it from the listen id after
     * the first connect request (event->id->verbs), or open eagerly here. */
    struct ibv_pd *pd = NULL;
    struct ibv_mr *mr = NULL;
    void *mem = NULL;
    struct xrd_mr_info info = { 0, 0, size };

    fprintf(stderr, "genie_memd: listening on :%d, size=%lu%s\n",
            port, (unsigned long)size, huge ? " (hugepages)" : "");

    while (1) {
        struct rdma_cm_event *ev;
        if (rdma_get_cm_event(ch, &ev)) dief("rdma_get_cm_event");

        if (ev->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
            struct rdma_cm_id *cid = ev->id;
            fprintf(stderr, "genie_memd: connect request from %s\n", peer(cid));
            if (!pd) {
                /* first connection: allocate pd + register the MR now */
                pd = ibv_alloc_pd(cid->verbs);
                if (!pd) dief("ibv_alloc_pd");
                int mf = MAP_PRIVATE | MAP_ANONYMOUS | (huge ? MAP_HUGETLB : 0);
                mem = mmap(NULL, size, PROT_READ | PROT_WRITE, mf, -1, 0);
                if (mem == MAP_FAILED) dief("mmap");
                mr = ibv_reg_mr(pd, mem, size, IBV_ACCESS_LOCAL_WRITE |
                        IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
                if (!mr) dief("ibv_reg_mr");
                info.raddr = (uint64_t)(uintptr_t)mem;
                info.rkey = mr->rkey;
                fprintf(stderr, "genie_memd: MR ready raddr=0x%lx rkey=0x%x\n",
                        (unsigned long)info.raddr, info.rkey);
            }
            struct ibv_qp_init_attr ia = { .qp_type = IBV_QPT_RC,
                .cap = { .max_send_wr = 1, .max_recv_wr = 1,
                         .max_send_sge = 1, .max_recv_sge = 1 } };
            if (rdma_create_qp(cid, pd, &ia)) dief("rdma_create_qp");
            struct rdma_conn_param cp = { .private_data = &info,
                .private_data_len = sizeof(info), .responder_resources = 16,
                .initiator_depth = 16, .rnr_retry_count = 7 };
            if (rdma_accept(cid, &cp)) dief("rdma_accept");
        } else if (ev->event == RDMA_CM_EVENT_ESTABLISHED) {
            fprintf(stderr, "genie_memd: connection up %s\n", peer(ev->id));
        } else if (ev->event == RDMA_CM_EVENT_DISCONNECTED) {
            /* ack BEFORE destroy: rdma_destroy_id() blocks until every event on
             * that id is acked, so destroying here would hang the event loop and
             * no later client could ever connect. */
            struct rdma_cm_id *cid = ev->id;
            rdma_ack_cm_event(ev);
            fprintf(stderr, "genie_memd: connection closed %s\n", peer(cid));
            rdma_destroy_qp(cid);
            rdma_destroy_id(cid);
            continue;
        } else {
            /* everything else (REJECTED, ADDR/ROUTE errors, TIMEWAIT_EXIT...)
             * used to be swallowed silently — that hides client-side failures. */
            fprintf(stderr, "genie_memd: event %s (status %d) %s\n",
                    rdma_event_str(ev->event), ev->status, peer(ev->id));
        }
        rdma_ack_cm_event(ev);
    }
    return 0;
}
