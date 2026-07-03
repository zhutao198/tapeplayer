/**
 * @file voice_prompt.h
 * @brief 语音播报模块 (P2 — V1.2+)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播报当前播放状态
 */
void voice_prompt_status(void);

/**
 * @brief 播报电量
 */
void voice_prompt_battery(void);

/**
 * @brief 播报指定文本（从 /voice/ 目录找 WAV 文件）
 */
void voice_prompt_play(const char *text_id);

#ifdef __cplusplus
}
#endif
