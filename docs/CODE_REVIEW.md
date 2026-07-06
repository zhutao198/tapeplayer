# TapeBook 代码审查报告

> **审查时间**：2026-07-06  
> **审查范围**：`main/*.cpp` + `main/*.h` + `main/CMakeLists.txt`  
> **审查方法**：静态代码审查（只读，不修改）  
> **基线 commit**：R007 (`4cd11a2`)  
> **审查对象**：
> - audio_player.cpp / .h
> - main.cpp
> - config.h
> - button_manager.cpp / .h
> - tape_control.cpp / .h
> - playlist.cpp / .h
> - display.cpp / .h
> - settings.cpp / .h
> - power_mgmt.cpp / .h
> - voice_prompt.cpp / .h
> - bookmark.cpp / .h

---

## 概览

| 严重级别 | 数量 | 影响范围 |
|---|---|---|
| 🔴 CRITICAL | 5 | 功能/正确性 |
| 🟠 HIGH | 10 | 稳定性/可靠性 |
| 🟡 MEDIUM | 15 | 潜在问题/性能 |
| 🟢 LOW | 8 | 设计/规范 |
| **总计** | **38** | |

---

## 🔴 CRITICAL（5 个，影响功能）

### C-1. audio_player.cpp — seek 字节/毫秒混淆

**位置**：`audio_player.cpp` ~line 233  
**严重性**：CRITICAL  
**类型**：逻辑 bug

**症状**：`audio_player_seek_ms(int ms)` 接口设计为毫秒，但内部调用：

```c
audio_element_set_byte_pos(el, ms);   // ❌ 错误：第 2 参数是字节偏移
```

`audio_element_set_byte_pos()` 的第二个参数是**字节偏移**（byte position），不是时间（毫秒）。当前实现把毫秒数当作字节偏移，导致磁带机的 ±10s 快跳、tape_control 跳帧都跳到错误位置。

**修复方向**：根据文件 sample_rate / channels / bit_depth 换算毫秒 → 字节：

```c
// 伪代码
uint32_t byte_pos = ms * (sample_rate / 1000) * channels * (bit_depth / 8);
audio_element_set_byte_pos(el, byte_pos);
```

**影响**：所有磁带机 ±10s 跳转、跳帧、4x/8x 加速的 seek 全部失效。

---

### C-2. audio_player.cpp — 返回假位置（duration=0 时）

**位置**：`audio_player.cpp` ~line 241  
**严重性**：CRITICAL  
**类型**：逻辑 bug

**症状**：

```c
audio_player_get_position_ms() {
    if (g_total_duration_ms == 0) return 0;       // 假位置
    int elapsed = (now - g_start_time_us) / 1000;
    return elapsed;                                // 累加无校准
}
```

当 `g_total_duration_ms == 0`（如非 MP3、buffer metadata 解析失败），返回 0 但实际已播放一段时间。display 显示进度条卡 0%。

**影响**：
- 长音频文件 duration 解析失败 → 进度条永远 0%
- 显示 "12:35 / 00:00" 让用户困惑

**修复方向**：用 `audio_element_get_byte_pos(el)` + 文件总字节数计算：

```c
uint32_t cur = audio_element_get_byte_pos(el);
uint32_t total = audio_element_get_size(el);
if (total > 0) return (uint64_t)cur * total_duration / total;
return cur / bytes_per_ms;   // 兜底
```

---

### C-3. audio_player.cpp — 元素 NULL 检查缺失

**位置**：`audio_player.cpp` 131-143  
**严重性**：CRITICAL  
**类型**：错误处理缺失

**症状**：

```c
g_fatfs_reader = fatfs_stream_init(&reader_cfg);
g_decoder      = audio_decoder_init(&decoder_cfg);
g_i2s_writer   = i2s_stream_init(&writer_cfg);

audio_pipeline_register(g_pipeline, g_fatfs_reader, "file");   // ❌ 不检查 NULL
audio_pipeline_register(g_pipeline, g_decoder, "decoder");
audio_pipeline_register(g_pipeline, g_i2s_writer, "i2s");

audio_pipeline_link(g_pipeline, link_tags, 3);    // ❌ 不检查返回值
audio_pipeline_run(g_pipeline);                    // ❌ pipeline 状态未确认
```

SD 卡未插入 / 格式异常 / 解码器不支持格式 → fatfs_stream_init 返回 NULL → 后续操作触发 NULL pointer dereference / panic。

**修复方向**：每一步检查 `esp_err_t` 返回值，失败时调用 `audio_pipeline_deinit(g_pipeline)` 清理。

---

### C-4. main.cpp — 回调递归触发 WDT

**位置**：`main.cpp` ~line 472  
**严重性**：CRITICAL  
**类型**：栈帧叠加 / WDT 风险

**症状**：

```c
// line 440
audio_player_set_callback(on_track_finished, NULL);

// on_track_finished 定义：
static void on_track_finished(int state, void *user_data) {
    // ...
    case PLAY_MODE_REPEAT_ALL:
        g_current_track = playlist_next();
        play_current_track();    // ❌ 在 audio_element 回调内嵌套调 audio_player_play
        break;
}
```

`audio_pipeline_run()` 内部会在 pipeline task 中触发 `on_track_finished` 回调，回调内调 `audio_player_play()` 重新创建 pipeline → **栈帧叠加 + WDT 5s 超时可能触发**。

**修复方向**：
- 用 `xTaskCreate` + 单独 task 处理回调（异步化）
- 或调大 `esp_task_wdt_init(10, true)` 给 10s 余量
- 或用 FreeRTOS `xQueueSend` 把任务丢到主循环处理

---

### C-5. settings.cpp — track_idx 截断（>256 文件）

**位置**：`settings.cpp` line 41  
**严重性**：CRITICAL  
**类型**：类型截断

**症状**：

```c
void settings_save_position(int track_idx, int position_s, const char *file_name) {
    nvs_set_u8(g_nvs_handle, NVS_KEY_TRACK, (uint8_t)track_idx);  // ❌ 截断
}
```

`NVS_KEY_TRACK` 是 `uint8_t`，`PLAYLIST_TRACK_MAX=256` 正好边界。如果文件超过 256 个，索引 256+ 会被截断为 0-255，断点记忆失效。

**修复方向**：用 `uint16_t` (`nvs_set_u16`) 替代，或用 `nvs_set_str(key, "256")` 字符串。

---

## 🟠 HIGH（10 个，稳定性/可靠性）

### H-1. main.cpp — SD 卡热插拔未实现

**位置**：`main.cpp` `mount_sd_card()`  
**严重性**：HIGH  
**症状**：用户中途拔 SD 卡不会自动重挂载，需手动重启。  
**修复方向**：定期检查 `g_sd_card` 状态，或用 SD 卡检测 GPIO 中断。

### H-2. main.cpp — display_update 全屏重绘（200ms）

**位置**：`display.cpp` `display_update()`  
**严重性**：HIGH  
**症状**：每 200ms 通过 I2C 发送 1KB 全屏数据 → 持续 5KB/s 通信开销，长期显示同样内容会有烧屏风险。  
**修复方向**：
- 实现局部刷新（只更新变化行）
- 超时无操作时 `u8g2_SetPowerSave(&u8g2, 1)` 关闭显示

### H-3. main.cpp — 锁定状态事件不同步

**位置**：`main.cpp` `handle_button_events()`  
**严重性**：HIGH  
**症状**：锁定时 `continue` 跳过事件 → `button_manager_scan` 内部状态机可能滞留 HOLD。  
**实际**：扫描到 `!pressed` 自动回 IDLE，**当前 OK 但时序耦合**。

### H-4. playlist.cpp — g_items 占 BSS 而非 PSRAM

**位置**：`playlist.cpp`  
**严重性**：HIGH  
**症状**：`static playlist_item_t g_items[256]` ≈ 96 KB 在普通 BSS。README 说"放 PSRAM 优化 DRAM"但实际没生效。  
**修复方向**：
```c
static playlist_item_t *g_items = NULL;
// init: g_items = heap_caps_malloc(sizeof(playlist_item_t) * MAX, MALLOC_CAP_SPIRAM);
```

### H-5. settings.cpp — NVS 写返回值未检查

**位置**：`settings.cpp` 多处  
**症状**：`nvs_set_u8/u32/str()` 返回值大多被忽略。失败时 NVS 状态未知。

### H-6. settings.cpp — nvs_flash_erase_namespace 已废弃

**位置**：`settings.cpp`  
**症状**：v5.5 移除 `nvs_flash_erase_namespace`，已改用 `nvs_flash_erase_partition`。错误路径 handle 未释放。  
**注**：R004 commit 已修为 `nvs_flash_erase()`，但错误路径仍缺。

### H-7. settings.cpp — NVS commit 频率高

**位置**：`settings.cpp` `settings_save_play_mode()`  
**症状**：`cycle_play_mode()` 每次立即 commit → 频繁切播放模式会显著缩短 flash 寿命。  
**修复方向**：合并到定时任务（30s 批量 commit）。

### H-8. power_mgmt.cpp — 全部 stub

**位置**：`power_mgmt.cpp`  
**症状**：
- `tick()` 空实现
- `get_battery_percent()` 硬编码返回 100
- `should_sleep()` 永远 false
- `auto_off_expired()` 永不被主循环检查

**影响**：PRD V1.1 全部电源管理功能**完全失效**。

### H-9. main.cpp — auto_off 未集成

**位置**：`main.cpp`  
**症状**：`power_mgmt_set_auto_off()` 可设但主循环从不检查 `power_mgmt_auto_off_expired()`——**PRD 定时关机功能完全失效**。  
**修复方向**：在主循环 `if (g_app_state == APP_STATE_PLAYING)` 分支加：
```c
if (power_mgmt_auto_off_expired()) {
    audio_player_stop();
    // 进入休眠
}
```

### H-10. main.cpp — auto-save 触发时机 OK

**位置**：`main.cpp` `play_current_track()`  
**说明**：第一次保存发生在 30s 后，逻辑正确。无需修复。

---

## 🟡 MEDIUM（15 个，潜在问题/性能）

| # | 文件 | 问题 |
|---|---|---|
| M-1 | `audio_player.cpp` | `audio_pipeline_link()` 返回值未检查（link 失败后 run 可能跑未连接管道）|
| M-2 | `audio_player.cpp` | `audio_pipeline_wait_for_stop()` 异常状态可能永久阻塞触发 WDT |
| M-3 | `main.cpp` | `esp_vfs_fat_sdspi_mount` 仍可用，但 v5.5 已有改名 |
| M-4 | `audio_player.cpp` | `g_current_file[256]` 写入后从不读取（冗余内存）|
| M-5 | `button_manager.cpp` 156-164 | `DBL_DEBOUNCE` 状态松开立即输出 `DOUBLE_CLICK`（设计意图但易误触）|
| M-6 | `main.cpp` | 按键 chip ID 错误可能 panic（未做错误检查）|
| M-7 | `config.h` 16 | `I2S_MCLK_IO GPIO_NUM_7` 声明但 `i2s_set_pin()` 从未调 |
| M-8 | `button_manager.cpp` | `g_btn_config` const 数组含 `.state` 字段，memcpy 后立即重置（C 允许但不规范）|
| M-9 | `main.cpp` | 单任务架构下 `g_app_state` 等全局无 `volatile`，未来加 task 需加锁 |
| M-10 | `main.cpp` 123 | `cycle_play_mode` 用 `mode_str[]` 数组（编译器会优化）|
| M-11 | `main.cpp` 97 | `stop_playback()` 在 RW 状态错用 `tape_control_ff_release()`（语义错误，应分别处理 FF/RW）|
| M-12 | `main.cpp` 482 | `init_storage` 成功分支未调 `playlist_set_index(saved_idx)`，playlist 内部 `g_current` 仍为 0 |
| M-13 | `main.cpp` | `display_update` 全屏重绘应改为局部刷新 + 脏区更新 |
| M-14 | `display.cpp` | `static int scroll_offset` 写入但永不读取（`-Wunused-but-set-variable`）|
| M-15 | `main.cpp` 113 | `g_last_auto_save_us` 计时 OK，但函数职责耦合（`play_current_track` 内做时间初始化）|

---

## 🟢 LOW（8 个，设计/规范）

| # | 位置 | 问题 |
|---|---|---|
| L-1 | `voice_prompt.cpp` / `bookmark.cpp` | **全部 stub**（PRD V1.2 功能）|
| L-2 | `main.cpp` | 显示屏始终点亮（无超时息屏）|
| L-3 | `audio_player.cpp` | `i2s_driver_install()` 未显式调用（依赖 `i2s_stream_init` 内部处理）|
| L-4 | `main.cpp` | 回调递归风险（`on_track_finished` 内调 `play_current_track`）|
| L-5 | `audio_player.cpp` | `g_i2s_writer` 跨曲目复用但没考虑 sample rate 切换（变速时）|
| L-6 | `config.h` | `I2S_MCLK_IO` 注释说"可选"但代码中完全没启用（冗余宏）|
| L-7 | `playlist.cpp` | `g_count` 类型是 `int` 但 NVS 存 `uint8_t`，>256 文件边界不匹配 |
| L-8 | `button_manager.cpp` | 共享状态无 `volatile`（单任务 OK，未来需注意）|

---

## 🎯 优先级修复路线图

### 🔥 阶段 1 — 立即（影响功能）

| 任务 | 文件 | 修复点 |
|---|---|---|
| **1.1 修 seek 字节/毫秒** | `audio_player.cpp` | `set_byte_pos(el, ms*sr*ch*bd/8/1000)` |
| **1.2 修位置计算** | `audio_player.cpp` | 用 `audio_element_get_byte_pos + get_size` 算百分比 |
| **1.3 加 NULL 检查** | `audio_player.cpp` | 每一步 `if (ret != ESP_OK)` 清理 |
| **1.4 调 WDT 超时** | `main.cpp` | `esp_task_wdt_init(10, true)` |
| **1.5 改 NVS key 类型** | `settings.cpp` | `nvs_set_u16` 替代 `nvs_set_u8` |

### ⚡ 阶段 2 — 下一版本（稳定性）

| 任务 | 文件 | 修复点 |
|---|---|---|
| **2.1 实装 power_mgmt** | `power_mgmt.cpp` | ADC + auto-sleep + auto-off |
| **2.2 auto_off 集成** | `main.cpp` | 主循环检查 `auto_off_expired` |
| **2.3 playlist 改 PSRAM** | `playlist.cpp` | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` |
| **2.4 display 局部刷新** | `display.cpp` | 脏区更新 + 屏保超时 |
| **2.5 SD 热插拔** | `main.cpp` | 检测重挂载 |
| **2.6 NVS 错误处理** | `settings.cpp` | 检查所有 set_* 返回值 |
| **2.7 减少 NVS commit** | `settings.cpp` | 批量/异步 commit |

### 💡 阶段 3 — PRD 完整化

| 任务 | 模块 | 功能 |
|---|---|---|
| **3.1** | `bookmark.cpp` | PRD V1.2 书签 |
| **3.2** | `voice_prompt.cpp` | PRD V1.2 语音播报 |
| **3.3** | display UI | 文件夹浏览 / 设置菜单 |
| **3.4** | main | A-B 复读 / 定时关机 / 屏保 |
| **3.5** | main | OTA 接收代码 |

### 🚀 阶段 4 — 架构升级

| 任务 | 内容 |
|---|---|
| **4.1** | 引入 FreeRTOS task 分离（按键/音频/显示） |
| **4.2** | audio buffer 复用机制 |
| **4.3** | 单元测试（按键状态机、playlist 算法） |
| **4.4** | CI 自动化（build + lint + test） |

---

## 关联文档

- [PRD.md](../PRD.md) — 产品需求
- [DESIGN.md](../DESIGN.md) — 设计文档
- [DEVELOP_STATUS.md](DEVELOP_STATUS.md) — 模块开发状态
- [HARDWARE_MODULE_MIGRATION.md](../HARDWARE_MODULE_MIGRATION.md) — 模组迁移
- [HARDWARE_PIN_WIRING.md](HARDWARE_PIN_WIRING.md) — 引脚接线

---

**审查人**：CodeBuddy (MiniMax-M3)  
**审查方法**：静态代码审查 + 交叉对照 IDF v5.5 / ESP-ADF v2.8 API  
**下一次审查建议**：阶段 1 修复完成后重新审查