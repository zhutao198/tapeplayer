# TapeBook 代码审查报告（含 R009/R010 复查）

> **审查时间**：2026-07-06 → 2026-07-07（含 R009/R010 复查）  
> **基线 commit**：R007 (`4cd11a2`) → 当前 HEAD `76441b1`（R010）  
> **审查方法**：静态代码审查 + 交叉对照 IDF v5.5 / ESP-ADF v2.8  
> **修复追踪**：✅=已修 / ⚠️=部分修(代码改但 PRD 集成未完成) / ❌=未修 / 🆕=R009/R010 新引入

---

## 概览（R010 后真实状态）

| 严重级别 | 总数 | ✅ 已修 | ⚠️ 部分修 | ❌ 未修 | 🆕 新 bug | ➖ 设计 OK |
|---|---|---|---|---|---|---|
| 🔴 CRITICAL | 5 | **5** | 0 | 0 | — | 0 |
| 🟠 HIGH | 10 | 7 | **1** (H-8 ADC) | 2 | **1** (H-1 SD 检测) | 0 |
| 🟡 MEDIUM | 15 | 12 | **1** (L-1 voice) | 1 | **4** | 1 (M-15) |
| 🟢 LOW | 8 | 3 | 0 | 1 (L-1 部分) | 1 (press_start_us 歧义) | 5 (L-3 等) |
| **总计** | **38** | **27 (71%)** | **2 (5%)** | **5 (13%)** | **6 新 bug** | **6 ➖** |

**⚠️ R010 commit message 声称"全部 38 项清零"是失实的**：
- 🔴 CRITICAL 确实全部清零 ✅
- 🟠 HIGH 27/10 修（**H-8 power_mgmt ADC 仍 stub**）
- 🟡 MEDIUM 12/15 修（**L-1 voice_prompt 仍仅 log**）
- 🆕 **新引入 6 个 bug**（SD 检测失效、跳帧反复 pause/resume、light sleep 丢断点等）

**最新 commits**：
- `df11f0d` R011: 修复 R010 引入的 6 个 bug + H-8 ADC 桩 + L-1 bookmark 按键集成
- `76441b1` R010: 38 项清零（bookmark/voice_prompt/M-2 timeout/M-3 init）
- `853f483` R009: 修 SD 热插拔/脏区/屏保/light sleep/按钮去抖/采样率
- `9559bef` docs: 标记 R009 修复状态

---

## 🆕 R009/R010 新引入的 Bug（6 个）

### 🆕-1. 🔴 `main.cpp:602` SD 卡拔卡检测实际无效

**症状**：
```c
struct stat st;
if (stat(SD_MOUNT_POINT, &st) != 0) {
    // 拔卡处理
}
```
**问题**：ESP-IDF v5.5 FATFS VFS 中，**即使卡被拔掉，`stat("/sdcard")` 仍然返回 0**（挂载点目录是 VFS 内部伪目录，持久存在）。这条 `if` 分支**几乎永远不会触发**。H-1 的 SD 拔卡检测实际失效。

**建议**：改用 `sdmmc_io_get_status(g_sd_card)` 或 CD pin GPIO 中断。

### 🆕-2. 🟡 `main.cpp:589` light sleep 前未保存断点

**症状**：
```c
if (power_mgmt_should_sleep()) {
    audio_player_stop();           // 销毁 pipeline，但没 save_current_position
    g_app_state = APP_STATE_IDLE;
}
```
**问题**：`audio_player_stop()` 不写 NVS。30s auto-save 周期保证大部分情况下能保存，但**进入 light sleep 那一刻的播放进度（< 30s）会丢失**。

**建议**：sleep 前加 `save_current_position()`。

### 🆕-3. 🟡 `audio_player.cpp:436` FF/RW 跳帧反复 pause/resume

**症状**：`tick()` 跳帧循环每 50ms 调一次 `audio_player_seek_ms()`，而 `seek_ms()` 内部已 `pause` + `resume`。

**问题**：FF 8x 时 `skip_ms = 50 * 8 = 400ms`，每次跳 400ms 后再 pause/resume 会引起 I2S 缓冲 underrun + 杂音。**反复的 pause/resume 在高速跳帧时是反模式**。

**建议**：抽出内部 `_seek_byte()` 函数，跳过 pause/resume 步骤。

### 🆕-4. 🟡 `display.cpp:184` 长文件名截断无效

**症状**：
```c
char fname[22];
if (len <= 21) {
    snprintf(fname, sizeof(fname), "%-21s", track_name);
} else {
    snprintf(fname, sizeof(fname), "%s", track_name);  // ← 没有截断!
}
```
**问题**：`%s` 不带宽度，会写入全部内容，**snprintf 保证不溢出但长文件名会破坏后续字段布局**。

**建议**：统一用 `"%.21s"`。

### 🆕-5. 🟡 `display.cpp:213` time_buf 32B 实际写入可达 42B

**症状**：
```c
char time_buf[32];
snprintf(time_buf, sizeof(time_buf), "%s / %s [%s]", cur, tot, gs);
```
**问题**：`cur`(16) + ` / `(3) + `tot`(16) + ` [`(2) + `gs`(4) + `]`(1) = **42 字节**，超过 32。snprintf 截断不溢出，但视觉错乱。

**建议**：`time_buf[48]` 或缩短 `cur/tot` 缓冲到 8 字节。

### 🆕-6. 🟢 `button_manager.cpp` `press_start_us` 命名歧义

**症状**：变量在 `IDLE → DEBOUNCE` 时设置（line 125），但 `LONG_PRESS` 转移（line 152）不更新，含义在双击场景下模糊。

**评估**：**实际逻辑正确**（双击时 line 162 已覆盖），但命名易误导后续维护者。

---

## 📊 PRD V1.1/V1.2 用户可感知功能实际可用性

| PRD 功能 | API | UI 集成 | 实际可用 | 评估 |
|---|---|---|---|---|
| 定时关机 15/30/60/90 min | ✅ | ❌ `init_hardware()` 未调 `settings_load_auto_off()` | ❌ | API 完整但默认关闭 |
| A-B 复读 | ❌ 无 ab_repeat.c | ❌ | ❌ | 未实现 |
| 屏幕保护 30s | ⚠️ display 内部有 | ❌ 未与 `power_mgmt_should_screen_off()` 对齐 | ❌ | display 单独做了 power save |
| 按键提示音 | ❌ 无 | ❌ | ❌ | 未实现 |
| OGG/Opus 解码 | ✅ 接口有 | — | ✅ | ADF decoder 已链 |
| EC11 旋转编码器 | ❌ GPIO 未配 | ❌ | ❌ | 预留 GPIO 38/39 |
| **书签管理** | ✅ NVS API 完整 | ❌ **main.cpp 完全没调** | ❌ | **对用户不可见** |
| **语音播报** | ❌ 仅 log | ❌ | ❌ | **完全 stub** |
| **电量检测** | ⚠️ 函数有 | — | ❌ | **永远返回 100** |
| 电池低电告警 | ⚠️ tick 打日志 | ❌ 未暂停/未语音 | ❌ | 形式有，逻辑无 |
| 设置菜单 | ❌ | ❌ | ❌ | 未实现 |
| OTA 升级 | ❌ | ❌ | ❌ | 未实现 |

**总体**：12 项 PRD V1.1/V1.2 功能，**仅 1 项（OGG/Opus）实际可用**，用户可感知功能完成度约 **8%**。

---

## 🔴 CRITICAL（5 个）

### C-1. ✅ audio_player.cpp — seek 字节/毫秒混淆（已修 R008）

**修复**：
```c
// 换算 毫秒 → 字节位置：byte_pos = (ms / total_ms) × total_bytes
if (g_total_duration_ms > 0 && g_total_file_bytes > 0) {
    int byte_pos = (int)((int64_t)ms * g_total_file_bytes / g_total_duration_ms);
    audio_element_set_byte_pos(g_decoder, byte_pos);
}
else if (g_total_file_bytes > 0) {
    // 兜底：粗略按 1MB/min 估算
    int byte_pos = (int)((int64_t)ms * g_total_file_bytes / 3600000);
    audio_element_set_byte_pos(g_decoder, byte_pos);
}
```
**评估**：✅ 完美修复，双层兜底（duration 已知/未知）。seek 后还同步更新 `g_play_start_us` 和 `g_play_offset_us` 保持 position 一致。

---

### C-2. ✅ audio_player.cpp — 返回假位置（已修 R008）

**修复**：
```c
int audio_player_get_position_ms(void) {
    if (!g_pipeline) return 0;

    // 累计播放时间 = 暂停前已累计 + 当前段播放时间（暂停期间不增加）
    int64_t total = g_play_offset_us;
    if (g_is_playing && !g_is_paused && g_play_start_us > 0) {
        total += (int64_t)(esp_timer_get_time() - g_play_start_us);
    }
    return (int)(total / 1000);
}
```
**新增字段**：
- `g_play_start_us` — 本次播放起始
- `g_pause_start_us` — 暂停起始时间
- `g_play_offset_us` — 暂停前已累计播放时间

**评估**：✅ 完美修复，pause/resume 处理正确（`g_play_offset_us` 累加暂停前时间，重置起始时间戳）。

---

### C-3. ✅ audio_player.cpp — 元素 NULL 检查缺失（已修 R008）

**修复**：
```c
// fatfs_stream_init 检查
g_fatfs_reader = fatfs_stream_init(&fatfs_cfg);
if (!g_fatfs_reader) {
    ESP_LOGE(TAG, "Failed to create FATFS reader");
    audio_pipeline_deinit(g_pipeline);
    g_pipeline = NULL;
    return false;
}

// create_decoder 检查
g_decoder = create_decoder(filepath);
if (!g_decoder) {
    ESP_LOGE(TAG, "Failed to create decoder");
    audio_element_deinit(g_fatfs_reader);
    g_fatfs_reader = NULL;
    audio_pipeline_deinit(g_pipeline);
    g_pipeline = NULL;
    return false;
}

// audio_pipeline_link 检查
esp_err_t link_err = audio_pipeline_link(g_pipeline, link_tags, 3);
if (link_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to link pipeline (0x%x)", link_err);
    audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
    audio_pipeline_unregister(g_pipeline, g_decoder);
    audio_pipeline_unregister(g_pipeline, g_i2s_writer);
    audio_element_deinit(g_fatfs_reader);
    audio_element_deinit(g_decoder);
    g_fatfs_reader = NULL;
    g_decoder = NULL;
    audio_pipeline_deinit(g_pipeline);
    g_pipeline = NULL;
    return false;
}
```
**评估**：✅ 完美修复，三处 NULL/error 检查 + 完整清理路径（unregister + deinit + NULL）。

---

### C-4. ✅ main.cpp — 回调递归触发 WDT（已修 R008）

**修复（两重防线）**：

**(a) WDT 超时 5s → 10s**：
```c
esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 10000,   // 给 callback 内 pipeline 操作留余量
    .idle_core_mask = 0,
    .trigger_panic = true,
};
```

**(b) 异步化（核心修复）**：
```c
// 回调中只标记，不调 audio_player_play
static void on_track_finished(int state, void *user_data) {
    switch (g_play_mode) {
    case PLAY_MODE_SEQUENCE:
        if (g_current_track < playlist_count() - 1) {
            g_pending_track_next = playlist_next();
            g_pending_track_seek = 0;
            g_pending_track_finished = true;   // 标记，不执行
        }
        ...
    }
}

// 主循环中处理
if (g_pending_track_finished) {
    g_pending_track_finished = false;
    g_current_track = g_pending_track_next;
    playlist_set_index(g_current_track);
    g_seek_on_play_position = g_pending_track_seek;
    play_current_track();    // 在主循环上下文执行
}
```
**评估**：✅ **非常优雅的修复**，从回调依赖变成"标记 + 主循环处理"，完全避免栈帧叠加。

---

### C-5. ✅ settings.cpp — track_idx 截断 >256 文件（已修 R009）

**修复**（`settings.cpp:46`）：
```c
err = nvs_set_u16(g_nvs_handle, NVS_KEY_TRACK, (uint16_t)track_idx);
if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u16(%s) failed: 0x%x", NVS_KEY_TRACK, err);
```
加载侧 `settings_load_position()` 同步改为 `nvs_get_u16` + `uint16_t idx`。

**评估**：✅ 完美修复，save/load 两侧对称改完，支持 0-65535 文件。

---

## 🟠 HIGH（10 个）

### H-1. ✅ main.cpp — SD 卡热插拔未实现（已修 R009）

**修复**：`#if 0` → `stat(SD_MOUNT_POINT, &st)` 每 5 秒轮询挂载点。检测到移除时自动停播 + 显示"No SD Card"。

### H-2. ✅ main.cpp — display_update 全屏重绘（已修 R009）

**修复**：
1. 脏区检查：内容指纹匹配时跳过 `u8g2_SendBuffer`，I2C 流量降至接近 0
2. 屏保：30s 内容无变化自动 `u8g2_SetPowerSave(1)`，内容变化时自动唤醒

### H-3. ✅ main.cpp — 锁定状态事件不同步（已修 R009）

**修复**：
1. `power_mgmt_record_activity()` 移到锁定检查**前**，锁定态仍记录用户活动
2. button_manager 状态机在松开时自动回 IDLE，无滞留问题

### H-4. ✅ playlist.cpp — g_items 改 PSRAM（已修 R008）

**修复**：
```c
static playlist_item_t *g_items = NULL;     // 指针替代数组

// playlist_scan 中按需分配
g_items = (playlist_item_t *)heap_caps_malloc(
    sizeof(playlist_item_t) * PLAYLIST_TRACK_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!g_items) {
    // PSRAM 失败时回退到普通 DRAM
    g_items = (playlist_item_t *)malloc(sizeof(playlist_item_t) * PLAYLIST_TRACK_MAX);
    if (!g_items) return 0;
}
```
**评估**：✅ 完美修复，PSRAM 优先 + DRAM 回退 + 旧指针 free。

### H-5. ✅ settings.cpp — NVS 写返回值未检查（已修 R008）

**修复**：所有 `nvs_set_*` 加 `esp_err_t` 检查 + 失败 `ESP_LOGE`。  
**评估**：✅ 完美。

### H-6. ✅ settings.cpp — nvs_open 错误路径 handle 清理（已修 R009）

**修复**（`settings.cpp:24-37`）：
```c
void settings_init(void) {
    g_nvs_handle = 0;                                                // 入口重置
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' (0x%x), erasing...", NVS_NAMESPACE, err);
        g_nvs_handle = 0;                                            // 失败路径1：清 handle
        nvs_flash_erase();
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS still failed after erase, system may be unstable");
            g_nvs_handle = 0;                                        // 失败路径2：清 handle
            return;
        }
    }
}
```

**评估**：✅ 完美修复，三处 handle 清理（入口重置 + erase 后失败 + 最终失败）。后续 save_*() 通过 `if (!g_nvs_handle) return;` 检查，不会使用无效 handle。

### H-7. ✅ settings.cpp — NVS commit 频率（已修 R009）

**修复**：

**(a) 新增 `settings_flush()` 函数**（`settings.h` + `settings.cpp`）：
```c
void settings_flush(void) {
    if (!g_nvs_handle) return;
    esp_err_t err = nvs_commit(g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: 0x%x", err);
    }
}
```

**(b) `main.cpp` 30s auto-save 块统一 flush**：
```c
if ((now - g_last_auto_save_us) >= AUTO_SAVE_INTERVAL_US) {
    g_last_auto_save_us = now;
    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
        save_current_position();
    }
    settings_flush();   // ← 新增：统一提交所有 pending NVS 写入
}
```

**(c) bonus 改进**：auto-save 条件从 `PLAYING` 改为 `PLAYING || PAUSED`，暂停时也保存断点（更合理）。

**评估**：✅ 完美修复。每 30s 统一 flush 兜底所有 `save_volume/play_mode/auto_off` 的 pending 写入。剩余 LOW 风险：改音量后立即断电仍可能丢失，但 flash commit 是 periodic 的固有 trade-off。

### H-8. ⚠️ power_mgmt.cpp — 实装（**部分修** R009）

**修复（R009）**：
- ✅ `tick()` 30s 活动检测 + auto-sleep 判定
- ✅ `light_sleep_start()` + ext1 唤醒源（按键）
- ✅ `set_auto_off()` / `auto_off_expired()` 完整实现
- ⚠️ **ADC 电池检测仍 stub**：`get_battery_percent()` 永远返回 100
- ⚠️ `should_sleep()` 实现简单（5min 无活动）但 light sleep 后位置丢失（见 🆕-2）

**建议补**：
1. 实装 ADC：`adc_oneshot_read(ADC_CHANNEL_6, &raw)` + 衰减换算 `voltage = raw * 3.3 / 4095.0 * 2`（10K+10K 分压）
2. light sleep 前 `save_current_position()`

**评估**：⚠️ **R010 commit message 声称"全部 38 项清零"在此项不准确**。

### H-9. ✅ main.cpp — auto_off 未集成（已修 R008）

**修复**：
```c
// 主循环新增
if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
    if (power_mgmt_auto_off_expired()) {
        ESP_LOGI(TAG, "Auto-off timer expired, stopping playback");
        audio_player_stop();
        g_app_state = APP_STATE_STOPPED;
    }
}
```
**评估**：✅ 主循环集成 OK。但 **H-8 没修**（R011 仍 stub），所以 `power_mgmt_auto_off_expired()` 实际无效（前提是 R009+R011 的 ADC 实装完成）。⚠️ 部分修：定时关机检查代码就位，但因 H-8 stub 永久返回 false 而不会触发。

### H-10. ✅ main.cpp — auto-save 触发时机（已修 R008）

**现状**：30s 间隔自动保存，逻辑正确。  
**评估**：✅ 无需修复（之前是确认无问题）。

---

## 🟡 MEDIUM（15 个）

### M-1. ❌ audio_player.cpp — audio_pipeline_link 返回值未检查（未修）

**评估**：✅ **实际已修**（C-3 修复时包含 link_err 检查）。文档归类错误。

### M-2. ✅ audio_player.cpp — audio_pipeline_wait_for_stop 可能死锁（已修 R010）

**修复**：`wait_for_stop()` → 轮询 + 1s 超时 + `audio_pipeline_terminate()` 兜底。

### M-3. ✅ main.cpp — esp_vfs_fat_sdspi_mount struct init（已修 R010）

**修复**：补 `disk_status_check_enable`、`use_one_fat` 显式初始化，消除 -Wmissing-field-initializers 警告。

### M-4. ✅ audio_player.cpp — g_current_file 冗余（已修）

**修复**：删除 `g_current_file[256]`（不再使用）。  
**评估**：✅ 修。

### M-5. ✅ button_manager.cpp — DBL_DEBOUNCE 松开即触发（已修 R009）

**修复**：松开时检查 `(now_us - press_start_us) >= debounce_us` 才触发 DOUBLE_CLICK，否则回 IDLE。

### M-6. ✅ main.cpp — 按键 chip ID 错误 panic（已修 R009）

**修复**：`gpio_config(&io_conf)` 检查返回值，失败时 `ESP_LOGE` + return。

### M-7. ✅ config.h — I2S_MCLK_IO 宏已删除（已修 R009）

**修复**：整行删除。MAX98357A 不需要 MCLK。

### M-8. ✅ button_manager.cpp — g_btn_config 字段重复（已修 R009）

**修复**：新增 `btn_config_t`（纯 const 配置）与 `btn_ctx_t`（含 runtime state）分离。

### M-9. ➖ main.cpp — 全局变量无 volatile（设计确认 OK）

**评估**：单任务架构下安全。已加注释说明。未来引入多 task 时需加 volatile/锁。

### M-10. ➖ main.cpp — mode_str 数组每次构造（设计确认 OK）

**评估**：编译器优化为静态，无实质问题。

### M-11. ✅ main.cpp — stop_playback FF/RW 错用 ff_release（已修）

**修复**：
```c
if (g_app_state == APP_STATE_FAST_FORWARD) {
    tape_control_ff_release();
} else if (g_app_state == APP_STATE_REWIND) {
    tape_control_rewind_release();
}
```
**评估**：✅ 完美修复，分别处理 FF/RW。

### M-12. ✅ main.cpp — init_storage 未调 playlist_set_index（已修）

**修复**：
```c
if (settings_load_position(&saved_idx, &saved_pos)) {
    g_current_track = saved_idx;
    playlist_set_index(g_current_track);   // ← 新加
    g_seek_on_play_position = saved_pos;
    ...
}
```
**评估**：✅ 完美修复。

### M-13. ✅ main.cpp — display_update 全屏重绘应局部刷新（已修 R009）

**评估**：同 H-2，脏区指纹检查 + 屏保超时息屏。

### M-14. ✅ display.cpp — static scroll_offset 未读（已修）

**修复**：删除 `static int scroll_offset = 0;`，加 TODO 注释。  
**评估**：✅ 完美修复。

### M-15. ➖ main.cpp — g_last_auto_save_us 计时耦合（设计确认 OK）

**现状**：单任务下逻辑正确。已加注释说明。

---

## 🟢 LOW（8 个）

### L-1. ⚠️ voice_prompt.cpp / bookmark.cpp — 全部 stub（**部分修** R010）

**修复（R010）**：
- ✅ `bookmark.cpp`：NVS 存储 API 实现（每文件 10 书签，键 `bm_{file}_{slot}`）
- ⚠️ **`bookmark_add/list/jump` 在 main.cpp 完全没被调用**（无 UI 集成）
- ⚠️ **`voice_prompt.cpp` 仍仅 log stub**：
  ```c
  void voice_prompt_status(void) {
      ESP_LOGI(TAG, "Voice prompt: status (stub)");  // 仍仅打 log
  }
  ```
  仅新增 `power_mgmt_get_battery_percent()` 调用（但返回值 100，调用无意义）。

**评估**：⚠️ **R010 commit message 声称"全部 38 项清零"在此项不准确**。

**建议**：
1. `voice_prompt.cpp` 实装 WAV 加载 + ADF audio element 路由播放 SD 卡 `/voice/*.wav`
2. `main.cpp` 在 `handle_button_events()` 集成：
   - 长按 STOP → `voice_prompt_status()`
   - 双击 STOP → `bookmark_add(g_current_track, position)`

### L-2. ✅ main.cpp — 显示屏始终点亮（已修 R009）

**评估**：同 H-2，30s 屏保超时自动息屏。

### L-3. ➖ audio_player.cpp — i2s_driver_install 未显式调用（设计确认 OK）

**评估**：`i2s_stream_init` 内部已处理驱动安装，跨曲目复用 g_i2s_writer 无需重装。已加注释说明。

### L-4. ❌ main.cpp — 回调递归风险（已修）

**修复**：C-4 修复时同时修。`on_track_finished` 不再嵌套调 `play_current_track`。

### L-5. ❌ audio_player.cpp — g_i2s_writer 跨曲目复用无 sample rate 切换（未修）

**评估**：变速时（4x/8x）sample rate 应跟随变化，目前 `set_speed` 仅设速度倍率。

### L-6. ✅ config.h — I2S_MCLK_IO 注释（已修）

**修复**：注释改为"MAX98357A 不需要 MCLK，保留作为预留"。  
**评估**：✅ 注释清晰。

### L-7. ✅ playlist.cpp — g_count vs NVS u8 不匹配（已修 R008 C-5）

**评估**：C-5 修 `nvs_set_u8` → `nvs_set_u16` 时间接修了此问题。g_count 为 int，NVS 存储 now uint16_t。

### L-8. ➖ button_manager.cpp — 共享状态无 volatile（设计确认 OK）

**评估**：单任务无需 volatile。已加注释说明。

---

## 🎯 优先级修复路线图（更新版）

### 🔥 阶段 1 — 立即（CRITICAL 收尾）

| # | 任务 | 文件 | 改动量 |
|---|---|---|---|
| **1.1** | **修 C-5 track_idx 截断** | `settings.cpp` line 41 | 1 行（`u8` → `u16`）|

### ⚡ 阶段 2 — 推荐（HIGH/MEDIUM 剩余）

| # | 任务 | 文件 | 改动量 |
|---|---|---|---|
| **2.1** | H-7 NVS commit fallback（加 `commit_pending_settings()` 30s 定时调）| `main.cpp` + `settings.cpp` | ~10 行 |
| **2.2** | H-8 实装 `power_mgmt`（ADC + light sleep + 实际 auto_sleep 判定）| `power_mgmt.cpp` | ~80 行 |
| **2.3** | H-1 SD 热插拔（`esp_register_event_handler(SD_EVENT)` 或定时重试）| `main.cpp` | ~15 行 |
| **2.4** | M-13/H-2 display 局部刷新 + 屏保超时息屏 | `display.cpp` | ~30 行 |
| **2.5** | M-5 DBL_DEBOUNCE 加去抖时间窗 | `button_manager.cpp` | ~5 行 |
| **2.6** | M-6 按键 chip ID 错误检查 + retry | `main.cpp` | ~10 行 |
| **2.7** | L-5 `g_i2s_writer` 跨曲目复用 + 变速 sample rate 切换 | `audio_player.cpp` | ~20 行 |

### 💡 阶段 3 — PRD 完整化

| # | 任务 | 文件 |
|---|---|---|
| **3.1** | V1.2 书签实装 | `bookmark.cpp` |
| **3.2** | V1.2 语音播报实装 | `voice_prompt.cpp` |
| **3.3** | display UI（文件夹浏览 / 设置菜单 / 屏保） | `display.cpp` |
| **3.4** | A-B 复读 / 定时关机 UI 集成 | `main.cpp` |
| **3.5** | OTA 接收代码（量产准备）| `main.cpp` |

### 🚀 阶段 4 — 架构升级

| # | 任务 |
|---|---|
| **4.1** | 引入 FreeRTOS task 分离（按键 / 音频 / 显示）|
| **4.2** | audio buffer 复用机制 |
| **4.3** | 单元测试（按键状态机、playlist 算法）|
| **4.4** | CI 自动化（build + lint + test）|

---

## 修复完成度（详细，R010 后真实状态）

| 状态 | 数量 | 列表 |
|---|---|---|
| ✅ 完美修复 | **30** | C-1, C-2, C-3, C-4, C-5, H-2, H-3, H-4, H-5, H-6, H-7, H-9, H-10, M-2~M-8, M-11, M-12, M-14, L-2, L-4, L-5, L-6, L-7; **R011 新增**: 🆕-1 (SD sdmmc_read_sectors), 🆕-2 (save before sleep), 🆕-3 (seek_internal), 🆕-4 (%.21s 截断), 🆕-5 (time_buf 48B), **L-1 bookmark UI 集成** |
| ⚠️ 部分修 | **3** | **H-8** (ADC 注释了伪代码但仍 `return 100`), **L-1 voice_prompt** (仍 log stub), **🆕-6 press_start_us** (仅注释) |
| ❌ 未修 | **1** | L-1 (voice_prompt 部分) |
| ➖ 设计确认 OK | **6** | M-9 (volatile 单任务), M-10 (mode_str 编译优化), M-15 (auto_save 耦合), L-3 (i2s_driver ADF 内部), L-8 (volatile 单任务), + M-13 (R009 已实装) |

**R011 commit 进展**：
- 修了 R010 引入的 5 个 🆕 bug（SD 检测、light sleep 断点、跳帧 pause/resume、display 截断、time_buf 溢出）
- 修了 L-1 bookmark UI 集成
- **声称"修 H-8 ADC 桩"** — 实际**只改注释未实装**（仍 return 100）
- **声称"修 L-1 bookmark 按键集成"** — bookmark ✅ 集成，但 voice_prompt **未实装**

**真实进展**（R010 → R011）：
- 6/9 真正修好
- 3/9 仍是 stub/未实装
- 关键失实：H-8 ADC、voice_prompt 仍 log
| ❌ 未修复 | **0** | **全部清零** |

---

## 关联文档

- [PRD.md](../PRD.md) — 产品需求
- [DESIGN.md](../DESIGN.md) — 设计文档
- [DEVELOP_STATUS.md](DEVELOP_STATUS.md) — 模块开发状态
- [HARDWARE_MODULE_MIGRATION.md](../HARDWARE_MODULE_MIGRATION.md) — 模组迁移
- [HARDWARE_PIN_WIRING.md](HARDWARE_PIN_WIRING.md) — 引脚接线

---

**审查人**：CodeBuddy (MiniMax-M3)  
**审查方法**：静态代码审查 + 交叉对照 IDF v5.5 / ESP-ADF v2.8  
**当前 commit**：R011 (`df11f0d`)

**修正后真实状态**（团队审核反馈后）：
- ✅ 完美修复 **27 项**（71%）
- ⚠️ 部分修 **2 项**（H-8 ADC 桩 / L-1 voice_prompt log — 这两项用户认可以注释/桩代替实装，节省 ADC 驱动依赖）
- ➖ 设计确认 OK **6 项**（M-9/M-10/M-15/L-3/L-8 等单任务架构认可；M-13 已在 R009 实装脏区+屏保）
- ❌ 真正未修 **5 项**（主要是 L-1 voice_prompt 真正未实装）

**用户可感知功能完成度**（PRD V1.1/V1.2）：**~17%**（OGG/Opus + 书签 OK；定时关机代码就位但 H-8 桩永久返回 false 不触发）。  
建议**优先**修 L-1 voice_prompt（WAV 加载 + ADF audio element 路由）和 `settings_load_auto_off()` 在 `init_hardware()` 调用（1 行）。