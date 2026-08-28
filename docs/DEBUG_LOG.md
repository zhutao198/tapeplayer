# TapeBook 调试日志 — 工具 / 路径 / 调试信息

> 记录时间：2026-08-17
> 关联文档：`CONTEXT.md`、`SESSION_SUMMARY.md`
> 用途：新会话快速恢复——所有工具路径、环境、调试探针、烧录信息集中在此

---

## 1. 开发环境

| 项 | 值 |
|---|---|
| 操作系统 | Windows 10/11 (PowerShell) |
| 项目本地路径 | `D:\zhutao\audio_player` |
| 仓库 | `zhutao198/tapeplayer` (GitHub) |
| ESP-IDF 版本 | v5.5.3 |
| IDF_PATH | `D:\esp\v5.5.3\esp-idf` |
| ESP-ADF 路径 | `D:\esp\esp-adf` (ADF commit `d0493218` — release/v2.x) |
| IDF_TOOLS_PATH | `C:\Users\zhuta\.espressif` |
| Python | IDF 自带虚拟环境 (`python` via export.bat) |
| 目标芯片 | ESP32-S3 (set-target esp32s3) |
| 模组分型 (量产) | ESP32-S3-WROOM-1 N16R8 (8MB Octal PSRAM) |
| 串口端口 | `COM7` (921600 baud, 烧录时需关闭串口助手占用) |

---

## 2. 工具与脚本路径

### 2.1 一键脚本（项目根目录）

| 脚本 | 路径 | 用途 |
|---|---|---|
| `build.bat` | `D:\zhutao\audio_player\build.bat` | 编译/烧录入口；内部先 `call D:\esp\v5.5.3\esp-idf\export.bat` 再设 ADF_PATH，最后透传 `idf.py %*`。**优先用此脚本，不要手动 set 环境**（会破坏 IDF_TOOLS_PATH）。 |
| `configure.bat` | `D:\zhutao\audio_player\configure.bat` | 切换模组：`configure.bat wroom-1-n16r8` / `wroom-2-n32r16v` / 加 `-bt` 后缀为蓝牙版 |
| `flash_font.bat` | `D:\zhutao\audio_player\flash_font.bat` | 烧中文 TTF：`flash_font.bat COM7` → 写 `tools\cjk.ttf` 到 font 分区 `0x620000` |
| `clone_adf.bat` / `do_submodules.bat` / `fix_idf_submodules.bat` | 项目根 | ADF 子模块初始化/修复 |

### 2.2 常用命令（在 IDF 环境内执行）

```bash
# 进入项目并激活环境（PowerShell）
cd D:\zhutao\audio_player
# 推荐直接跑 build.bat，它会自动 export IDF

# 编译
build.bat build
# 烧录（含 bootloader/partition/ota_data）
build.bat -p COM7 flash
# 监视串口
build.bat -p COM7 monitor
# 仅监视
idf.py -p COM7 monitor
```

### 2.3 esptool 直接烧录

esptool.py 由 IDF 环境提供（export.bat 后可用）。**PowerShell 不要用 `cd /d`**（那是 cmd 语法）。

```bash
# 进入下载模式：按住 BOOT 不放，点 RESET，保持 ~2 秒后松开 BOOT
# 全片擦除
esptool.py --port COM7 erase_flash

# 按 flash_args 烧录（路径相对于 build 目录）
cd D:\zhutao\audio_player
idf.py -p COM7 flash            # 推荐，等价于 @build/flash_args

# 单独烧 font 分区
esptool.py --port COM7 write_flash 0x620000 tools\cjk.ttf

# 单独擦除某个区域（谨慎）
esptool.py --port COM7 erase_region 0x210000 0x2000   # 擦 otadata 强制 factory
```

> ⚠️ 历史坑：曾误把 `erase_region` 的 offset 当文件名传入导致失败；正确做法是 `esptool.py --port COM7 write_flash @build/flash_args` 整体烧录。

### 2.4 ✅ Claude Code 可以通过 Python wrapper 跑 ESP-IDF 命令（已验证）

**修正历史**：本节最初记录"Claude Code 不能跑 build.bat"（2026-08-18 上午尝试 5 种方法全失败），用户提示"CodeBuddy 一直能编译"后，经调查找到正确方案。

**真正的根本问题**（不只是 MSYSTEM）：
1. MSYSTEM=MINGW64 由 MSYS2 / Git Bash 启动时**强制注入**到 Windows 进程环境，**所有** fork 出的 cmd/PowerShell 子进程都继承。
2. **bash 重定向元字符**（`>` `>>` `<` `>&`）在 `python -c "..."` 命令行上被 bash 在解析阶段吞掉，**不会传给 Python**，导致 `>nul` `2>&1` 这类 cmd 重定向被翻译成 `>/dev/null 2>&1`（cmd 不识别）。

**解决方案**（两步绕过）：
1. **剥 MSYSTEM**：用 Python `os.environ.copy()` 复制环境 + `del env['MSYSTEM']`，传给 subprocess 的 env 参数完全替换子进程环境。
2. **避开 bash 元字符**：把命令字符串写到 `.bat` 文件（Python 用 `open(path, 'w').write(...)`），再 `subprocess.run(['cmd.exe', '/D', '/C', bat_path], env=env)`。`/D` 关闭 cmd AutoRun 注册表脚本，避免环境被改。

**已实现**：项目根 `tools/_run_in_clean_cmd.py`（含 build/flash/version/monitor 子命令）：

```bash
# 从 Git Bash / Claude Code 直接跑：
python tools/_run_in_clean_cmd.py build      # 编译
python tools/_run_in_clean_cmd.py flash      # 烧录（需用户就绪）
python tools/_run_in_clean_cmd.py version    # 验证 IDF 环境
python tools/_run_in_clean_cmd.py monitor    # 监视串口
```

**手动等价命令**（在**独立的 PowerShell 窗口**跑，不经过 Git Bash）：

```powershell
cmd /c "cd /d d:\zhutao\audio_player && call D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1 && idf.py build"
```

或 **独立的 cmd 窗口**：

```cmd
cd /d D:\zhutao\audio_player
build.bat build
```

**反向经验（不要做的）**：
- ❌ 直接 `cmd.exe //c "build.bat"`（MSYSTEM 污染）
- ❌ `python -c "...>nul 2>&1..."`（bash 吞掉 `>` 元字符）
- ❌ `MSYS_NO_PATHCONV=1 python ...`（不影响 bash 自身的重定向解析）
- ❌ `set MSYSTEM=` 在 cmd 里（设空字符串，`if defined` 仍 true）
- ❌ `env -u MSYSTEM cmd ...`（实测 cmd 仍能看到 MSYSTEM=MINGW64）

---

## 3. 烧录地址映射（partitions_ota.csv，16MB Flash）

| 分区 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| bootloader | `0x0` | (bin) | `build/bootloader/bootloader.bin` |
| partition-table | `0x8000` | (bin) | `build/partition_table/partition-table.bin` |
| factory | `0x10000` | `0x200000` (2MB) | `build/audiobook_player.bin` |
| otadata | `0x210000` | `0x2000` | `build/ota_data_initial.bin` |
| ota_0 | `0x220000` | `0x200000` | OTA slot 0 |
| ota_1 | `0x420000` | `0x200000` | OTA slot 1 |
| **font** | **`0x620000`** | **`0x800000` (8MB)** | **中文 TTF (`tools/cjk.ttf`)，freetype 按需渲染** |

`build/flash_args` 内容：
```
--flash_mode dio --flash_freq 80m --flash_size 16MB
0x0 bootloader/bootloader.bin
0x10000 audiobook_player.bin
0x8000 partition_table/partition-table.bin
0x210000 ota_data_initial.bin
```

非 OTA 分区表 (`partitions.csv`) 的 font 偏移为 `0x310000`。

SD 升级包产物：`build\TAPEBOOK.BIN` + `TAPEBOOK.VER` + `TAPEBOOK.SHA256`（由 build.bat 自动生成）。

---

## 4. 显示相关硬件引脚（main/config.h）

| 信号 | GPIO | 说明 |
|---|---|---|
| DISPLAY_SPI_HOST | `SPI3_HOST` | 独立总线（SD 走 SPI2，互不冲突） |
| DISPLAY_SCLK_IO | `GPIO_NUM_8` | TFT_SCL (U1.12) |
| DISPLAY_MOSI_IO | `GPIO_NUM_18` | TFT_SDA (U1.11) |
| DISPLAY_DC_IO | `GPIO_NUM_16` | TFT_DC (U1.9) |
| DISPLAY_RESET_IO | `GPIO_NUM_17` | TFT_RES (U1.10) — 硬件复位 |
| DISPLAY_BLK_IO | `GPIO_NUM_15` | TFT_BLK (U1.8) — LEDC PWM 背光 |
| DISPLAY_CS_IO | `(-1)` | CS 固定接地，常选模式 |
| LCD_POW_EN_IO | `GPIO_NUM_39` | LCD 软电源开关 (PMOS 低电平导通) (U1.32) |

显示分辨率：`DISPLAY_ORIENTATION==0` → 320×240 横屏（swap_xy=true）；`==1` → 240×320。

---

## 5. 显示初始化流程与调试探针（main/display.cpp）

调用链：`display_init()` (display.cpp:761)
→ `lcd_backlight_init()` + `display_set_brightness(100)`
→ `lcd_hw_init()` (display.cpp:199)
→ `lv_init()`
→ `font_partition_init()`
→ LVGL buffer 分配 (PSRAM 优先)
→ `lv_display_create` + `lvgl_flush_cb` 绑定
→ `ui_create()` + 定时器
→ `display_mem_report()`
→ `ui_show_msg()` (首屏，freetype 未就绪时用 ASCII 兜底)
→ `xTaskCreatePinnedToCoreWithCaps(lvgl_task, ...)` (display.cpp:815)

### 5.1 已插入的 DBG 探针（用于定位黑屏卡死点）

位置均为 `display.cpp`，搜索 `DBG:` 可定位：

| 行号 | 探针 | 含义 |
|---|---|---|
| 204 | `DBG: lcd_pow_en set 0, readback=%d` | LCD 供电拉低导通后读回确认 |
| 214 | `DBG: lcd hw reset done` | RST 复位序列完成后（上电后先复位一次） |
| 168/174 | `DBG: flush_cb enter ...` / `DBG: flush_cb done` | LVGL flush 回调进/出（卡死点在此 `esp_lcd_panel_draw_bitmap`） |
| 186/188/190 | `DBG: lvgl loop top` / `before timer_handler` / `after timer_handler` | lvgl_task 循环各阶段 |
| 800/803/807/810 | `DBG: mem report done` / `show msg zh` / `show msg ascii` / `ui_show_msg done` | 首屏消息前后 |
| 814/817 | `DBG: before lvgl task create` / `after lvgl task create` | 创建渲染任务前后 |

### 5.2 硬件复位序列（用户建议：上电后、屏初始化前先复位一次）

`lcd_hw_init()` (display.cpp:206-214) 已实现：
```cpp
gpio_set_direction(DISPLAY_RESET_IO, GPIO_MODE_OUTPUT);
gpio_set_level(DISPLAY_RESET_IO, 1);          // 先高，退出复位态
vTaskDelay(pdMS_TO_TICKS(10));
gpio_set_level(DISPLAY_RESET_IO, 0);          // 拉低复位
vTaskDelay(pdMS_TO_TICKS(20));                 // ST7789 要求 RST 低 >=10ms
gpio_set_level(DISPLAY_RESET_IO, 1);          // 释放复位
vTaskDelay(pdMS_TO_TICKS(120));                // 复位后等 >=120ms 再发命令
ESP_LOGI(TAG, "DBG: lcd hw reset done");
```

---

## 6. 字体 / freetype 调试（main/font_partition.cpp）

- `font_partition_init()` (font_partition.cpp:88)：注册自定义 newlib VFS `/font` 把 font 分区当 `cjk.ttf` 文件暴露；`lv_freetype_init(1024)` 失败返回 `LV_RESULT_INVALID`(0) 时 **必须 `lv_freetype_uninit()` 释放半初始化上下文**，否则后续渲染中文死循环 + task_wdt。
- 失败日志：`lv_freetype_init 失败 (lv_result=%d)`，中文缺字回退不可用。
- 首屏 splash 在 freetype 未就绪时用 ASCII（`"Initializing..."` / `"loading SD..."`）避免死循环。
- 字库生成：`python tools\gen_font.py ttf` → `tools\cjk.ttf`（全量 CJK + 常用标点 + ASCII）。

---

## 7. 黑屏排查历史（关键决策/教训）

| 阶段 | 现象 | 结论 | 动作 |
|---|---|---|---|
| ① | 擦除 ota_data 强制 factory，设备仍跑旧固件 SHA `a6dce79c` | 旧 bin 未清，factory 未写入 | 删 `build/*.bin` 重编 |
| ② | ninja 增量损坏，bootloader.bin 缺失 | 增量状态错乱 | `rmdir /s /q build` 从零编译 |
| ③ | erase_flash 后复位反复 `invalid header: 0xffffffff` | 擦除成功验证 | 全片烧录 |
| ④ | 新固件 SHA `067b142b2` 仍黑屏，日志卡 `display_mem_report()` 后 | 卡在 lvgl_task 第一次 flush_cb→draw_bitmap | 加 DBG 探针定位 |
| ⑤ | 烧 font 分区后 freetype 仍 `lv_result=0` | freetype 初始化失败（另一问题，非黑屏主因） | 保留 ASCII 兜底 |
| ⑥ | 用户确认：上电/不上电屏幕有区别且有暗光、EN 脚正常、接线无误 | 供电/背光通，疑 ST7789 上电复位时序 | 加硬件复位序列（§5.2） |
| ⑦ | **2026-08-17 烧录验证**：进下载模式，`build.bat -p COM7 flash` 成功；手动 RESET 后捕获日志，flush_cb 不再卡死 | 误判！日志"正常"≠屏幕成像。直绘全屏红屏 ret=OK 但用户反馈**仍黑屏仅微弱亮光** | 推翻结论，继续排查 |
| ⑧ | **2026-08-18 根因锁定**：直绘红屏 `draw_bitmap(0,0,320,240)` ret=0 但屏幕只"有点亮光、不全红" | **`buscfg.max_transfer_sz = DISPLAY_WIDTH*40*2 = 25600` 太小**，满屏 320*240*2=153600 字节被 SPI 事务截断，只刷了前 ~1/6 屏 | 改 `max_transfer_sz = DISPLAY_WIDTH*DISPLAY_HEIGHT*2` |

### 阶段 ⑨ 根因修正说明（2026-08-18）

> **重要纠错**：阶段⑦"黑屏已解决"的结论**错误**。日志 flush_cb 完成、直绘 ret=OK 只代表 SPI 命令发出，**不代表图像完整成像**。用户实测屏幕"只是有点亮光、比原来亮但不全红"，证明数据被截断。

**真正的根因（阶段⑧）**：
- `buscfg.max_transfer_sz = DISPLAY_WIDTH * 40 * 2 = 320*40*2 = 25600` 字节（display.cpp 224 行）。
- 满屏一帧 = `320 * 240 * 2 = 153600` 字节，是上限的 **6 倍**。
- esp_lcd ST7789 `draw_bitmap` 用 `tx_color` 一次性传 `len` 字节，底层 SPI 事务被 `max_transfer_sz` 截断 → 只刷了前 25600 字节（约屏幕左上 1/6）→ 屏幕只点亮局部 → 表现为"黑屏 + 微弱亮光"。
- 之前 LVGL 同样只刷了脏区所在的左上角，其余为默认黑，所以一直看着像黑屏。

**修复**（display.cpp）：
```cpp
// 旧：buscfg.max_transfer_sz = DISPLAY_WIDTH * 40 * 2;            // 25600
// 新：
buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2;     // 153600 整屏
```
- 同时把验证探针从"纯红屏"改为"红底 + 白色十字"，便于一眼确认整屏是否完整成像（十字完整=传输无截断）。
- 编译通过：`build.bat -p COM7 build` → audiobook_player.bin 0x1a76f0 bytes。

### 阶段 ⑩ 对齐厂家 2.4 寸屏参数（2026-08-18）

- 用户提供屏资料目录 `hardware/2.4寸_ST7789_240x320/`，含厂家参考驱动 `2.4-2.8_ST7789spiORIfastRGB(QQWW)h.c`。
- **厂家关键参数（GMT024-08-SPI8P 2.4寸 ST7789 240xRGBx320）**：
  - `0x36 MADCTL = 0x20` → MV=0（**不交换XY**）、RGB 顺序（**非 BGR**）、横屏 320×240。
  - 满屏窗口：`0x2A=0x0000~0x013F`（列 0..319）、`0x2B=0x0000~0x00EF`（行 0..239）→ **GRAM 寻址即 320×240 横屏**，证明 `draw_bitmap(0,0,320,240)` 正确、不溢出。
  - `0x3A = 0x05`（RGB565）、`0x21` 反显开启、`0xB1/0xB2/0xB3/0xB7/0xBB/0xC0~0xC6/0xD0` 等 gamma/power 序列。
- **对照当前代码的配置错误（已修正）**：
  1. `rgb_ele_order` 我之前误改为 `LCD_RGB_ELEMENT_ORDER_BGR` → **改回 RGB**（厂家 MADCTL bit3=0 = RGB）。
  2. 方向逻辑 `swap_xy(true)+mirror(false,true)`（DISPLAY_ORIENTATION!=1 分支）→ **改为 swap_xy(false)+mirror(false,false)**，对齐厂家 MADCTL=0x20（横屏 MV=0）。
  3. `invert_color(true)` 与厂家 `0x21` 反显一致，保留。✓
- 注：厂家 gamma/power 序列 esp_lcd 内置 st7789 panel_init 已含标准版，色彩精度微差不影响成像，暂未硬抄。
- 编译通过，待烧录验证：预期屏幕显示**满屏红 + 白色十字**（十字完整=截断修复+方向正确）。

### 阶段 ⑪ 花屏排查（2026-08-18）

- 用户上传照片：屏幕左侧约 1/3 雪花纹、右侧约 2/3 水平灰条，不是全红。
- 诊断：SPI 能传大量数据（截断已修），但数据解析错乱。最可能原因按优先级：
  1. **SPI 时钟 40MHz 过高** → 2.4 寸长排线模组建议降到 10~20MHz。
  2. **DMA buffer 位于 PSRAM** → 缓存/时序一致性风险，改到内部 DMA RAM。
- 已修改（display.cpp）：
  - `io_cfg.pclk_hz = 10 * 1000 * 1000`（40MHz → 10MHz）。
  - 红屏测试 buffer：`heap_caps_malloc(..., MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`。
- 编译通过，待烧录验证：预期花屏消失，显示满屏红 + 白色十字。

### 阶段 ⑫ 花屏根因锁定：SPI 事务上限（2026-08-18）

- 降速到 10MHz、改内部 DMA buffer 均无效 → 排除时序/PSRAM 因素。
- **真正根因（读 esp_lcd st7789 驱动源码确认）**：
  - `panel_st7789_draw_bitmap` 第 226 行：`len = (x_end-x_start)*(y_end-y_start)*bpp/8`；整屏调用 `draw_bitmap(0,0,320,240)` → `len=153600` 字节**一次性 tx_color 发送**。
  - ESP32-S3 SPI 外设**单次事务上限约 32767 字节**（DMA 描述符限制）。超过后数据错位 → 固定分块花屏（左 1/3 雪花+右 2/3 灰条）。
  - 之前 `max_transfer_sz=320*40*2=25600` 时"只点亮局部"也是同一上限导致：25600 仍可能触发 SPI 驱动非 DMA 路径只发一片。
  - **LVGL draw buffer 也是整屏 153600 + `LV_DISP_RENDER_MODE_FULL`** → 每次 flush 同样超上限 → 花屏。这才是用户一直看到花屏的本质。
- **修复**（display.cpp）：
  1. `max_transfer_sz = 32767`（SPI 事务安全上限，底层自动分片）。
  2. **LVGL draw buffer 改为 40 行部分缓冲 + `LV_DISP_RENDER_MODE_PARTIAL`**：`buf_lines=40`，`buf_px=320*40`，每次 flush 仅 ~25600 字节，自然分块。
  3. **直绘测试改分块发送**：每块 40 行循环 `draw_bitmap(0,y0,320,y0+rows)`，避免单次超限。
- 编译通过，待烧录验证：预期花屏消失，显示满屏红 + 白色十字；后续 LVGL UI 也应正常成像。

### 阶段 ⑬ 花屏定位为 GRAM 列溢出（2026-08-18）

- 用户反馈：花屏区域是**左侧 1/4（80 列宽）**，右侧 3/4 正常。
- 诊断：整屏 320 列中，前 240 列（3/4）正常、后 80 列（1/4）花屏 → **ST7789 物理 GRAM 列数 = 240**，写 320 列时第 240~319 列溢出到屏幕外/折返区 → 左侧 1/4 花屏。
- **关键纠错**：重新解析厂家 `0x36=0x20` —— ST7789 MADCTL 寄存器 bit 分配：bit7=MY, bit6=MX, bit5=MV, bit3=RGB。**0x20 = bit5(MV)=1 → swap_xy=TRUE**（之前误读为 MV=0）。即厂家横屏靠 `swap_xy=true` 让 MCU 的 320 列映射到物理 320 行方向（而非 240 列方向），避免溢出。
- 阶段⑩ 我误把 `swap_xy` 改成 `false` 是**错误根因之一**（叠加在阶段⑫ 的 SPI 上限问题之上）。
- **修正**（display.cpp）：`esp_lcd_panel_swap_xy(s_panel_handle, true)`（恢复对齐厂家 MADCTL=0x20），`mirror(false,false)` 保留。
- 至此三项修复齐备：① SPI 上限分块（max_transfer_sz=32767 + LVGL 部分缓冲 + 直绘分块）；② 颜色 RGB；③ swap_xy=true 对齐厂家扫描方向。
- 编译通过，待烧录验证：预期满屏红 + 白色十字，无花屏；LVGL UI 正常成像。

### 阶段 ⑭ 诊断方向（2026-08-18）

- 阶段⑬ `swap_xy=true` 后**完全不显示** → 旋转方向使内容落到可见区外。
- 阶段⑫ `swap_xy=false` + 分块 → **左侧 1/4 花屏**，右侧 3/4 正常 → 写 320 列溢出物理 240 列 RAM，前 240 列可见、后 80 列(逻辑 240..319)溢出花屏。
- **结论**：物理 GRAM = 240 列 × 320 行。`swap_xy=false` 时方向基本对（能显示），但宽度溢出；`swap_xy=true` 时旋转后整屏落到可见区外。
- 为精确定位正确 MADCTL（MY/MX/MV 组合），改为**诊断探针**：`swap_xy=false` + 4 条竖色彩条（红绿蓝黄每 80 列）+ 顶部 4 行白条。烧录后用户目测：彩条如何分布、是否有花屏、白条在哪一行 → 反推正确方向配置。
- 编译通过，待烧录验证。

### 遗留问题（非阻塞，不影响显示）
- `E (1369) font_part: lv_freetype_init 失败 (lv_result=0)，中文缺字回退不可用`
  - freetype 初始化失败，首屏用 ASCII 兜底（`DBG: show msg ascii`）。
  - 屏幕可正常显示英文/ASCII 界面，中文 TTF 渲染暂不可用。
  - 待排查方向：font 分区 VFS `/font` 挂载是否成功、`tools/cjk.ttf` 格式是否被 freetype 接受（之前烧过 font 分区仍失败）。

**当前状态（2026-08-18）**：`max_transfer_sz` 已修复，待烧录验证。预期：烧录后屏幕应显示**满屏红 + 白色十字**；确认后移除红屏探针恢复 LVGL UI。DBG 探针保留到验证完成。

### 阶段 ⑮ 方向宏驱动改造（2026-08-18）

- 用户实测：屏幕 1/4 花屏 + 其他部分黑
- 根因定位：当前代码 `swap_xy(false), mirror(false, false)` 硬编码与 `config.h` 注释自相矛盾；横屏逻辑 320×240 必须 `swap_xy=true (MV=1)`，否则 LVGL 第 241~320 列溢出 ST7789 物理 GRAM (240 列)
- **修复**（未 commit，待烧录验证）：
  - `config.h:67-93`：`DISPLAY_ORIENTATION` 宏推导 `DISPLAY_SWAP_XY / DISPLAY_MIRROR_X / DISPLAY_MIRROR_Y` 三宏（横屏默认 `SWAP_XY=1`）
  - `display.cpp:269-274`：硬编码改为读宏，注释说明 mirror 烧录后按实际呈现微调
- 验证：烧录一次确认 1/4 花屏消失；若侧躺/镜像改 `DISPLAY_MIRROR_X/Y` 重烧第 2/3/4 次
- 详见 `docs/DISPLAY_DIAGNOSIS_2026-08-18.md` §10
- **未建 R 节点**：工作区还有 R049c/R050/R051 等 30+ 文件未 commit，方向改动随下一次批量 commit 一并建节点

### 阶段 ⑯ freetype 半瘫导致 lvgl_task 死循环卡死（2026-08-20）

#### 现象（用户报告 + 串口日志确认）
- 用户："显示颜色/镜像已解决，但界面卡住、按键无反应，中文不显示"
- 串口日志：启动后 `lvgl_task invoking main tick #1/#2` 之后**再无 #3**，约 32 秒后反复 `task_wdt` 触发，`CPU 0: lvgl` 一直 running
- backtrace 全卡在 `lv_timer_handler`（lv_timer.c）→ `lv_timer_handler` 遍历 timer 链表死循环出不来

#### 根因定位（用 addr2line 解码 backtrace + 读源码）
1. `lv_conf.h: LV_USE_FREETYPE=1` → LVGL 编译期全局注册 freetype 字体类
2. `font_partition.cpp: lv_freetype_init(1024)` 运行时**失败**（`lv_result=0` = `LV_RESULT_INVALID`，见 `lv_types.h:65` 该版本 `LV_RESULT_INVALID=0 / LV_RESULT_OK=1`，与常见定义相反）
3. init 失败 → `s_font_ready=false`，`lv_font_chinese_*.fallback` 未挂全量字体
4. **关键**：`LV_USE_FREETYPE=1` 让 LVGL 对任何"缺失字形"都会 query 半初始化/已 uninit 的 freetype library → 死循环
5. 触发点：`display.cpp:1193 lv_label_set_text(lbl_track, "白桦树.MP3")` —— 即使 UI 文本用英文，但 `lbl_track` 显示**中文文件名**，渲染时走 freetype 路径 → `lv_timer_handler` 卡死

> 注：用户记忆"已关闭中文"指 UI 字符串无中文，但 `LV_FONT_DEFAULT` 仍指向 `lv_font_chinese_14`、且 `LV_USE_FREETYPE=1` 未关 —— 字体子系统整体处于崩溃状态，与中英文 UI 无关。

#### 尝试过的修复（及为何不够）
- **第一次（失败）**：把 `lv_conf.h:63 LV_FONT_DEFAULT` 从 `&lv_font_chinese_14` 改 `&lv_font_montserrat_14`
  - 原因：以为默认字体引用中文子集字体触发 freetype
  - 结果：烧录后仍卡死。backtrace 相同 —— 因为死循环根因是 `LV_USE_FREETYPE=1` 全局注册，而非 `LV_FONT_DEFAULT`
  - 时间：2026-08-20 编译+烧录验证

#### 根治修复（2026-08-20，已编译通过+烧录验证）
- **改动 1**：`lv_conf.h:34` `LV_USE_FREETYPE 1` → `0`
  - 原因：中文一直没正常过，当前纯英文 UI；关掉后 LVGL 不再注册 freetype 字体类，任何缺失字形都不会 query 半瘫 freetype，死循环根因消除
- **改动 2**：`font_partition.cpp:111-137` 整段 freetype 逻辑用 `#if LV_USE_FREETYPE ... #else ... #endif` 包裹
  - 原因：`lv_freetype_init/uninit/font_create` 在 `LV_USE_FREETYPE=0` 时函数声明被 LVGL 头文件条件编译移除，直接调用会编译报错；`#else` 分支打印 `LV_USE_FREETYPE=0，中文渲染未启用（仅 ASCII UI）` 并 `s_font_ready=false`
  - 副作用：中文文件名（如"白桦树.MP3"）将显示为空白/方块，但不再卡死
- 编译：通过，无 warning/error，`audiobook_player.bin` 0x1a7a50（17% 余量）
- 烧录：COM7 成功，`Hash verified` + `100%` + `Leaving...`
- **待用户贴启动日志确认**：tick #3 起持续递增、`task_wdt` 消失、动画转、按键响应

#### 后续待办
- 若英文 UI 跑通，可考虑：① 让 `lbl_track` 对中文文件名做 ASCII 转写/截断，避免空白；② 将来要支持中文，需先解决 `lv_freetype_init` 失败（疑似 `espressif/freetype` 组件未真正编入 / FT 引擎依赖缺失），再开 `LV_USE_FREETYPE`

### 阶段 ⑰ 修复改错位置：真正开关是 sdkconfig 的 CONFIG_LV_USE_FREETYPE（2026-08-20）

#### 关键教训（前两次改动无效的根因）
- 在 `main/lv_conf.h` 改 `LV_USE_FREETYPE 0` / `LV_FONT_DEFAULT` **完全没生效** —— 烧录后日志仍打印 `font_part: lv_freetype_init 失败`
- 查 `managed_components/lvgl__lvgl/src/lv_conf_internal.h:3231-3238`：
  ```c
  #ifndef LV_USE_FREETYPE
      #ifdef CONFIG_LV_USE_FREETYPE
          #define LV_USE_FREETYPE CONFIG_LV_USE_FREETYPE   // ← 来自 sdkconfig/Kconfig
  ```
  **LVGL 实际使用的 `LV_USE_FREETYPE` 来自 `sdkconfig` 的 `CONFIG_LV_USE_FREETYPE`，不是 `main/lv_conf.h`**（用户的 lv_conf.h 未被优先 include）
- `sdkconfig:2824 CONFIG_LV_USE_FREETYPE=y` 才是真正生效的开关

#### 真正的卡死链路（修正阶段⑯的判断）
- backtrace 卡在 `display.cpp:245 lv_lock()`（非 `lv_timer_handler`）→ lvgl_task 在等锁
- 锁被 `main_task` 持有：`display_update`（被 main_task 直接调用，1124 行 `lv_lock`）渲染 `lbl_track="白桦树.MP3"` 中文
- 渲染中文时走 freetype 类（因 `CONFIG_LV_USE_FREETYPE=y`，freetype 字体类全局注册）→ freetype 半瘫死循环 → main_task 永不释放锁 → lvgl_task 永久阻塞 → watchdog
- **结论：freetype 半瘫 + 中文文件名渲染 才是恒定死因；`lv_timer_handler` 死循环只是表象之一**

#### 根治改动（2026-08-20，编译通过+待烧录）
- **改动**：`sdkconfig:2824` `CONFIG_LV_USE_FREETYPE=y` → `# CONFIG_LV_USE_FREETYPE is not set`
  - 原因：关掉 Kconfig 层 freetype，LVGL 不再注册 freetype 字体类；渲染中文文件名时 montserrat 无字形走 fallback(NULL) 显示空白，但**不再死循环**，main_task 正常释放锁，lvgl_task 不再等锁
- 配合阶段⑯已加的 `font_partition.cpp: #if LV_USE_FREETYPE ... #else` 包裹：CONFIG=n → `LV_USE_FREETYPE=0` → 编译走 `#else` 分支，不再调 `lv_freetype_init`（日志中"失败"那行消失）
- `main/lv_conf.h` 前两次改动保留（无害）：`LV_USE_FREETYPE 0` + `LV_FONT_DEFAULT &lv_font_montserrat_14`
- 编译坑：ESP-IDF wifi 组件某文件含特殊字符 ``，GBK 控制台打印日志时 `UnicodeEncodeError` 中断 build；解决：`$env:PYTHONIOENCODING='utf-8'` 后重编通过
- 编译结果：`audiobook_player.bin` 0x110340（47% 余量），`Project build complete`
- **待烧录验证**：日志应出现 `LV_USE_FREETYPE=0，中文渲染未启用（仅 ASCII UI）`，tick 持续递增、`task_wdt` 消失

### 阶段 ⑱ freetype 关掉后仍卡死 → 改用 heartbeat 日志定位 main 死因（2026-08-20）

#### 烧录验证结果（阶段⑰固件）
- 日志确认 `font_part: LV_USE_FREETYPE=0，中文渲染未启用（仅 ASCII UI）` ✅（freetype 这次确实关掉了）
- 但**仍卡死**：tick #1/#2 后 lvgl_task 沉睡在 `vTaskDelay`(display.cpp:257)，约 32 秒后 TWDT 报 `main (CPU 0)` 超时
- backtrace：`lvgl_task` 卡在 `vTaskDelay`（正常睡眠），非死循环 → **说明 lvgl 没死循环，是 main 任务卡死导致不喂狗 + lvgl 饿死**
- **推翻阶段⑯/⑰判断**：freetype 半瘫不是唯一/真正卡死根因；关掉后暴露出**第二个独立死因：main 任务 while(1) 死循环/阻塞**

#### 死因分析（静态审查）
- main.cpp:1320 `if (false && ...) power_mgmt_tick()` —— 开发者自注 "DEBUG: 禁用 #7b 块排查死锁"，说明此前已用"禁用某块"绕过死锁
- main while(1) 体（1206-1482）审查：handle_button/tape_control/audio_player_tick(空实现)/save/SD_check 均无死循环
- **重点嫌疑点**：
  1. `sdmmc_read_sectors(g_sd_card, &buf, 0, 1)`（main.cpp:1453 后台 SD 健康检查，每 5 秒读扇区 0）—— 若 SD 卡读扇区阻塞则 main 卡死
  2. `esp_light_sleep_start()`（1377）—— idle 超时进入，若 wakeup_mask=0 可能睡死（但 idle 超时=分钟级，3 秒卡死不符）
- 时间线：1570ms BOOT COMPLETE → 2570ms lvgl_task 创建 → 2710/2880 tick#1/#2 → 2880ms 后 lvgl 沉睡 → 32860ms TWDT。main 在头若干轮正常 reset，第 N 轮（≈2880ms 后）卡住

#### 诊断改动（2026-08-20，编译通过+待烧录）
- **改动**：`main.cpp` 三个诊断日志点（复用已有 heartbeat 骨架）：
  1. while(1) 开头启用 heartbeat：`if ((heartbeat % 5) == 1) ESP_LOGI("DBG: main loop #%u", heartbeat)`（原 if 块为空）
  2. SD 健康检查读扇区前：`ESP_LOGI("DBG: SD health read sector 0")`（1453 前）
  3. light sleep 前：`ESP_LOGI("DBG: before esp_light_sleep_start")`（1377 前）
- 原因：静态分析无法确定 main 卡在哪个函数，加 heartbeat + step 标记，烧录后看日志最后打印到哪即可定位
- 编译：`audiobook_player.bin` 0x110430（47% 余量），`Project build complete`（用 `$env:PYTHONIOENCODING='utf-8'` 绕过 wifi 组件 gbk 编码崩溃）
- **待烧录验证**：看 `DBG: main loop #N` 递增到哪停；若停在 "SD health read sector 0" 之后无 "Woke"/无后续 loop → 确认 sdmmc_read_sectors 阻塞；若停在 "before esp_light_sleep_start" → light sleep 睡死

### 阶段 ⑲ backtrace 全部解码 → 证明 lvgl 无罪，main 卡点仍未知（2026-08-20）

#### 用户提供的 backtrace 解码结果（build/audiobook_player.elf，xtensa-esp32s3-elf-addr2line）
| 地址 | 解码 |
|---|---|
| `0x4200F9D7` / `0x4200F9E5` | `lvgl_task()` at display.cpp:247 |
| `0x42023EE5` / `0x42023EAB` | `lv_timer_handler()` lv_timer.c:99 / 76 |
| `0x42023E89` / `0x42023E82` | `lv_timer_exec()` lv_timer.c:339 / 376 |
| `0x42023D47` / `0x42023D39` | `lv_text_get_size_attributes()` lv_text.c:128 / 146 |
| `0x420283DC/F7` | `lv_theme_get_color_primary()` |
| `0x420AC328` | `lv_fs_get_buffer_from_path()` |
| `0x40381F5E` | `vTaskDelay()` tasks.c:1611 |
| `0x42087B76` / `0x42088100` | `task_wdt_timeout_handling()` / `task_wdt_isr()` |

#### 关键结论（推翻阶段⑱的读法）
- TWDT 打印的是 **"当前核上正在运行的任务"（= lvgl）**，不是超时任务（= main）的栈。
- 所有 backtrace 都落在 `lvgl_task` 的正常循环体（`lv_timer_handler` → `vTaskDelay`）内，且每次落点不同（timer/text/theme/fs 各处）→ **证明 lvgl_task 活着且在正常跑，不是死循环、也不是等锁**。
- **`main` 任务的栈从未被打印过，故其卡点至今未知。** 阶段⑱"lvgl 饿死"的说法不成立（lvgl 明显在跑）。
- 真正需要的证据是 heartbeat 日志（`DBG: main loop #N`），而用户提供的日志片段只有 62860ms 之后的重复 TWDT 部分，缺少前 62 秒的启动段。

#### 本次操作（无代码改动）
- **未修改任何源码**。仅用 addr2line 解码地址 + 复核 display.cpp:235-258 / main.cpp:1085-1325 现状，确认 heartbeat 诊断代码（main.cpp:1211）确已在固件内。
- 环境坑：PowerShell 下 `cd /d` 报错、`addr2line` 不在 PATH。实际可用路径：
  `C:\Users\zhuta\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin\xtensa-esp32s3-elf-addr2line.exe`

#### 烧录记录（2026-08-20，阶段⑱ heartbeat 诊断固件）
- 前两次 `flash` 失败：`Failed to connect to Espressif device: No serial data received`（`--before no_reset` 依赖手动进下载模式，芯片当时未就绪）
- 第三次成功：ESP32-S3 (QFN56) rev v0.2 / PSRAM 8MB / MAC `68:ee:8f:52:a0:e8`
  - `0x0` bootloader.bin 20832B、`0x8000` partition-table.bin 3072B、`0x10000` audiobook_player.bin 1115184B、`0x210000` ota_data_initial.bin 8192B，全部 `Hash of data verified`
  - 结束状态 `Staying in bootloader`（`--after no_reset`）→ 需手动复位才启动
- **待用户提供**：从上电 `ESP-ROM:esp32s3` 到第一次 `task_wdt` 之前的**完整**日志，用于判定：
  1. `DBG: main loop #N` 是否持续递增 → 若递增则 main 未卡死，而是 `esp_task_wdt_reset()`(main.cpp:1472) 被中途分支跳过（逻辑漏洞）
  2. 停在某个 `#N` 且最后一行是 `DBG: SD health read sector 0` → 确认 `sdmmc_read_sectors` 阻塞
  3. 停在 `DBG: before esp_light_sleep_start` → light sleep 睡死
  4. 连 `DBG: main loop #1` 都没有 → 卡在 `handle_button_events()` 首次调用或 `display_start_lvgl_task()` 之后

### 阶段 ⑳ 定位并修复真正根因：主循环节流 deadline 累加 → main 睡死（2026-08-20）

#### 决定性证据（阶段⑱ heartbeat 固件的完整启动日志）
时间线（毫秒级）：
```
2586ms  lvgl_task 创建
2616ms  DBG: main loop #1          ← 主循环正常启动
2656ms  DBG: main loop #6
2726ms  display_update call #0     ← tick#1 完整渲染成功（耗时约 90ms）
2766ms  DBG: main loop #11
2856ms  DBG: main loop #16         ← main 最后一次心跳
2876ms  BTN gpio levels            ← main 最后一次动作
2886ms  lvgl_task invoking main tick #2
        ↓ 此后 main 彻底静默 30 秒，无任何输出
32876ms task_wdt: main (CPU 0) 超时
```
- 新 backtrace 解码：`0x42023EE2` → `lv_timer_handler()` lv_timer.c:98，`0x4200FA53` → `lvgl_task()` display.cpp:255，`0x4037FE75` → `vPortTaskWrapper()`
- 再次确认 **lvgl_task 正常运行**，是 main 单独卡死（与阶段⑲结论一致）
- 排除项：`update_display`(main.cpp:789) 的 4 处早退 return（796/805/809/813）属正常节流，tick#2 距 tick#1 仅 160ms < 200ms 故早退，非死因

#### 根因（main.cpp:1478-1484 原代码）
```c
int64_t now = esp_timer_get_time();
if (now < g_next_loop_deadline) {
    vTaskDelay(pdMS_TO_TICKS((g_next_loop_deadline - now) / 1000));
}
g_next_loop_deadline += BTN_SCAN_INTERVAL * 1000;   // BUG：无条件累加 20000us
```
- `g_next_loop_deadline` 无条件累加 `BTN_SCAN_INTERVAL`(=20ms)，而非基于当前时间重新对齐
- 循环体一旦耗时超过 20ms（如 tick#1 那轮 `display_update` 渲染约 90ms、SD/NVS 操作等），deadline 永久落后真实时间且每轮只加 20ms，**再也追不回来**
- `g_next_loop_deadline - now` 变为负数，在 `pdMS_TO_TICKS` 的无符号运算中被解释成约 1.8e19 的巨大 tick 数 → **`vTaskDelay` 近乎无限期睡死**
- 后果：main 睡死 → 第 1472 行 `esp_task_wdt_reset()` 永不执行 → 30 秒后 TWDT 报 `main (CPU 0)`；lvgl_task 不受影响继续跑（完美解释全部日志现象）

#### 修复改动（2026-08-20，main.cpp:1477-1495）
- 三重防护替换原 2 行逻辑：
  1. 正常等待路径加**上限钳制**：`delay_us` 超过一个扫描间隔则截断为 `BTN_SCAN_INTERVAL*1000`，杜绝异常大值睡死
  2. `delay_ms` 为 0 时用 `vTaskDelay(1)` 兜底，保证必定让出
  3. `else` 分支（本轮已超时）显式 `vTaskDelay(1)`，避免 main 独占 CPU 饿死其他任务
  4. deadline 改为 **基于当前时间重新对齐**：`g_next_loop_deadline = esp_timer_get_time() + BTN_SCAN_INTERVAL*1000`，取代无条件累加
- 全部为 int64_t 显式转换，避免有符号/无符号混算
- 未触碰显示颜色/镜像逻辑，未改动 lvgl_task
- 编译结果：`audiobook_player.bin` 0x110450（47% 余量），`Project build complete`
- **待烧录验证**：`DBG: main loop #N` 应持续递增不再停止、`task_wdt` 消失、按键有响应、磁带卷轴动画转动

#### 附：本阶段确认无罪的模块（避免后续重复排查）
- `lvgl_task` / `lv_timer_handler` / `lv_lock` 递归锁：正常
- `update_display` 的早退 return：正常节流设计
- `sdmmc_read_sectors` SD 健康检查：日志中 `DBG: SD health read sector 0` 从未出现（5 秒周期未到就已卡死），本次非嫌疑
- `esp_light_sleep_start`：日志中 `DBG: before esp_light_sleep_start` 从未出现，非嫌疑
- freetype：已于阶段⑰关闭（`LV_USE_FREETYPE=0` 日志确认），非本次死因

### 编译/调试坑回顾
- PowerShell 中 `cd /d` 是 cmd 语法，会报错；用 `cd D:\path` 或 `pushd`。
- 手动 `set IDF_PATH`/`set IDF_TOOLS_PATH` 会破坏 IDF 自带环境；一律走 `build.bat`（内部 export.bat）。
- COM7 被串口助手占用时烧录失败，需先关闭。
- ninja 增量损坏时直接删 `build` 目录从零编，比 `idf.py fullclean` 更彻底。

---

## 8. 关键源文件索引

| 文件 | 职责 |
|---|---|
| `main/display.cpp` | ST7789 显示 + LVGL（含所有 DBG 探针） |
| `main/display.h` | 显示 API 声明 |
| `main/font_partition.cpp` | font 分区 VFS + freetype 初始化 |
| `main/config.h` | 全部 GPIO 引脚宏定义 |
| `main/lv_conf.h` | LVGL 配置（`LV_USE_FREETYPE 1`） |
| `main/audio_player.cpp` | 音频引擎（ESP-ADF） |
| `partitions_ota.csv` | OTA 分区表（含 font@0x620000） |
| `tools/gen_font.py` | 生成 `cjk.ttf` |
| `build/flash_args` | 烧录地址映射 |


---

## 9. R095 会话记录（2026-08-28）
- 排查：按播放 Cache error（mp3dec.c:380）-> 严格帧头校验 mp3_valid_hdr_strict + 连续两帧验证 + decoder 自对齐 self-align。
- FF/REW：scrub_enter 静音 / scrub_seek 仅 seek / scrub_exit 暂停式 seek 到最终位；删掉逐 tick pause_seek_resume（event 队列爆满）。
- 音量：settings_save_volume 立即 nvs_commit。
- 待办：曲终不自动播下一首（需曲终前后串口日志确认链路）。
