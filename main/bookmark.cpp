/**
 * @file bookmark.cpp
 * @brief 书签管理 stub (P2 — V1.2+)
 *
 * 当前为占位实现，V1.2 时完善。
 */

#include "bookmark.h"
#include "esp_log.h"

static const char *TAG = "bookmark";

void bookmark_init(void) {
    ESP_LOGI(TAG, "Bookmark module init (stub for V1.2)");
}

int bookmark_add(int file_idx, int position_s) {
    ESP_LOGI(TAG, "Bookmark add: file=%d pos=%ds (stub)", file_idx, position_s);
    return -1;
}

bool bookmark_delete(int file_idx, int bm_idx) {
    return false;
}

int bookmark_list(int file_idx, bookmark_t *out, int max_count) {
    return 0;
}

int bookmark_jump(int file_idx, int bm_idx) {
    return -1;
}
