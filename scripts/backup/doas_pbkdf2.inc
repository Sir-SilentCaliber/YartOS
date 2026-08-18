#define DOAS_PBKDF2_ITERS 10000

static void hmac_sha256_k(const u8 *key, size_t keylen,
                          const u8 *msg, size_t mlen, u8 out[32]) {
    u8 k[64];
    memset(k, 0, sizeof k);
    if (keylen > 64) {
        sha256_ctx_t c; sha256_init(&c); sha256_update(&c, key, keylen); sha256_final(&c, k);
    } else {
        memcpy(k, key, keylen);
    }
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    sha256_ctx_t c; u8 inner[32];
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

static void pbkdf2_hmac_sha256(const u8 *pw, size_t pwlen,
                               const u8 *salt, size_t slen,
                               u32 iters, u8 *out, size_t outlen) {
    u32 block = 1;
    while (outlen) {
        u8 msg[80]; size_t m = 0;
        if (slen > 64) slen = 64;
        memcpy(msg, salt, slen); m = slen;
        msg[m++] = (u8)(block >> 24); msg[m++] = (u8)(block >> 16);
        msg[m++] = (u8)(block >> 8);  msg[m++] = (u8)block;
        u8 u[32], t[32];
        hmac_sha256_k(pw, pwlen, msg, m, u);
        memcpy(t, u, 32);
        for (u32 i = 1; i < iters; i++) {
            hmac_sha256_k(pw, pwlen, u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t take = outlen > 32 ? 32 : outlen;
        memcpy(out, t, take);
        out += take; outlen -= take; block++;
    }
}

/* Shared password check: PBKDF2-HMAC-SHA256, constant-time compare, failure
 * lockout.  Used by SYS_DOAS (elevate) and SYS_AUTH_VERIFY (verify WITHOUT
 * elevating, e.g. the lock screen). */
static bool doas_check(task_t *t, const char *password) {
    doas_user_t *u = NULL;
    for (int i = 0; i < g_doas_nusers; i++)
        if (strcmp(g_doas_users[i].account, t->account) == 0) {
            u = &g_doas_users[i];
            break;
        }
    if (!u) {
        kprintf("doas: no account '%s' for task %d\n", t->account, t->pid);
        return false;
    }
    if (u->fails >= DOAS_MAX_FAILS &&
        pit_ticks() - u->fail_t0 < DOAS_LOCKOUT_TICKS) {
        kprintf("doas: account '%s' temporarily locked (too many failures)\n",
                u->account);
        return false;
    }
    if (u->fails >= DOAS_MAX_FAILS) u->fails = 0;   /* lockout expired */
    u8 h[32];
    pbkdf2_hmac_sha256((const u8 *)password, strlen(password),
                       (const u8 *)u->salt, strlen(u->salt),
                       DOAS_PBKDF2_ITERS, h, 32);
    if (ct_neq(h, u->hash, 32)) {
        u->fails++;
        u->fail_t0 = pit_ticks();
        kprintf("doas: auth FAILED for '%s' (%u/%u)\n",
                u->account, u->fails, DOAS_MAX_FAILS);
        return false;
    }
    u->fails = 0;
    return true;
}
