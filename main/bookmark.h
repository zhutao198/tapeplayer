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
} bookmark_t;

/**
 * @brief 初始化书签模块
 */
void bookmark_init(void);

/**
 * @brief 在指定文件指定位置添加书签
 * @param file_idx   文件索引
 * @param position_s 位置（秒）
 * @return 书签编号(0~9)，-1=失败
 */
int bookmark_add(int file_idx, int position_s);

/**
 * @brief 获取指定文件的所有书签（按slot顺序）
 * @param file_idx  文件索引
 * @param out       输出数组
 * @param max_count 数组最大容量
 * @return 实际书签数量
 */
int bookmark_get_all(int file_idx, bookmark_t *out, int max_count);

/**
 * @brief 删除指定文件的指定书签
 * @param file_idx 文件索引
 * @param slot     书签编号(0~9)
 */
void bookmark_delete(int file_idx, int slot);

#ifdef __cplusplus
}
#endif
