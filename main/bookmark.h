/**
 * @file bookmark.h
 * @brief 书签管理模块 (P2 — V1.2+)
 *
 * 每个文件最多 10 个书签，保存在 NVS。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOKMARK_MAX_PER_FILE  10

typedef struct {
    int  position_s;     // 书签位置（秒）
    char label[16];      // 可选标签（如 "CH03"）
} bookmark_t;

/**
 * @brief 初始化书签模块
 */
void bookmark_init(void);

/**
 * @brief 在指定文件指定位置添加书签
 * @param file_idx   文件索引
 * @param position_s 位置（秒）
 * @return 书签编号(0~9)，-1=已满
 */
int bookmark_add(int file_idx, int position_s);

/**
 * @brief 删除指定书签
 */
bool bookmark_delete(int file_idx, int bm_idx);

/**
 * @brief 获取指定文件的书签列表
 * @return 实际书签数
 */
int bookmark_list(int file_idx, bookmark_t *out, int max_count);

/**
 * @brief 获取书签位置（用于跳转）
 * @return 书签位置（秒），-1=不存在
 */
int bookmark_jump(int file_idx, int bm_idx);

#ifdef __cplusplus
}
#endif
