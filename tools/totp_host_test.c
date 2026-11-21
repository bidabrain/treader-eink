// SPDX-FileCopyrightText: 2026 Treader contributors
// SPDX-License-Identifier: Apache-2.0
// 验证器组件主机测试：RFC 6238 附录 B 向量、base32、账号表、加密文件读写与各种损坏。运行：tools/run_totp_host_test.sh
// Host test for the authenticator component: RFC 6238 appendix B vectors, base32, the table, and the encrypted file
// with its failure cases. Run: tools/run_totp_host_test.sh

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "read_pico_totp.h"

static int s_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            s_fail++;                                                    \
        }                                                                \
    } while (0)

static totp_account_t make_acct(const char* secret, totp_algo_t algo, int digits) {
    totp_account_t a = {0};
    snprintf(a.name, sizeof(a.name), "test");
    a.secret_len = (uint8_t)strlen(secret);
    memcpy(a.secret, secret, a.secret_len);
    a.algo = (uint8_t)algo;
    a.digits = (uint8_t)digits;
    a.period = 30;
    return a;
}

static void test_rfc6238(void) {
    static const struct { int64_t t; uint32_t sha1, sha256, sha512; } V[] = {
        {59, 94287082, 46119246, 90693936},
        {1111111109, 7081804, 68084774, 25091201},
        {1111111111, 14050471, 67062674, 99943326},
        {1234567890, 89005924, 91819424, 93441116},
        {2000000000, 69279037, 90698825, 38618901},
        {20000000000LL, 65353130, 77737706, 47863826},
    };
    totp_account_t a1 = make_acct("12345678901234567890", TOTP_SHA1, 8);
    totp_account_t a2 = make_acct("12345678901234567890123456789012", TOTP_SHA256, 8);
    totp_account_t a5 = make_acct("1234567890123456789012345678901234567890123456789012345678901234", TOTP_SHA512, 8);
    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
        uint32_t c = 0;
        CHECK(totp_code(&a1, V[i].t, &c) == TOTP_OK && c == V[i].sha1);
        CHECK(totp_code(&a2, V[i].t, &c) == TOTP_OK && c == V[i].sha256);
        CHECK(totp_code(&a5, V[i].t, &c) == TOTP_OK && c == V[i].sha512);
    }
    // 6 位取后 6 位。/ Six digits keep the last six.
    totp_account_t a6 = make_acct("12345678901234567890", TOTP_SHA1, 6);
    uint32_t c = 0;
    CHECK(totp_code(&a6, 59, &c) == TOTP_OK && c == 287082);
    char s[16];
    totp_format(7081804, 8, s, sizeof(s));
    CHECK(!strcmp(s, "0708 1804"));
    totp_format(81804, 6, s, sizeof(s));
    CHECK(!strcmp(s, "081 804"));
    CHECK(totp_remaining(&a6, 59) == 1);
    CHECK(totp_remaining(&a6, 60) == 30);
    a6.period = 60;
    CHECK(totp_remaining(&a6, 61) == 59);
    a6.digits = 7;
    CHECK(totp_code(&a6, 59, &c) == TOTP_ERR_ARG);
}

static void test_base32(void) {
    uint8_t out[64];
    size_t n = 0;
    CHECK(totp_base32_decode("JBSWY3DPEE", out, sizeof(out), &n) && n == 6 && !memcmp(out, "Hello!", 6));
    CHECK(totp_base32_decode("gezd gnbv-gy3t qojq gezd gnbv gy3t qojq", out, sizeof(out), &n) && n == 20 &&
          !memcmp(out, "12345678901234567890", 20));
    CHECK(totp_base32_decode("JBSWY3DPEE======", out, sizeof(out), &n) && n == 6);
    CHECK(!totp_base32_decode("JBSWY3DPE1", out, sizeof(out), &n));
    CHECK(!totp_base32_decode("", out, sizeof(out), &n));
    CHECK(!totp_base32_decode("JBSWY3DPEE", out, 3, &n));
}

static void test_table(void) {
    static totp_table_t t;
    memset(&t, 0, sizeof(t));
    totp_account_t a = make_acct("12345678901234567890", TOTP_SHA1, 6);
    bool dup = false;
    CHECK(totp_table_add(&t, &a, &dup) && t.count == 1 && t.items[0].id == 1);
    CHECK(!totp_table_add(&t, &a, &dup) && dup);
    snprintf(a.name, sizeof(a.name), "b");
    CHECK(totp_table_add(&t, &a, &dup));
    snprintf(a.name, sizeof(a.name), "c");
    CHECK(totp_table_add(&t, &a, &dup));
    CHECK(totp_table_find(&t, 3) == 2);
    totp_table_move(&t, 2, 0);
    CHECK(!strcmp(t.items[0].name, "c") && !strcmp(t.items[1].name, "test") && !strcmp(t.items[2].name, "b"));
    totp_table_move(&t, 0, 9);
    CHECK(!strcmp(t.items[2].name, "c"));
    totp_table_remove(&t, 0);
    CHECK(t.count == 2 && !strcmp(t.items[0].name, "b"));
    // 编号不复用。/ Ids are not reused.
    snprintf(a.name, sizeof(a.name), "d");
    CHECK(totp_table_add(&t, &a, &dup) && t.items[2].id == 4);
    a.name[0] = 0;
    CHECK(!totp_table_add(&t, &a, &dup));
    for (int i = t.count; i < TOTP_MAX_ACCOUNTS; i++) {
        snprintf(a.name, sizeof(a.name), "n%d", i);
        CHECK(totp_table_add(&t, &a, &dup));
    }
    snprintf(a.name, sizeof(a.name), "full");
    CHECK(!totp_table_add(&t, &a, &dup) && !dup);
}

static bool exists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void flip(const char* p, long off) {
    FILE* f = fopen(p, "r+b");
    if (!f) {
        printf("FAIL cannot open %s (%d)\n", p, errno);
        s_fail++;
        return;
    }
    fseek(f, off, SEEK_SET);
    int c = fgetc(f);
    fseek(f, off, SEEK_SET);
    fputc(c ^ 1, f);
    fclose(f);
}

static void test_vault(void) {
    const char* path = "/tmp/totp_vault/vault.bin";
    mkdir("/tmp/totp_vault", 0755);
    totp_vault_remove(path);
    CHECK(!totp_vault_exists(path));
    uint8_t dev[TOTP_DEV_SECRET_LEN], dev2[TOTP_DEV_SECRET_LEN];
    CHECK(totp_random(dev, sizeof(dev)));
    memcpy(dev2, dev, sizeof(dev2));
    dev2[0] ^= 1;
    static totp_table_t t, r;
    memset(&t, 0, sizeof(t));
    totp_account_t a = make_acct("12345678901234567890", TOTP_SHA256, 8);
    snprintf(a.issuer, sizeof(a.issuer), "示例");
    snprintf(a.name, sizeof(a.name), "me@example.com");
    bool dup;
    CHECK(totp_table_add(&t, &a, &dup));
    a.algo = TOTP_SHA1;
    a.digits = 6;
    a.period = 60;
    a.secret_len = 10;
    snprintf(a.name, sizeof(a.name), "second");
    CHECK(totp_table_add(&t, &a, &dup));

    totp_key_t k, k2;
    CHECK(totp_key_new("abc12345", dev, 1000, &k) == TOTP_OK && k.iters == TOTP_MIN_ITERS);
    // 空表也能存。/ An empty table saves too.
    static totp_table_t empty;
    CHECK(totp_vault_save(path, &k, &empty) == TOTP_OK && exists(path));
    CHECK(totp_vault_unlock(path, "abc12345", dev, &k2, &r) == TOTP_OK && r.count == 0);
    CHECK(totp_vault_save(path, &k, &t) == TOTP_OK && exists("/tmp/totp_vault/vault.bin.bak"));
    CHECK(!exists("/tmp/totp_vault/vault.bin.tmp"));

    CHECK(totp_vault_unlock(path, "abc12345", dev, &k2, &r) == TOTP_OK);
    CHECK(r.count == 2 && r.next_id == 3 && !strcmp(r.items[0].issuer, "示例") && r.items[1].period == 60 &&
          r.items[1].secret_len == 10 && !memcmp(&k, &k2, sizeof(k)));
    uint32_t c1, c2;
    CHECK(totp_code(&t.items[0], 1234567890, &c1) == TOTP_OK && totp_code(&r.items[0], 1234567890, &c2) == TOTP_OK &&
          c1 == c2);

    CHECK(totp_vault_unlock(path, "abc12346", dev, &k2, &r) == TOTP_ERR_AUTH);
    CHECK(totp_vault_unlock(path, "abc12345", dev2, &k2, &r) == TOTP_ERR_AUTH);
    CHECK(totp_vault_load(path, &k, &r) == TOTP_OK);

    // 改坏正式文件的密文：回退到 .bak（同一把钥匙，空表）。/ Corrupt the main ciphertext: falls back to .bak.
    flip(path, 50);
    CHECK(totp_vault_load(path, &k, &r) == TOTP_OK && r.count == 0);
    // 两份都坏：口令不对的错误。/ Both bad: auth error.
    flip("/tmp/totp_vault/vault.bin.bak", 46);
    CHECK(totp_vault_load(path, &k, &r) == TOTP_ERR_AUTH);
    // 重新存一次就好了。/ A fresh save fixes it.
    CHECK(totp_vault_save(path, &k, &t) == TOTP_OK && totp_vault_load(path, &k, &r) == TOTP_OK && r.count == 2);
    // 改文件头（附加数据）也解不开。/ Tampering with the header (AAD) fails too.
    totp_vault_drop_backup(path);
    flip(path, 6);
    CHECK(totp_vault_load(path, &k, &r) == TOTP_ERR_AUTH);
    flip(path, 6);
    // 截断：格式错。/ Truncated: format error.
    CHECK(truncate(path, 60) == 0);
    CHECK(totp_vault_load(path, &k, &r) == TOTP_ERR_FORMAT);
    CHECK(totp_vault_save(path, &k, &t) == TOTP_OK);

    // 写一半断电：.tmp 留着、正式文件没了，照样能读。/ Power cut mid-save: .tmp kept, main gone; still readable.
    CHECK(rename(path, "/tmp/totp_vault/vault.bin.tmp") == 0);
    CHECK(totp_vault_exists(path));
    CHECK(totp_vault_unlock(path, "abc12345", dev, &k2, &r) == TOTP_OK && r.count == 2);
    CHECK(totp_vault_save(path, &k, &t) == TOTP_OK);

    // 换口令：存新钥匙、删备份后，旧口令什么都解不开。/ Passphrase change: after saving with the new key and dropping
    // backups, the old passphrase opens nothing.
    totp_key_t kn;
    CHECK(totp_key_new("new98765", dev, TOTP_MIN_ITERS, &kn) == TOTP_OK);
    CHECK(totp_vault_save(path, &kn, &t) == TOTP_OK);
    totp_vault_drop_backup(path);
    CHECK(totp_vault_unlock(path, "abc12345", dev, &k2, &r) == TOTP_ERR_AUTH);
    CHECK(totp_vault_unlock(path, "new98765", dev, &k2, &r) == TOTP_OK && r.count == 2);

    totp_vault_remove(path);
    CHECK(!totp_vault_exists(path));
    CHECK(totp_vault_unlock(path, "new98765", dev, &k2, &r) == TOTP_ERR_NOENT);
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(void) {
    test_rfc6238();
    test_base32();
    test_table();
    test_vault();
    CHECK(totp_pbkdf2_time(1000, now_us) >= 0);
    if (s_fail) {
        printf("%d check(s) failed\n", s_fail);
        return 1;
    }
    printf("totp host test ok\n");
    return 0;
}
