/*
 * Copyright 2018-2019 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright (c) 2018-2019, Oracle and/or its affiliates.  All rights reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/err.h>
#include <openssl/bn.h>
#include "crypto/bn.h"
#include "rsa_local.h"

#define RSA_FIPS1864_MIN_KEYGEN_KEYSIZE 2048
#define RSA_FIPS1864_MIN_KEYGEN_STRENGTH 112
#define RSA_FIPS1864_MAX_KEYGEN_STRENGTH 256

/*
 * Generate probable primes 'p' & 'q'. See FIPS 186-4 Section B.3.6
 * "Generation of Probable Primes with Conditions Based on Auxiliary Probable
 * Primes".
 *
 * Params:
 *     rsa  Object used to store primes p & q.
 *     p1, p2 The returned auxiliary primes for p. If NULL they are not returned.
 *     Xpout An optionally returned random number used during generation of p.
 *     Xp An optional passed in value (that is random number used during
 *        generation of p).
 *     Xp1, Xp2 Optionally passed in randomly generated numbers from which
 *              auxiliary primes p1 & p2 are calculated. If NULL these values
 *              are generated internally.
 *     q1, q2 The returned auxiliary primes for q. If NULL they are not returned.
 *     Xqout An optionally returned random number used during generation of q.
 *     Xq An optional passed in value (that is random number used during
 *        generation of q).
 *     Xq1, Xq2 Optionally passed in randomly generated numbers from which
 *              auxiliary primes q1 & q2 are calculated. If NULL these values
 *              are generated internally.
 *     nbits The key size in bits (The size of the modulus n).
 *     e The public exponent.
 *     ctx A BN_CTX object.
 *     cb An optional BIGNUM callback.
 * Returns: 1 if successful, or  0 otherwise.
 * Notes:
 *     p1, p2, q1, q2, Xpout, Xqout are returned if they are not NULL.
 *     Xp, Xp1, Xp2, Xq, Xq1, Xq2 are optionally passed in.
 *     (Required for CAVS testing).
 */
int rsa_fips186_4_gen_prob_primes(RSA *rsa, BIGNUM *p1, BIGNUM *p2,
                                  BIGNUM *Xpout, const BIGNUM *Xp,
                                  const BIGNUM *Xp1, const BIGNUM *Xp2,
                                  BIGNUM *q1, BIGNUM *q2, BIGNUM *Xqout,
                                  const BIGNUM *Xq, const BIGNUM *Xq1,
                                  const BIGNUM *Xq2, int nbits,
                                  const BIGNUM *e, BN_CTX *ctx, BN_GENCB *cb)
{
    int ret = 0, ok;
    BIGNUM *Xpo = NULL, *Xqo = NULL, *tmp = NULL;

    /* (Step 1) Check key length
     * NOTE: SP800-131A Rev1 Disallows key lengths of < 2048 bits for RSA
     * Signature Generation and Key Agree/Transport.
     */
    if (nbits < RSA_FIPS1864_MIN_KEYGEN_KEYSIZE) {
        RSAerr(RSA_F_RSA_FIPS186_4_GEN_PROB_PRIMES, RSA_R_INVALID_KEY_LENGTH);
        return 0;
    }

    if (!rsa_check_public_exponent(e)) {
        RSAerr(RSA_F_RSA_FIPS186_4_GEN_PROB_PRIMES,
               RSA_R_PUB_EXPONENT_OUT_OF_RANGE);
        return 0;
    }

    /* (Step 3) Determine strength and check rand generator strength is ok -
     * this step is redundant because the generator always returns a higher
     * strength than is required.
     */

    BN_CTX_start(ctx);
    tmp = BN_CTX_get(ctx);
    Xpo = (Xpout != NULL) ? Xpout : BN_CTX_get(ctx);
    Xqo = (Xqout != NULL) ? Xqout : BN_CTX_get(ctx);
    if (tmp == NULL || Xpo == NULL || Xqo == NULL)
        goto err;

    if (rsa->p == NULL)
        rsa->p = BN_secure_new();
    if (rsa->q == NULL)
        rsa->q = BN_secure_new();
    if (rsa->p == NULL || rsa->q == NULL)
        goto err;

    /* (Step 4) Generate p, Xp */
    if (!bn_rsa_fips186_4_gen_prob_primes(rsa->p, Xpo, p1, p2, Xp, Xp1, Xp2,
                                          nbits, e, ctx, cb))
        goto err;
    for(;;) {
        /* (Step 5) Generate q, Xq*/
        if (!bn_rsa_fips186_4_gen_prob_primes(rsa->q, Xqo, q1, q2, Xq, Xq1,
                                              Xq2, nbits, e, ctx, cb))
            goto err;

        /* (Step 6) |Xp - Xq| > 2^(nbitlen/2 - 100) */
        ok = rsa_check_pminusq_diff(tmp, Xpo, Xqo, nbits);
        if (ok < 0)
            goto err;
        if (ok == 0)
            continue;

        /* (Step 6) |p - q| > 2^(nbitlen/2 - 100) */
        ok = rsa_check_pminusq_diff(tmp, rsa->p, rsa->q, nbits);
        if (ok < 0)
            goto err;
        if (ok == 0)
            continue;
        break; /* successfully finished */
    }
    rsa->dirty_cnt++;
    ret = 1;
err:
    /* Zeroize any internally generated values that are not returned */
    if (Xpo != Xpout)
        BN_clear(Xpo);
    if (Xqo != Xqout)
        BN_clear(Xqo);
    BN_clear(tmp);

    BN_CTX_end(ctx);
    return ret;
}

/*
 * Validates the RSA key size based on the target strength.
 * See SP800-56Br1 6.3.1.1 (Steps 1a-1b)
 *
 * Params:
 *     nbits The key size in bits.
 *     strength The target strength in bits. -1 means the target
 *              strength is unknown.
 * Returns: 1 if the key size matches the target strength, or 0 otherwise.
 */
int rsa_sp800_56b_validate_strength(int nbits, int strength)
{
    int s = (int)rsa_compute_security_bits(nbits);

    if (s < RSA_FIPS1864_MIN_KEYGEN_STRENGTH
            || s > RSA_FIPS1864_MAX_KEYGEN_STRENGTH) {
        RSAerr(RSA_F_RSA_SP800_56B_VALIDATE_STRENGTH, RSA_R_INVALID_MODULUS);
        return 0;
    }
    if (strength != -1 && s != strength) {
        RSAerr(RSA_F_RSA_SP800_56B_VALIDATE_STRENGTH, RSA_R_INVALID_STRENGTH);
        return 0;
    }
    return 1;
}

/*
 *
 * Using p & q, calculate other required parameters such as n, d.
 * as well as the CRT parameters dP, dQ, qInv.
 *
 * See SP800-56Br1
 *   6.3.1.1 rsakpg1 - basic (Steps 3-4)
 *   6.3.1.3 rsakpg1 - crt   (Step 5)
 *
 * Params:
 *     rsa An rsa object.
 *     nbits The key size.
 *     e The public exponent.
 *     ctx A BN_CTX object.
 * Notes:
 *   There is a small chance that the generated d will be too small.
 * Returns: -1 = error,
 *           0 = d is too small,
 *           1 = success.
 */
int rsa_sp800_56b_derive_params_from_pq(RSA *rsa, int nbits,
                                        const BIGNUM *e, BN_CTX *ctx)
{
    int ret = -1;
    BIGNUM *p1, *q1, *lcm, *p1q1, *gcd;

    BN_CTX_start(ctx);
    p1 = BN_CTX_get(ctx);
    q1 = BN_CTX_get(ctx);
    lcm = BN_CTX_get(ctx);
    p1q1 = BN_CTX_get(ctx);
    gcd = BN_CTX_get(ctx);
    if (gcd == NULL)
        goto err;

    /* LCM((p-1, q-1)) */
    if (rsa_get_lcm(ctx, rsa->p, rsa->q, lcm, gcd, p1, q1, p1q1) != 1)
        goto err;

    /* copy e */
    BN_free(rsa->e);
    rsa->e = BN_dup(e);
    if (rsa->e == NULL)
        goto err;

    BN_clear_free(rsa->d);
    /* (Step 3) d = (e^-1) mod (LCM(p-1, q-1)) */
    rsa->d = BN_secure_new();
    if (rsa->d == NULL || BN_mod_inverse(rsa->d, e, lcm, ctx) == NULL)
        goto err;

    /* (Step 3) return an error if d is too small */
    if (BN_num_bits(rsa->d) <= (nbits >> 1)) {
        ret = 0;
        goto err;
    }

    /* (Step 4) n = pq */
    if (rsa->n == NULL)
        rsa->n = BN_new();
    if (rsa->n == NULL || !BN_mul(rsa->n, rsa->p, rsa->q, ctx))
        goto err;

    /* (Step 5a) dP = d mod (p-1) */
    if (rsa->dmp1 == NULL)
        rsa->dmp1 = BN_new();
    if (rsa->dmp1 == NULL || !BN_mod(rsa->dmp1, rsa->d, p1, ctx))
        goto err;

    /* (Step 5b) dQ = d mod (q-1) */
    if (rsa->dmq1 == NULL)
        rsa->dmq1 = BN_secure_new();
    if (rsa->dmq1 == NULL || !BN_mod(rsa->dmq1, rsa->d, q1, ctx))
        goto err;

    /* (Step 5c) qInv = (inverse of q) mod p */
    BN_free(rsa->iqmp);
    rsa->iqmp = BN_secure_new();
    if (rsa->iqmp == NULL
            || BN_mod_inverse(rsa->iqmp, rsa->q, rsa->p, ctx) == NULL)
        goto err;

    rsa->dirty_cnt++;
    ret = 1;
err:
    if (ret != 1) {
        BN_free(rsa->e);
        rsa->e = NULL;
        BN_free(rsa->d);
        rsa->d = NULL;
        BN_free(rsa->n);
        rsa->n = NULL;
        BN_free(rsa->iqmp);
        rsa->iqmp = NULL;
        BN_free(rsa->dmq1);
        rsa->dmq1 = NULL;
        BN_free(rsa->dmp1);
        rsa->dmp1 = NULL;
    }
    BN_clear(p1);
    BN_clear(q1);
    BN_clear(lcm);
    BN_clear(p1q1);
    BN_clear(gcd);

    BN_CTX_end(ctx);
    return ret;
}

/*
 * Generate a SP800-56B RSA key.
 *
 * See SP800-56Br1 6.3.1 "RSA Key-Pair Generation with a Fixed Public Exponent"
 *    6.3.1.1 rsakpg1 - basic
 *    6.3.1.3 rsakpg1 - crt
 *
 * See also FIPS 186-4 Section B.3.6
 * "Generation of Probable Primes with Conditions Based on Auxiliary
 * Probable Primes."
 *
 * Params:
 *     rsa The rsa object.
 *     nbits The intended key size in bits.
 *     efixed The public exponent. If NULL a default of 65537 is used.
 *     cb An optional BIGNUM callback.
 * Returns: 1 if successfully generated otherwise it returns 0.
 */
int rsa_sp800_56b_generate_key(RSA *rsa, int nbits, const BIGNUM *efixed,
                               BN_GENCB *cb)
{
    int ret = 0;
    int ok;
    BN_CTX *ctx = NULL;
    BIGNUM *e = NULL;

    /* (Steps 1a-1b) : Currently ignores the strength check */
    if (!rsa_sp800_56b_validate_strength(nbits, -1))
        return 0;

    ctx = BN_CTX_new_ex(rsa->libctx);
    if (ctx == NULL)
        return 0;

    /* Set default if e is not passed in */
    if (efixed == NULL) {
        e = BN_new();
        if (e == NULL || !BN_set_word(e, 65537))
            goto err;
    } else {
        e = (BIGNUM *)efixed;
    }
    /* (Step 1c) fixed exponent is checked later . */

    for (;;) {
        /* (Step 2) Generate prime factors */
        if (!rsa_fips186_4_gen_prob_primes(rsa, NULL, NULL, NULL, NULL, NULL,
                                           NULL, NULL, NULL, NULL, NULL, NULL,
                                           NULL, nbits, e, ctx, cb))
            goto err;
        /* (Steps 3-5) Compute params d, n, dP, dQ, qInv */
        ok = rsa_sp800_56b_derive_params_from_pq(rsa, nbits, e, ctx);
        if (ok < 0)
            goto err;
        if (ok > 0)
            break;
        /* Gets here if computed d is too small - so try again */
    }

    /* (Step 6) Do pairwise test - optional validity test has been omitted */
    ret = rsa_sp800_56b_pairwise_test(rsa, ctx);
err:
    if (efixed == NULL)
        BN_free(e);
    BN_CTX_free(ctx);
    return ret;
}

/*
 * See SP800-56Br1 6.3.1.3 (Step 6) Perform a pair-wise consistency test by
 * verifying that: k = (k^e)^d mod n for some integer k where 1 < k < n-1.
 *
 * Returns 1 if the RSA key passes the pairwise test or 0 it it fails.
 */
int rsa_sp800_56b_pairwise_test(RSA *rsa, BN_CTX *ctx)
{
    int ret = 0;
    BIGNUM *k, *tmp;

    BN_CTX_start(ctx);
    tmp = BN_CTX_get(ctx);
    k = BN_CTX_get(ctx);
    if (k == NULL)
        goto err;

    ret = (BN_set_word(k, 2)
          && BN_mod_exp(tmp, k, rsa->e, rsa->n, ctx)
          && BN_mod_exp(tmp, tmp, rsa->d, rsa->n, ctx)
          && BN_cmp(k, tmp) == 0);
    if (ret == 0)
        RSAerr(RSA_F_RSA_SP800_56B_PAIRWISE_TEST, RSA_R_PAIRWISE_TEST_FAILURE);
err:
    BN_CTX_end(ctx);
    return ret;
}
