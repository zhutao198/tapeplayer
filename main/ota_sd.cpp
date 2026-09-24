/**
 * @file ota_sd.cpp
 * @brief TF 卡固件升级实现 (R049c 真实化)
 *
 * 流程：扫描 TAPEBOOK.BIN → 版本防降级 + 电量检查 → 用户确认（main 路由 PLAY）
 *       → 写循环（同步，每包喂 WDT + 更新进度）→ esp_ota_end 校验 → set_boot_partition → 重启。
 */

#include "ota_sd.h"
#include "config.h"
#include "display.h"
#include "power_mgmt.h"
#include "audio_player.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "esp_timer.h"

static const char *TAG = "ota_sd";

/* ---- 模块内部状态 ---- */
static ota_phase_t s_phase = OTA_PHASE_CONFIRM;
static char        s_cur_ver[16]   = {0};
static char        s_new_ver[16]   = {0};
static long        s_img_size      = 0;
static bool        s_battery_ok    = true;
static char        s_err_msg[72]   = {0};
static int         s_progress      = 0;

/* review #24: 最近一次用户操作/进入时间, 用于空闲超时自动退出, 保证
 * g_ota_in_progress 在失败路径也必然清零 (不再仅依赖用户 STOP 或重启) */
static uint64_t       s_last_activity_us = 0;
static const uint64_t OTA_AUTO_EXIT_US = 120 * 1000000ULL;  // 确认/错误态无操作 120s

/* ============================================================
 * 小工具
 * ============================================================ */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 读取文本首行并去掉尾部空白（用于 TAPEBOOK.VER） */
static int read_text_line(const char *path, char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, outsz - 1, f);
    fclose(f);
    if (n == 0) return -1;
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' '  || out[n - 1] == '\t')) {
        out[--n] = 0;
    }
    return 0;
}

/* 从 TAPEBOOK.SHA256 提取 64 hex → 32 字节；返回 true 表示解析成功 */
static bool read_sha_manifest(uint8_t *out)
{
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, OTA_SHA_NAME);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    int hi = -1, cnt = 0;
    for (size_t i = 0; i < n && cnt < 32; i++) {
        int h = hexval(buf[i]);
        if (h < 0) continue;
        if (hi < 0) hi = h;
        else { out[cnt++] = (uint8_t)((hi << 4) | h); hi = -1; }
    }
    return cnt == 32;
}

/* "x.y.z" 语义版本比较：a>b 返回 >0，a<b 返回 <0，相等返回 0 */
static int ver_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        int va = 0, vb = 0;
        while (*a && *a != '.') { va = va * 10 + (*a - '0'); a++; }
        while (*b && *b != '.') { vb = vb * 10 + (*b - '0'); b++; }
        if (va != vb) return (va > vb) ? 1 : -1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* ============================================================
 * 进入升级：扫描镜像 + 前置校验
 * ============================================================ */
void ota_sd_begin(void)
{
    s_phase = OTA_PHASE_CONFIRM;
    s_last_activity_us = esp_timer_get_time();
    s_progress = 0;
    snprintf(s_cur_ver, sizeof(s_cur_ver), "%s", APP_VERSION_STR);
    s_new_ver[0] = 0;

    /* 1) 镜像文件是否存在 */
    char img_path[80];
    snprintf(img_path, sizeof(img_path), "%s/%s", SD_MOUNT_POINT, OTA_IMAGE_NAME);
    struct stat st;
    if (stat(img_path, &st) != 0) {
        s_phase = OTA_PHASE_ERROR;
        snprintf(s_err_msg, sizeof(s_err_msg),
                 "未找到 %s\n请确认 TF 卡已插入", OTA_IMAGE_NAME);
        return;
    }
    s_img_size = st.st_size;

    /* 2) 版本防降级（有 .VER 才校验） */
    char ver_path[80];
    snprintf(ver_path, sizeof(ver_path), "%s/%s", SD_MOUNT_POINT, OTA_VER_NAME);
    char tmp[32];
    if (read_text_line(ver_path, tmp, sizeof(tmp)) == 0) {
        snprintf(s_new_ver, sizeof(s_new_ver), "%.15s", tmp);
        if (ver_cmp(s_new_ver, s_cur_ver) < 0) {
            s_phase = OTA_PHASE_ERROR;
            snprintf(s_err_msg, sizeof(s_err_msg),
                     "新版本 %s 低于当前 %s\n拒绝降级", s_new_ver, s_cur_ver);
            return;
        }
    }

    /* 3) 电量保护 */
    bat_state_t bs = power_mgmt_get_state();
    s_battery_ok = power_mgmt_is_charging() ||
                   (bs != BAT_STATE_LOW && bs != BAT_STATE_CRITICAL);
}

/* ============================================================
 * 真正写镜像（同步，在按键回调内调用）
 * ============================================================ */
static void ota_sd_perform(void)
{
    s_phase = OTA_PHASE_PROGRESS;
    s_progress = 0;
    display_show_ota_progress(0);

    char img_path[80];
    snprintf(img_path, sizeof(img_path), "%s/%s", SD_MOUNT_POINT, OTA_IMAGE_NAME);

    FILE *f = fopen(img_path, "rb");
    if (!f) {
        s_phase = OTA_PHASE_ERROR;
        snprintf(s_err_msg, sizeof(s_err_msg), "打开镜像失败");
        return;
    }

    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) {
        fclose(f);
        s_phase = OTA_PHASE_ERROR;
        snprintf(s_err_msg, sizeof(s_err_msg), "无可用 OTA 分区");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > dst->size) {
        fclose(f);
        s_phase = OTA_PHASE_ERROR;
        snprintf(s_err_msg, sizeof(s_err_msg), "镜像超过分区大小");
        return;
    }

    /* 可选：清单 SHA256（用于防篡改，缺失则跳过） */
    uint8_t expect_sha[32] = {0};
    bool have_sha = read_sha_manifest(expect_sha);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    esp_ota_handle_t h;
    esp_err_t r = esp_ota_begin(dst, (size_t)sz, &h);
    if (r != ESP_OK) {
        fclose(f);
        mbedtls_sha256_free(&sha);
        s_phase = OTA_PHASE_ERROR;
        snprintf(s_err_msg, sizeof(s_err_msg), "初始化写分区失败");
        return;
    }

    uint8_t buf[4096];
    long written = 0;
    int last_pct = -1;
    int n;
    while ((n = (int)fread(buf, 1, sizeof(buf), f)) > 0) {
        mbedtls_sha256_update(&sha, buf, n);
        r = esp_ota_write(h, buf, n);
        if (r != ESP_OK) {
            esp_ota_abort(h);
            fclose(f);
            mbedtls_sha256_free(&sha);
            s_phase = OTA_PHASE_ERROR;
            snprintf(s_err_msg, sizeof(s_err_msg), "写入失败（卡被拔出？）");
            return;
        }
        written += n;
        int pct = (int)(written * 100 / sz);
        if (pct != last_pct) {
            last_pct = pct;
            s_progress = pct;
            display_show_ota_progress(pct);
        }
        esp_task_wdt_reset();  // 长写入期间喂看门狗
    }
    fclose(f);

    /* 收尾 SHA256 并比对清单 */
    uint8_t calc_sha[32];
    mbedtls_sha256_finish(&sha, calc_sha);
    mbedtls_sha256_free(&sha);
    if (have_sha && memcmp(calc_sha, expect_sha, 32) != 0) {
        esp_ota_abort(h);
        s_phase = OTA_PHASE_ERROR;
        snprintf(s_err_msg, sizeof(s_err_msg), "校验失败：镜像与 SHA256 不符");
        return;
    }

    /* esp_ota_end 内部校验镜像 SHA256 + 适用本芯片；失败即拒绝 */
    r = esp_ota_end(h);
    if (r != ESP_OK) {
        s_phase = OTA_PHASE_ERROR;
        snprintf(s_err_msg, sizeof(s_err_msg), "镜像校验失败（非本机/损坏）");
        return;
    }

    r = esp_ota_set_boot_partition(dst);
    if (r != ESP_OK) {
        s_phase = OTA_PHASE_ERROR;
        snprintf(s_err_msg, sizeof(s_err_msg), "设置启动分区失败");
        return;
    }

    ESP_LOGI(TAG, "OTA write done, boot partition set; reboot to apply");
    s_phase = OTA_PHASE_DONE;
}

/* ============================================================
 * 按键路由（仅 APP_STATE_OTA 态）
 * ============================================================ */
void ota_sd_handle_button(const btn_event_info_t *events, int n)
{
    for (int k = 0; k < n; k++) {
        const btn_event_info_t *e = &events[k];
        if (e->event != BTN_EVENT_SHORT_PRESS) continue;
        s_last_activity_us = esp_timer_get_time();   // 任意有效按键重置空闲计时

        switch (s_phase) {
        case OTA_PHASE_CONFIRM:
            if (e->id == BTN_ID_PLAY_PAUSE) {
                if (!s_battery_ok) break;   // 低电量：锁死确认
                ota_sd_perform();
            } else if (e->id == BTN_ID_STOP) {
                app_ota_exit();
            }
            break;

        case OTA_PHASE_PROGRESS:
            break;  // 写入中全键锁死

        case OTA_PHASE_DONE:
            esp_restart();   // 任意键重启以生效
            break;

        case OTA_PHASE_ERROR:
            if (e->id == BTN_ID_PLAY_PAUSE)      ota_sd_begin();   // 重试
            else if (e->id == BTN_ID_STOP)       app_ota_exit();   // 返回
            break;

        default:
            break;
        }
    }
}

/* 由 main 周期调用：确认/错误态若长时间无操作, 自动 app_ota_exit() 清零
 * g_ota_in_progress, 避免失败路径依赖用户按键或重启才复位 (review #24) */
void ota_sd_tick(void)
{
    if (s_phase == OTA_PHASE_CONFIRM || s_phase == OTA_PHASE_ERROR) {
        if (esp_timer_get_time() - s_last_activity_us > OTA_AUTO_EXIT_US) {
            ESP_LOGW(TAG, "OTA idle timeout, auto exit");
            app_ota_exit();
        }
    }
}

/* ============================================================
 * 渲染当前阶段
 * ============================================================ */
void ota_sd_render(void)
{
    switch (s_phase) {
    case OTA_PHASE_CONFIRM:
        display_show_ota_confirm(s_cur_ver, s_new_ver, s_img_size / 1024,
                                 OTA_IMAGE_NAME, s_battery_ok);
        break;
    case OTA_PHASE_PROGRESS:
        display_show_ota_progress(s_progress);
        break;
    case OTA_PHASE_DONE:
        display_show_ota_done();
        break;
    case OTA_PHASE_ERROR:
        display_show_ota_error(s_err_msg);
        break;
    default:
        break;
    }
}
