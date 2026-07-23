#include "ext_crypto.h"
#include <string.h>
#include <stdatomic.h>
#include <sys/random.h>
#include <openssl/evp.h>

static uint8_t g_key[32];
static uint32_t g_salt;
static _Atomic uint64_t g_ctr;

void ext_crypto_init(const uint8_t key[32]) {
    memcpy(g_key, key, 32);
    if (getrandom(&g_salt, sizeof(g_salt), 0) != sizeof(g_salt)) {
        /* getrandom shouldn't fail for 4 bytes; if it does, refuse to run
         * rather than seal with a predictable salt. */
        abort();
    }
    atomic_store(&g_ctr, 0);
}

static void make_nonce(uint8_t n[12]) {
    uint64_t c = atomic_fetch_add(&g_ctr, 1);
    memcpy(n, &g_salt, 4);
    memcpy(n + 4, &c, 8);
}

int ext_crypto_seal(uint8_t *out, const void *pt, size_t ptlen,
                    const struct ext_aad *aad) {
    uint8_t nonce[12];
    make_nonce(nonce);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int len, rv = -1;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto out;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto out;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, g_key, nonce) != 1) goto out;
    if (EVP_EncryptUpdate(ctx, NULL, &len, (const uint8_t *)aad, sizeof(*aad)) != 1) goto out;
    if (EVP_EncryptUpdate(ctx, out + 12, &len, pt, ptlen) != 1) goto out;
    if (EVP_EncryptFinal_ex(ctx, out + 12 + len, &len) != 1) goto out;  /* len=0 for GCM */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out + 12 + ptlen) != 1) goto out;
    memcpy(out, nonce, 12);
    rv = (int)ptlen + EXT_CRYPTO_OVERHEAD;
out:
    EVP_CIPHER_CTX_free(ctx);
    return rv;
}

int ext_crypto_open(void *pt_out, const uint8_t *in, size_t inlen,
                    const struct ext_aad *aad) {
    if (inlen < EXT_CRYPTO_OVERHEAD) return -1;
    size_t ctlen = inlen - EXT_CRYPTO_OVERHEAD;
    const uint8_t *nonce = in;
    const uint8_t *ct = in + 12;
    uint8_t tag[16];
    memcpy(tag, in + inlen - 16, 16);        /* copy: DecryptFinal wants non-const */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int len, rv = -1;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto out;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto out;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, g_key, nonce) != 1) goto out;
    if (EVP_DecryptUpdate(ctx, NULL, &len, (const uint8_t *)aad, sizeof(*aad)) != 1) goto out;
    if (EVP_DecryptUpdate(ctx, pt_out, &len, ct, ctlen) != 1) goto out;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) != 1) goto out;
    if (EVP_DecryptFinal_ex(ctx, (uint8_t *)pt_out + len, &len) != 1) goto out;  /* tag check */
    rv = (int)ctlen;
out:
    EVP_CIPHER_CTX_free(ctx);
    return rv;
}
