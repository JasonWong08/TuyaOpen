#ifndef H_MESH_CRYPT_AES_
#define H_MESH_CRYPT_AES_

#include "mesh_crypt/constants.h"

#include "mbedtls/aes.h"

struct tc_aes_key_sched_struct {
    mbedtls_aes_context ctx;
};

static inline int tc_aes128_set_encrypt_key(struct tc_aes_key_sched_struct *s, const uint8_t *key)
{
    if (!s || !key) {
        return TC_CRYPTO_FAIL;
    }

    mbedtls_aes_init(&s->ctx);
    if (mbedtls_aes_setkey_enc(&s->ctx, key, 128) != 0) {
        mbedtls_aes_free(&s->ctx);
        return TC_CRYPTO_FAIL;
    }

    return TC_CRYPTO_SUCCESS;
}

static inline int tc_aes_encrypt(uint8_t *out, const uint8_t *in, const struct tc_aes_key_sched_struct *s)
{
    if (!out || !in || !s) {
        return TC_CRYPTO_FAIL;
    }

    if (mbedtls_aes_crypt_ecb((mbedtls_aes_context *)&s->ctx, MBEDTLS_AES_ENCRYPT, in, out) != 0) {
        return TC_CRYPTO_FAIL;
    }

    return TC_CRYPTO_SUCCESS;
}

#endif
