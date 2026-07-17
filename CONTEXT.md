# CONTEXT.md — TapeBook 30 秒恢复指南

> **项目**：ESP32-S3 听书机（磁带机风格音频播放器）  
> **仓库**：`zhutao198/tapeplayer`（GitHub）  
> **本地**：`D:\zhutao\audio_player`  
> **最后更新**：2026-07-17（R023 — R021/R022 文档补正 + M3 ALC 注释落地）

---

## 1. 项目一句话定位

基于 **ESP32-S3** 的磁带机风格音频播放器，支持快进/快退 4 档变速模拟传统磁带体验，量产模组为 **WROOM-1 N16R8**（带 OTA）。

---

## 2. 30 秒恢复（新会话开场必做）

```bash
cd D:/zhutao/audio_player
git log --oneline -5           # 最新 5 个 commit
git tag -l "R*" | tail -5      # 最新 5 个 R 节点
git status --short            # 未提交改动
```

参考文件：
- 本文件（`CONTEXT.md`）— 项目状态速查
- `SESSION_SUMMARY.md` — 关键决策、教训、经验
- `开发日志.md` — R 节点详细记录

---

## 3. 关键文件速查

| 类别 | 路径 | 说明 |
|---|---|---|
| **入口** | `main/main.cpp` | 程序入口 + 主循环 |
| **音频** | `main/audio_player.cpp` | 音频引擎（依赖 ESP-ADF） |
| **磁带控制** | `main/tape_control.cpp` | 4 档变速状态机 |
| **按键** | `main/button_manager.cpp` | 6 按键 + 状态机去抖 |
| **OLED** | `main/display.cpp` | SSD1306 显示（依赖 u8g2） |
| **设置** | `main/settings.cpp` | NVS 持久化（断点续播） |
| **书签** | `main/bookmark.cpp` | NVS API + STOP 双击集成 |
| **电源** | `main/power_mgmt.cpp` | 定时关机 / 电量检测 stub |
| **语音** | `main/voice_prompt.cpp` | 预录 WAV 提示音 stub |
| **配置** | `main/config.h` | GPIO 引脚定义 |
| **自定义 Board** | `components/tapebook_board/` | ADF 板级支持（V1.0） |
| **构建** | `CMakeLists.txt` + `main/CMakeLists.txt` | 顶层 + 组件 |
| **分区** | `partitions.csv` / `partitions_ota.csv` | 单一 factory / OTA |
| **配置模板** | `configs/sdkconfig.defaults.wroom-*` | 双模组切换 |

---

## 4. R 节点全景（维护中）

| R 节点 | 日期 | commit | 内容 | 状态 |
|---|---|---|---|---|
| baseline | 2026-07-03 | `938abbe` | 首次提交（仓库基线） | ✅ |
| R001 | 2026-07-03 | `c0c67e4` | 启用 ESP-ADF（解锁音频播放） | ✅ |
| R002 | 2026-07-03 | `d773f05` | 启用 u8g2 OLED 显示（改用 idf component 替换手动源码） | ✅ |
| R003 | 2026-07-03 | `333e44e` | build 验证未通过；修 R001 ADF REQUIRES + 修 R002 u8g2 暂禁用 | ⚠️ |
| R004 | 2026-07-03 | `377a893` | 修复 CMakeLists.txt 启用 ADF（EXTRA_COMPONENT_DIRS 移到项目根）| ✅ |
| **R005** | **2026-07-03** | **`65ca4ea`** | **修 HARDWARE_PIN_WIRING.md 5 处错误（SD_MODE/GPIO47-48/EC11/USB-JTAG/UART0）+ 补 MAX98357A 规格书** | **✅** |
| **R006** | **2026-07-03** | **`126af18`** | **修 HARDWARE_PIN_WIRING.md 5 处错误（SD_MODE 公式/GPIO47-48 R8V-R16V/GPIO45）+ 补 SSD1315 规格书** | **✅** |
| **R007** | **2026-07-03** | **`e7fb604`** | **首次成功构建！修 board.h / audio_player.cpp API / u8g2_esp32_hal 兼容性** | **✅** |
| **R008** | **2026-07-06** | **`2530f23`** | **代码审查 33 项修复（seek/位置/NULL/PSRAM/WDT/NVS/...）** | **✅** |
| **R009** | **2026-07-06** | `853f483` | **审查剩余 9 项修复（SD 热插拔/脏区/屏保/light sleep/锁定态/button/采样率）** | **✅** |
| **R010** | **2026-07-06** | `76441b1` | **审查余下 8 项清零（bookmark NVS/voice_prompt/M-2 timeout/M-3 init/设计确认）** | **✅** |
| **R011** | **2026-07-07** | `df11f0d` | **修复 R010 引入的 6 个 bug + H-8 ADC 桩 + L-1 bookmark 按键集成** | **✅** |
| **R012** | **2026-07-07** | `1d95d12` | **文件夹浏览（V1.0 MVP 最后功能）** | **✅** |
| **R013** | **2026-07-07** | `4f3b25e` | **R012 review 修复（scroll clamp + API cleanup）** | **✅** |
| **R014** | **2026-07-09** | `eca38cc` | **PRD 审查 5 项修复（OLED/音量/书签/电源/休眠）+ 原理图设计** | **✅** |
| **R015** | **2026-07-10** | **`d54d0ed`** | **硬件设计修复（B2/N1/N2/N3/N4/N5）+ LE Audio 方案文档** | **✅** |
| **R018** | **2026-07-11** | **`8a90513`** | **代码审计修复 19 项（6 Critical + 7 High + 5 Medium + 1 Low）** | **✅** |
| **R019** | **2026-07-11** | **`06f9be9`** | **R018 build 验证 + 修复 3 个编译副作用** | **✅** |
| **R020** | **2026-07-11** | **`06bb8d0`** | **R018 评审闭环 + H-3 用户重做（整数四舍五入 trick）** | **✅** |
| **R021** | **2026-07-17** | **`1d03d03`** | **Batch 1 深度评审修复 10 项（C2/C3/H1/H2/H3/M4/M5/L1/L3/L4）** | **✅** |
| **R022** | **2026-07-17** | **`584cf67`** | **Batch 2 深度评审修复 5 项（C1/C3跳帧/M1/M2/M6）** | **✅** |
| **R023** | **2026-07-17** | **`3655ff3`** | **R021/R022 文档补正 + M3 ALC 注释落地** | **✅** |

> 详细变更见 `开发日志.md`，回滚命令：`git checkout <tag>`

---

## 5. 关键决策速查

| 决策 | 选择 | 理由 | 文档 |
|---|---|---|---|
| 主控 | ESP32-S3-WROOM-1 N16R8（量产） | BOM 低、Octal PSRAM 8MB 够用、3.3V VDD_SPI 简化 PCB | HARDWARE_MODULE_MIGRATION.md |
| 音频框架 | ESP-ADF v2.7 | 多格式解码（MP3/AAC/FLAC/OGG/Opus） | README.md |
| 存储 | MicroSD SPI | 简单可靠 | README.md |
| 显示 | SSD1306 0.96寸 OLED I2C | 便宜、低功耗 | README.md |
| 断点续播 | NVS 命名空间 `tapebook` | 不占 SD 写寿命 | DETAILED_DESIGN.md |
| 音量控制 | I2S ALC (i2s_alc_volume_set) | ADF 内置 ALC 音量，-96~+12dB 范围 | audio_player.cpp |
| OLED 驱动 | u8g2_esp32_hal（源码编入 main 组件）| 避免静态库链接顺序问题 | CMakeLists.txt |
| 蓝牙方案 | LE Audio（LC3，无需额外 BOM）| ESP32-S3 仅有 BLE 5.0，无 BT Classic；LE Audio 通过 `esp-ble-audio` 组件实现 | BT_AUDIO_PLAN.md |
| ME6211C33 封装 | SOT-23-5（M5G-N）| 实际采购型号带 CE 使能引脚；非 SOT-89 | SCH_TapeBook_V1.3.md |

---

## 6. 服务 / 工具信息

| 项 | 值 |
|---|---|
| 串口（烧录） | 待确认（Win 上 `idf.py -p COMx flash`） |
| ESP-IDF 版本 | v5.5.3（实际） |
| ESP-ADF 版本 | v2.7 |
| 模组切换脚本 | `configure.bat wroom-1-n16r8` / `configure.bat wroom-2-n32r16v` |
| 构建脚本 | `build.bat build` / `build.bat -p COM3 flash` |
| GitHub 远程 | `https://github.com/zhutao198/tapeplayer.git` |

---

## 7. 紧急恢复命令

```bash
# 仓库挂了
cd D:/zhutao/audio_player
git status
git log --oneline -10

# 回滚到任意 R 节点
git checkout R001
# 或查看
git show R001

# 重置 main 到 baseline
git reset --hard baseline
git clean -fdx
```

---

## 8. 关联文档

| 文档 | 用途 |
|---|---|
| `README.md` | 硬件清单 + 接线 + 编译指南 |
| `PRD.md` | 产品需求（V1.0/V1.1/V1.2/V2.0） |
| `DESIGN.md` | 总体设计 |
| `DETAILED_DESIGN.md` | 详细设计 |
| `REVIEW_REPORT.md` | 评审报告 |
| `docs/DEVELOP_STATUS.md` | 功能完成度对照表（vs PRD） |
| `HARDWARE_MODULE_MIGRATION.md` | 模组迁移指南 |
| `docs/HARDWARE_PIN_WIRING.md` | 硬件引脚接线图（V1.2，R006 修正）|
| `docs/HARDWARE_PIN_WIRING_REVIEW.md` | 上述文档的评审报告（V1.0，7/10）|
| `docs/BT_AUDIO_PLAN.md` | LE Audio 蓝牙耳机支持方案（V1.1 规划）|
| `SESSION_SUMMARY.md` | 关键决策 / 教训 |
| `开发日志.md` | R 节点详细记录 |

---

**作者**：Claude（按全局 CLAUDE.md 9.x 规范创建）  
**维护规则**：每次 R 节点 commit 后必须更新
