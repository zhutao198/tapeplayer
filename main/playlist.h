/**
 * @file playlist.h
 * @brief 播放列表管理
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYLIST_TRACK_MAX PLAYLIST_MAX_SIZE

/** 支持的音频格式判断 */
bool playlist_is_audio_file(const char *filename);

/**
 * @brief 从 SD 卡目录扫描音频文件，构建播放列表
 * @param base_path SD 卡挂载路径，如 "/sdcard"
 * @return 找到的音频文件数量
 */
int playlist_scan(const char *base_path);

/**
 * @brief 获取播放列表大小
 */
int playlist_count(void);

/**
 * @brief 通过索引获取文件名
 * @param index 播放列表索引
 * @param buffer 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 成功返回 true
 */
bool playlist_get_name(int index, char *buffer, size_t buf_size);

/**
 * @brief 通过索引获取完整路径
 * @param index 播放列表索引
 * @param buffer 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 成功返回 true
 */
bool playlist_get_path(int index, char *buffer, size_t buf_size);

/**
 * @brief 获取当前播放索引
 */
int playlist_current_index(void);

/**
 * @brief 设置当前播放索引
 */
void playlist_set_index(int index);

/**
 * @brief 移动到下一首 (循环)
 */
int playlist_next(void);

/**
 * @brief 移动到上一首 (循环)
 */
int playlist_prev(void);

/**
 * @brief 检查播放列表是否为空
 */
bool playlist_is_empty(void);

#ifdef __cplusplus
}
#endif
