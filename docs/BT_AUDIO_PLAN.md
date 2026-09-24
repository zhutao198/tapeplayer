# BT_AUDIO_PLAN.md — LE Audio 蓝牙耳机支持方案（V1.0）

> **状态**：规划阶段（待实施）
> **目标**：ESP32-S3 作为 LE Audio Source，将音频流式传输到 LE Audio 蓝牙耳机

---

## 1. 硬件能力确认

ESP32-S3-WROOM-1 N16R8：
- 支持 **BLE 5.0**（含 2M PHY、Coded PHY）
- 自带 **BLE ISO 同步信道**（CIS/BIS），经 ESP-IDF master 分支验证可行
- 共用 2.4GHz Wi-Fi 天线（时分共存）
- 无需额外硬件（复用现有天线 + 射频）

依赖条件：
- ESP-IDF **master 分支**（v5.5.3 不包含 `esp-ble-audio`、`esp-ble-iso`）
- 蓝牙耳机需支持 **LE Audio**（蓝牙 5.2+，LC3 编解码）

---

## 2. 架构总览

### 2.1 音频流

```
  SD卡 → ADF Decoder (MP3/AAC/FLAC/OGG)
                │
                ├──→ I2S Writer → MAX98357A → 喇叭
                │
                └──→ RingBuf → LC3 Encoder → BLE ISO (CIS) → LE Audio 耳机
```

### 2.2 角色定义

| 角色 | 本机 | 耳机 |
|---|---|---|
| CAP Role | CAP Initiator | CAP Acceptor |
| BAP Role | BAP Unicast Client (Source) | BAP Unicast Server (Sink) |
| Audio Source | ADF 解码输出的 PCM | 解码渲染 |

### 2.3 协议栈分层

```
  Application (main.cpp / bt_audio.cpp)
  ──────────────────────────────────
  ESP-BLE-AUDIO (BAP / CAP / HAP / LC3)
  ──────────────────────────────────
  ESP-BLE-ISO (CIS / BIS 等时通道)
  ──────────────────────────────────
  ESP-Bluedroid / ESP-NimBLE (BLE 5.0 Host)
  ──────────────────────────────────
  ESP32-S3 BLE Controller (BLE 5.0)
```

---

## 3. 音频分流设计

### 3.1 当前 Pipeline（V1.0）

```
fatfs_stream → decoder → i2s_writer
```

### 3.2 V1.1 改动

```
fatfs_stream → decoder → ringbuf → i2s_writer (喇叭)
                           │
                           ↓
                    BT 任务 → LC3 Encode → BLE ISO CIS (耳机)
```

ringbuf 的大小：`LC3_FRAME_SAMPLES * 2`（10ms @ 48kHz = 960 样本 = 1920 字节），建议 double buffer：2 × 1920 = 3840 字节。

### 3.3 输出切换逻辑

| 输出模式 | I2S Speaker | BT Headphones |
|---|---|---|
| `OUTPUT_SPEAKER` | ALC 正常 | CIS 断开 / 暂停 |
| `OUTPUT_BT` | ALC -96dB（静音）| CIS 激活，发送 LC3 帧 |
| `OUTPUT_BOTH` | ALC 正常 | CIS 激活，发送 LC3 帧 |

三种模式通过按键循环切换，状态存入 NVS 持久化。

---

## 4. LE Audio Source 关键 API 调用流程

### 4.1 初始化

```cpp
// 1. 初始化 LE Audio 协议栈
esp_ble_audio_init();

// 2. 注册回调（连接状态、数据路径事件）
esp_ble_audio_register_callback(bt_audio_callback, NULL);

// 3. 设置本地音频能力（LC3 Sink Capability 作为 Source）
//    48kHz / 16bit / Mono / 7.5ms 帧
esp_ble_audio_set_loc_cap(ESP_BLE_AUDIO_CAP_TYPE_SOURCE, ...);
```

### 4.2 扫描与配对

```cpp
// 1. 发现 LE Audio 耳机（HAP / CAP Acceptor）
esp_ble_audio_start_discovery();
// → 回调返回发现设备

// 2. 连接目标耳机
esp_ble_audio_connect(remote_addr);

// 3. 协商 CIS 参数（采样率、帧间隔、SDU 大小）
//    48kHz / Mono / 10ms / SDU=120 bytes
esp_ble_audio_cis_connect(...);
```

### 4.3 音频传输

```cpp
// 主循环中（定时器 / 任务触发）
void bt_audio_tx_task(void *arg)
{
    while (bt_audio_is_connected()) {
        // 1. 从 ringbuf 读取 PCM（来自 ADF decoder）
        int16_t pcm[LC3_FRAME_SAMPLES];
        ringbuf_read(ringbuf, pcm, sizeof(pcm), portMAX_DELAY);

        // 2. LC3 编码 PCM → LC3 帧
        uint8_t lc3_frame[LC3_MAX_FRAME_BYTES];
        lc3_encoder_encode(encoder, LC3_FRAME_SAMPLES, pcm, lc3_frame);

        // 3. 通过 CIS 发送 LC3 帧
        esp_ble_audio_send(cis_handle, lc3_frame, frame_size, timeout_ms);
    }
    vTaskDelete(NULL);
}
```

### 4.4 断开与重连

```cpp
// 耳机断开 → 自动切回喇叭输出 + 记录配对信息
void bt_audio_callback(esp_ble_audio_event_t event, void *data)
{
    switch (event) {
    case ESP_BLE_AUDIO_DISCONNECTED:
        audio_player_set_output(OUTPUT_SPEAKER);
        settings_save_bt_device(NULL); // 清配对缓存
        break;
    case ESP_BLE_AUDIO_CONNECTED:
        audio_player_set_output(OUTPUT_BT);
        settings_save_bt_device(&remote_addr);
        display_show_bt_connected(true);
        break;
    }
}
```

---

## 5. 文件改动清单

| 文件 | 操作 | 说明 |
|---|---|---|
| `main/bt_audio.h` | **新建** | LE Audio 管理类头文件（初始化、扫描、连接、TX） |
| `main/bt_audio.cpp` | **新建** | LE Audio 实现（Source 角色） |
| `main/audio_player.h` | 修改 | 新增 `audio_player_set_output()`、`audio_player_get_output()`、`output_mode_t` 枚举 |
| `main/audio_player.cpp` | 修改 | pipeline 增加 ringbuf 分流；输出切换控制 I2S 静音 |
| `main/main.cpp` | 修改 | 新增 BT 初始化、BT 事件处理、输出切换按键逻辑 |
| `main/display.h` | 修改 | 新增 `display_show_bt_status()` |
| `main/display.cpp` | 修改 | OLED 增加 BT 状态区域（连接/断开/搜索中） |
| `main/settings.h` | 修改 | 新增 `settings_save_bt_device()`、`settings_load_bt_device()` |
| `main/settings.cpp` | 修改 | NVS 读写配对设备地址 + 输出模式 |
| `main/Kconfig.projbuild` | 修改 | 新增 `CONFIG_USE_BT_AUDIO` 开关 |
| `main/CMakeLists.txt` | 修改 | REQUIRES 增加 `bt`、`esp_ble_audio`、`esp_ble_iso` |
| `sdkconfig.defaults` | 修改 | 启用 `CONFIG_BT_ENABLED`、`CONFIG_BT_BLE_ENABLED` |
| `configs/sdkconfig.defaults.wroom-1-n16r8` | 修改 | 同上 |
| `configs/sdkconfig.defaults.wroom-2-n32r16v` | 修改 | 同上 |

---

## 6. 按键映射

| 操作 | 功能 |
|---|---|
| **Play + Prev 同时长按** | 扫描 / 连接已配对 LE Audio 耳机 |
| **Play + Next 同时长按** | 切换输出：喇叭 → BT → 同时 → 喇叭 |
| OLED 状态图标 | `BT 🔵` 已连接 / `BT ⚪` 未连接 / `BT 📡` 搜索中 |

---

## 7. 内存与性能估算

| 项目 | 估算 |
|---|---|
| LC3 编码 (48kHz/16bit/Mono/10ms 帧) | ~15 MIPS |
| BLE ISO 中断 + 协议栈处理 | ~5 MIPS |
| Ringbuf 数据拷贝 + 任务切换 | ~3 MIPS |
| **合计** | **~23 MIPS**（S3 双核 240MHz，余量 >85%）|
| **额外 RAM** | ~60 KB（代码 + ringbuf + LC3 编码器 + BLE 协议栈）|
| **额外 Flash** | ~80 KB（esp-ble-audio + esp-ble-iso + LC3 库）|

---

## 8. 风险点

| 风险 | 影响 | 缓解措施 |
|---|---|---|
| IDF master API 不稳定 | 编译失败、行为变化 | 固定 master 的 commit hash；隔离 esp-ble-audio 为本地组件 |
| ADF + esp-ble-audio 组件冲突 | 构建或运行时冲突 | 确保 REQUIRES 无循环依赖；先做最小构建验证 |
| LC3 编码实时性不足 | 音频 xrun / 断音 | 分离编码任务到独立核 (core 1)；pre-encode 缓冲 |
| CIS 时序与 Wi-Fi 共存干扰 | ISO 丢包音频卡顿 | 使用 ESP BLE/Wi-Fi 共存机制（ESP_COEX_POLICY）|
| 耳机兼容性差异 | 部分耳机连接失败 | 以主流 LE Audio 耳机为参考（Sony WH-1000XM5、三星 Buds3 Pro）|
| NVS 配对信息格式变更 | 固件升级后不兼容 | 版本化 NVS key；升级时旧记录自动清除 |

---

## 9. V1.1 实施步骤（按优先级）

| # | 步骤 | 工作量 | 前置 |
|---|---|---|---|
| 1 | IDF master 分支验证：确认项目在 master 下正常编译 + 运行 | 2h | — |
| 2 | menuconfig 启用 BT + BLE：确认 `CONFIG_BT_ENABLED` + Bluedroid 正常 | 1h | #1 |
| 3 | 新建 `bt_audio.h/cpp`：空壳 + 初始化 `esp_ble_audio_init()` + 回调注册 | 2h | #2 |
| 4 | 实现扫描 + 发现回调：命令行输出发现的 LE Audio 设备 | 3h | #3 |
| 5 | 实现连接 + CIS 建立：真机连 LE Audio 耳机，确认 CIS 参数协商通过 | 4h | #4 |
| 6 | LC3 编码集成：从 ADF ringbuf 读 PCM → LC3 编码 → log 帧大小 | 4h | #5 |
| 7 | CIS 音频发送：将 LC3 帧通过 CIS 发出，真机听音验证 | 4h | #6 |
| 8 | 输出切换 UI：OLED BT 图标 + 按键切输出 | 2h | #7 |
| 9 | 配对信息持久化：NVS 存/读配对地址 + 自动重连 | 2h | #8 |
| 10 | 断线重连 + 异常恢复：蓝牙异常断开自动切回喇叭 | 2h | #9 |
| 11 | 电源管理集成：BT 发射时的低功耗策略（连接间隔优化） | 3h | #10 |
| 12 | 压力测试：连续 2h 播放 + 反复断开/重连 + 输出切换 | 2h | #11 |

---

## 10. Kconfig 变更参考

```kconfig
# main/Kconfig.projbuild 新增
config USE_BT_AUDIO
    bool "Enable LE Audio headphone support (V1.1)"
    default n
    help
        Enable BLE ISO + ESP-BLE-AUDIO for streaming audio to
        LE Audio compatible Bluetooth headphones.
        Requires esp-ble-audio and esp-ble-iso components (IDF master).
```

```ini
# sdkconfig.defaults 新增或修改
CONFIG_BT_ENABLED=y
CONFIG_BT_BLE_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y
CONFIG_BT_BLE_ISO_ENABLED=y
CONFIG_BT_BLE_AUDIO_ENABLED=y
```

---

## 11. 参考资源

| 资源 | 链接 |
|---|---|
| ESP-IDF LE Audio 架构文档 | `https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-guides/ble/ble-audio.html` |
| ESP-BLE-AUDIO API Reference | `https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/bluetooth/esp-ble-audio.html` |
| CAP Acceptor 示例 | IDF `examples/bluetooth/esp_ble_audio/cap/acceptor/` |
| LE Audio Source 示例（TODO） | 待社区 / Espressif 提供 |
| LC3 编解码器 | ESP-IDF 内置（esp-ble-audio 组件） |

---

**修订历史**

| 版本 | 日期 | 变更 | 作者 |
|---|---|---|---|
| V1.0 | 2026-07-10 | 初稿 | Claude |
