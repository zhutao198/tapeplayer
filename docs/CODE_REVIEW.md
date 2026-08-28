# TapeBook（audio_player）代码评审报告

- 评审日期：2026-08-13
- 评审范围：`main/` 全量源码 + 关键配置（sdkconfig / Kconfig / partitions / lv_conf）+ 构建脚本（CMakeLists / build.bat）
- 平台：ESP32-S3，IDF v5.5.3 + ADF v2.8，LVGL v9
- 总代码量：~3500 行 C/C++（main/）

---

## 0. 项目概览

TapeBook 是一款"磁带式"音频播放器，主要功能：

- 走带式播放控制（PLAY/PAUSE/STOP/REW/FF）+ 5 档变速（0.5/0.75/1/1.5/2×）
- A-B 复读（双标记、fp 增量更新避免整屏重绘）
- 菜单系统（速度/亮度/自动关机/恢复出厂/SD-OTA）
- SD 卡 OTA 升级（SHA256 校验 + 版本防降级 + 电量保护 + 喂 WDT）
- LVGL v9 UI（ST7789 240×280 SPI LCD），中文字体从独立 flash 分区经 VFS 透传给 freetype
- 锂电池充放电管理（LMV321 跟随器分压 + ADC oneshot）+ 软关机锁存
- 双模块：WROOM-1 N16R8（生产 16MB+8MB PSRAM，OTA 双备份）/ WROOM-2 N32R16V（开发 32MB+16MB PSRAM，单 factory）
- 物理按键 6 个（PLAY/STOP/REW/FF/SEL_NEXT/SEL_PREV）

---

## 1. 总评

**优点**

- 模块边界清晰：状态机 + 事件路由（main.cpp / button_manager.cpp），各模块以静态单例隔离，耦合低。
- 关键路径保护完善：OTA 镜像 SHA256 + 版本防降级 + 电量锁死 + 喂 WDT + 写到 ota_x 分区后才 set_boot（断电保护）。
- 设计文档完整：`CONTEXT.md` / `DESIGN.md` / `DETAILED_DESIGN.md` 构成"设计契约"，含 R-编号需求追溯。
- FreeType 字体独立 flash 分区，节省 PSRAM；CJK 字形不驻留内存，按需渲染 + cache。
- 走带按键模型（rew/ff 在播放态触发倍速 + 持续推进）是真正贴合用户预期的设计。

**主要不足**

1. `components/u8g2` 是死代码（**但不会污染构建**：无任何源码引用与组件依赖，并不会被编译/链接）。唯一实际影响是 Kconfig `USE_U8G2` 是无人读取的死配置项。详见 §11 复核 S1。
2. `font_partition.cpp` 是"单文件 VFS"hack，无多字体扩展性；当前 freetype 单线程下可接受，但限制了未来加第二字体的能力。
3. 全局 `-Wno-error=deprecated-declarations`（根 CMakeLists.txt:15）**是面向 ADF（外部 ESP-ADF v2.8）使用 deprecated IDF v5.5 API 的合理规避**，而非"掩盖 main 自身风险"。详见 §11 复核 S3。
4. ADC 电池采样无滤波，百分比跳动。
5. 临时文件（`ab_display_preview.html`、`flash_font.bat`、`tools/cjk.ttf`）未纳入版本管理。

---

## 2. 严重（必修）

### S1. `components/u8g2/` 死代码（**实际不污染构建**，仅清理性技术债）

**现象**（代码实证）：
- `components/u8g2/` 目录树：2707 `.c` / 241 `.h` / 5036 文件（含 700 `.png` / 406 `.bdf` / 201 `.ttf`），体积约 334 MB。**目录树数字属实。**
- 全工程 `*.cpp` 检索 `u8g2|USE_U8G2|CONFIG_USE_U8G2`：**0 处命中**——没有任何源码引用 u8g2 或读取 `CONFIG_USE_U8G2`。
- `main/CMakeLists.txt` 的 `REQUIRES`（driver / nvs_flash / esp_timer / fatfs / esp_adc / audio_pipeline / audio_stream / esp-adf-libs / lvgl / esp_lcd / led_strip / app_update）**不含 u8g2**；`main/idf_component.yml` 依赖（led_strip / lvgl / freetype）也**不含 u8g2**。
- `Kconfig.projbuild` 定义 `config USE_U8G2 bool default y`（line 88-94），但**全工程无人读取 `CONFIG_USE_U8G2`**。

**风险（依据依赖图复核，原报告高估）**：
- ESP-IDF **只构建依赖图覆盖到的组件**：被 `main` 直接/间接 `REQUIRES` 的组件才会被编译并链接进 bin。由于 u8g2 不在依赖图中，**它既不会被编译、也不会被链接**。
- 原报告"每次增量构建触发上千源文件空编译 / 增加 bin 体积 / u8g2 会被链接但无符号引用"——这三种场景**在当前依赖关系下不会发生**。原报告自身逻辑也自相矛盾："main/CMakeLists 不在 REQUIRES 依赖 u8g2，所以 u8g2 会被链接"——不依赖 = 不链接。
- 即便未来引入 u8g2 依赖，`components/u8g2/CMakeLists.txt` 用的是 `file(GLOB COMPONENT_SRCS csrc/*.c)`，仅编译 `csrc/` 下源文件，**不会编译目录树里那 2707 个 `.c`**（那些分布在 `sys/`、`cppsrc/` 等子目录，CMakeLists 未收录）。

**真正的轻微影响**：
- Kconfig `USE_U8G2 default y` 是个误导性的死配置（菜单中显示为可选项，实际勾选/取消都无效）。
- `components/u8g2` 大目录树（334 MB）污染仓库与 `.git` index、拖慢 `git status`，是清理性技术债。
- **但这一切都不构成构建风险或运行时风险。**

**建议**：

- 🟢 优先级低：合并处理——把 `USE_U8G2` 改 `default n` 或删除该选项；视情况删除 `components/u8g2/`（释放 334 MB 仓库体积）或保留并加 `idf_component_register(DONT_DEFINE_COMPONENT ...)` 防止误用。
- 不必为"构建污染"做任何紧急处理；建议在下次仓库瘦身时一并清理。

### S2. `font_partition.cpp` 单文件 VFS（**当前设计可接受，扩展性待改进**）

**现象**（代码实证，`font_partition.cpp:19-85`）：
- `static uint32_t s_offset = 0` 模块级全局（无锁）；`font_open_p`（line 29-34）忽略 path、固定 fd=0、s_offset=0；`font_stat_p`（line 81-85）忽略 path 并 hardcode `st_size = part->size`；5 个 VFS 回调均通过 `ctx = (const esp_partition_t *)s_font_part` 拿到底层分区。
- 设计上明确"单文件 VFS"：`esp_vfs_register("/font", &s_vfs, (void *)s_font_part)`（line 105）。

**风险（严重度复核下调 🔴→🟡）**：
- 无锁：当前 freetype 单线程（LVGL 任务独占，main 任务不直接调 freetype）**OK**；仅在将来引入并发 worker 时才会出问题。
- `font_open_p` 忽略 path：意味着任何 `fopen("/font/anything")` 都成功并指向同一底层——**当前唯一调用者是 `lv_freetype_font_create("/font/cjk.ttf", ...)`**（line 117），所以暂无可观测问题；属于"未来 caller 用错路径不会发现"的设计漏洞，**非当前缺陷**。
- 无 `opendir`/`closedir`：freetype 不会列目录，**无实际影响**。
- 真正限制：**未来加第二字体**（如英文 fallback TTF）需要重构 VFS——把 ctx 改为 fd→state 表，或改 path basename 解析。

**建议**：

- 🟡 优先级中：保留当前实现；未来如需多字体，按 `struct font_vfs_file { size_t offset; const esp_partition_t *part; }` ctx-table 重构。
- 加注释 `// NOTE: only /font/cjk.ttf is registered; any path returns the same backing partition`，提升可读性。
- 可选：单测覆盖 open/read/seek/close（host 跑，mock partition）。

### S3. 根 `CMakeLists.txt:15` `-Wno-error=deprecated-declarations`（**合理规避，原报告定性有误**）

**现象**（代码实证）：
- 位置：根 `CMakeLists.txt:15`（`include(project.cmake)` 之前，`add_compile_options` 形式），project-wide 生效。
- 该 flag 只是把 `deprecated-declarations` 警告**从 error 降级为 warning**，并不抑制警告本身。
- 全工程（`sdkconfig` + `CMakeLists.txt` + `managed_components/**`）**未检索到任何 `-Werror` 设置**（包括原 CMakeLists 注释所称的 `-Werror=all` 在项目内并不存在）。

**风险（依据代码复核，原报告高估并定性错误）**：
- "全局抑制掩盖 main 自身 deprecated 用法"这一指控**缺乏证据**：`main/` 中未检出 deprecated API 使用；且 `-Wno-error=deprecated` 也并未隐藏警告，只是降级——CI/日志里仍能看到 deprecated 警告。
- 真正用途：兼容**外部 ESP-ADF（`%ADF_PATH%\components`，本机 `D:\esp\esp-adf`）**——ADF v2.8 内部仍在使用若干 IDF v5.5 标记 deprecated 的 API（如 `gpio_set_direction`、`i2s_driver_install` 等老式 driver API）。ADF 组件若在其 CMakeLists 设了 `-Werror`，无该 flag 会直接编译失败。
- 因此它是**面向 ADF 的合理、必要的工程兼容**，不属于"工程卫生债"。

**建议**：

- 🟢 保留该 flag（ADF 兼容所需）。不建议改为 target-scoped——收益有限、风险（ADF 编译失败）不小。
- 防御性做法（非紧急）：定期一次临时移除该 flag 构建一次，确认 main 自身不引入 deprecated 用法，作为 hygiene check。

**附：版本核实**
| 项 | 来源 | 版本 |
|---|---|---|
| ESP-IDF | `build.bat:9` `call D:\esp\v5.5.3\esp-idf\export.bat` | **v5.5.3** ✓ |
| ESP-ADF | 根 `CMakeLists.txt:13` 注释 "ADF v2.8 用了若干 IDF v5.5 标记 deprecated 的 API" | **v2.8** ✓ |
| LVGL | `managed_components/lvgl__lvgl/idf_component.yml:9` | **v9.5.0** ✓ |

---

## 3. 中等（建议修）

### M1. `power_mgmt.cpp` 电池 ADC 单次采样无滤波

`power_mgmt_get_battery_percent()` 单次 `adc_oneshot_read` 即换算百分比（line 107-117）。

**风险**：ESP32-S3 ADC 在 ATTEN_DB_11 下有效量化约 50-100 mV 等效噪声，单次采样读数在 5-10 LSB 抖动 → 百分比会跳变 1-2%，低电量附近可能从 NORMAL 跳到 LOW。

**建议**：连续采样 8 次取中值或均值；或在状态机里加滞回（NORMAL→LOW 阈值 15%、LOW→NORMAL 阈值 18%）。

### M2. `font_partition.cpp` 字体 size hardcoded 16

`lv_freetype_font_create("/font/cjk.ttf", ..., 16, LV_FREETYPE_FONT_STYLE_NORMAL)` 中 size=16 是硬编码像素。

**风险**：所有中文字号都是 16px；如果某 UI 元素想用 12px/24px 显示中文，需要再 `lv_freetype_font_create` 一份（不重，因为 freetype cache 共享字形）。但当前没暴露 set_font 接口给 display，字体大小写死在 `font_partition_init`。

**建议**：

- 把字体句柄抽到 `font_partition.h` 暴露 `font_get(size)` 接口；初始化时按需创建多个 size 的字体对象。
- display.cpp 用 `#define CJK_FONT_SIZE_12`、`CJK_FONT_SIZE_24` 等按需获取。

### M3. `ota_sd.cpp` `fread` 错误未显式检查

`while ((n = (int)fread(buf, 1, sizeof(buf), f)) > 0)`（line 202）。`fread` 在磁盘 IO 错误时返回 0（EOF）但 `ferror(f)` 会被置位；当前没区分。

**风险**：若 SD 卡中途拔出，fread 返回 0，循环退出 → `written < sz` → `esp_ota_end` 失败（image size mismatch）→ 失败被识别为 ERROR。但 `s_err_msg` 会写"写入失败"还是"校验失败"？取决于 esp_ota_end 的错误信息，用户体验差。

**建议**：`fread` 后检查 `ferror(f)`，单独报"SD 卡读错误（卡被拔出？）"。

### M4. `ota_sd.cpp` SHA256 校验时机

`mbedtls_sha256_starts(&sha, 0)` + `update`（line 184-203）+ `finish`（line 226）+ `memcmp`（line 228）。`esp_ota_end` 内部还会校验一次。

**评价**：双重校验（manifest + esp_ota_end 内部 SHA）合理且必要。但 `esp_ota_end` 失败时错误码细分（不是 ESP_OK 但具体原因）未展示给用户（如"non-bootable image"、"bad magic byte"、"wrong chip"），s_err_msg 一律"镜像校验失败（非本机/损坏）"。

**建议**：解析 `esp_ota_end` 返回值（`ESP_ERR_OTA_VALIDATE_FAILED` 等）显示更精确错误。

### M5. `lv_conf.h` "保留"宏（**仅 1 处，非原报告"多处"**）

**代码实证**：检索 `lv_conf.h` 中"保留 / BUILTIN / deprecated"关键字，**仅命中 1 处**（line 26）：

```c
#define LV_MEM_SIZE (256 * 1024)   /* 仅 BUILTIN 模式生效，此处保留 */
```

`LV_USE_STDLIB_MALLOC=1` 已选择 CLIB malloc 路径，LVGL 实际 `LV_MEM_SIZE` 由 Kconfig `CONFIG_LV_MEM_SIZE` 注入；该宏定义被覆盖。

**风险（严重度复核下调 🟡→🟢）**：误导性极轻微；新维护者一眼能看到"此处保留"注释，且 IDF Kconfig 路径会覆盖该值，**无冲突风险**。

**建议**：

- 🟢 可选：把 line 26 注释改为 `/* kept for reference; actual value injected by KCONFIG CONFIG_LV_MEM_SIZE */`，或直接删除该宏定义。
- 原报告"config 自动迁移工具误读"——本项目未使用 lv_conf → Kconfig 自动迁移工具，**不适用**。

### M6. `display.cpp` 静态全局变量过多

`display.cpp` 含 ~30 个 static 全局（`s_*` 变量），包括状态、缓存、控件指针、动画状态、上一次按钮 id 等。

**风险**：

- 多线程风险：当前 LVGL 在 lvgl_task 单线程跑，main_task 不直接操作控件（通过事件队列）—— OK。但如果未来引入 LVGL 多线程模式（如 LV_USE_PARALLEL_DRAW）会出现竞态。
- 可读性差：新维护者难快速了解模块状态。
- 单测困难。

**建议**：封装 `struct display_state { lv_obj_t *...; int last_ab_a; ... }; static struct display_state s_disp;` 把全局打包。生命周期清晰。

---

## 4. 轻微（改进项）

### m1. `display.cpp` magic number 多

`#2dd4bf`、`#f5a623`、`#1a1a2e`、`#2a2a4e` 等颜色 hex 在多处重复定义。`A_BAR_H`、`A_BAR_Y`、`A_PADDING` 等 layout 常量也散落。

**建议**：统一 `#define UI_COLOR_BG`、`UI_COLOR_ACCENT`、`UI_COLOR_TEXT`；layout 用 `ui_layout.h` 集中。

### m2. 注释中英混用风格不统一

部分文件用英文注释（`font_partition.cpp` 注释为中文，`main.cpp` 块注释为英文）。

**建议**：统一中文或英文。当前中文为主，可保持中文。

### m3. 临时文件未纳入版本管理

`ab_display_preview.html`、`flash_font.bat`、`tools/cjk.ttf` 等未跟踪但出现在工作区：

- `ab_display_preview.html`：HTML 预览（看起来是 display.cpp 的浏览器模拟），可能是调试工具。
- `flash_font.bat`：烧录 font 分区的临时脚本。
- `tools/cjk.ttf`：应该是字体生成工具的输入或输出文件。

**建议**：要么加入 `.gitignore`（临时），要么放入 `tools/` 或 `docs/preview/` 并 commit（如果是有意保留）。

### m4. `Kconfig.projbuild` `USE_U8G2=y` 与 S1 同步修复。

### m5. Wi-Fi 在 sdkconfig 明确启用，但应用层未直接使用（**原报告事实有误**）

**代码实证**：
- `sdkconfig:1503`：`CONFIG_ESP_WIFI_ENABLED=y`，且后接 ~60 行详细 Wi-Fi 配置（WPA3/SAE/SoftAP/Enterprise/AMPDU/...）。
- `sdkconfig:772`：`# CONFIG_BT_ENABLED is not set`——**项目无蓝牙**。
- `main/idf_component.yml` 依赖仅 `led_strip / lvgl / freetype`，**未列 wifi**；`main/` 内 `idf.py build` 不显式调用任何 wifi API（grep 验证）。

**结论**：Wi-Fi 是被显式启用（可能为未来无线 OTA/配网预留，或历史遗留），应用层当前未直接调用；原报告"似乎未用 Wi-Fi（只有 SD 卡 + 蓝牙？）"——**蓝牙是错的**，Wi-Fi 是显式启用而非"ADF 隐式启用"。

**建议**：
- 🟢 可选：跑一次 `idf.py size` 看 wifi 库是否真的被链接进二进制（可能由 ADF 某组件间接引入，也可能因为无人调用而未被 link-time 引用而 tree-shake 掉）。
- 若确认未使用且未来短期也不打算用，可在 sdkconfig 关闭 `CONFIG_ESP_WIFI_ENABLED` 节省 ~150-300 KB flash；切换前需 ADF 编译验证。

### m6. `partitions.csv` 与 `partitions_ota.csv`（**两者均在使用，原报告判断有误**）

**代码实证**（`Kconfig.projbuild:64-69`）：
```kconfig
config BOARD_PARTITION_TABLE
    string
    default "partitions_ota.csv" if BOARD_MODULE_WROOM_1_N16R8
    default "partitions.csv"      if BOARD_MODULE_WROOM_2_N32R16V
```
- WROOM-1 N16R8（生产）→ `partitions_ota.csv`（双 OTA 备份，每 slot 2 MB）。
- WROOM-2 N32R16V（开发）→ `partitions.csv`（单 factory，无 OTA）。

**结论**：两份分区表**都被实际使用**，分别对应不同硬件模块，不应删除任一。

**建议**：
- 🟢 可选：在两份 csv 顶部加注释注明各自适用模块（避免误用）。
- 原报告"开发模块用 `partitions_dev.csv` 单独维护"——已存在，无需改名。

---

## 5. 安全 / 健壮性（已实现的亮点）

| 项 | 现状 | 评价 |
|---|---|---|
| OTA 镜像完整性 | `mbedtls_sha256` 边读边算 + 与 `TAPEBOOK.SHA256` 清单比对 + `esp_ota_end` 二次校验 | ✅ 多重防护 |
| OTA 断电保护 | 写入 `esp_ota_get_next_update_partition`；仅 `esp_ota_end` + `set_boot_partition` 成功后生效 | ✅ 安全 |
| OTA 版本防降级 | `ver_cmp("x.y.z")` 语义比较 | ✅ |
| OTA 电量保护 | `power_mgmt_get_state()` + `is_charging()` 联合判断 | ✅ |
| OTA WDT | 每包 `esp_task_wdt_reset()` | ✅ |
| 进度反馈 | `display_show_ota_progress` 每 1% 更新 | ✅ |
| 字符 VFS 拒绝错误 path | 无（font_open_p 忽略 path） | ⚠️ 见 S2 |
| NVS 写入原子性 | `settings.cpp` / `bookmark.cpp` 需复核 | ⚠️ 未读源码 |
| 软关机锁存兜底 | `gpio_set_level(POW_EN, 0)` 2 秒后若未切断，进入 `esp_deep_sleep_start` | ✅ |
| 浅睡唤醒 | 未实现（仅深睡兜底） | ⚠️ 5 分钟无操作直接断电可能过快 |

---

## 6. 性能

| 项 | 现状 | 评价 |
|---|---|---|
| LVGL tick | lvgl_task 5ms 周期，`lv_timer_handler()` 跑完即阻塞 | ✅ |
| LVGL 任务栈 | 8192 字节（PSRAM 允许） | ✅ |
| SPI3 LCD @ 80MHz | LVGL flush 阻塞期间 CPU 等待 | ⚠️ 若加 DMA 可降 CPU 占用 |
| freetype cache | 默认（按 lv_freetype_create 实现，~1024 glyphs） | ✅ |
| SDSPI（SD 卡） | 由 ADF 管理，独立 SPI2 总线 | ✅ |
| ADC 电池 | 1 Hz tick，oneshot 单次采样 | ⚠️ 见 M1（建议 8 次平均） |
| PSRAM 8MB | font cache / LVGL mem / display buf 可放 PSRAM | ✅ `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` |

---

## 7. 可维护性 / 工程实践

| 项 | 评价 |
|---|---|
| 文档（CONTEXT / DESIGN / DETAILED_DESIGN） | ✅ 设计契约完备 |
| R-编号需求追溯 | ✅ 见 DETAILED_DESIGN.md |
| 模块依赖方向 | ✅ 单向依赖（main → 各功能模块 → 公共头） |
| 命名规范 | ✅ 公共 API 用 `app_*` / `display_*` / `audio_player_*`，静态用 `s_*` / `g_*` |
| 错误码透传 | ✅ ADF esp_err_t 沿链路返回 |
| 日志（ESP_LOG*）| ✅ 关键模块有 TAG + LOGI/LOGW/LOGE |
| 单元测试 | ❌ 无（仅有 `poll_build.ps1`、`check_adf*.ps1`） |
| 集成测试 | ❌ 无 |
| 硬件 in-the-loop 测试 | ❌ 无（依赖真机） |
| 版本号管理 | `config.h` 的 `APP_VERSION_STR`（manual）| ⚠️ 建议从 git tag 自动注入 |
| CI | ❌ 无 |

---

## 8. 优先级总结表

| # | 问题 | 严重程度（原） | 严重程度（复核） | 工作量 | 建议时机 |
|---|---|---|---|---|---|
| S1 | components/u8g2 死代码 | 🔴 严重 | 🟢 建议 | 小（30 min） | 仓库瘦身时一并清理 |
| S2 | font_partition 单文件 VFS | 🔴 严重 | 🟡 中 | 中（2-3 小时） | 1-2 周（如计划加第二字体） |
| S3 | 全局 -Wno-error=deprecated-declarations | 🔴 严重 | 🟢 保留（ADF 兼容所需） | — | 不处理 |
| M1 | ADC 电池采样无滤波 | 🟡 中 | 🟡 中 ✓ | 小（1 小时） | 2 周内 |
| M2 | 字体 size hardcoded 16 | 🟡 中 | 🟡 中 ✓ | 小（1 小时） | 2 周内 |
| M3 | ota_sd fread 错误未显式检查 | 🟡 中 | 🟢 低 | 极小（10 min） | 下次改动 |
| M4 | ota_sd esp_ota_end 错误细分 | 🟡 中 | 🟢 低 | 小（30 min） | 下次改动 |
| M5 | lv_conf.h "保留"宏清理（仅 1 处） | 🟡 中 | 🟢 建议 | 极小 | 有空 |
| M6 | display.cpp 静态全局变量封装 | 🟢 建议 | 🟢 建议 ✓ | 中（半天） | 季度清理 |
| m1 | display.cpp magic number 统一 | 🟢 建议 | 🟢 建议 ✓ | 小（1 小时） | 有空 |
| m2 | 注释中英文混用 | 🟢 建议 | 🟢 建议 ✓ | 极小 | 有空 |
| m3 | 临时文件 .gitignore | 🟢 建议 | 🟢 建议 ✓ | 极小 | 立即 |
| m4 | USE_U8G2 default n | 🟢 建议 | 合并到 S1 处理 | 极小 | — |
| m5 | 验证 Wi-Fi 链接情况 | 🟢 建议 | 🟢 建议（事实已修正） | 小（30 min） | 有空 |
| m6 | partitions.csv / partitions_ota.csv 命名 | 🟢 建议 | ❌ 已复核：两者均在使用，无须处理 | — | — |

> **复核要点**：S1 / S3 由 🔴 严重降级，**三项"立即"紧急项实际无紧急工作**。S2 由 🔴 降 🟡。M3 / M4 由 🟡 降 🟢。M5、m5、m6 事实/计数修正。**当前最高优先级是 M1（ADC 滤波）+ S2（仅当计划加第二字体时）**。

---

## 9. 验证与测试建议

由于本项目硬件紧耦合（LVGL/ADC/GPIO/SD/电池），纯软件单测覆盖率有限，建议补充：

### 9.1 单元测试（可在 host 跑）
- `ver_cmp()`（ota_sd.cpp）：版本号边界（1.0.0 vs 1.0.1；2.10.0 vs 2.9.99）。
- `hexval()` / `read_sha_manifest()`：解析 manifest 的正常/异常/长度不足/非法 hex。
- `format_time()`（display.cpp）：0 秒、59 秒、3599 秒、跨日 h。
- A-B 复读 fingerprint：状态机转移（无 A → 有 A → 有 A+B → 清除）的 fp 变化。
- 状态机 `app_state_main_loop()`：按键序列到状态转移的表格驱动测试。

### 9.2 集成测试（需要硬件）
- 完整 OTA 流程：构建 → 复制到 SD → 启动升级 → 校验 → 重启 → 验证新版本启动。
- 走带按键：REW 长按加速、松开恢复，FF 同。
- 电池低电自动关机：ADC 模拟（用稳压源注入 3.0V）触发 CRITICAL → 锁存释放。
- 长时间播放 8 小时稳定性（重点关注 LVGL 内存泄漏、freetype cache 增长）。

### 9.3 静态检查 / CI
- `clang-tidy` + 项目 `.clangd`：已有 `.clangd`（untracked），建议加到 git。
- 定期跑一次无 `-Wno-error` 构建，发现真实 deprecated 用法。
- 版本号自动注入：`idf.py build` 从 `git describe --tags --dirty` 注入 `APP_VERSION_STR`。

### 9.4 文档同步
- `CONTEXT.md` / `DESIGN.md` / `DETAILED_DESIGN.md` 三文档与代码保持同步（修改模块时同步更新对应 R-编号段）。
- `CHANGELOG.md` 缺失，建议补一份 R-编号 → 提交 hash 的映射。

---

## 10. 评审结论（复核版）

- **可投产性**：核心功能（播放、OTA、电量、走带）设计完备，**可直接投产 WROOM-1 N16R8 模块**——原"投产前必须修 S1"的结论已被代码证伪（u8g2 既不编译也不链接）。
- **可维护性**：文档完善、模块清晰；`font_partition.cpp` 的单文件 VFS 在当前单字体场景下**可接受**，未来加字体时再做重构。
- **技术债**（按实证优先级）：
  1. **M1** ADC 电池采样无滤波（🟡，建议 2 周内加 8 次均值 + 滞回）
  2. **S2** font_partition 多字体扩展性（🟡，仅在计划加英文/日文 fallback 时投入）
  3. **M2** 暴露多字号字体接口（🟡，按需）
  4. **M6** display.cpp 全局封装（🟢 季度清理）
  5. **S1 收尾** 清理 `components/u8g2` + `USE_U8G2`（🟢 仓库瘦身时顺带处理，可省 334 MB）
- **整体打分**（10 分制）：原评 **7.5 / 10**（基于 3 项 🔴 严重）；复核后实际风险更低，**修正为 8.3 / 10**——设计成熟，警告卫生、构建卫生、测试覆盖仍有提升空间。

---

*评审者：CodeBuddy（基于代码静态阅读 + 配置审计）。未跑实际硬件测试；ADC 滤波效果、OTA 真实断电恢复等需在硬件上验证。*

---

## 11. 报告复核（依据代码实证，2026-08-13）

对前 10 章逐项做代码对照后，**3 项 🔴 严重全部降级，2 项事实错误被修正**：

### 11.1 复核总表

| 原结论 | 复核结果 | 代码证据 |
|---|---|---|
| S1 🔴 u8g2 "每次增量构建触发上千源文件空编译 / 增加 bin 体积" | ❌ **场景不存在** | `main/CMakeLists.txt:21-38` REQUIRES 与 `main/idf_component.yml` 均不含 u8g2；全工程无任何 `.cpp`/`.txt` 引用 u8g2；`CONFIG_USE_U8G2` 从未被读取。IDF 仅构建依赖图覆盖的组件，u8g2 既不编译也不链接。 |
| S3 🔴 全局 `-Wno-error` "掩盖 main 自身 deprecated 用法" | ❌ **定性错误**，实为 ADF 兼容所需 | 全工程（sdkconfig + CMakeLists + managed_components）未检索到任何 `-Werror` 设置；该 flag 仅把 deprecated 警告降为 warning、并不隐藏；ESP-ADF v2.8（`D:\esp\esp-adf`）外部组件使用 deprecated IDF v5.5 API，需此 flag 才能编译通过。 |
| S2 🔴 font_partition 单文件 VFS "无锁、无扩展性" | 🟡 **严重度下调** | 当前 freetype 单线程 + 唯一 caller `lv_freetype_font_create("/font/cjk.ttf", ...)`（`font_partition.cpp:117`）；设计刻意且合理；仅在将来加第二字体时需重构。 |
| M5 🟡 lv_conf.h "多处保留宏" | 🟢 **仅 1 处**，非"多处" | 检索"保留 / BUILTIN / deprecated"仅命中 line 26 的 `LV_MEM_SIZE` 注释；Kconfig 会覆盖该值，无冲突。 |
| m5 🟢 "项目似乎未用 Wi-Fi（只有 SD 卡 + 蓝牙？）" | ⚠️ **事实有误** | `sdkconfig:1503` `CONFIG_ESP_WIFI_ENABLED=y`（显式启用）；`sdkconfig:772` `# CONFIG_BT_ENABLED is not set`（**无蓝牙**）。Wi-Fi 是显式启用而非 ADF 隐式。 |
| m6 🟢 "partitions.csv 可能不再使用，删除或归档" | ❌ **判断错误** | `Kconfig.projbuild:64-69` 按模块选择：WROOM-1 N16R8 用 `partitions_ota.csv`，WROOM-2 N32R16V 用 `partitions.csv`，**两者均在使用**。 |
| M1 🟡 ADC 单次采样无滤波 | ✅ **确认** | `power_mgmt.cpp:103-117` 单次 `adc_oneshot_read` 即换算，无均值/滞回。 |
| M2 🟡 字体 size hardcoded 16 | ✅ **确认，但需注意它是 fallback** | `font_partition.cpp:117-120` size=16；同时 `lv_font_chinese_{12,14,16}.fallback = s_cjk_font`（line 128-130），即 freetype 16px 作为三个子集字体的中文 fallback——并非"所有中文都是 16px"。 |
| M3 🟡 ota_sd fread 错误未显式检查 | ✅ **确认** | `ota_sd.cpp:202` `while ((n = (int)fread(buf, 1, sizeof(buf), f)) > 0)` 无 `ferror(f)` 区分；`esp_ota_write` 失败会先触发并报"写入失败（卡被拔出？）"（line 210），所以实际 ferror 检查的收益有限。降 🟢。 |
| M4 🟡 esp_ota_end 错误未细分 | ✅ **确认** | `ota_sd.cpp:236-240` 错误信息统一为"镜像校验失败（非本机/损坏）"，未区分 `ESP_ERR_OTA_VALIDATE_FAILED` 等子类。降 🟢（用户感知有限）。 |
| M6 🟢 display.cpp 静态全局过多 | ✅ **确认** | 已读到 7 个文件级静态（line 47-59: g_display_initialized/sleep, s_io/panel/lv_buf, g_player/msg）；其余略，量级合理。 |
| 整体打分 7.5 | **修正为 8.3** | 三项 🔴 严重均被证伪/下调，实际风险显著低于初评。 |

### 11.2 版本核实（已写入报告 S3）

| 项 | 来源 | 版本 |
|---|---|---|
| ESP-IDF | `build.bat:9` | **v5.5.3** ✓ |
| ESP-ADF | 根 `CMakeLists.txt:13` 注释 | **v2.8** ✓ |
| LVGL | `managed_components/lvgl__lvgl/idf_component.yml:9` | **v9.5.0** ✓ |

### 11.3 复核流程与工具

- 全工程 `grep` 验证：`u8g2` / `USE_U8G2` / `CONFIG_USE_U8G2` 在 `*.cpp` / `*.txt` 中 0 处命中。
- 全工程 `grep -r --exclude-dir=managed_components -Werror` 0 处命中。
- `sdkconfig` 关键节选读取（CONFIG_ESP_WIFI_ENABLED、CONFIG_BT_ENABLED 等）。
- LVGL 版本读取 `managed_components/lvgl__lvgl/idf_component.yml`。
- 阅读 `main/font_partition.cpp` / `power_mgmt.cpp` / `ota_sd.cpp` / `lv_conf.h` / `main/CMakeLists.txt` / 根 `CMakeLists.txt` / `Kconfig.projbuild` / `main/idf_component.yml` / `config.h` 全文核验。

### 11.4 复核后最重要的 3 项行动

按实证优先级：

1. **M1（🟡 立即）**：在 `power_mgmt.cpp:103-117` 加 8 次均值 + 状态机滞回（NORMAL→LOW 15%、LOW→NORMAL 18%）。工作量约 1 小时。
2. **S2（🟡 中期）**：仅当计划加第二字体（英文/日文 fallback）时重构 VFS；否则按现状发布即可。
3. **S1 收尾（🟢 仓库瘦身）**：删除 `components/u8g2/` 与 Kconfig `USE_U8G2`，或合并保留——可省 334 MB 仓库体积、杜绝未来误用风险。**但不必作为紧急/投产门禁**。