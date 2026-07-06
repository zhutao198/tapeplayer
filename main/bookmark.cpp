#include "bookmark.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bookmark";
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

    // 找第一个空位
    int slot = -1;
    for (int i = 0; i < BOOKMARK_MAX_PER_FILE; i++) {
        char key[24];
        make_key(file_idx, i, key, sizeof(key));
        int32_t val = 0;
        esp_err_t err = nvs_get_i32(g_bm_handle, key, &val);
        if (err != ESP_OK) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "Bookmark full for file %d", file_idx);
        return -1;
    }
    char key[24];
    make_key(file_idx, slot, key, sizeof(key));
    esp_err_t err = nvs_set_i32(g_bm_handle, key, (int32_t)position_s);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s) failed: 0x%x", key, err);
        return -1;
    }
    nvs_commit(g_bm_handle);
    ESP_LOGI(TAG, "Bookmark added: file=%d slot=%d pos=%ds", file_idx, slot, position_s);
    return slot;
}

bool bookmark_delete(int file_idx, int bm_idx)
{
    if (!g_bm_handle || file_idx < 0 || bm_idx < 0) return false;
    char key[24];
    make_key(file_idx, bm_idx, key, sizeof(key));
    esp_err_t err = nvs_erase_key(g_bm_handle, key);
    if (err == ESP_OK) {
        nvs_commit(g_bm_handle);
        ESP_LOGI(TAG, "Bookmark deleted: file=%d slot=%d", file_idx, bm_idx);
        return true;
    }
    return false;
}

int bookmark_list(int file_idx, bookmark_t *out, int max_count)
{
    if (!g_bm_handle || !out || max_count <= 0) return 0;

    int count = 0;
    for (int i = 0; i < BOOKMARK_MAX_PER_FILE && count < max_count; i++) {
        char key[24];
        make_key(file_idx, i, key, sizeof(key));
        int32_t val = 0;
        esp_err_t err = nvs_get_i32(g_bm_handle, key, &val);
        if (err == ESP_OK) {
            out[count].position_s = (int)val;
            snprintf(out[count].label, sizeof(out[count].label), "BM%d", i + 1);
            count++;
        }
    }
    return count;
}

int bookmark_jump(int file_idx, int bm_idx)
{
    if (!g_bm_handle || file_idx < 0 || bm_idx < 0) return -1;
    char key[24];
    make_key(file_idx, bm_idx, key, sizeof(key));
    int32_t val = 0;
    esp_err_t err = nvs_get_i32(g_bm_handle, key, &val);
    if (err != ESP_OK) return -1;
    return (int)val;
}
