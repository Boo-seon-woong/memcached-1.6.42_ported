/* Measure the cost of ibv_advise_mr on this host, with no fabric traffic:
 * open the device, register an MR, and time the advise call itself.
 * On a stock (unpatched) stack the call returns EOPNOTSUPP — which is exactly
 * what we want to know, because it isolates the ioctl round-trip floor from the
 * SWIOTLB sync work the patched guest module actually does.
 *
 * cc -O2 -o advise_cost advise_cost.c -libverbs
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <infiniband/verbs.h>

#ifndef IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU
#define IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU 3
#endif

static double now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 20000;
    size_t slot = 256;

    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    if (!list || !n) { fprintf(stderr, "no RDMA device\n"); return 1; }
    struct ibv_context *ctx = ibv_open_device(list[0]);
    if (!ctx) { fprintf(stderr, "open_device failed\n"); return 1; }
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { fprintf(stderr, "alloc_pd failed\n"); return 1; }

    size_t bufsz = slot * 64;
    char *buf = aligned_alloc(4096, bufsz);
    memset(buf, 0, bufsz);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, bufsz, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) { fprintf(stderr, "reg_mr failed\n"); return 1; }

    printf("device=%s iters=%d\n", ibv_get_device_name(list[0]), iters);

    /* sweep the sge count: tells us whether cost is per-call or per-object,
     * which decides whether batching more objects per advise would help. */
    for (int nsge = 1; nsge <= 32; nsge *= 2) {
        struct ibv_sge sg[32];
        for (int i = 0; i < nsge; i++)
            sg[i] = (struct ibv_sge){ .addr = (uintptr_t)(buf + i * slot),
                                      .length = slot, .lkey = mr->lkey };
        int rc = 0;
        double t0 = now_us();
        for (int i = 0; i < iters; i++)
            rc = ibv_advise_mr(pd, IBV_ADVISE_MR_ADVICE_SYNC_FOR_CPU,
                               IBV_ADVISE_MR_FLAG_FLUSH, sg, nsge);
        double t1 = now_us();
        printf("  nsge=%-3d %8.3f us/call   (%s)\n", nsge, (t1 - t0) / iters,
               rc ? strerror(rc) : "supported");
    }
    return 0;
}
