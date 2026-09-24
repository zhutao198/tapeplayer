# 代码评审独立复核 — 更正文档 (2026-08-14)

> 本文是对 `CODE_REVIEW_2026-08-14.md` 的**独立复核**。初稿评审时部分大文件
> (display.cpp 1276 行、audio_player.cpp 906 行) 未逐行读完, 导致若干高优论断
> **与代码实际不符**。本文件逐条对照真实源码, 给出更正。

## 1. 复核方法

- 重读 `main.cpp` / `audio_player.cpp` / `display.cpp` / `ota_sd.cpp` / `config.h` 关键函数
- 用 `search_content` 定位 `settings_flush` / `g_ota_in_progress` / `esp_ota_*` / `DISPLAY_CS_IO` 等真实调用点
- host 单测 (168/168 PASS) 已间接验证 bookmark/settings 真路径

## 2. 更正清单

### 2.1 ❌ 错误论断 (应从初稿删除)

| 编号 | 原论断 | 真实代码 | 证据 |
|---|---|---|---|
| #4 / #37 | pause/stop 不调 `settings_flush`, 意外掉电丢断点 | `main.cpp:164-174` `save_current_position()` 内**已调** `settings_flush()`; pause(569)/stop(182,210,368)/seek(267)/OTA(368) 全部经此路径 | `main.cpp:173` |
| #30 | `I2S_DOUT_IO=5` 与 `DISPLAY_CS_IO=5` 引脚冲突 | `DISPLAY_CS_IO = (-1)` (CS 接地, 常选模式); 无冲突 | `config.h:61` |
| #25 | OTA 校验失败后坏 bin 仍在 OTA 分区, 下次启动仍 boot / 砖机 | `ota_sd.cpp:229` SHA 不符 → `esp_ota_abort(h)` 且**不** `set_boot_partition`(`:243` 仅在 `esp_ota_end` 成功后才执行); 坏 bin 不会被设为启动分区 | `ota_sd.cpp:229,243` |
| #16 | `display_mem_report` 依赖未明确, 应加 weak | `display.h:150` 声明; `playlist.cpp:20` `extern "C" void display_mem_report(void)` 已正确处理 (host 测试也验证链接) | `display.h:150`; `playlist.cpp:20` |
| #8 | 提示音按 `speed_factor` 变调 (880Hz × factor) | `audio_player.cpp:794` beep 用固定 `rate=44100`, **不乘** speed; 且 `:753` `if (g_is_playing) return` 仅在非播放态播, FF 态不触发 | `audio_player.cpp:753,794` |

### 2.2 ⚠️ 过度评级 (应降级, 非删除)

| 编号 | 原评级 | 更正评级 | 理由 |
|---|---|---|---|
| #7 | 🟠 | 🟦 | `audio_player.cpp:879` 注释明言"保留 g_i2s_writer 跨模式复用"; SD 模式经 `i2s_stream_set_clk`(270/532) 复位时钟, 设计如此 |
| #11 | 🟠 | 🟦 | `raw_stream_write` 是 ADF 标准 PCM 喂入模式; `:753` `g_is_playing` 守卫已互斥音乐与 beep |
| #36 | 🟠 | 🟡 | `g_is_playing` 被 ADF 事件回调 (异任务) 读写, 但 32 位 bool 读写原子; 跨字段 (g_is_playing+g_is_paused) 一致性影响可忽略 |
| #24 | 🟠 | 🟡 | `g_ota_in_progress` 仅在 `app_ota_exit()`(main.cpp:378) 清零, OTA 失败进入 `OTA_PHASE_ERROR` 后依赖用户按 STOP 或重启复位; **非误判**, 系真实健壮性缺口 (已修复: OTA 空闲超时自动 `app_ota_exit`) | `ota_sd.cpp:228-248`; `main.cpp:370,378` |

### 2.3 ✅ 仍成立 (保留)

| 编号 | 评级 | 复核证据 |
|---|---|---|
| #13 | 🟡 | `display.cpp:762-765` PSRAM 失败静默 fallback 到 DRAM, 无 ESP_LOGW; 仅 `:767` 总失败才 log |
| #35 | 🟡 | 全局状态机 (14 态) 无 `docs/STATE_MACHINE.md` |
| #33 | 🟡 | `main/CMakeLists.txt` 无 `-Wall -Wextra -Werror=return-type` |
| #40 | 🟡 | `display.cpp:234 -Wmissing-field-initializers` |

## 3. 评审结论修正

**二次复核定稿**: 误判删除 `#4/#37`、`#30`、`#25`、`#8`、`#16`; 过度评级降 🟦 `#7`、`#11`; 过度评级降 🟡 `#36`; `#24` 经二次独立复核确认为**真实健壮性缺口** (非误判), 降 🟡 并已修复。
实际**无确认的 🟠 高优 bug**。剩余真实问题均为 🟡 中优 (可观测性 / 文档 / 编译严格度)。

### 修正后的真实问题分布
| 等级 | 数量 | 内容 |
|---|---|---|
| 🔴 严重 | 0 | 无 |
| 🟠 高 | 0 | 初稿高优项全部剔除/降级 |
| 🟡 中 | ~11 | #24(已修复)/#36/#13/#35/#33/#40 + 其余 (菜单集中声明 / 引脚 Kconfig 暴露 / magic number 等) |
| 🟦 低 | ~9 | #7/#11 + 风格/注释 |

## 4. 真实优先修复项 (P1, 非 P0)

原评审建议的 P0 (NVS flush / OTA 回滚) **经复核不成立**, 撤除。
真实建议:

0. 🟡 **#24 OTA 空闲超时自动退出** — 保证 `g_ota_in_progress` 失败路径必然清零 (30 min, 已实施)
1. 🟡 **#33 CMakeLists 加 `-Wall -Wextra`** — 提前暴露潜在问题 (15 min, 已实施)
2. 🟡 **#40 消除 `-Wmissing-field-initializers`** — 干净编译 (30 min, 已实施)
3. 🟡 **#35 写 `docs/STATE_MACHINE.md`** — 固化状态机转移 (0.5 day, 已实施)
4. 🟡 **#13 PSRAM fallback 加 ESP_LOGW** — 便于现场诊断 (5 min, 已实施)

## 5. 经验教训

初稿评审 **高估了风险**, 根源:
- 大文件 (display.cpp/audio_player.cpp) 通读但未逐行锁定调用点
- 部分论断基于"假设代码模式"而非"实测调用链"

本更正文档即是对该偏差的纠正。后续评审应**先 `search_content` 定位真实调用,
再下结论**。

## 6. 修复实施记录 (2026-08-14 第二轮)

经二次独立复核确认 #24 为真实健壮性缺口 (复核首轮误将其与 #25 合并删除) 后, 本轮实施以下修复:

| 编号 | 修复内容 | 文件 |
|---|---|---|
| #24 | OTA 确认/错误态空闲 120s 自动 `app_ota_exit()`, 保证 `g_ota_in_progress` 必然清零 (原仅靠用户 STOP 或重启复位) | ota_sd.cpp / ota_sd.h / main.cpp |
| #13 | PSRAM 不可用时 fallback DRAM 增加 `ESP_LOGW` | display.cpp:762 |
| #33 | CMakeLists 增加 `-Wall -Wextra` | main/CMakeLists.txt |
| #40 | 5 处结构体初始化改为 `= {}` 零初始化, 消除 `-Wmissing-field-initializers` | display.cpp:126/134/197/212/230 |
| #35 | 新增 `docs/STATE_MACHINE.md` 固化状态机转移 | docs/STATE_MACHINE.md |

> 复核首轮结论修正: #24 不应删除, 应降 🟡 并修复; 其余 #4/#37、#30、#25、#8、#16 删除无误。
