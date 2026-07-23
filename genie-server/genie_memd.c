/* genie_memd — passive one-sided RDMA memory server for the extstore RDMA port.
 * Allocates one big MR, hands (addr,rkey,size) to a single client over TCP,
 * transitions QPs to RTS, then idles. Client does all READ/WRITE (one-sided).
 *
 * Build: cc -O2 -o genie_memd genie_memd.c -libverbs
 * Run:   ./genie_memd <port> <size> [--hugepages]     size accepts k/m/g/t suffix
 *
 * ponytail: single client, no reconnect, fixed PSN, port 1 / gid idx 0 —
 * experiment harness only. Wire format must match EXTSTORE_RDMA_SPEC.md §2.7.
 * Any error = exit (abnormal exit is itself the "experiment invalid" signal).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define MAGIC "XRD1"
#define GID_IDX 0
#define IB_PORT 1
#define PSN 0

static void die(const char *m) { perror(m); exit(1); }
static void dief(const char *m) { fprintf(stderr, "%s\n", m); exit(1); }

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

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) { ssize_t r = read(fd, p, n); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}
static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) { ssize_t r = write(fd, p, n); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) dief("usage: genie_memd <port> <size> [--hugepages]");
    int port = atoi(argv[1]);
    uint64_t size = parse_size(argv[2]);
    int huge = (argc > 3 && strcmp(argv[3], "--hugepages") == 0);

    /* device + pd */
    int ndev; struct ibv_device **devs = ibv_get_device_list(&ndev);
    if (!devs || ndev == 0) dief("no RDMA device");
    struct ibv_context *ctx = ibv_open_device(devs[0]);
    if (!ctx) dief("ibv_open_device");
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) dief("ibv_alloc_pd");

    /* backing memory + MR */
    int mf = MAP_PRIVATE | MAP_ANONYMOUS | (huge ? MAP_HUGETLB : 0);
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, mf, -1, 0);
    if (mem == MAP_FAILED) die("mmap");
    struct ibv_mr *mr = ibv_reg_mr(pd, mem, size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) dief("ibv_reg_mr");

    union ibv_gid gid;
    if (ibv_query_gid(ctx, IB_PORT, GID_IDX, &gid)) dief("query_gid");

    /* TCP accept one client */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                              .sin_port = htons(port) };
    if (bind(ls, (void *)&sa, sizeof(sa))) die("bind");
    if (listen(ls, 1)) die("listen");
    fprintf(stderr, "genie_memd: mr=%p size=%lu rkey=0x%x, waiting on :%d\n",
            mem, (unsigned long)size, mr->rkey, port);
    int cs = accept(ls, NULL, NULL);
    if (cs < 0) die("accept");

    /* client → genie: [magic][u32 nqp][nqp×{qpn,psn}][gid] */
    char magic[4]; uint32_t nqp;
    if (read_full(cs, magic, 4) || memcmp(magic, MAGIC, 4)) dief("bad magic");
    if (read_full(cs, &nqp, 4)) dief("read nqp");
    if (nqp == 0 || nqp > 64) dief("bad nqp");
    struct { uint32_t qpn, psn; } cli[64];
    if (read_full(cs, cli, nqp * 8)) dief("read qpns");
    union ibv_gid cli_gid;
    if (read_full(cs, &cli_gid, 16)) dief("read gid");

    /* create nqp QPs, INIT */
    struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!cq) dief("create_cq");
    struct ibv_qp *qps[64];
    for (uint32_t i = 0; i < nqp; i++) {
        struct ibv_qp_init_attr ia = {
            .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
            .cap = { .max_send_wr = 1, .max_recv_wr = 1,
                     .max_send_sge = 1, .max_recv_sge = 1 },
        };
        qps[i] = ibv_create_qp(pd, &ia);
        if (!qps[i]) dief("create_qp");
        struct ibv_qp_attr a = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
            .port_num = IB_PORT,
            .qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE };
        if (ibv_modify_qp(qps[i], &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) dief("modify INIT");
    }

    /* genie → client: [magic][u64 raddr][u32 rkey][u64 size][nqp×{qpn,psn}][gid] */
    uint8_t buf[8 + 4 + 8 + 64 * 8 + 16], *w = buf;
    uint64_t raddr = (uint64_t)(uintptr_t)mem;
    memcpy(w, &raddr, 8); w += 8;
    memcpy(w, &mr->rkey, 4); w += 4;
    memcpy(w, &size, 8); w += 8;
    for (uint32_t i = 0; i < nqp; i++) {
        uint32_t qpn = qps[i]->qp_num, psn = PSN;
        memcpy(w, &qpn, 4); w += 4; memcpy(w, &psn, 4); w += 4;
    }
    memcpy(w, &gid, 16); w += 16;
    if (write_full(cs, MAGIC, 4) || write_full(cs, buf, w - buf)) dief("reply");

    /* RTR + RTS against client's qpn/psn/gid */
    for (uint32_t i = 0; i < nqp; i++) {
        struct ibv_qp_attr r = {
            .qp_state = IBV_QPS_RTR, .path_mtu = IBV_MTU_1024,
            .dest_qp_num = cli[i].qpn, .rq_psn = cli[i].psn,
            .max_dest_rd_atomic = 16, .min_rnr_timer = 12,
            .ah_attr = { .is_global = 1, .port_num = IB_PORT,
                .grh = { .hop_limit = 1, .sgid_index = GID_IDX } },
        };
        memcpy(&r.ah_attr.grh.dgid, &cli_gid, 16);
        if (ibv_modify_qp(qps[i], &r, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                IBV_QP_MIN_RNR_TIMER)) dief("modify RTR");
        struct ibv_qp_attr t = {
            .qp_state = IBV_QPS_RTS, .sq_psn = PSN, .timeout = 14, .retry_cnt = 7,
            .rnr_retry = 7, .max_rd_atomic = 16,
        };
        if (ibv_modify_qp(qps[i], &t, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC))
            dief("modify RTS");
    }
    close(cs); close(ls);
    fprintf(stderr, "genie_memd: %u QP(s) up, serving.\n", nqp);
    pause();
    return 0;
}
