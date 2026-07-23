/* AES-256-GCM AEAD for extstore RDMA remote objects.
 * Object layout on the wire/remote: [ nonce 12B ][ ciphertext ][ tag 16B ].
 * nonce = boot_salt(4B) || global monotonic counter(8B), stored with the object
 * so the reader never recomputes it. See EXTSTORE_RDMA_SPEC.md §2.1 / P-4.
 */
#ifndef EXT_CRYPTO_H
#define EXT_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define EXT_CRYPTO_OVERHEAD 28   /* 12 nonce + 16 tag */

/* AAD binds an object to its logical identity + physical slot, so a stale read
 * of a slot reused by another key fails the tag. Packed, hashed as raw bytes. */
struct ext_aad {
    uint32_t hv;
    uint16_t page_id;
    uint16_t pad;
    uint32_t offset;
    uint32_t page_version;
};

/* One-time. key = 32 bytes. Draws a random 4B boot salt. */
void ext_crypto_init(const uint8_t key[32]);

/* out must hold ptlen + EXT_CRYPTO_OVERHEAD. Returns bytes written, or -1. */
int ext_crypto_seal(uint8_t *out, const void *pt, size_t ptlen,
                    const struct ext_aad *aad);

/* inlen = ptlen + EXT_CRYPTO_OVERHEAD. Writes ptlen bytes to pt_out.
 * Returns plaintext length on success, -1 on tag mismatch (torn read / tamper). */
int ext_crypto_open(void *pt_out, const uint8_t *in, size_t inlen,
                    const struct ext_aad *aad);

#endif
