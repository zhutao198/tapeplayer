# 显示异常诊断报告 — 2026-08-18

> **目的**：定位 ESP32-S3 + ST7789 + LVGL v9 显示链路中"显示异常"的所有可疑点，**不改代码**，仅沉淀排查结论与待办。
>
> **关联文档**：`docs/DEBUG_LOG.md`（§7 阶段⑧~⑭ 排查过程）、`main/display.cpp`、`main/config.h`、`main/lv_conf.h`、`main/main.cpp`
>
> **范围**：仅评审显示相关源码（display.cpp/h、config.h 的 DISPLAY_* 宏、lv_conf.h 关键开关、main.cpp 的初始化顺序），不动音频/按键/电源等其他模块。
>
> **状态**：诊断完成，待用户确认修复顺序后动手。

---

## 1. 背景

DEBUG_LOG §7 已记录 ST7789 显示链路在阶段⑧~⑭ 的多轮排查（`max_transfer_sz`、SPI 32K 上限、GRAM 240 列、MADCTL bit5 误读、`swap_xy`/`mirror` 多次反复）。当前代码（`display.cpp`）对应"阶段⑭ 探针状态"：4 色竖条 + 顶部白行的诊断画图**永久驻留**在 `lcd_hw_init()` 末尾，且 `swap_xy/mirror` 硬编码为 `(false, false, false)`，与 `config.h` 中的 `DISPLAY_ORIENTATION` 宏**完全脱钩**。

**本次诊断目标**：把当前显示链路所有可疑点一次性列齐，按严重度分级，给出"只观察不改代码"的验证手段，作为后续修复的输入。

---

## 2. 扫描范围

| 文件 | 行号范围 | 用途 |
|---|---|---|
| `main/display.cpp` | 全文 1346 行 | ST7789 + LVGL 全栈（init / flush / DBG / UI / OTA / A-B / BT） |
| `main/display.h` | 全文 163 行 | 显示 API 声明 |
| `main/config.h` | L60-82 | 显示 GPIO 宏 + `DISPLAY_ORIENTATION` + `DISPLAY_WIDTH/HEIGHT` |
| `main/lv_conf.h` | L11/L32-34 | LVGL 关键开关（`LV_COLOR_DEPTH=16`、`LV_USE_FREETYPE=1`） |
| `main/main.cpp` | L983-985 | 初始化顺序（display → button → menu → tape → audio → power → ws2812 → bookmark → wdt） |

**未扫描**：`audio_player.cpp/h`、`font_partition.cpp/h`、`menu.cpp/h`、`ota_sd.cpp/h`（不在本次范围；显示异常若与它们耦合，会在 UI 渲染层显现，留待显示层修复后再评估）。

---

## 3. 问题清单

### 3.1 🔴 P0 — 高概率直接导致当前显示异常

#### P0-1 `lvgl_task` 严重日志洪水，帧率被日志拖垮

| 项 | 内容 |
|---|---|
| 位置 | `main/display.cpp:186-193` + `:169, :175` |
| 现象 | 每 5 ms 循环固定打印 **4 行 ESP_LOGI**（`DBG: lvgl loop top` / `before timer_handler` / `after timer_handler`），加 `flush_cb` 入口出口 2 行 = **每帧 6 行** |
| 量级 | 200 帧/秒 × 6 行 ≈ **1200 行/秒** UART 输出（115200 容易丢字符） |
| 机制 | ESP-IDF 日志走 `vprintf` 涉及 mutex + 字符发送，单次调用 5-30 ms（取决于 UART 阻塞/非阻塞模式与系统负载） |
| 后果 | `vTaskDelay(5ms)` 实际被日志拖成 30-100 ms → LVGL 渲染掉帧 → **屏闪 / 抖 / 花 / 残影** |
| 验证手段 | 抓 `idf.py -p COM7 monitor` 启动 10 秒日志，统计每秒 ESP_LOG 行数；或临时关掉所有 DBG 探针重编译一次（仅诊断） |
| DEBUG_LOG 关联 | 阶段⑦ 探针残留（`DBG:` 标记设计本就是"黑屏定位"临时工具，未在结论⑧~⑭ 中拆除） |

#### P0-2 `flush_cb` 整屏 153600 字节未手动分片

| 项 | 内容 |
|---|---|
| 位置 | `main/display.cpp:166-177`（`lvgl_flush_cb`） |
| 现象 | LVGL splash（"Initializing..."）整屏 invalidate 时 area = `0,0,319,239` → `esp_lcd_panel_draw_bitmap` 一次性 153600 字节 |
| 机制 | `max_transfer_sz=32767`（`display.cpp:224`）**不会**让 `esp_lcd_panel_io_spi` 自动分片 —— 该参数是 SPI bus 驱动限制，**不是 esp_lcd panel_io 的分片开关**。ESP32-S3 SPI 单次事务实际安全上限约 **32752 字节**（DMA 描述符），32767 在边缘 |
| 对照 | 直绘彩条代码（`display.cpp:282-294`）已用 `block_rows=40` 手动分块（25 KB/块）—— **但 `flush_cb` 没做同样处理** |
| 后果 | splash 整屏 flush_cb → 一次 SPI 事务 153600 字节 → 触发上限 → **花屏 / 截断** |
| 验证手段 | 在 `flush_cb` 入口加临时 log 打 `len = (x2-x1+1)*(y2-y1+1)*2`，看是否经常出现 `len > 32767` |
| DEBUG_LOG 关联 | 阶段⑧（`max_transfer_sz` 太小）→ 阶段⑫（设了 32767 安全值）→ 阶段⑫ 的"分片"修复**只覆盖了直绘路径，flush_cb 路径漏修** |

#### P0-3 诊断彩条代码驻留，每次启动都强制画

| 项 | 内容 |
|---|---|
| 位置 | `main/display.cpp:273-299`（`lcd_hw_init()` 末尾的彩条块） |
| 现象 | DEBUG_LOG 阶段⑭ 探针代码**未拆除** —— 每次启动都强制画 4 彩条 + 顶部 4 行白条 |
| 启动顺序 | 彩条（`lcd_hw_init` 末尾）→ LVGL 创建 → `ui_show_msg("Initializing...")` → `lvgl_task` 首次 `lv_timer_handler` → 第一次 `flush_cb` |
| 关键点 | splash 第一次 `flush_cb` 会覆盖彩条。**若 splash 没覆盖成功（= P0-2 截断）→ 屏幕永久显示彩条残影**，误导所有后续显示判断 |
| 验证手段 | 观察开机后是否一直是彩条（=P0-3 / P0-2 命中），还是 splash 短暂出现后被覆盖 |
| DEBUG_LOG 关联 | 阶段⑭ 探针代码本应是"一次性验证 → 拆除 → 回 git"，但至今未回 git |

---

### 3.2 🟡 P1 — 高度可疑，需烧录验证

#### P1-1 方向硬编码，与 `DISPLAY_ORIENTATION` 宏脱钩

| 项 | 内容 |
|---|---|
| 位置 | `main/display.cpp:264-266` vs `main/config.h:72-74` |
| 现象 | 代码硬编码 `swap_xy(false), mirror(false, false)`；`DISPLAY_ORIENTATION` 宏**完全没参与方向决策** |
| 历史 | DEBUG_LOG 阶段⑩/⑬/⑭ 对方向做了多轮猜测（`swap_xy` 真值、`MADCTL` bit5 误读），**当前硬编码没对应任何确定结论** |
| 后果 | 若 `DISPLAY_ORIENTATION=0`（横屏 320×240）实际需要 `swap_xy=true`（厂家 MADCTL=0x20 解读），则方向错位 → 内容落到可见区外 / 倒置 |
| 验证手段 | 把方向改成 `(true,false,false)` 或 `(false,false,true)` 等 4 种组合分别烧一遍，对比屏显 |
| DEBUG_LOG 关联 | 阶段⑬ 误把 `swap_xy` 改 `false`，阶段⑭ 未得出结论 |

#### P1-2 手动复位 + `esp_lcd_panel_reset` 二次复位

| 项 | 内容 |
|---|---|
| 位置 | `main/display.cpp:207-214`（手动复位序列）+ `:250`（`panel_cfg.reset_gpio_num=DISPLAY_RESET_IO`） |
| 机制 | `esp_lcd_panel_init` 内部会驱动 `reset_gpio` 再次复位；手动复位后 panel_init 又复位 → **双重复位** |
| 注释意图 | "避免二次复位后时序不满足 ST7789 要求的 120ms 上电稳定时间导致 init 失效" —— 但 esp_lcd 内部自己加延时，不会破坏时序 |
| 后果 | 双重复位一般不致命，但某些 ST7789 变种对复位次数敏感；多余操作可能掩盖其他时序问题 |
| 验证手段 | 观察日志 `DBG: lcd hw reset done` 与 `panel_init ret` 之间的时间间隔（应有 ~120 ms，但实际两次复位叠加可能更短） |

#### P1-3 `max_transfer_sz=32767` 在 SPI DMA 安全上限边缘

| 项 | 内容 |
|---|---|
| 位置 | `main/display.cpp:224` |
| 机制 | ESP32-S3 SPI DMA 单次实际安全上限是 **32752 字节**（部分 SDK 版本是 32764），32767 是 `int16_t` 边界，**触发后表现不稳定**：偶发 OK / 偶发截断 |
| 后果 | 冷启 10 次看是否稳定；有时花有时不花 = 边缘上限命中 |
| 验证手段 | 把它降到 8192 强制分片重编一次，观察花屏是否消失（仅诊断，不入库） |

#### P1-4 PSRAM LVGL buffer 与 SPI DMA 缓存一致性

| 项 | 内容 |
|---|---|
| 位置 | `main/display.cpp:809-810` |
| 机制 | `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`（PSRAM）。ESP32-S3 SPI 控制器可读 cache（理论 OK），但 esp_lcd 内部默认 `MALLOC_CAP_DMA` 路径对 PSRAM 不友好 —— 部分 SDK 版本存在 cache miss / 写穿延迟 |
| 后果 | flush 时偶发读到旧数据 → 屏上"鬼影" |
| 验证手段 | 把 buffer 临时改 `MALLOC_CAP_INTERNAL` 重编一次，看异常是否消失（仅诊断） |

#### P1-5 背光 PWM `ch.duty=0` 与 `set_brightness(100)` 竞态

| 项 | 内容 |
|---|---|
| 位置 | `main/display.cpp:139`（init 时 duty=0）+ `:793`（紧接 set 100%） |
| 机制 | LEDC `ledc_channel_config` 后立即 `set_brightness(100)`，但 ledc 不一定有 update 同步；少数情况 `set_brightness(100)` 调用前被其他中断打断 → 第一次刷屏时背光实际是 0 |
| 后果 | 用户看到"屏幕瞬间黑"（背光未跟上） |
| 验证手段 | 观察背光在 splash 出现的瞬间是否同步亮（用逻辑分析仪抓 GPIO15 PWM） |

---

### 3.3 🟢 P2 — 隐患/代码质量（不直接导致异常，但建议修）

| # | 位置 | 问题 |
|---|---|---|
| P2-1 | `display.cpp:268 + :270` | `esp_lcd_panel_disp_on_off(true)` 调用两次，无意义，可能是阶段调试残留 |
| P2-2 | `display.cpp:807-825` | LVGL buffer 40 行 + `LV_DISP_RENDER_MODE_PARTIAL` 已落地（DEBUG_LOG ⑫ 修复）✓；但 UI 对象创建后才第一次 flush，splash 整屏 invalidate 仍是 P0-2 风险 |
| P2-3 | `display.cpp:761-855` | `display_init` 未测 `lcd_hw_init` 失败后的状态恢复（`g_display_initialized=false`，但 LVGL 对象已 null —— 后续 `show_splash` 会 safe return）✓ 可接受 |
| P2-4 | `display.cpp:184-194` | `esp_task_wdt_add(NULL)` 写在 task 函数体内，未配套 `esp_task_wdt_delete`（死循环外）。task 永不退出，可接受 ✓ |
| P2-5 | `display.cpp:267` | `invert_color(true)` 与厂家 0x21 反显命令一致（DEBUG_LOG ⑩ 已确认）✓ |
| P2-6 | `display.cpp:236` | SPI 时钟 10 MHz 太保守（2.4 寸 ST7789 可 40-62.5 MHz），导致 splash 出现慢，但不会异常 |
| P2-7 | `display.cpp:280` | 彩条 buffer 用 `MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`（DEBUG_LOG ⑪ 修复）✓ |
| P2-8 | `display.cpp:182-195` | `lvgl_task` 用 8 KB 栈 + `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_INTERNAL)`，栈 DRAM 优先 ✓ |

---

## 4. DEBUG_LOG 阶段结论 vs 当前代码 落账

| DEBUG_LOG §7 阶段 | 根因 | 代码修复状态 | 评估 |
|---|---|---|---|
| ⑧ `max_transfer_sz` 太小 | ✅ 已改为 153600 | ❌ **当前是 32767 边缘值**（非 DEBUG_LOG 结论，是阶段⑫ 改的） |
| ⑨ 一帧 153600 字节 | （同⑧） | — | — |
| ⑩ RGB 颜色 / 方向 | ❌ 误读 MADCTL bit5 | 后续⑬ 纠错 | 阶段⑩ 误读、阶段⑬ 纠错后又改回去（swap_xy 反转） |
| ⑪ 降速 10 MHz / DMA buffer | ✅ 已改 | ✅ | 直绘彩条路径正确，flush_cb 路径**未沿用 DMA buffer 思路**（P0-2 漏修） |
| ⑫ SPI 32767 上限 | ⚠️ 设了上限但**未在 flush_cb 强制分片** | ❌ | **P0-2 漏修的关键** |
| ⑬ GRAM 240 列 + `swap_xy` 真值 | ❌ 多次反复 | ❌ **当前硬编码 swap_xy=false 未定论** | P1-1 |
| ⑭ 探针彩条 | ✅ 加探针 | ❌ **未拆除，永久驻留** | P0-3 |

---

## 5. 验证手段汇总（**只观察不改代码**）

| 序号 | 手段 | 命中判定 |
|---|---|---|
| V-1 | 抓 10 秒串口日志，统计每秒 ESP_LOG 行数 | >500 行/秒 → P0-1 命中 |
| V-2 | 观察开机后是否一直停在彩条 | 是 → P0-3 / P0-2 命中（LVGL splash 未成功） |
| V-3 | 若 splash 出现但异常（花屏/错位/截断） | 是 → P0-2 命中 |
| V-4 | 若 splash 正常但 UI 异常（方向错、鬼影、抖动） | 方向错 → P1-1；鬼影 → P1-4；抖动 → P1-5 |
| V-5 | 连续冷启 10 次统计是否稳定 | 不稳定 → P1-3 边缘上限 |
| V-6 | 逻辑分析仪抓 GPIO15 PWM 与 splash 出现的同步性 | 不同步 → P1-5 |

---

## 6. 后续建议（**不立即执行**，待用户确认）

按严重度与依赖关系排列：

1. **P0-3 拆除诊断彩条**（必做）—— 不拆无法判断后续现象
2. **P0-1 关掉 lvgl_task / flush_cb 日志**（必做）—— 不关无法做帧率/性能判断
3. **P0-2 flush_cb 手动分片**（高概率花屏根因）
4. P1-1 方向按 `DISPLAY_ORIENTATION` 宏走，先把 4 种组合各烧一遍验证
5. P1-2 选择"手动复位"或"esp_lcd 复位"二选一，避免双重复位
6. P1-3 `max_transfer_sz` 降到 8192（强制分片）或 32752（边缘安全）
7. P1-4 LVGL buffer 改为 `MALLOC_CAP_INTERNAL` 验证
8. P1-5 调整背光 PWM 时序
9. P2 系列按需清理

---

## 7. 待办状态

| 项 | 状态 |
|---|---|
| 显示异常诊断报告（本文件） | ✅ 完成 2026-08-18 |
| 修复方案与顺序确认 | ⏳ 待用户 |
| R 节点框架 | ❌ 未建（无代码修改，按全局规范 8.1 不触发） |
| `DEBUG_LOG.md` 同步本报告结论 | ⏳ 待 R 节点提交后追加 |
| `CONTEXT.md` / `SESSION_SUMMARY.md` 同步 | ❌ 当前 R030 严重滞后，本报告暂不入 SESSION |

---

## 8. 风险与免责

- 本报告**未实测**，所有 P0/P1 判定均基于代码静态分析与 ESP-IDF / ESP32-S3 文档推导。
- 实际命中项需用 V-1~V-6 验证手段实测后才能确定。
- 若修复后出现新异常，请回头检查"是否同时改了多个 P 项导致互相干扰"（建议**逐项单独验证**，不并发）。
- 本报告不构成修复承诺；修复顺序与方式以用户最终确认的方案为准。

---

## 9. 重新梳理 — 用户改动后状态（2026-08-18 同日下午）

用户在收到 §6 建议后，自行做了一轮修复（未 commit，工作区改动）。重新梳理结果如下：

### 9.1 用户已修复（与旧诊断对照）

| 旧 ID | 问题 | 修复方式 | 证据 |
|---|---|---|---|
| **P0-1** | lvgl_task 日志洪水 | 删除 4 行 ESP_LOGI | `display.cpp:188-200` |
| **P0-2** | flush_cb 未手动分片 | 实现 `max_bytes=32752` 按行分块循环 | `display.cpp:166-185` |
| **P0-3** | 诊断彩条驻留 | 删除彩条块 | `display.cpp:269-280` 彩条完全删除 |
| **P1-3** | max_transfer_sz 32767 边缘 | 改 32752，注释说明避开 int16 边界 | `display.cpp:229` |
| 配套 | lv_conf.h 关 LVGL 内部日志 | `LV_USE_LOG=0` | `lv_conf.h:44` |
| 配套 | sdkconfig 改 PSRAM 优先分配 | `SPIRAM_USE_MALLOC=y + MALLOC_ALWAYSINTERNAL=0` | `sdkconfig.defaults:22-25` |

### 9.2 仍存 / 部分修（与旧诊断对照）

| ID | 状态 | 备注 |
|---|---|---|
| P1-1 方向硬编码 | ⚠️ 仍存（见 §10 后续修复） | `swap_xy(false), mirror(false,false)` 与 config.h 注释自相矛盾 |
| P1-2 双重复位 | ⚠️ 仍存 | 手动复位 + esp_lcd_panel_init 又复位 |
| P1-4 PSRAM 缓存一致性 | ⚠️ 部分缓解 | sdkconfig 改了，但 `display.cpp:789-790` 仍 `MALLOC_CAP_SPIRAM` 显式分配 |
| P1-5 背光 PWM 竞态 | ⚠️ 仍存 | `lcd_backlight_init` duty=0 后立即 set 100% |
| P2-1 `disp_on_off(true)` 重复调用 | ⚠️ 仍存 | `display.cpp:276 + :278` |

### 9.3 新引入风险

| ID | 风险 | 评估 |
|---|---|---|
| **N-1** 🔴 | `LV_USE_LOG=0` 静默 LVGL 内部错误 | freetype 初始化失败（DEBUG_LOG 历史遗留 `lv_result=0`）现在更难定位 |
| N-2 🟡 | BT 模式与按键提示音路径潜在冲突 | 理论安全（`app_play_beep` 不在 BT 模式触发），但 `audio_player_play_beep()` 无 `if (g_bt_active) return;` 守卫 |
| N-3 🟡 | OTA 升级加 `ota_sd_tick()` 主循环每 200ms | 不直接导致显示异常，可能拖慢主循环 |
| N-4 🟢 | `LV_DRAW_THREAD_STACK_SIZE` 位置不当 | 当前未启用绘制线程，预留值无影响 |

---

## 10. P1-1 方向宏驱动改造（2026-08-18 用户症状"屏幕 1/4 花屏 + 其他部分黑"）

### 10.1 症状分析

用户实测屏幕呈现"1/4 花屏 + 其他部分黑"。

读 ESP-IDF `esp_lcd_st7789` 驱动源码（`esp_lcd_panel_st7789.c`）确认：
- `panel_st7789_init`（L180-199）：发初始 MADCTL（基于默认 `madctl_val`）
- `panel_st7789_swap_xy`（L267-280）：改 `madctl_val |= MV_BIT`，**立即**重发 MADCTL → 在 panel_init 之后调也生效
- `panel_st7789_mirror`（L247-265）：改 MX/MY 位，立即重发 MADCTL
- `panel_st7789_draw_bitmap`（L201-230）：单次 `tx_color(RAMWR, ..., len)` 不分片

→ 驱动层支持"panel_init 之后调 swap_xy"，方向动态生效。

**症状与方向错匹配**：
- ST7789 物理 GRAM = **240 列 × 320 行**
- 当前代码 `swap_xy(false)`（MV=0）→ 按列方向扫描，LVGL 写 320 列 → **第 241~320 列溢出**
- 折返/丢失 → 屏"左 1/4 花屏 + 其他部分黑"
- 与 DEBUG_LOG §7 阶段⑬"1/4 花屏"特征一致，但用户场景是"3/4 黑屏"而非"3/4 正常"——更接近**列地址溢出 + 折返区落到屏外**

### 10.2 修复方案

按"彻底方向宏驱动"（方案 C）：
1. `config.h:67-99`：`DISPLAY_ORIENTATION` 推导 `DISPLAY_SWAP_XY / DISPLAY_MIRROR_X / DISPLAY_MIRROR_Y` 三宏
   - 横屏 (DISPLAY_ORIENTATION=0)：`SWAP_XY=1, MIRROR_X=0, MIRROR_Y=0`
   - 竖屏 (DISPLAY_ORIENTATION=1)：`SWAP_XY=0, MIRROR_X=0, MIRROR_Y=0`
2. `display.cpp:269-274`：把用户 437 行 diff 中合并保留的一段硬编码（`swap_xy(false), mirror(false, false)`）改为读宏
3. 注释解释：`mirror_x/mirror_y` 烧录后按实际呈现微调（无需改驱动代码）

### 10.3 预期效果

- **横屏 320×240 默认配置**：`swap_xy=true, mirror=(false, false)`
- LVGL 320 列方向 → GRAM 320 行方向（MV=1）→ 物理读出旋转 90° → 屏可见 320×240 横屏
- 若烧录后**内容侧躺或镜像**：改 `config.h` 的 `DISPLAY_MIRROR_X / DISPLAY_MIRROR_Y` 即可，无需改代码

### 10.4 待验证

- [ ] 烧录一次，确认 1/4 花屏是否消失
- [ ] 若侧躺/镜像，调整 mirror_x/mirror_y 烧第 2/3/4 次
- [ ] 找对方向后，回填本节"实际验证结果"小节

### 10.5 R 节点状态

- **未建 R 节点**（无 commit，按全局规范 8.3）
- 等用户烧录验证 OK 后，**连同工作区其他未提交改动**（R049c OTA / R050 BT / R051 A-B 屏 / 字体等）一并 commit
- 建议建节点时命名 `R052` 或并入 `R049d`，按届时方案定

---

**作者**：Claude（按全局 CLAUDE.md 9.x 规范）  
**维护规则**：每次 R 节点（涉及显示修复）提交后必须更新本文件 §7 与"修复记录"小节。