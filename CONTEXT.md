# CONTEXT.md — TapeBook 30 秒恢复指南

> **项目**：ESP32-S3 听书机（磁带机风格音频播放器）  
> **仓库**：`zhutao198/tapeplayer`（GitHub）  
> **本地**：`D:\zhutao\audio_player`  
> **最后更新**：2026-07-03

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
| **书签** | `main/bookmark.cpp` | V1.2 stub |
| **电源** | `main/power_mgmt.cpp` | 定时关机 / 电量检测 stub |
| **语音** | `main/voice_prompt.cpp` | 预录 WAV 提示音 stub |
| **配置** | `main/config.h` | GPIO 引脚定义 |
| **构建** | `CMakeLists.txt` + `main/CMakeLists.txt` | 顶层 + 组件 |
| **分区** | `partitions.csv` / `partitions_ota.csv` | 单一 factory / OTA |
| **配置模板** | `configs/sdkconfig.defaults.wroom-*` | 双模组切换 |

---

## 4. R 节点全景（维护中）

| R 节点 | 日期 | 内容 | 状态 |
|---|---|---|---|
| baseline | 2026-07-03 | 首次提交（仓库基线） | ✅ |
| R001 | 待定 | 启用 ESP-ADF（解锁音频播放） | ⏳ |
| R002 | 待定 | 安装 u8g2 component（OLED 真实显示） | ⏳ |
| R003 | 待定 | build 验证通过 | ⏳ |

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

---

## 6. 服务 / 工具信息

| 项 | 值 |
|---|---|
| 串口（烧录） | 待确认（Win 上 `idf.py -p COMx flash`） |
| ESP-IDF 版本 | v5.3（推荐） |
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
| `SESSION_SUMMARY.md` | 关键决策 / 教训 |
| `开发日志.md` | R 节点详细记录 |

---

**作者**：Claude（按全局 CLAUDE.md 9.x 规范创建）  
**维护规则**：每次 R 节点 commit 后必须更新
