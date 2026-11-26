/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 保存的多个 Wi-Fi：列表加密放在 TF 卡 treader/wifi/known.bin，钥匙是 NVS（命名空间 rp_wifi）里的随机数；
 * NVS 里原来那一份凭据是“当前网络”（最后一次连上的保存网络），本次开机临时用的网络只在内存。番茄联网连不上当前网络时，
 * 这里扫描并给出保存过、在范围内的别的网络，连上的写回 NVS。
 * Several saved Wi-Fi networks: the list sits encrypted on the TF card at treader/wifi/known.bin under a random key in
 * NVS (namespace rp_wifi); the original NVS credentials are the "current network" (the saved network last joined) and a
 * network used for this boot only stays in memory. When Tomato networking cannot join the current network, this module
 * scans and offers other saved networks in range, and the one that joins is written back to NVS.
 *
 * 冻结：卡上只存加密文件，密码不进日志、不进界面；没卡、卡交给 USB 或文件读不开时只剩 NVS 里那一个，不报错。
 * 恢复出厂或换设备后卡上列表作废，下次保存时覆盖。
 * Frozen: the card holds only the encrypted file and passwords never reach logs or the UI; without a card, with the
 * card handed to USB or an unreadable file only the NVS network remains, silently. After a factory reset or on another
 * device the card list is void and the next save overwrites it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define TR_WIFI_PATH "/sdcard/treader/wifi/known.bin"
#define TR_WIFI_LIST_MAX 17   ///< 卡上 16 个加 NVS 里不在卡上的那个 / Sixteen on the card plus an NVS one missing there

/// 列表里的一个网络，不含密码。/ One listed network, without its password.
typedef struct {
    char ssid[33];
    uint32_t last_ok;  ///< 上次连上的 Unix 时间，0 表示不知道 / Unix time of the last join, 0 when unknown
    bool current;      ///< 当前要连的网络 / The network currently used
    bool on_card;      ///< 卡上有它 / Stored on the card
    bool saved;        ///< 保存过（卡上或 NVS 里），否则是本次开机的临时网络 / Saved (card or NVS), else this boot's temporary network
} tr_wifi_item_t;

/// 开机调用一次：注册番茄联网的多网络回调，记下当前网络名。/ Call once at boot: register the multi-network hooks for
/// Tomato networking and remember the current network's name.
void tr_wifi_init(void);
/// 当前网络名（状态栏用，只读内存），没有为空串。/ Current network name for the status bar (memory only); empty when none.
const char* tr_wifi_current_name(void);
/// 重新读当前网络名。/ Re-read the current network's name.
void tr_wifi_refresh_name(void);
/// 卡能不能存多个网络（已挂载）。/ Whether the card can hold several networks (mounted).
bool tr_wifi_card_ok(void);
/// 列出保存的网络：当前的在最前，其余按上次连上从新到旧。返回个数。/ List saved networks: the current one first, the
/// rest newest join first. Returns the count.
int tr_wifi_list(tr_wifi_item_t* out, int max);
/// 取一个保存网络的密码（卡上或 NVS 里）；调用方用毕擦除。/ Fetch a saved network's password (card or NVS); the caller
/// wipes it after use.
bool tr_wifi_take(const char* ssid, char pass[65]);
/// 记成当前网络并保存：写 NVS 和卡，清掉临时网络。/ Make it current and save it: NVS and the card, clearing any
/// temporary network.
esp_err_t tr_wifi_commit(const char* ssid, const char* pass);
/// 只在本次开机用这个网络，不写 NVS 和卡。/ Use this network for this boot only, writing neither NVS nor the card.
esp_err_t tr_wifi_use_once(const char* ssid, const char* pass);
/// 忘记一个网络：从卡上删掉；是 NVS 里的就一起删，是临时网络就清掉。/ Forget a network: remove it from the card, and from
/// NVS or the temporary slot when it is there.
esp_err_t tr_wifi_forget(const char* ssid);
/// 传书用当前网络连上后调用：卡上没有或密码变了就补上，顺便记下时间。/ Call after transfer joins with the current
/// network: add or fix it on the card and record the time.
void tr_wifi_joined_current(void);
