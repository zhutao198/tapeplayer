#include "bookmark.h"
#include "config.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "bookmark";

/*
 * NVS handle 说明：
 *
 * bookmark 和 settings 各持有独立的 nvs_handle（g_bm_handle / g_nvs_handle），
 * 虽共享同一 namespace "tapebook"，但 ESP-IDF 的 nvs_commit(handle) 按 handle 隔离提交：
 *   - settings_flush() 只提交 g_nvs_handle 的待写缓存
 *   - g_bm_handle 的写入（nvs_set / nvs_erase）不会因此落盘
 *
 * 因此 bookmark_add 在成功路径上必须调用
 * nvs_commit(g_bm_handle)，否则书签写入仅在 RAM 缓存中生效，
 * 重启/断电后全部丢失。
 *
 * 提交策略说明（R032-202）：bookmark 采用即时提交（低频且需耐久），
 * 与 settings 的"批量写入 + 显式 flush"各有取舍，属有意设计。
 */
static nvs_handle_t g_bm_handle = 0;

void bookmark_init(void)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_bm_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for bookmarks (0x%x)", err);
        g_bm_handle = 0;
    } else {
        ESP_LOGI(TAG, "Bookmark module ready (namespace: %s)", NVS_NAMESPACE);
    }
}

static void make_key(int file_idx, int bm_idx, char *key, size_t size)
{
    snprintf(key, size, "bm_%d_%d", file_idx, bm_idx);
}

int bookmark_add(int file_idx, int position_s)
{
    if (!g_bm_handle || file_idx < 0 || position_s < 0) return -1;

    // 找第一个空位；满则覆盖最旧（slot 0），其余向前移位
    int slot = -1;
    for (int i = 0; i < BOOKMARK_MAX_PER_FILE; i++) {
        char key[24];
        make_key(file_idx, i, key, sizeof(key));
        int32_t val = 0;
        esp_err_t err = nvs_get_i32(g_bm_handle, key, &val);
        if (err == ESP_OK) {
            // 已占用，继续向下找空位
        } else if (slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) {
        // 满：擦除最旧（slot 0），向前移位，新书签放末尾
        esp_err_t err = ESP_OK;
        {
            char key0[24];
            make_key(file_idx, 0, key0, sizeof(key0));
            err = nvs_erase_key(g_bm_handle, key0);
        }
        if (err == ESP_OK) {
            for (int i = 1; i < BOOKMARK_MAX_PER_FILE; i++) {
                char key_old[24], key_new[24];
                make_key(file_idx, i, key_old, sizeof(key_old));
                make_key(file_idx, i - 1, key_new, sizeof(key_new));
                int32_t val = 0;
                esp_err_t r = nvs_get_i32(g_bm_handle, key_old, &val);
                if (r == ESP_OK) {
                    r = nvs_set_i32(g_bm_handle, key_new, val);
                } else {
                    r = nvs_erase_key(g_bm_handle, key_new);
                }
                if (r != ESP_OK) err = r;
            }
        }
        if (err != ESP_OK) return -1;
        slot = BOOKMARK_MAX_PER_FILE - 1;
        char key[24];
        make_key(file_idx, slot, key, sizeof(key));
        err = nvs_set_i32(g_bm_handle, key, (int32_t)position_s);
        if (err != ESP_OK) return -1;
        if (nvs_commit(g_bm_handle) != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed after circular overwrite");
        }
        ESP_LOGI(TAG, "Bookmark circular overwrite: file=%d slot=%d pos=%ds", file_idx, slot, position_s);
        return slot;
    }

    char key[24];
    make_key(file_idx, slot, key, sizeof(key));
    esp_err_t err = nvs_set_i32(g_bm_handle, key, (int32_t)position_s);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s) failed: 0x%x", key, err);
        return -1;
    }
    if (nvs_commit(g_bm_handle) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed after bookmark add");
    }
    ESP_LOGI(TAG, "Bookmark added: file=%d slot=%d pos=%ds", file_idx, slot, position_s);
    return slot;
}


