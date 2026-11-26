/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：保存的 Wi-Fi 列表主机测试：增删、上限、排序、加密文件的往返、钥匙不对、篡改、截断和 .tmp/.bak 回退。
 *
 * English: host test for the saved Wi-Fi list: add/remove, the limit, sorting, the encrypted file round trip, a wrong
 * key, tampering, truncation and the .tmp/.bak fallback.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "read_pico_wifi_vault.h"

static const char* PATH = "/tmp/wifi_vault_test.bin";

static void clean(void) {
    unlink(PATH);
    unlink("/tmp/wifi_vault_test.bin.tmp");
    unlink("/tmp/wifi_vault_test.bin.bak");
}

static long file_size(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fclose(f);
    return n;
}

static void test_valid(void) {
    assert(wifi_vault_valid("Home", ""));
    assert(wifi_vault_valid("Home", "12345678"));
    assert(!wifi_vault_valid("", "12345678"));
    assert(!wifi_vault_valid("Home", "1234567"));
    assert(!wifi_vault_valid("123456789012345678901234567890123", ""));
    assert(!wifi_vault_valid("Bad\n", ""));
    char hex[65];
    memset(hex, 'a', 64);
    hex[64] = 0;
    assert(wifi_vault_valid("Home", hex));
    hex[3] = 'z';
    assert(!wifi_vault_valid("Home", hex));
}

static void test_list(void) {
    wifi_vault_t v = {0};
    assert(wifi_vault_put(&v, "A", "password-a", 10));
    assert(wifi_vault_put(&v, "B", "password-b", 30));
    assert(wifi_vault_put(&v, "C", "", 20));
    assert(v.count == 3);
    // 同名更新密码；时间给 0 时保留原来的。/ The same name updates the password; a zero time keeps the old one.
    assert(wifi_vault_put(&v, "A", "password-new", 0));
    assert(v.count == 3 && !strcmp(v.items[0].password, "password-new") && v.items[0].last_ok == 10);
    assert(!wifi_vault_put(&v, "D", "short", 1));
    wifi_vault_sort(&v);
    assert(!strcmp(v.items[0].ssid, "B") && !strcmp(v.items[1].ssid, "C") && !strcmp(v.items[2].ssid, "A"));
    assert(wifi_vault_remove(&v, "C") && v.count == 2 && wifi_vault_find(&v, "C") < 0);
    assert(!wifi_vault_remove(&v, "C"));
    // 满了挤掉最旧的。/ When full the oldest goes.
    memset(&v, 0, sizeof(v));
    for (int i = 0; i < WIFI_VAULT_MAX; ++i) {
        char name[8];
        snprintf(name, sizeof(name), "N%d", i);
        assert(wifi_vault_put(&v, name, "", (uint32_t)(100 + i)));
    }
    v.items[5].last_ok = 1;
    assert(wifi_vault_put(&v, "New", "", 500));
    assert(v.count == WIFI_VAULT_MAX && wifi_vault_find(&v, "N5") < 0 && wifi_vault_find(&v, "New") >= 0);
}

static void test_file(void) {
    uint8_t dev[WIFI_VAULT_KEY_LEN], key[WIFI_VAULT_KEY_LEN], other[WIFI_VAULT_KEY_LEN];
    assert(wifi_vault_random(dev, sizeof(dev)));
    assert(wifi_vault_derive(dev, key) == WIFI_VAULT_OK);
    dev[0] ^= 1;
    assert(wifi_vault_derive(dev, other) == WIFI_VAULT_OK);
    assert(memcmp(key, other, sizeof(key)));
    clean();
    wifi_vault_t v = {0}, got;
    assert(wifi_vault_load(PATH, key, &got) == WIFI_VAULT_ERR_NOENT && got.count == 0);
    // 空列表也能存。/ An empty list saves too.
    assert(wifi_vault_save(PATH, key, &v) == WIFI_VAULT_OK);
    assert(wifi_vault_load(PATH, key, &got) == WIFI_VAULT_OK && got.count == 0);
    assert(wifi_vault_put(&v, "Home", "secret-home", 1700000000));
    assert(wifi_vault_put(&v, "中文网络", "", 1700000100));
    assert(wifi_vault_save(PATH, key, &v) == WIFI_VAULT_OK);
    assert(file_size(PATH) == 18 + 2 * 102 + 16);
    // 存下后不留 .tmp 和 .bak。/ No .tmp or .bak is left after a save.
    assert(file_size("/tmp/wifi_vault_test.bin.tmp") < 0 && file_size("/tmp/wifi_vault_test.bin.bak") < 0);
    assert(wifi_vault_load(PATH, key, &got) == WIFI_VAULT_OK);
    assert(got.count == 2 && !strcmp(got.items[0].password, "secret-home") && got.items[1].last_ok == 1700000100);
    assert(!strcmp(got.items[1].ssid, "中文网络"));
    // 明文里找不到密码。/ The password is not in the plain bytes.
    FILE* f = fopen(PATH, "rb");
    uint8_t raw[512];
    const size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    assert(!memmem(raw, n, "secret-home", 11) && !memmem(raw, n, "Home", 4));
    assert(wifi_vault_load(PATH, other, &got) == WIFI_VAULT_ERR_AUTH && got.count == 0);
    // 改一个字节：密文和文件头都会被发现。/ One changed byte, in the ciphertext or the header, is caught.
    for (size_t at = 0; at < n; at += 37) {
        uint8_t bad[512];
        memcpy(bad, raw, n);
        bad[at] ^= 0x40;
        f = fopen(PATH, "wb");
        fwrite(bad, 1, n, f);
        fclose(f);
        assert(wifi_vault_load(PATH, key, &got) != WIFI_VAULT_OK && got.count == 0);
    }
    // 截断。/ Truncated.
    f = fopen(PATH, "wb");
    fwrite(raw, 1, n - 1, f);
    fclose(f);
    assert(wifi_vault_load(PATH, key, &got) == WIFI_VAULT_ERR_FORMAT);
    // 正式文件坏了，.bak 还在：读 .bak。/ The current file is broken and .bak survives: read .bak.
    f = fopen("/tmp/wifi_vault_test.bin.bak", "wb");
    fwrite(raw, 1, n, f);
    fclose(f);
    assert(wifi_vault_load(PATH, key, &got) == WIFI_VAULT_OK && got.count == 2);
    // 改名到一半断电：只剩 .tmp。/ Power lost mid-rename: only .tmp is left.
    clean();
    f = fopen("/tmp/wifi_vault_test.bin.tmp", "wb");
    fwrite(raw, 1, n, f);
    fclose(f);
    assert(wifi_vault_load(PATH, key, &got) == WIFI_VAULT_OK && got.count == 2);
    // 再存一次会清掉旧文件。/ Saving again clears the leftovers.
    assert(wifi_vault_remove(&v, "Home"));
    assert(wifi_vault_save(PATH, key, &v) == WIFI_VAULT_OK);
    assert(wifi_vault_load(PATH, key, &got) == WIFI_VAULT_OK && got.count == 1);
    assert(file_size("/tmp/wifi_vault_test.bin.bak") < 0);
    // 存满 16 个。/ A full list of 16.
    memset(&v, 0, sizeof(v));
    for (int i = 0; i < WIFI_VAULT_MAX; ++i) {
        char name[33], pass[65];
        snprintf(name, sizeof(name), "network-%02d-abcdefghijklmnopqrs", i);
        snprintf(pass, sizeof(pass), "password-%02d", i);
        assert(wifi_vault_put(&v, name, pass, (uint32_t)i));
    }
    assert(wifi_vault_save(PATH, key, &v) == WIFI_VAULT_OK);
    assert(wifi_vault_load(PATH, key, &got) == WIFI_VAULT_OK && got.count == WIFI_VAULT_MAX);
    assert(!memcmp(&got, &v, sizeof(v)));
    clean();
}

int main(void) {
    test_valid();
    test_list();
    test_file();
    puts("wifi vault host tests passed");
    return 0;
}
