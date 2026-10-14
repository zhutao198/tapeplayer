/*
 * mp3_decoder_libhelix.h
 *
 * R080: 用 Helix MP3 解码器（chmorgan/esp-libhelix-mp3, Apache-2.0）替代
 *       ADF 闭源的 PV-MP3 (minimp3)。
 *
 * 背景：
 *   PV-MP3(minimp3) 在解码特定合法 MP3（白桦树/相反的我，转码 128k/320k 均复现）
 *   时于 mp3_decoder_open / 解码路径确定性 BREAK(@0x403743c0) 崩溃，且闭源无法 patch。
 *   Helix MP3 解码器健壮性更好：遇到坏帧返回负错误码而非崩溃，可由应用层跳过。
 *
 * 本文件提供一个 audio_element wrapper，把 Helix MP3Decode 接入 audio_pipeline，
 * 使 file -> mp3_decoder_libhelix -> i2s 链路彻底绕开 PV-MP3。
 *
 * 跳曲保护：连续解码错误超过阈值（或 EOS 仍解不出）时 process 返回 AEL_IO_DONE，
 * 元素进入 FINISHED，audio_player_tick() 监测到后回调 on_track_finished 自动播下一首。
 */
#pragma once

#include "audio_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  MP3 decoder 配置 (基于 Helix MP3)
 */
typedef struct {
    int  task_stack;     /*!< 解码任务栈大小 (字节), -1 用默认 */
    int  out_rb_size;    /*!< 输出 ringbuffer 大小 (字节) */
    bool stack_in_ext;   /*!< 栈是否分配到 PSRAM (R145: 改 true, 规避长播 DRAM 碎片化致解码任务创建失败; FLAC ext=1 在 S3 已验证可正常工作) */
} mp3_decoder_libhelix_cfg_t;

#define DEFAULT_MP3_DECODER_LIBHELIX_CONFIG() { \
    .task_stack   = 32 * 1024,  \
    .out_rb_size  = 16 * 1024,  \
    .stack_in_ext = true,       \
}

/**
 * @brief  创建基于 Helix MP3 的 MP3 decoder audio_element
 *
 * @param[in]  config  解码器配置（可传 NULL 用默认）
 * @return     audio_element handle, 失败返回 NULL
 */
audio_element_handle_t mp3_decoder_libhelix_init(const mp3_decoder_libhelix_cfg_t *config);

/**
 * @brief  设置解码器软件音量（R091：替代 ADF 脆弱的 i2s ALC，改在 decoder 侧缩放 PCM）
 *
 * @param[in]  level  逻辑音量 0..14（0=静音，14=最大/0dB 统一增益）
 */
void mp3_decoder_set_volume(int level);

/**
 * @brief  seek/跳曲前重置解码器输入缓冲与坏帧计数（R094）
 *
 * s_in/s_in_len 为模块级 static，跨 pause/resume/seek 保留；播放中 seek 后
 * reader 在新位置重开但 decoder 元素未 close/reopen，残留的旧位置半截数据会
 * 与新数据拼接成非法 MP3 导致误判曲终跳下一首。调用本函数清空并从新位置重新同步。
 *
 * R154: 本函数只置重建标志，真正的重建由解码任务在自己 process 顶部执行
 * (mp3_task_rebuild_if_req)——不可在 main 线程原地 free/init，会与解码任务
 * 并发写坏 Helix 状态(实测致解码器永久静默失败：无声、无日志、仅切歌可恢复)。
 * 元素处于 PAUSED 时标志会延迟到 resume 后第一个 process 消费，安全。
 *
 * @param[in]  el  解码器 audio_element handle（保留参数兼容，R154 起未使用）
 */
void mp3_decoder_libhelix_reset(audio_element_handle_t el);

/**
 * @brief  R154: 请求解码任务在下一个 process 入口重建 Helix 解码器（任务侧安全执行）
 */
void mp3_decoder_libhelix_request_rebuild(void);

void mp3_decoder_libhelix_clear_errors(audio_element_handle_t el);

#ifdef __cplusplus
}
#endif
