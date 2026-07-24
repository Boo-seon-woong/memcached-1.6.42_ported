/* Self-check for ext_crypto. Build: cc -o t test_ext_crypto.c ext_crypto.c -lcrypto */
#include "ext_crypto.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = i;
    ext_crypto_init(key);

    const char *pt = "the quick brown fox jumps over 64 bytes of value payload........";
    size_t ptlen = strlen(pt);
    struct ext_aad aad = { .hv = 0xdeadbeef, .page_id = 7, .offset = 4096, .page_version = 3 };
    uint8_t obj[256], back[256];

    /* round trip */
    int n = ext_crypto_seal(obj, pt, ptlen, &aad);
    assert(n == (int)ptlen + EXT_CRYPTO_OVERHEAD);
    assert(ext_crypto_open(back, obj, n, &aad) == (int)ptlen);
    assert(memcmp(back, pt, ptlen) == 0);

    /* AAD mismatch (slot reused by another key) → reject */
    struct ext_aad bad = aad; bad.hv ^= 1;
    assert(ext_crypto_open(back, obj, n, &bad) == -1);
    assert(ext_crypto_open(back, obj, n, &aad) == (int)ptlen);
    assert(memcmp(back, pt, ptlen) == 0);

    /* torn read: a byte of ciphertext overwritten mid-flight → reject */
    uint8_t torn[256]; memcpy(torn, obj, n);
    torn[20] ^= 0xff;
    assert(ext_crypto_open(back, torn, n, &aad) == -1);

    /* two seals never reuse a nonce */
    uint8_t o2[256];
    ext_crypto_seal(o2, pt, ptlen, &aad);
    assert(memcmp(obj, o2, 12) != 0);

    puts("ext_crypto: ok");
    return 0;
}
