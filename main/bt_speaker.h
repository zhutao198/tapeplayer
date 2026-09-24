/**
 * @file bt_speaker.h
 * @brief 蓝牙音箱 (A2DP Sink) 对外接口
 *
 * 设备作为蓝牙音箱，手机通过 A2DP 推流到本机 I2S/MAX98357 输出。
 * 仅在 CONFIG_USE_ESP_ADF + CONFIG_USE_BT_SPEAKER 时编译进实现 (bt_speaker.cpp)。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_element.h"   // audio_element_handle_t

#ifdef __cplusplus
extern "C" {
#endif

/* 蓝牙音箱状态（用于 UI 刷新） */
typedef enum {
    BT_SPEAKER_STATE_IDLE = 0,          // 未初始化
    BT_SPEAKER_STATE_INITIALIZED,       // 已初始化，等待连接
    BT_SPEAKER_STATE_CONNECTED,         // 已连接（可出声）
    BT_SPEAKER_STATE_DISCONNECTED,      // 断开
    BT_SPEAKER_STATE_STOPPED,           // 已停止
} bt_speaker_state_t;

/* 状态变化回调（连接/断开时通知主程序刷新 UI） */
typedef void (*bt_speaker_state_cb_t)(bt_speaker_state_t state, const char *device_name);
/* 手机端音量回调（AVRCP 绝对音量 0..127） */
typedef void (*bt_speaker_volume_cb_t)(uint8_t vol_0_127);

/**
 * @brief 初始化蓝牙协议栈 (Bluedroid) 并设为可发现/可连接
 * @param device_name 蓝牙设备名（手机扫描时显示）
 */
esp_err_t bt_speaker_init(const char *device_name);

/** @brief 反初始化（停协议栈） */
esp_err_t bt_speaker_deinit(void);

/**
 * @brief 启动蓝牙音频管线：A2DP Sink 解码元素 → i2s_writer
 * @param i2s_writer 由 audio_player 提供的 I2S 输出元素（复用，避免双管线冲突）
 */
esp_err_t bt_speaker_start(audio_element_handle_t i2s_writer);

/** @brief 停止蓝牙音频管线 */
esp_err_t bt_speaker_stop(void);

/** @brief 注册状态回调（UI 显示连接状态/设备名） */
void bt_speaker_register_state_cb(bt_speaker_state_cb_t cb);

/** @brief 注册手机音量回调（把 AVRCP 音量映射到本地 ALC） */
void bt_speaker_set_volume_cb(bt_speaker_volume_cb_t cb);

/** @brief 把本地音量回传手机（AVRCP 绝对音量 0..127） */
void bt_speaker_report_volume(uint8_t vol_0_127);

/** @brief 是否处于已连接状态 */
bool bt_speaker_is_connected(void);

/** @brief 向手机发送 AVRCP 播放命令 */
void bt_speaker_avrc_play(void);

/** @brief 向手机发送 AVRCP 暂停命令 */
void bt_speaker_avrc_pause(void);

#ifdef __cplusplus
}
#endif
