/**
 * @file ota_sd.h
 * @brief TF 卡固件升级（SD-OTA）模块 (R049c 真实化)
 *
 * 从 TF 卡根目录读取 TAPEBOOK.BIN 镜像，经 esp_ota 写入 ota_0/ota_1 分区。
 * 安全：A/B 分区（factory 永不覆盖）、esp_ota_end 内置 SHA256+芯片校验、
 *       启动回滚（需 sdkconfig 开启 BOOTLOADER_APP_ROLLBACK_ENABLE）、
 *       可选版本防降级 + TAPEBOOK.SHA256 清单校验、低电量屏蔽确认、写中全键锁死。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "button_manager.h"

/* TF 卡根目录约定文件名 */
#define OTA_IMAGE_NAME   "TAPEBOOK.BIN"    // 固件镜像
#define OTA_VER_NAME     "TAPEBOOK.VER"    // 文本版本号（首行，如 "1.2.0"），缺失则跳过版本校验
#define OTA_SHA_NAME     "TAPEBOOK.SHA256" // 镜像 SHA256 文本（64 hex），缺失则跳过清单校验

typedef enum {
    OTA_PHASE_CONFIRM,   // 摘要 + 二次确认
    OTA_PHASE_PROGRESS,  // 写入中
    OTA_PHASE_DONE,      // 成功
    OTA_PHASE_ERROR,     // 失败
} ota_phase_t;

/* 由 main 调用：进入升级流程（扫描 SD 镜像，决定进入 CONFIRM 或 ERROR） */
void ota_sd_begin(void);

/* 由 main 的按键路由调用（仅在 APP_STATE_OTA 态） */
void ota_sd_handle_button(const btn_event_info_t *events, int n);

/* 由 main 的 update_display 调用（渲染当前阶段） */
void ota_sd_render(void);

/* 宿主回调：退出 OTA（取消/返回），由 main.cpp 实现 */
void app_ota_exit(void);

#ifdef __cplusplus
}
#endif
