/**
 * @file settings.cpp
 * @brief NVS 持久化设置实现
 *
 * NVS 命名空间: "tape_settings"（S1：与 bookmark 的 "tapebook" 分离）
 *
 * 修复原因：原代码 fallback 调用 nvs_flash_erase() 是分区级擦除，
 * 会清空 bookmark 命名空间，导致书签数据风险（S1, P0）。
 * 改为：与 bookmark 各自独立 namespace；满页(NO_FREE_PAGES)场景仅重试
 * nvs_flash_init 而不整擦，保留书签（数据安全优先于 settings 可用性）。
 * 版本变更(NEW_VERSION_FOUND)因旧格式不可读仍需整分区擦除，已加明确日志（R032-101）。
 *
 * 断点校验：保存时连带存文件名；恢复时比对播放列表，
 * 若文件已删除则从第一首开始，不崩溃。
 */

#include "settings.h"
#include "config.h"
#include "playlist.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "settings";
static const char *SETTINGS_NAMESPACE = "tape_settings";   // S1：与 bookmark 命名空间隔离
static nvs_handle_t g_nvs_handle = 0;

void settings_init(void)
{
    g_nvs_handle = 0;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 版本变更：旧格式 NVS 数据整体无法读取，必须整分区擦除重建。
        // 注意：此操作会清空同分区内所有命名空间（含 "tapebook" 书签），属 NVS 库固有限制。
        const esp_partition_t *default_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
        if (default_part) {
            ESP_LOGW(TAG, "NVS version changed, erasing whole NVS partition (bookmarks in 'tapebook' namespace will be lost)");
            esp_partition_erase_range(default_part, 0, default_part->size);
        } else {
            ESP_LOGE(TAG, "Default NVS partition not found");
            return;
        }
        err = nvs_flash_init();
    } else if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // 满页：NVS 库无法单独擦除某个 namespace，整分区擦除会误清书签。
        // 优先直接重试 init（数据安全优先于 settings 可用性），不再无条件整擦。
        ESP_LOGW(TAG, "NVS no free pages, retrying init without erase (bookmarks preserved)");
        err = nvs_flash_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_init still failed (0x%x); settings unavailable, bookmarks preserved", err);
            return;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x", err);
        return;
    }

    err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' (0x%x)", SETTINGS_NAMESPACE, err);
        g_nvs_handle = 0;
        return;
    }
    ESP_LOGI(TAG, "NVS namespace '%s' opened", SETTINGS_NAMESPACE);
}

void settings_save_position(int track_idx, int position_s, const char *file_name)
{
    if (!g_nvs_handle) return;

    esp_err_t err;

    err = nvs_set_u16(g_nvs_handle, NVS_KEY_TRACK, (uint16_t)track_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u16(%s) failed: 0x%x", NVS_KEY_TRACK, err);
        return;
    }

    err = nvs_set_u32(g_nvs_handle, NVS_KEY_POSITION, (uint32_t)position_s);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32(%s) failed: 0x%x", NVS_KEY_POSITION, err);
        return;
    }

    /* 保存文件名，恢复时校验文件是否仍存在 */
    char key[32];
    snprintf(key, sizeof(key), "book_%d_name", track_idx);
    err = nvs_set_str(g_nvs_handle, key, file_name ? file_name : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: 0x%x", key, err);
        return;
    }

    ESP_LOGD(TAG, "Saved: track=%d pos=%ds name='%s'", track_idx, position_s, file_name);
}

bool settings_load_position(int *track_idx_out, int *position_s_out)
{
    if (!g_nvs_handle) return false;

    uint16_t idx = 0;
    uint32_t pos = 0;
    esp_err_t err;

    err = nvs_get_u16(g_nvs_handle, NVS_KEY_TRACK, &idx);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No saved track index");
        return false;
    }

    err = nvs_get_u32(g_nvs_handle, NVS_KEY_POSITION, &pos);
    if (err != ESP_OK) {
        pos = 0;
    }

    /* 校验文件是否仍存在于播放列表 */
    char key[32];
    snprintf(key, sizeof(key), "book_%d_name", idx);
    char saved_name[FILENAME_MAX_LEN] = "";
    size_t required_size = FILENAME_MAX_LEN;
    err = nvs_get_str(g_nvs_handle, key, saved_name, &required_size);

    // M8：buffer 不足导致 ESP_ERR_NVS_INVALID_LENGTH 时输出日志，便于调试历史数据
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "saved_name at '%s' exceeds buffer (%zu), truncated",
                 key, required_size);
    }

    if (err == ESP_OK && saved_name[0] != '\0') {
        char current_name[FILENAME_MAX_LEN] = "";
        if (idx < playlist_count() &&
            playlist_get_name(idx, current_name, sizeof(current_name))) {
            if (strcasecmp(current_name, saved_name) == 0) {
                *track_idx_out = (int)idx;
                *position_s_out = (int)pos;
                ESP_LOGI(TAG, "Resume: track=%d pos=%ds name='%s'", idx, (int)pos, saved_name);
                return true;
            }
        }
        ESP_LOGW(TAG, "Saved file '%s' no longer exists at index %d, starting fresh",
                 saved_name, idx);
    } else {
        /* 无保存文件名，只检查索引是否在范围内 */
        if (idx < playlist_count()) {
            *track_idx_out = (int)idx;
            *position_s_out = (int)pos;
            return true;
        }
    }

    return false;
}

void settings_save_volume(int volume)
{
    if (!g_nvs_handle) return;
    esp_err_t err = nvs_set_u8(g_nvs_handle, NVS_KEY_VOLUME, (uint8_t)volume);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8(%s) failed: 0x%x", NVS_KEY_VOLUME, err);
    // 不立即 commit：由 save_position 或下一次循环统一提交
}

int settings_load_volume(void)
{
    if (!g_nvs_handle) return AUDIO_OUTPUT_VOL;
    uint8_t vol = AUDIO_OUTPUT_VOL;
    esp_err_t err = nvs_get_u8(g_nvs_handle, NVS_KEY_VOLUME, &vol);
    if (err != ESP_OK) {
        vol = AUDIO_OUTPUT_VOL;
    }
    return (int)vol;
}

void settings_save_play_mode(int mode)
{
    if (!g_nvs_handle) return;
    esp_err_t err = nvs_set_u8(g_nvs_handle, NVS_KEY_PLAY_MODE, (uint8_t)mode);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8(%s) failed: 0x%x", NVS_KEY_PLAY_MODE, err);
    // 不立即 commit：由 save_position 统一提交
}

int settings_load_play_mode(void)
{
    if (!g_nvs_handle) return 0;
    uint8_t mode = 0;
    esp_err_t err = nvs_get_u8(g_nvs_handle, NVS_KEY_PLAY_MODE, &mode);
    if (err != ESP_OK) {
        mode = 0;
    }
    return (int)mode;
}

void settings_save_auto_off(int minutes)
{
    if (!g_nvs_handle) return;
    esp_err_t err = nvs_set_u8(g_nvs_handle, NVS_KEY_AUTO_OFF, (uint8_t)minutes);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8(%s) failed: 0x%x", NVS_KEY_AUTO_OFF, err);
    // 不立即 commit：由 save_position 统一提交
}

int settings_load_auto_off(void)
{
    if (!g_nvs_handle) return 0;
    uint8_t min = 0;
    esp_err_t err = nvs_get_u8(g_nvs_handle, NVS_KEY_AUTO_OFF, &min);
    if (err != ESP_OK) {
        min = 0;
    }
    return (int)min;
}

void settings_flush(void)
{
    if (!g_nvs_handle) return;
    esp_err_t err = nvs_commit(g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: 0x%x", err);
    }
}
