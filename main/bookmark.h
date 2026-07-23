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
    // R036-004：移除未使用的 label[16] 字段（R035-007 删除 bookmark_list 后再无写入路径）
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

#ifdef __cplusplus
}
#endif
