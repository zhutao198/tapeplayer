# 蓝牙音箱（A2DP Sink）可行性评估与实现骨架

> 评估目标：手机通过蓝牙连接本设备，把设备作为**蓝牙音箱**推流播放。
> 评估依据：基于 `main/` 现有代码（audio_player / display / menu / main）与 ESP-IDF v5.5.3 + ESP-ADF v2.8 + LVGL v9.5.0。
> 日期：2026-08-13

---

## 1. 结论

**可行。硬件零改动，软件中等复杂度。**

- **MVP（配对 → 手机出声 → 播放/暂停透传 AVRCP → 本地音量）**：约 **3–5 天**。
- **完整版（+ 配对 UI、双向音量同步、断连自动重连、设备名/UUID、低电优雅关机、硬件联调）**：约 **2–3 周**。

当前状态：**BT 被关闭、Wi-Fi 被开启**（`sdkconfig` 中 `# CONFIG_BT_ENABLED is not set`、`CONFIG_ESP_WIFI_ENABLED=y`）。本方案新增一个 **BT 音箱构建变体**：开启 BT（Classic/A2DP Sink）并**关闭 Wi-Fi**，通过 `configure.bat <target>-bt` 选择。

---

## 2. 为什么可行（代码实证）

### 2.1 硬件链路已就位
- 输出是 `I2S → MAX98357A`（`config.h`：BCLK=IO6 / WS=IO7 / DOUT=IO5）。A2DP Sink 解码后的 PCM 走的就是这条 I2S 路径，**无需任何硬件改动**。
- 模组 `WROOM-1 N16R8` 自带 PCB 天线，ESP32-S3 原生支持 BT Classic（BR/EDR），可做 A2DP Sink。

### 2.2 软件栈可直接复用
- `audio_player.cpp` 用 ESP-ADF 的 `audio_pipeline`：当前 SD 播放链路 `fatfs_reader → decoder → i2s_stream_writer`；
  `g_i2s_writer`（`audio_player.cpp` 中跨曲目复用的单例 I2S 输出流，**BT 只需挂到同一个 `g_i2s_writer` 上即可出声**。
- 音量当前用 `i2s_alc_volume_set(g_i2s_writer, ...)`（软件 ALC，MAX98357A 无硬件音量）——BT 模式下**直接复用**，无需新音量通路。
- ESP-ADF v2.8 自带 `bluetooth_service` 组件（Bluedroid 封装），提供 A2DP Sink 的 `audio_element`，标准接法就是 `bt_element → i2s_stream_writer`，与现有管线同构。

### 2.3 当前 BT / Wi-Fi 状态（来自 `sdkconfig`）
| 项 | 状态 | 来源 |
|---|---|---|
| BT | 关闭 | `sdkconfig` `# CONFIG_BT_ENABLED is not set` |
| Wi-Fi | 开启（应用层未直接使用） | `sdkconfig:1503 CONFIG_ESP_WIFI_ENABLED=y` |
| 蓝牙栈 | 无 | `# CONFIG_BT_BLE_ENABLED is not set` 等 |
| LVGL | v9.5.0 | `managed_components/lvgl__lvgl/idf_component.yml` |
| IDF / ADF | v5.5.3 / v2.8 | `build.bat:9` / 根 `CMakeLists.txt` 注释 |

---

## 3. 改造清单（模块级）

| 模块 | 改动 | 说明 |
|---|---|---|
| `sdkconfig` / 构建 | 开 BT：Bluedroid + Classic + A2DP + AVRCP；关 Wi-Fi | A2DP Sink 必须 Bluedroid（NimBLE 不支持 A2DP）。由 `configs/sdkconfig.bt_speaker` 片段 + `configure.bat <target>-bt` 注入 |
| `main/idf_component.yml` / `main/CMakeLists.txt` | 条件 `REQUIRES bluetooth_service bt`；加入 `bt_speaker.cpp` | 仅 `CONFIG_USE_BT_SPEAKER` 时链接 ADF 蓝牙组件 |
| `main/Kconfig.projbuild` | 新增 `USE_BT_SPEAKER`（bool, default n, depends on USE_ESP_ADF） | 功能开关 |
| `main/bt_speaker.{h,cpp}`（新增） | BT 协议栈初始化 + 回调 + 音频管线 `bt_element → i2s_writer` | 自包含模块，复用 `g_i2s_writer` |
| `main/audio_player.{h,cpp}` | 新增 `audio_player_start_bt/stop_bt/is_bt_active`；`audio_player_stop/tick` 兼容 BT | 关键约束：`g_i2s_writer` 同一时刻只能被一个管线占用 |
| `main/main.cpp` 状态机 | 新增 `APP_STATE_BT_SPEAKER`；`app_enter_bt_speaker()`；PLAY/PAUSE 透传 AVRCP；STOP 退出；自动关机/LED 逻辑纳入 BT 态 | 复用统一菜单入口 |
| `main/menu.cpp` | 根菜单新增「蓝牙音箱」`MI_ACTION` → `app_enter_bt_speaker` | **统一菜单，不用组合键**（用户记不住） |
| `main/display.{h,cpp}` | 新增 `display_show_bt_status(device_name, connected, volume)` | 复用播放界面：设备名作曲目名、无进度 |
| 入口 UX | 菜单「蓝牙音箱」项 | 见上 |

**AVRCP 音量策略**：手机发绝对音量(0–127) → 映射到现有 `i2s_alc` 0–14 档；同时把本地旋钮音量回传手机（`esp_a2dp_set_volume`），使两端一致。

---

## 4. 复杂度与工时

- **MVP**（可配对、手机出声、播放/暂停透传、本地音量、菜单入口、基础 UI）：~3–5 天。
- **完整版**（+ 配对 UI、双向音量同步、自动重连、设备名/UUID、低电优雅关机、共存决策、硬件联调）：~2–3 周。

---

## 5. 主要风险 / 坑

1. **I2S 独占**：SD 播放与 BT 必须互斥使用 `g_i2s_writer`，否则两个管线同时写 I2S 会爆音/崩溃。已用 `audio_source_t`（BT 激活标志 `g_bt_active`）统一仲裁：`audio_player_stop()` 在 BT 激活时先停 BT。
2. **采样率**：BT 音频多为 44.1k/48k。若与 SD 默认值不同，BT 元素与 I2S 间可能要插 `filter_resample`（代码已 `include filter_resample.h`，可直接用）。
3. **RAM**：Bluedroid 约 50–80KB + BT controller。N16R8 有 8MB PSRAM，充足；固件体积：当前 bin ~1.7MB，加 BT 栈约 +300–500KB，仍 < 2MB OTA 槽。
4. **共存**：保留 Wi-Fi 会与 BT 抢 2.4GHz，故 **BT 音箱构建关闭 Wi-Fi**（见 §6）。
5. **延迟**：A2DP 固有 ~100–200ms，对音箱无感，但需在 UI/说明里告知用户「非低延迟模式」。

---

## 6. 构建配置：BT 音箱构建关闭 Wi-Fi

新增 `configs/sdkconfig.bt_speaker` 片段（BT 开 / Wi-Fi 关），由 `configure.bat <target>-bt` 在生成 `sdkconfig.defaults` 时追加：

- `configure.bat wroom-1-n16r8-bt` → 应用 `sdkconfig.defaults.wroom-1-n16r8` + 追加 BT 片段（`BT_FLAVOR=1`）。
- `configure.bat wroom-2-n32r16v-bt` → 同上，基于 N32R16V 模板。

片段内容要点（符号名以 IDF v5.5.3 为准，menuconfig 可微调）：
```
CONFIG_BT_ENABLED=y
CONFIG_BLUEDROID_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BR_EDR_BLE=y
CONFIG_BT_BLE_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_A2DP_ENABLE=y
CONFIG_AVRC_ENABLE=y
CONFIG_CLASSIC_PAIRING_ENABLED=y
# CONFIG_ESP_WIFI_ENABLED is not set   # 关闭 Wi-Fi
```

---

## 7. 菜单集成（统一菜单，非组合键）

在 `main/menu.cpp` 根菜单 `g_root[]` 直接加一项，用户从菜单进入，**无需记忆组合键**：

```c
{ "蓝牙音箱", MI_ACTION, NULL, 0, NULL, NULL, NULL, 0, app_enter_bt_speaker },
```

仅当 `CONFIG_USE_BT_SPEAKER` 编译时显示该项（未启用则菜单隐藏该项，避免误导）。

---

## 8. 代码骨架说明（已实现于本仓库草案）

- **`main/bt_speaker.h / .cpp`**：自包含 BT 模块。
  - `bt_speaker_init(name)`：初始化 Bluedroid、设设备名、设为可发现+可连接。
  - `bt_speaker_start(i2s_writer)`：创建 ADF `bluetooth_service`（A2DP Sink），取解码元素，建 `bt_element → i2s_writer` 管线并 run。
  - 回调：`bt_a2d_evt_handler`（连接/断开状态）、`bt_avrc_ct_evt_handler`（手机音量）→ 通过 `bt_speaker_register_state_cb / set_volume_cb` 上报 UI 与本地音量。
  - `bt_speaker_report_volume(0..127)`：本地音量回传手机；`bt_speaker_avrc_play/pause()`：透传 AVRCP。
- **`audio_player.cpp`**：
  - `audio_player_start_bt()`：停 SD → `bt_speaker_start(g_i2s_writer)` → 注册音量回调 → 标记 `g_bt_active`。
  - `audio_player_stop()`：BT 激活时先 `audio_player_stop_bt()`（保证 I2S 单占用）。
  - `audio_player_tick()`：BT 激活时 early-return（无 SD pipeline）。
  - `audio_player_set_volume()`：设本地 ALC，BT 激活时回传手机（避免与 phone 音量回调形成循环：回调路径只调 `apply_volume_alc` 不回传）。
- **`main.cpp`**：`APP_STATE_BT_SPEAKER` 状态；`app_enter_bt_speaker()` 由菜单调用；PLAY/PAUSE 透传 AVRCP、STOP 退出；自动关机/WS2812 绿灯逻辑纳入 BT 态；`update_display` 调用 `display_show_bt_status`。
- **`configure.bat` + `configs/sdkconfig.bt_speaker`**：`-bt` 构建变体（BT 开、Wi-Fi 关）。

> 注：ADF `bluetooth_service` 的回调/创建 API 随版本略有差异；若编译报错，请对照
> `D:\esp\esp-adf\components\bluetooth_service\include\bluetooth_service.h` 微调
> （尤其是 `esp_bluetooth_service_create` 的返回类型与 `periph_bluetooth_get_element` 的入参）。

---

## 9. 验证步骤

1. `configure.bat wroom-1-n16r8-bt` → `build.bat menuconfig` 确认 BT 已开、Wi-Fi 已关。
2. `build.bat build` 通过；`build.bat flash` 烧录。
3. 设备上电 → 菜单「蓝牙音箱」→ 手机扫描到 `TapeBook` → 配对连接 → 手机播放音乐，设备出声。
4. 设备音量键调节 → 手机音量条同步（AVRCP）。
5. 手机端调音量 → 设备音量变化（本地 ALC）。
6. 手机播放/暂停 → 设备（或手机本身）响应（AVRCP 透传，取决于手机支持）。
7. 设备 STOP → 退出 BT 模式回 STOPPED；自动关机在 BT 态仍生效。

---

## 10. 工作量与优先级建议

| 阶段 | 内容 | 工时 |
|---|---|---|
| P0 MVP | sdkconfig 变体 + 菜单入口 + `bt_speaker` + `audio_player` BT 通路 + 基础 UI | 3–5 天 |
| P1 | AVRCP 双向音量同步、PLAY/PAUSE 透传、断连提示 | 2–3 天 |
| P2 | 配对 UI、自动重连、低电优雅关机、硬件联调与采样率/filter 收尾 | 1–2 周 |

**最高优先级**：先打通 P0 MVP，验证「手机能连上并出声」这一核心链路，再迭代 P1/P2。
