# CONTEXT.md — TapeBook 30 秒恢复指南

> **项目**：ESP32-S3 听书机（磁带机风格音频播放器）  
> **仓库**：`zhutao198/tapeplayer`（GitHub）  
> **本地**：`D:\zhutao\audio_player`  
> **最后更新**：2026-08-12（R047 — 全量代码评审通过；R048 打磨进行中：音量 15 档重构 + SD 检测增强 + 硬件 PCB 微调）

---

## 1. 项目一句话定位

基于 **ESP32-S3** 的磁带机风格音频播放器，支持快进/快退 3 档变速模拟传统磁带体验，量产模组为 **WROOM-1 N16R8**（带 OTA）。V1.0 MVP 功能已基本实现并通过代码评审。

---

## 2. 30 秒恢复（新会话开场必做）

```bash
cd D:/zhutao/audio_player
git log --oneline -5           # 最新 5 个 commit
git tag -l "R*" | Select-Object -Last 5      # 最新 5 个 R 节点 (PowerShell)
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
| **音频** | `main/audio_player.cpp` | 音频引擎（依赖 ESP-ADF，R031 启用） |
| **磁带控制** | `main/tape_control.cpp` | 3 档变速状态机（R044 改 3 档 2x/4x/8x） |
| **按键** | `main/button_manager.cpp` | 8 按键（6 直键 + 2 音量拨轮）+ 状态机去抖 |
| **显示** | `main/display.cpp` | SPI TFT ST7789（原生 esp_lcd + LVGL v9，`lv_conf.h` 配置） |
| **设置** | `main/settings.cpp` | NVS 持久化（断点续播、音量、模式） |
| **书签** | `main/bookmark.cpp` | NVS 持久化书签（浏览中停止长按添加，R011） |
| **电源** | `main/power_mgmt.cpp` | 定时关机 / 电量检测 stub + 锁存唤醒 |
| **配置** | `main/config.h` | GPIO 引脚定义 + 音量 15 档 + SD_CD 极性 |
| **自定义 Board** | `components/audio_board/` | 覆盖 ADF 自带 audio_board 的 tapebook 桩板（`CONFIG_ESP_TAPEBOOK_BOARD=y`） |
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
| R004 | 2026-07-03 | `377a893` | 修复 CMakeLists.txt 启用 ADF | ✅ |
| **R005** | **2026-07-03** | **`65ca4ea`** | **修 HARDWARE_PIN_WIRING.md 5 处错误 + 补 MAX98357A 规格书** | **✅** |
| **R006** | **2026-07-03** | **`126af18`** | **修 HARDWARE_PIN_WIRING.md 5 处错误 + 补 SSD1315 规格书** | **✅** |
| **R007** | **2026-07-03** | **`e7fb604`** | **首次成功构建！修 board.h / audio_player.cpp API / u8g2_esp32_hal 兼容性** | **✅** |
| **R008** | **2026-07-06** | **`2530f23`** | **代码审查 33 项修复** | **✅** |
| **R009** | **2026-07-06** | `853f483` | **审查剩余 9 项修复** | ✅ |
| **R010** | **2026-07-06** | `76441b1` | **审查余下 8 项清零** | ✅ |
| **R011** | **2026-07-07** | `df11f0d` | **修复 R010 引入的 6 个 bug + H-8 ADC 桩 + L-1 bookmark 按键集成** | ✅ |
| **R012** | **2026-07-07** | `1d95d12` | **文件夹浏览（V1.0 MVP 最后功能）** | ✅ |
| **R013** | **2026-07-07** | `4f3b25e` | **R012 review 修复** | ✅ |
| **R014** | **2026-07-09** | `eca38cc` | **PRD 审查 5 项修复 + 原理图设计** | ✅ |
| **R015** | **2026-07-10** | **`d54d0ed`** | **硬件设计修复 + LE Audio 方案文档** | ✅ |
| **R018** | **2026-07-11** | **`8a90513`** | **代码审计修复 19 项** | ✅ |
| **R019** | **2026-07-11** | **`06f9be9`** | **R018 build 验证 + 修复 3 个编译副作用** | ✅ |
| **R020** | **2026-07-11** | **`06bb8d0`** | **R018 评审闭环 + H-3 用户重做** | ✅ |
| **R021** | **2026-07-17** | **`1d03d03`** | **Batch 1 深度评审修复 10 项** | ✅ |
| **R022** | **2026-07-17** | **`584cf67`** | **Batch 2 深度评审修复 5 项** | ✅ |
| **R023** | **2026-07-17** | **`3655ff3`** | **R021/R022 文档补正 + M3 ALC 注释落地** | ✅ |
| **R024** | **2026-07-17** | **`907923b`** | **Claude 对照审核 R023** | ✅ |
| **R025** | **2026-07-17** | **`22f1540`** | **完整全项目代码审计** | ✅ |
| **R026** | **2026-07-17** | **`110c238`** | **R025 全项目审计合并到 docs/CODE_REVIEW_R023.md** | ✅ |
| **R027** | **2026-07-17** | **`3242c60`** | **团队评审反馈修正：M4/M5 撤回** | ✅ |
| **R028** | **2026-07-17** | **`e64c6d6`** | **R025 P0/P1/L1/L6 修复** | ✅ |
| **R029** | **2026-07-17** | **`f15ec83`** | **H1 显示一致性微调** | ✅ |
| **R030** | **2026-07-20** | **`8e8490f`** | **批量修复合并评审 17 项** | ✅ |
| **R031** | **2026-07-29** | **—** | **ADF 选板迁移：项目内 `components/audio_board` 覆盖 ADF 组件** | ✅ |
| **R032** | **2026-07-29** | **—** | **显示层迁移 Phase 1：自写 ST7789 + 原生 esp_lcd + LVGL 9.5.0** | ✅ |
| **R033** | **2026-07-29** | **—** | **UI 重写（Phase 2）：主播放/浏览/提示三界面 LVGL** | ✅ |
| **R034** | **2026-07-29** | **—** | **UI 三点改进：全中文 / 磁带动画 / 按键便捷** | ✅ |
| **R035** | **2026-07-29** | **—** | **UI 三项优化：快退同速 / 图形电量音量 / 按键按物理位置重排** | ✅ |
| **R036** | **2026-07-29** | **—** | **按键映射重构：移除锁定 / 双击改长按 / 音量专用键占位** | ✅ |
| **R037** | **2026-07-29** | **—** | **浏览模式长按连续移曲** | ✅ |
| **R038** | **2026-07-30** | **—** | **浏览 FF/RW 翻页+跳头尾 + 按键名按功能显示** | ✅ |
| **R039** | **2026-07-30** | **—** | **停止改安全停止/续播 + 快退+停止组合键跳曲首** | ✅ |
| **R040** | **2026-07-30** | **—** | **FF/RW 态按键互锁 + 组合键时间戳清理** | ✅ |
| **R042** | **2026-08-03** | **—** | **新增 GPIO0/GPIO3 专用音量+/-键（LCK 拨轮）** | ✅ |
| **R043** | **2026-08-03** | **—** | **长按全检修复（FF/REW 重复 press / VOL 计数器串扰 / EXTRA_LONG 覆盖）** | ✅ |
| **R044** | **2026-08-03** | **—** | **变速档位 4 档→3 档（2x/4x/8x）** | ✅ |
| **R045** | **2026-08-03** | **—** | **短按阈值 800ms + 进态即 2x + 短按跳 5 秒** | ✅ |
| **R046** | **2026-08-03** | **—** | **修复"刚过长按倒退反少于短按"断层（进态先补 ±5s 基准跳退）** | ✅ |
| **R047** | **2026-08-03** | `1d18f57` | **清理 button_manager 双击状态机死代码 + 全量代码评审报告 v2/v3（O6 已修）** | ✅ |
| **R048** | **2026-08-12** | **（WIP，未提交）** | **音量系统 V1.2 重构（0-100 → 15 档逻辑音量 dB 线性 -96..+12）+ SD 卡检测增强（极性宏/状态栏图标/插拔提示）+ 硬件 PCB 微调 + 评审报告 v3 更新** | 🚧 |
> 详细变更见 `开发日志.md`，回滚命令：`git checkout <tag>`。（注：R016/R017/R041 编号在历史上被跳过/合并，不影响连续性）

---

## 5. 关键决策速查

| 决策 | 选择 | 理由 | 文档 |
|---|---|---|---|
| 主控 | ESP32-S3-WROOM-1 N16R8（量产） | BOM 低、Octal PSRAM 8MB 够用、3.3V VDD_SPI 简化 PCB | HARDWARE_MODULE_MIGRATION.md |
| 音频框架 | ESP-ADF v2.7 | 多格式解码（MP3/AAC/FLAC/OGG/Opus） | README.md |
| 存储 | MicroSD SPI | 简单可靠 | README.md |
| 显示 | ST7789 2.0寸 SPI TFT 320×240（SPI3_HOST 独立总线） | 原理图 V1 定型；方向可切换（`DISPLAY_ORIENTATION`） | DESIGN.md §5.6 |
| 断点续播 | NVS 命名空间 `tapebook` | 不占 SD 写寿命 | DETAILED_DESIGN.md |
| 音量控制 | I2S ALC (i2s_alc_volume_set) + **15 档逻辑音量**（R048）| ADF 内置 ALC，-96~+12dB；15 档更贴合实体键手感 | audio_player.cpp / config.h |
| 显示驱动 | 原生 esp_lcd（`esp_lcd_panel_st7789`，IDF v5.5.3 内置）+ LVGL v9 | 零第三方 ST7789 库；u8g2/自写 SPI 驱动废弃 | docs/PLAN_LVGL_ESP_LCD_MIGRATION.md |
| ADF 选板 | 项目内 `components/audio_board` 覆盖 ADF 组件（tapebook 桩板） | 不依赖现成开发板、方案随仓库版本化 | docs/PLAN_TAPEBOOK_ADF_BOARD.md |
| 蓝牙方案 | LE Audio（LC3，无需额外 BOM）| ESP32-S3 仅有 BLE 5.0，无 BT Classic | BT_AUDIO_PLAN.md |
| 音量键 | GPIO0/GPIO3 专用 LCK 拨轮（R042）| 替代 EC11/Prev-Next 长按，避免双路径冲突 | button_manager.cpp |
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
git checkout R047
# 或查看
git show R047

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
| `docs/DEVELOP_STATUS.md` | 功能完成度对照表（vs PRD，2026-08-12 更新） |
| `HARDWARE_MODULE_MIGRATION.md` | 模组迁移指南 |
| `docs/HARDWARE_PIN_WIRING.md` | 硬件引脚接线图 |
| `docs/BT_AUDIO_PLAN.md` | LE Audio 蓝牙耳机支持方案 |
| `SESSION_SUMMARY.md` | 关键决策 / 教训 |
| `开发日志.md` | R 节点详细记录 |

---

**作者**：Claude（按全局 CLAUDE.md 9.x 规范创建）  
**维护规则**：每次 R 节点 commit 后必须更新
