# TapeBook 开发状态对照 (vs PRD V1.x)

> 本文档对照 PRD (V1.0/V1.1/V1.2/V2.0) 跟踪每个功能模块的实现状态。
> 最后更新：2026-07-03

## 速查图

```
PRD V1.0 MVP    ████████░░  8/10 完成 (缺音频播放 + OLED + 文件夹浏览)
PRD V1.1 增强   ██░░░░░░░░  2/7  完成 (缺定时/A-B/提示音/变暗/EC11)
PRD V1.2 进阶   ░░░░░░░░░░  0/6  完成 (缺书签/语音/电量/菜单/固件升级)
PRD V2.0 扩展   ░░░░░░░░░░  0/5  完成 (蓝牙/录音/EQ/USB)
```

---

## V1.0 — MVP（最小可行产品）

| 功能 | PRD 章节 | 状态 | 实现位置 | 备注 |
|---|---|---|---|---|
| 播放/暂停 | FR-001 4.2.1 | ✅ 完成 | main.cpp + button_manager.cpp | |
| 停止（位置归零）| FR-001 4.2.2 | ✅ 完成 | main.cpp stop_playback | |
| 上一首/下一首 | FR-001 4.2.3 | ✅ 完成 | main.cpp handle_button_events | 循环 |
| 音量调节（按键）| FR-001 4.2.4 | ✅ 完成 | main.cpp 长按 Prev/Next | |
| 磁带机快进（4 档）| FR-002 4.2.5 | ⚠️ 部分 | tape_control.cpp | 控制层 OK，audio 速度切换未通 |
| 磁带机快退（4 档）| FR-002 4.2.6 | ⚠️ 部分 | tape_control.cpp | 同上 |
| 跳帧（≥4x）| FR-002 4.2.7 | ⚠️ 接口有 | audio_player.cpp | 在 ESP-ADF 路径下 |
| 断点续播 | FR-003 4.2.8 | ✅ 完成 | settings.cpp | NVS 持久化 + 校验 |
| 播放列表自动扫描 | FR-004 | ✅ 完成 | playlist.cpp | 递归 3 层 |
| 文件夹浏览 | FR-004 | ❌ 未实现 | — | 长按 Stop 应进入 UI |
| 播放模式（顺序/单曲/全部）| FR-005 | ✅ 完成 | main.cpp + settings.cpp | 双击 Play 切换 |
| 按键锁定（超长按 3s）| FR-009 | ✅ 完成 | main.cpp | 超长按 EXTRA_LONG_PRESS |
| 快跳 ±10s | FR-010 | ✅ 完成 | main.cpp skip_seconds | |
| OLED 显示 | FR-006 | ❌ stub | display.cpp | `#ifdef CONFIG_USE_U8G2` 未启用 |
| SD 卡 FATFS | — | ✅ 完成 | main.cpp mount_sd_card | |
| **音频播放** | **核心** | **❌ stub** | **audio_player.cpp** | **`#ifdef CONFIG_USE_ESP_ADF` 未启用** |
| 总进度 | | **8/10** | | 缺：音频播放、OLED、文件夹浏览 |

---

## V1.1 — 体验增强（P2）

| 功能 | 状态 | 备注 |
|---|---|---|
| 定时关机（15/30/60/90 分钟）| ⚠️ stub | power_mgmt.cpp 接口全，逻辑未实现 |
| A-B 区间复读 | ❌ 未实现 | — |
| 屏幕保护/自动变暗 | ❌ 未实现 | — |
| 按键提示音（"滴"声）| ❌ 未实现 | 需 I2S 输出 PCM 片段 |
| OGG/Opus 解码 | ⚠️ 接口 | ADF 启用后自动可用 |
| EC11 旋转编码器（音量）| ❌ 未实现 | GPIO 38/39 预留未用 |
| 总进度 | **0/6** | |

---

## V1.2 — 进阶功能（P2）

| 功能 | 状态 | 实现位置 |
|---|---|---|
| 书签管理（每文件 10 个）| ⚠️ stub | bookmark.cpp 接口全，未实现 |
| 语音播报（预录 WAV）| ⚠️ stub | voice_prompt.cpp 全 stub |
| 电量检测 + 显示 | ⚠️ stub | power_mgmt.cpp stub |
| 电池低电告警 | ❌ 未实现 | — |
| 设置菜单（OLED 导航）| ❌ 未实现 | — |
| 固件 USB 升级 | ❌ 未实现 | — |
| 总进度 | **0/6** | |

---

## V2.0 — 扩展生态（P3 远期）

| 功能 | 状态 |
|---|---|
| LE Audio 蓝牙耳机（V1.1） | 📄 已规划 | `docs/BT_AUDIO_PLAN.md` |
| 线路输入录音 | ❌ |
| 速度微调（0.5x ~ 2.0x 不变调）| ❌ |
| EQ 均衡器 | ❌ |
| USB 大容量存储 | ❌ |

---

## 🚧 关键阻塞（必先解决）

### BLOCKER 1: ESP-ADF 没启用
```cpp
// audio_player.cpp 第 19 行
#ifdef CONFIG_USE_ESP_ADF
    // 真实实现 (audio_pipeline, mp3_decoder, i2s_stream, ...)
#else
    // stub - 只打日志，不播任何声音
#endif
```
**修复**：
1. `main/CMakeLists.txt` 取消注释 `set(EXTRA_COMPONENT_DIRS $ENV{ADF_PATH}/components)`
2. `sdkconfig.defaults` 加 `CONFIG_USE_ESP_ADF=y`
3. 重 build（约 +5 分钟）

### BLOCKER 2: u8g2 OLED 库没装
```cpp
// display.cpp 第 20 行 (已修)
#ifdef CONFIG_USE_U8G2
    // 真实显示 (u8g2_Setup_ssd1306_i2c_128x64_noname_f)
#else
    // stub - ESP_LOG 输出
#endif
```
**修复**：
1. `idf.py add-dependency "lfengineering/u8g2_esp32"`
2. `sdkconfig.defaults` 加 `CONFIG_USE_U8G2=y`

### BLOCKER 3: 量产 OTA 接收代码未写
分区表已配 `partitions_ota.csv`，但 **app 代码没有 HTTP OTA 接收逻辑**。
**修复**：main.cpp 加 `esp_https_ota` 调用 + WiFi 配置（需要先启用 WiFi）。

---

## 📋 V1.0 必需补完清单（按优先级）

### P0 - 解锁真实功能（不解决无法演示）
- [ ] 启用 ESP-ADF（BLOCKER 1）
- [ ] 安装 u8g2 component（BLOCKER 2）

### P1 - 完善 V1.0 MVP
- [ ] 文件夹浏览（display 屏 + 主循环导航）
- [ ] 屏幕底部行对齐 PRD（操作提示布局）

### P2 - 实现 V1.1
- [ ] 定时关机（power_mgmt 补完）
- [ ] 按键提示音（PCM 片段）
- [ ] OGG/Opus 解码（启用 ADF 后自动）
- [ ] 屏幕保护/变暗

### P3 - 实现 V1.2
- [ ] 书签管理（bookmark.cpp 补完）
- [ ] 语音播报（voice_prompt.cpp 补完）
- [ ] 电量检测（power_mgmt ADC）
- [ ] 设置菜单（display UI）
- [ ] OTA 接收代码（量产准备）

### P4 - V2.0
- [ ] 蓝牙 A2DP
- [ ] EQ 均衡器

---

## 🎯 当前建议执行顺序

1. **现在（5 分钟）**：启用 ESP-ADF + u8g2，重 build，验证音频播放 + OLED 显示
2. **接着（半天）**：补完 stub 模块（power_mgmt、bookmark、voice_prompt）
3. **再接着（1 天）**：实现文件夹浏览 + 设置菜单 UI
4. **量产前（1 周）**：OTA 接收代码 + 完整测试

---

**作者**：CodeBuddy  
**数据来源**：PRD.md、main/ 源代码