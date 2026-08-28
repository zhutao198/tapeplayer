# 项目协作策略（CLAUDE.md）

> 本文件为 AI 助手与用户的协作约定，每次打开项目时自动生效。

## 全局策略

### 1. 打开项目先读日志
每次开始新会话/任务前，**必须先读取项目调试日志** `docs/DEBUG_LOG.md`，了解：
- 工具路径与环境（IDF/ADF/串口）
- 烧录地址映射与分区表
- 显示引脚与已知调试探针（display.cpp 中 `DBG:` 标记）
- 历史排查结论与当前待办状态

若日志缺失或与代码不符，先更新日志再动手。

### 1.5 烧录硬性约束（最高优先级）
- **硬件不能通过串口控 boot**，必须用户**手动进下载模式**（按住 BOOT→按 RESET→松开 BOOT）且**烧录前关闭串口助手**。
- 烧录**只能用 esptool 直写**（`--before no_reset`），**禁止 `idf.py flash`**。
- 工作流：先编译 → 等用户说"就绪"再烧。详细命令见 `docs/BUILD_FLASH.md`。
- 串口 `COM7`；PowerShell 下需用 `cmd /c "..."` 包裹 IDF 命令（否则 `export.bat` 不生效，`python -m esptool` 报 `No module named esptool`）。

### 2. 重要操作均写入日志
以下操作完成后，**必须同步更新** `docs/DEBUG_LOG.md`：
- 编译 / 烧录 / 擦除（含命令、端口、结果 SHA）
- 硬件接线、引脚变更、分区表改动
- 调试探针增删（display.cpp `DBG:` 行号与含义）
- 排查结论、根因、待办状态变化
- 环境/工具路径变更

写入原则：保留历史记录（追加或更新对应章节），不覆盖已完成的排查结论。

## 项目关键信息速查
- 日志主文件：`docs/DEBUG_LOG.md`
- 编译/烧录入口：`build.bat`（内部自动 export IDF 环境，**不要手动 set IDF_PATH**）
- 串口：`COM7`（烧录前关闭串口助手）
- 芯片：ESP32-S3-WROOM-1 N16R8，IDF v5.5.3，ADF commit `d0493218`
- 显示：ST7789 SPI3，引脚见 config.h；调试探针见 display.cpp `DBG:`
- 中文 TTF 烧录：`flash_font.bat COM7`（font 分区 @0x620000）
