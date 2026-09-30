/* host_crypto.c — the core crypto calls magnet_pkg.c needs, on OpenSSL, so
 * the firmware's verifier runs unchanged on a laptop (tools/pkgtest). */
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <string.h>
#include <stdlib.h>
#include "magnet_crypto.h"

int mn_verify(const uint8_t pub65[65], const uint8_t *msg, size_t len, const uint8_t sig64[64]) {
    int ok = 0;
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    OSSL_PARAM_BLD *b = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(b, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
    OSSL_PARAM_BLD_push_octet_string(b, OSSL_PKEY_PARAM_PUB_KEY, pub65, 65);
    OSSL_PARAM *p = OSSL_PARAM_BLD_to_param(b);
    EVP_PKEY *key = NULL;
    if (EVP_PKEY_fromdata_init(kctx) == 1 &&
        EVP_PKEY_fromdata(kctx, &key, EVP_PKEY_PUBLIC_KEY, p) == 1) {
        /* r||s -> DER */
        ECDSA_SIG *s = ECDSA_SIG_new();
        ECDSA_SIG_set0(s, BN_bin2bn(sig64, 32, NULL), BN_bin2bn(sig64 + 32, 32, NULL));
        unsigned char *der = NULL;
        int dlen = i2d_ECDSA_SIG(s, &der);
        EVP_MD_CTX *md = EVP_MD_CTX_new();
        ok = EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, key) == 1 &&
             EVP_DigestVerify(md, der, dlen, msg, len) == 1;
        EVP_MD_CTX_free(md);
        OPENSSL_free(der);
        ECDSA_SIG_free(s);
    }
    EVP_PKEY_free(key);
    OSSL_PARAM_free(p);
    OSSL_PARAM_BLD_free(b);
    EVP_PKEY_CTX_free(kctx);
    return ok ? 0 : -1;
}

int mn_sha256_start(mn_sha256_t *c) {
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    memcpy(c, &m, sizeof(m));
    return EVP_DigestInit_ex(m, EVP_sha256(), NULL) == 1 ? 0 : -1;
}
int mn_sha256_update(mn_sha256_t *c, const void *d, size_t n) {
    EVP_MD_CTX *m; memcpy(&m, c, sizeof(m));
    return EVP_DigestUpdate(m, d, n) == 1 ? 0 : -1;
}
int mn_sha256_finish(mn_sha256_t *c, uint8_t out[32]) {
    EVP_MD_CTX *m; memcpy(&m, c, sizeof(m));
    unsigned int n = 0;
    int ok = EVP_DigestFinal_ex(m, out, &n) == 1 && n == 32;
    EVP_MD_CTX_free(m);
    return ok ? 0 : -1;
}

/* TweetNaCl wants randombytes; verification never calls it. */
void randombytes(unsigned char *x, unsigned long long n) { (void)x; (void)n; abort(); }
