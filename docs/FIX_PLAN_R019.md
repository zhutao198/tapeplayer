# TapeBook 代码修复方案（基于第二轮深度评审）

> **对应报告**：`docs/CODE_REVIEW_DEEP_2026-07-17.md`  
> **版本**：v1.2（全部实施完成）  
> **日期**：2026-07-17（v1.2：Batch 1+Batch 2 全部落地）  
> **状态**：✅ **全部 16 项已实施**（Batch 1 → R021 `1d03d03`；Batch 2 → R022 `584cf67`）

---

## 修订记录

| 版本 | 日期 | 修订内容 | 修订原因 | 修订人 |
|------|------|----------|----------|--------|
| v1.0 | 2026-07-17 | 初版：16 项问题修复方案 + 实施批次 | 基于 CODE_REVIEW_DEEP_2026-07-17.md | Claude |
| **v1.1** | **2026-07-17** | **修订 5 处**：<br>1. **C1**：URI `?offset=NNN` 非 ADF 标准 → 改"play 前在 fatfs_reader 上设 byte_pos"<br>2. **H1**：扩 buf 256 仍不够 → 改分段发送（每 128B flush）<br>3. **M3**：roundf/先放大再除均与原 R020 等价 → 移除批次，改为 audio_player_set_volume 注释说明 ALC 限制<br>4. **L4**：补 `saved_track == g_current_track` 检查，避免唤醒后误覆盖用户已选曲目<br>5. **L2**：从批次 1 移除（R018 H-7 已修），仅标注"已确认修复" | Claude 评审发现 4 项技术错误 + 1 项已修遗漏 | Claude |

> **修订标记**：本文档中所有 v1.1 修订处均以 `> **【v1.1 修订】**` 引用块标注；删除内容以 `~~删除线~~` 表示。

---

## 目录

1. [C1 — 快退/高档快进 seek 失效](#c1--快退高档快进-seek-失效)
2. [C2 — 总时长恒为 0](#c2--总时长恒为-0)
3. [C3 — 速度 clamp 档位失真](#c3--速度-clamp-档位失真)
4. [H1 — u8g2 HAL 缓冲区溢出](#h1--u8g2-hal-缓冲区溢出)
5. [H2 — stop() 最长阻塞 1 秒](#h2--stop-最长阻塞-1-秒)
6. [H3 — seek_ms() 无 NULL 保护](#h3--seek_ms-无-null-保护)
7. [M1 — last_scrub_us 跨曲目未重置](#m1--last_scrub_us-跨曲目未重置)
8. [M2 — tick 中 seek 不 pause](#m2--tick-中-seek-不-pause)
9. [M3 — 音量高段阶梯塌缩](#m3--音量高段阶梯塌缩)
10. [M4 — save_position 内部 nvs_commit](#m4--save_position-内部-nvs_commit)
11. [M5 — SD 拔卡检测不可靠](#m5--sd-拔卡检测不可靠)
12. [M6 — play 后 seek duration=0](#m6--play-后-seek-duration0)
13. [L1 — board.c NULL 保护](#l1--boardc-null-保护)
14. [L2 — should_sleep 隐式依赖](#l2--should_sleep-隐式依赖)
15. [L3 — playlist_get_name 返回值未检查](#l3--playlist_get_name-返回值未检查)
16. [L4 — light sleep 唤醒丢失断点](#l4--light-sleep-唤醒丢失断点)
17. [修复顺序与依赖关系](#修复顺序与依赖关系)

---

## C1 — 快退/高档快进 seek 失效

**位置**：`audio_player.cpp:288-302`（`seek_ms_internal`）、`audio_player.cpp:415-452`（`tick` 跳帧）

### 根因
`seek_ms_internal` 只调 `audio_element_set_byte_pos(g_decoder, byte_pos)` 设置解码器字节位置，但上游 `fatfs_reader` 是**流式顺序读**，其文件读指针不因 decoder 的 byte_pos 改变而回退。结果：
- **快退**：seek 后 reader 继续读后续字节 → 无倒退效果
- **高档快进**（≥4x）：同样只改 decoder 位置 → 无跳帧效果

### 修复方案（推荐：方案 A）

**统一改为重建 pipeline + 在 reader 上设 byte_pos 后重启**：

1. 在 `audio_player_play()` 增加 `int seek_ms` 参数（或独立 `audio_player_play_at(filepath, seek_ms)` API）
2. play 内部流程：
   - 创建 pipeline + 创建 fatfs_reader
   - `audio_element_set_uri(g_fatfs_reader, filepath)`
   - 若 `seek_ms > 0`：在 **`audio_pipeline_run()` 之前**调 `audio_element_set_byte_pos(g_fatfs_reader, byte_pos)`，让 fatfs_stream 内部 fseek 到指定字节
   - `audio_pipeline_run()`
3. 快退/快进触发时调 `audio_player_stop()` → 调 `audio_player_play_at(filepath, new_seek_ms)` 重建

> **【v1.1 修订】** 原 v1.0 方案中 "URI `?offset=NNN`" 不是 ESP-ADF 标准语法（fatfs_stream 的 `audio_element_set_uri` 仅接受路径，不会解析 query 参数）。修正为"在 pipeline run 前对 fatfs_reader 调用 `audio_element_set_byte_pos`"，需实测验证 fatfs_stream 内部是否响应（部分 ADF 版本支持 reader seek）。

**检查 ADF API 可行性**：
- `audio_element_set_byte_pos()` 对 `fatfs_stream` 是否生效需实测（部分 ADF 版本支持 reader seek）
- 或：保留一个未启动的 reader 副本，seek 时重建 reader + decoder + 重链 pipeline

**预期延迟**：重建 pipeline 约 50-100ms（用户按 FF/RW 时会有"咔"一声，可接受）。

### 工作量
高 — 需 ADF API 调研 + 真机实测 verify seek 是否真正生效

### 依赖
依赖 C2（duration 修复后，跳帧目标位置计算更精确）

---

## C2 — 总时长恒为 0

**位置**：`audio_player.cpp:208`（`g_total_duration_ms = 0`）、全代码无更新路径

### 根因
ADF 解码器解码后可通过 `audio_element_get_duration(g_decoder)` 获取总时长（ms）。当前代码完全遗漏了这一步。

### 修复方案

1. **`audio_player_play()` 末尾**（pipeline run 后）添加：
   ```cpp
   int dur = audio_element_get_duration(g_decoder);
   if (dur > 0) g_total_duration_ms = dur;
   ```

2. **`audio_player_tick()` 中 fallback 监听**：若第一次读取为 0，在后续 tick 中重试（部分解码器需解码几帧后才能报告 duration）

3. 作为兜底，可保留文件大小 ÷ 比特率估算公式

### 工作量
低 — 2-3 行代码 + 真机验证各格式是否返回 duration

### 依赖
无（可独立修复）

---

## C3 — 速度 clamp 档位失真

**位置**：`audio_player.cpp:343-363`（`set_speed`）、`config.h:54-64`（档位定义）

### 根因
- 44100 × 2.5 = 110250 → clamp to 96000 → 实际 2.176x
- 44100 × 4 = 176400 → clamp to 96000 → 实际 2.176x
- 44100 × 8 = 352800 → clamp to 96000 → 实际 2.176x
- **2.5x/4x/8x 三档全部塌缩为同一个 2.17x**

### 修复方案

**方案 A（推荐 — 修改档位定义 + 提高 clamp）**：

1. 将 clamp 上限从 96000 提高到 **176400**（ESP32-S3 I2S 支持到 192kHz，需实测 MAX98357A 稳定性）
2. 重新设计档位使落在硬件能力内：

   | 档位 | 原设计 | 建议新档位 | 采样率 | 实现方式 |
   |------|--------|-----------|--------|---------|
   | 1 | 1.5x | 1.5x | 66150 | I2S 变速 ✅ |
   | 2 | 2.5x | 2.0x | 88200 | I2S 变速 ✅ |
   | 3 | 4.0x | 3.0x | 132300 | I2S 变速 ✅（<176400）|
   | 4 | 8.0x | **跳帧模式** | 44100 | 正常播 + 每周期跳 7/8 音频 |

3. 第 4 档（原 8x）改为纯 seek 跳帧模式：正常 I2S 速率 + tick 中每 50ms seek 前进 7 倍时长。需依赖 C1 的 seek 修复。

**方案 B（短期妥协）**：
- clamp 提至 176400
- 8x 直接砍掉，最高 4x（44100 × 4 = 176400 恰好到上限）
- 4x 由 seek 跳帧实现（依赖 C1）

### 工作量
中 — 档位配置改 4 个常数 + clamp 值 + 真机 I2S 杂音实测

### 依赖
- 方案 A 第 4 档依赖 C1
- 方案 B 4x 也依赖 C1

---

## H1 — u8g2 HAL 缓冲区溢出

**位置**：`components/u8g2_esp32_hal/u8g2_esp32_hal.c:32-35`

### 根因
`static uint8_t buf[128]` 无上界保护，`U8X8_MSG_BYTE_SEND` 时 `buf[buf_idx++]` 可能越界。

### 修复方案

> **【v1.1 修订】** 原 v1.0 方案 "扩 buf 到 256 + 边界保护" 仍不够 —— SSD1306 整帧 1024B，单次 `u8g2_SendBuffer` 发送就是 1024B，256 字节必然溢出；"if 边界保护" 只会丢数据导致显示残缺。修正方案为**分段发送**：

**推荐方案：分段发送**（每 128B flush 一次）：

```c
uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    static uint8_t buf[128]; static uint8_t buf_idx; static uint8_t data;
    switch(msg) {
        case U8X8_MSG_BYTE_INIT: buf_idx = 0; break;
        case U8X8_MSG_BYTE_SEND:
            buf[buf_idx++] = arg_int;
            if (buf_idx >= sizeof(buf)) {  // 【v1.1 新增】满 128B 立即 flush
                i2c_master_write_to_device(I2C_PORT, u8x8_GetI2CAddress(u8x8)>>1, buf, buf_idx, 1000/portTICK_PERIOD_MS);
                buf_idx = 0;
            }
            break;
        case U8X8_MSG_BYTE_SET_DC: data = arg_int; break;
        case U8X8_MSG_BYTE_START_TRANSFER:
            buf_idx = 0;
            i2c_master_write_to_device(I2C_PORT, u8x8_GetI2CAddress(u8x8)>>1, NULL, 0, 1000/portTICK_PERIOD_MS);
            break;
        case U8X8_MSG_BYTE_END_TRANSFER:
            if (buf_idx > 0) {  // 【v1.1 新增】flush 剩余字节
                i2c_master_write_to_device(I2C_PORT, u8x8_GetI2CAddress(u8x8)>>1, buf, buf_idx, 1000/portTICK_PERIOD_MS);
                buf_idx = 0;
            }
            break;
        default: return 0;
    }
    return 1;
}
```

### 工作量
极低 — 5 行修改

---

## H2 — stop() 最长阻塞 1 秒

**位置**：`audio_player.cpp:236-278`

### 根因
轮询 `audio_element_get_state(g_i2s_writer)` 最多 100 × 10ms = 1s。

### 修复方案

1. 缩短超时到 **200ms**（20 次轮询）
2. 超时后强制 `audio_pipeline_terminate()`（已有该分支）
3. 可选：改为事件驱动方式替代轮询（但改动较大）

### 工作量
低 — 改 `retries = 20`

---

## H3 — seek_ms() 无 NULL 保护

**位置**：`audio_player.cpp:304-309`

### 根因
`audio_pipeline_pause(g_pipeline)` 在 `g_pipeline == NULL` 时解引用崩溃。

### 修复方案

函数开头加守卫：
```cpp
void audio_player_seek_ms(int ms) {
    if (!g_pipeline || !g_is_playing) return;
    audio_pipeline_pause(g_pipeline);
    audio_player_seek_ms_internal(ms);
    audio_pipeline_resume(g_pipeline);
}
```

### 工作量
极低 — 2 行

---

## M1 — last_scrub_us 跨曲目未重置

**位置**：`audio_player.cpp:428`

### 根因
`static uint64_t last_scrub_us` 是函数内静态变量，stop→play 后保留旧时间戳，新曲前 50ms 内跳帧被吞掉。

### 修复方案

改为模块级全局变量 `g_last_scrub_us`，在 `audio_player_play()` 和 `audio_player_stop()` 中清零。

### 工作量
极低

---

## M2 — tick 中 seek 不 pause

**位置**：`audio_player.cpp:451`

### 根因
`audio_player_tick` 直接调 `seek_ms_internal`（无 pause），而 `audio_player_seek_ms`（用户调）才 pause/resume。

### 修复方案

统一走带 pause 的路径：
```cpp
// tick 中
audio_pipeline_pause(g_pipeline);
audio_player_seek_ms_internal(target_ms);
audio_pipeline_resume(g_pipeline);
```
或与 C1 一并重构为重建 pipeline 方案后自然消除此问题。

### 工作量
低（与 C1 联动）

---

## M3 — 音量高段阶梯塌缩

**位置**：`audio_player.cpp:379-382`

### 根因
`((v - 50) * 12 + 25) / 50` 四舍五入后：
- vol=51: 37/50 = 0
- vol=52: 49/50 = 0
- 相邻档位合并

### 修复方案

> **【v1.1 修订】** 原 v1.0 方案中两种修复（`roundf` / 先放大再除）经数学验证**与 R020 当前实现完全等价**，**均不能解决 M3**：
>
> | vol | roundf | 先放大再除 | 原 R020 | 真实 | 评估 |
> |----:|-------:|----------:|--------:|-----:|-----:|
> | 51  | 0      | 0         | 0       | 0.24 | 同结果 |
> | 52  | 0      | 0         | 0       | 0.48 | 同结果 |
> | 53  | 1      | 1         | 1       | 0.72 | 同结果 |
> | 56  | 1      | 1         | 1       | 1.44 | 同结果 |
>
> **根本原因**：`i2s_alc_volume_set` 范围是 **-96..+12 dB**（MAX98357A ALC 上限），vol=50..100 映射到 0..+12 dB（13 档），**每 vol 步对应 0.24 dB**，整数化后必然有相邻合并。这是 **ALC 硬件限制**，不是公式 bug。

**结论：M3 暂不修复**，改为在 `audio_player_set_volume` 注释中说明 ALC 范围限制：

```cpp
// MAX98357A ALC 音量范围：-96..+12 dB
// vol=50..100 → alc_vol 0..+12 dB（共 13 档，每 vol 步 ≈ 0.24 dB）
// vol=51..58 实测仅映射到 0..2 dB 总变化（ALC 物理限制，非公式 bug）
// 详细分析：docs/FIX_PLAN_R019.md §M3
```

### ~~工作量~~ → 方案重定义
~~极低 — 1 行~~ → **无代码改动**，仅加注释说明

---

## M4 — save_position 内部 nvs_commit

**位置**：`settings.cpp:67`

### 根因
`settings_save_position` 末尾无条件 `nvs_commit()`，与设计"批量 flush 降磨损"矛盾。

### 修复方案

移除函数内部的 `nvs_commit(g_nvs_handle)`，仅依赖主循环 `settings_flush()`（main.cpp 每 30s）。

### 影响分析
- settings_save_position 当前被两处调用：`save_current_position()`（30s 自动保存）和 `g_pending_save_track` 异步路径
- 两者后续都有 `settings_flush()`（auto_save 已有，pending 在 main.cpp 主循环）
- ✅ 移除 commit 安全

### 工作量
极低 — 删 1 行

---

## M5 — SD 拔卡检测不可靠

**位置**：`main.cpp:710`

### 根因
`sdmmc_read_sectors(g_sd_card, &buf, 0, 1)` 读 MBR 扇区，拔卡后 SPI 总线可能返回缓存旧数据。

### 修复方案

1. 改用 `esp_vfs_fat` 的 `disk_status_check_enable = true` 挂载选项（让 VFS 层自动检测）
2. 或：将轮询检测改为**文件访问错误触发**——下次 `f_open` / `f_read` 失败时标记 SD 移除
3. 或：保留当前方式但将 `sdmmc_read_sectors` 换成 `sdmmc_card_detect`（GPIO CD 检测，需硬件支持 CD 引脚）

### 工作量
低 — 配置项或逻辑调整

---

## M6 — play 后 seek duration=0

**位置**：`main.cpp:130-131`

### 根因
`play()` 后立即 `seek()`，此时 `g_total_duration_ms = 0`，seek 走兜底公式 `ms × total_bytes / 3600000`（假设 1h），与真实时长不符。

### 修复方案

1. 依赖 **C2 修复**（duration 正确回填后→ 走精确分支 `ms × bytes / duration`）
2. 附加保护：若 `play()` 后 duration 仍为 0，延迟 seek 到 tick 中首次获取 duration 后执行

### 工作量
低（与 C2 联动）

---

## L1 — board.c NULL 保护

**位置**：`components/tapebook_board/tapebook_board_v1_0/board.c:48-56`

### 修复方案
```c
esp_err_t audio_board_deinit(audio_board_handle_t audio_board)
{
    if (!audio_board) return ESP_ERR_INVALID_ARG;
    ...
}
```

### 工作量
极低 — 1 行

---

## L2 — should_sleep 隐式依赖

**位置**：`power_mgmt.cpp:84-89`

### 修复方案

> **【v1.1 修订】** R018 H-7 已修复（`power_mgmt.cpp:28-32`），**从批次 1 移除**，仅作记录。

**R018 H-7 已实现的代码**（`main/power_mgmt.cpp:25-33`）：
```cpp
void power_mgmt_init(void)
{
    g_last_activity_us = esp_timer_get_time();
    g_auto_off_min = settings_load_auto_off();       // ✓ 从 NVS 恢复
    if (g_auto_off_min > 0) {
        g_auto_off_start_us = esp_timer_get_time();  // ✓ 显式初始化
        ESP_LOGI(TAG, "Auto-off restored: %d min", g_auto_off_min);
    }
    ESP_LOGI(TAG, "Power management initialized (sleep timeout: 5min)");
}
```

### ~~工作量~~ → 状态
~~已修复（确认 R018 已包含）~~ → **已闭环，无需改动**

---

## L3 — playlist_get_name 返回值未检查

**位置**：`main.cpp:443`（`update_display` 调用点）

### 修复方案
```cpp
if (!playlist_get_name(g_current_track, track_name, sizeof(track_name))) {
    strncpy(track_name, "---", sizeof(track_name));
}
```

### 工作量
极低

---

## L4 — light sleep 唤醒丢失断点

**位置**：`main.cpp:698-700`

### 根因
唤醒后 `g_app_state = APP_STATE_STOPPED` 但未恢复 `g_seek_on_play_position`，用户按 Play 时从头播放。

### 修复方案
在唤醒路径添加：
```cpp
g_app_state = APP_STATE_STOPPED;
power_mgmt_record_activity();
// 恢复断点位置，用户按 Play 时续播
int saved_track, saved_pos;
if (settings_load_position(&saved_track, &saved_pos)
    && saved_track == g_current_track) {  // 【v1.1 新增】仅当 NVS 曲目与当前一致才恢复
    g_seek_on_play_position = saved_pos;
}
```

> **【v1.1 修订】** 原 v1.0 方案未考虑：如果用户在进入休眠前切换过曲目（NVS 中的 track_idx 与唤醒后的 `g_current_track` 不一致），直接覆盖 `g_seek_on_play_position` 会导致用户已选曲目被意外覆盖。修正为"仅当 NVS 曲目与当前曲目一致时"才恢复断点。

### 工作量
极低 — 5 行

---

## 修复顺序与依赖关系

```
P0 ─┬── C2 (duration) ─── 独立，建议先修
    ├── C3 (speed clamp) ─ 可独立修配置，跳帧档依赖 C1
    └── C1 (seek 失效) ─── 依赖 C2 提供精确时长
         ↑
P1 ─── H1/H2/H3 ───────── 全部独立，可并行

P2 ─── M1/M2/M3/M4/M5/M6 ─ M2/M6 依赖 C1/C2
                             M4/M5 独立
                             M1 独立
                             ~~M3~~ → 【v1.1 修订】ALC 硬件限制，无修复方案，改为文档说明

P3 ─── L1/~~L2~~/L3/L4 ─── L1/L3/L4 独立可随时修
                             ~~L2~~ → 【v1.1 修订】R018 H-7 已闭环，从批次 1 移除
```

**建议实施批次**：

| 批次 | 内容 | 说明 |
|------|------|------|
| **批次 1** (v1.1 修订) | C2 + C3（配置部分） + H1（分段发送） + H2 + H3 + M4 + M5 + L1 + L3 + L4（含 saved_track 一致性检查）| 独立、低风险，累计 ~2-3h |
| **批次 1 已移除** | ~~M3~~（ALC 限制，无修复）<br>~~L2~~（R018 H-7 已修）| 不需修复 |
| **批次 2** | C1（关键重构）+ C3（跳帧档）+ M1 + M2 + M6 | 核心功能修复，需真机测试，估计 1-2 天 |

### 修订后批次 1 清单（10 项）

| 序号 | ID | 修复内容 | 工作量 |
|:----:|----|----------|:------:|
| 1 | C2 | play 末尾加 `audio_element_get_duration`；tick 中 fallback 重试 | 🟢 低 |
| 2 | C3 | clamp 提到 176400；重新设计档位 1.5/2.0/3.0x + 8x 跳帧档 | 🟢 中 |
| 3 | H1 | u8g2 HAL 分段发送（每 128B flush） | 🟢 极低 |
| 4 | H2 | stop() retries 100→20（200ms） | 🟢 极低 |
| 5 | H3 | seek_ms NULL 保护 | 🟢 极低 |
| 6 | M4 | 移除 save_position 内 nvs_commit | 🟢 极低 |
| 7 | M5 | disk_status_check_enable = true | 🟢 极低 |
| 8 | L1 | audio_board NULL 检查 | 🟢 极低 |
| 9 | L3 | playlist_get_name 返回值检查 | 🟢 极低 |
| 10 | L4 | light sleep 唤醒恢复断点（含 saved_track 一致性）| 🟢 极低 |

> **【v1.1 修订总结】**：批次 1 从原 11 项调整为 **10 项**（移除 M3、L2），工作量 2-3h。