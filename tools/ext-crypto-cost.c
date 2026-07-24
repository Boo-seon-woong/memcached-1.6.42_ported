/* Exact production ext_crypto_{open,seal} CPU cost. */
#include "../ext_crypto.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

static uint64_t process_ns(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

struct bench {
    const uint8_t *input;
    const uint8_t *plaintext;
    const struct ext_aad *aad;
    size_t ptlen;
    int objlen;
    unsigned long iters;
};

static void *run_open(void *opaque) {
    struct bench *b = opaque;
    uint8_t back[4096];
    for (unsigned long i = 0; i < b->iters; i++)
        assert(ext_crypto_open(back, b->input, b->objlen, b->aad) == (int)b->ptlen);
    return NULL;
}

static void *run_seal(void *opaque) {
    struct bench *b = opaque;
    uint8_t obj[4096];
    for (unsigned long i = 0; i < b->iters; i++)
        assert(ext_crypto_seal(obj, b->plaintext, b->ptlen, b->aad) == b->objlen);
    return NULL;
}

static uint64_t run_threads(void *(*fn)(void *), struct bench *bench, unsigned int n) {
    pthread_t *threads = calloc(n, sizeof(*threads));
    assert(threads);
    uint64_t start = process_ns();
    for (unsigned int i = 0; i < n; i++)
        assert(pthread_create(&threads[i], NULL, fn, bench) == 0);
    for (unsigned int i = 0; i < n; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    uint64_t elapsed = process_ns() - start;
    free(threads);
    return elapsed;
}

int main(int argc, char **argv) {
    size_t ptlen = argc > 1 ? strtoul(argv[1], NULL, 10) : 131;
    unsigned long iters = argc > 2 ? strtoul(argv[2], NULL, 10) : 500000;
    unsigned int threads = argc > 3 ? strtoul(argv[3], NULL, 10) : 1;
    assert(ptlen > 0 && ptlen + EXT_CRYPTO_OVERHEAD <= 4096 && iters > 0 &&
           threads > 0 && threads <= 64);

    uint8_t key[32], pt[4096], obj[4096], back[4096];
    for (size_t i = 0; i < sizeof(key); i++) key[i] = (uint8_t)i;
    memset(pt, 0x5a, ptlen);
    struct ext_aad aad = {
        .hv = 0xdeadbeef, .page_id = 7, .offset = 4096, .page_version = 3
    };
    ext_crypto_init(key);
    int objlen = ext_crypto_seal(obj, pt, ptlen, &aad);
    assert(objlen == (int)ptlen + EXT_CRYPTO_OVERHEAD);
    assert(ext_crypto_open(back, obj, objlen, &aad) == (int)ptlen);
    assert(memcmp(pt, back, ptlen) == 0);

    for (int i = 0; i < 10000; i++)
        assert(ext_crypto_open(back, obj, objlen, &aad) == (int)ptlen);
    struct bench bench = {
        .input = obj, .plaintext = pt, .aad = &aad, .ptlen = ptlen,
        .objlen = objlen, .iters = iters,
    };
    uint64_t open_ns = run_threads(run_open, &bench, threads);
    uint64_t seal_ns = run_threads(run_seal, &bench, threads);
    double operations = (double)iters * threads;

    printf("ptlen=%zu object_len=%d threads=%u iterations_per_thread=%lu "
           "open_cpu_ns_op=%.2f seal_cpu_ns_op=%.2f\n",
           ptlen, objlen, threads, iters, open_ns / operations, seal_ns / operations);
    return 0;
}
