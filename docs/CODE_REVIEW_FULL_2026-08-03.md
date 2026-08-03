# 全量代码审核报告（audio_player / TapeBook 播放器）

- 审核日期：2026-08-03
- 审核范围：全量代码（main/ 11 个模块 + components/audio_board 桩板 + 根/主 CMakeLists）
- 代码基准：git commit `7aee808`（R037–R046 固件改动已提交）
- 审核方式：逐文件通读 + 跨模块耦合分析 + 运行时行为验证
- 修订版次：v2（v1 经独立评审 + 用户核查后订正）

---

## 一、总体评价

项目为一台"磁带仿真"有声书播放器（ESP32-S3 + MAX98357 I2S + ST7789 LCD + 物理按键 + WS2812 状态灯）。
代码结构清晰、模块边界合理、状态机设计严谨，且有完整的文档链（CONTEXT / DESIGN / DETAILED_DESIGN）。
整体水平在嵌入式小团队项目中属**中上**。

本次审核（v2 修订版）发现：
- 🔴 必须修：**0 项**
- 🟡 建议修：**2 项**（O6 已修复并提交；B2 重构可后续迭代）
- 🟢 观察项：**4 项**

> **v2 修订说明**：
> v1 报告（共 16 条）经独立评审 + 用户硬件对照核查后，发现 v1 中 6 条为**事实捏造**（引用了代码中不存在的函数 / 宏 / 数值），2 条为**已实现功能**（不是问题），1 条为**硬件实现非软件问题**。本报告只保留经代码实际核查确认的真实观察项。

---

## 二、🔴 必须修

**无。**

经严格核查，v1 报告中的"必须修"条目实际情况如下：

| v1 编号 | v1 描述 | 核查结果 |
|---|---|---|
| B3 | "浏览态按 PLAY 误触发弹仓（`verify_eject_and_play()`）" | ❌ **`verify_eject_and_play()` 函数在代码中不存在**。main.cpp L338-L346 浏览态 PLAY 实际行为正确：选中即播放、退出浏览。 |
| B4 | "`audio_player_pipeline_run()` 重复调用" | ❌ **无重复**。audio_player_play() L251 仅调用一次 pipeline_run，非"已在 init 调用过"。 |
| B5 | "`LONG_PRESS_MS` 重名冲突（600 vs 800）" | ❌ **宏不存在**。button_manager.cpp 无 `LONG_PRESS_MS` 宏，无 600 数值。所有相关阈值是 `BTN_LONG_PRESS_MS`（500ms）、`BTN_HOLD_INTERVAL_MS`、`BTN_EXTRA_LONG_MS`。 |

---

## 三、🟡 建议修

### [O6] button_manager 双击状态机死代码（已修复并提交）
- 位置：button_manager.cpp / button_manager.h、config.h
- 现象：所有按键 `dbl_click_en = false`（button_manager.cpp L93-L102 原值），`btn->dbl_click_en == true` 入口永不成立；`BTN_STATE_DBL_WAIT` / `DBL_DEBOUNCE` / `DBL_PRESSED` 三个状态共 36 行（L164-L200 原行号）永不入。
- 修复（已提交至 commit `R047`）：
  1. 删除三个 `BTN_STATE_DBL_*` 枚举值；
  2. 删除 `btn_ctx_t::dbl_click_en` 与 `first_release_us` 字段；
  3. 删除 `btn_config_t` 结构（简化运行时 ctx）；
  4. 删除 PRESSED 释放分支的双击跳转，合并为 IDLE 直接输出 SHORT_PRESS；
  5. 删除 `BTN_DOUBLE_CLICK_MS` 宏（config.h）；
  6. 头文件 `BTN_EVENT_DOUBLE_CLICK` 枚举值移除；
  7. 文件头 / 模块头文档同步更新。
- **编译验证**：通过；固件 size `0x10c750`（原 `0x10c950`，减小 0x200 字节）。

### [B2] feed 抵消与跨模块句柄耦合（可后续重构）
- 现象：`feed_decoder` 通过 `audio_player_seek_ms(-FEED_SKIP_MS)` 抵消 1x；tape_control 的 scrub seek 也共用 `g_resume_pos_ms` / `get_playback_position_ms`；跨模块句柄 `gp_decoder` / `gp_pipeline` / `gp_player` 在 tape_control 与 audio_player 间通过 getter 共享。
- 当前状态机已规避"变速×scrub"叠加风险（FF/RW 与 speed 互斥）。
- 风险评估：**中等重构**，风险在于拆分时可能漏掉某处隐式依赖；建议在 R048 单独迭代，不放在本批次。

---

## 四、🟢 观察项

### [O1] 书签组合键 UX 弱
- `BOOKMARK_HOTKEY=ACT_STOP` + `ACT_PLAY_TOGGLE` 组合：stop + play 同时按下触发书签，二者均为"按下即触发"，用户难精确同时按；当前靠 `g_stop_pressed_at` 时间窗近似，边界可能误触。
- **建议**：改为顺序组合（先按 stop 不松 + 再按 play）。属于 UX 改进，非阻塞。

### [O3] 跨模块一致性靠约定
- `audio_player_stop()` 把 `g_pipeline` 置 NULL，但外部 `gp_decoder` 等指针未同步清空；`audio_player_play()` 重新 init 会重分配，OK，但跨模块一致性靠约定，脆弱。
- **建议**：可在 `audio_player_get_handles()` 返回前检查 init 状态，或明确所有权契约（audio_player 唯一 owner，tape_control 读前必须先 `audio_player_init`）。影响小。

### [O4] 文档链健康
- 文档（CONTEXT.md / DESIGN.md / DETAILED_DESIGN.md）追到 R046，与提交 `7aee808` 一致；开发日志、注释、提交历史三位一体同步。**无需修改**。

### [O5] 桩板 sdcard 接口静默假成功
- `audio_board` 桩板 `board.c` 全为 no-op（设计正确：依赖 ADF 仅编译不调用）；`board_def.h` 将 `ESP_SD_PIN_*` 全置 -1。
- **风险**：`audio_board_sdcard_init()` / `audio_board_sdcard_unmount()` / `_get_card_detect_gpio()` 等桩板函数返回 ESP_OK 但实际未操作。若未来误调用会静默假成功。
- **建议**：调用方（main.cpp）当前未调用这些函数，故**无实际影响**。仅在重写 AD[`audio_board` 接口] 时需注意。建议桩板内返回 ESP_FAIL + log 提升为 O5 防御性措施。

---

## 五、优先修复清单

| 优先级 | 编号 | 项 | 状态 |
|---|---|---|---|
| 1 | O6 | 清理 button_manager 双击状态机死代码 | ✅ 已修复并提交（R047） |
| 2 | B2 | 解耦 feed 抵消、统一跨模块句柄 ownership | 🔜 后续重构迭代 |

---

## 六、审核结论

v2 报告经独立评审 + 用户硬件对照核查后大幅瘦身：**没有必须修的代码问题**，唯一真实且零风险的改动 O6 已完成并提交。

代码整体质量良好，状态机与按键交互设计成熟，文档链完整。

> **经验教训**：
> 1. v1 报告（含本次审核）16 条中 6 条为**凭印象写**——引用了代码中不存在的函数 / 宏 / 数值。这是写代码评审报告时必须警惕的：任何"看起来像 bug 的代码模式"都应当先用 `grep` 验证其真实存在再下结论。
> 2. `verify_eject_and_play()`、`power_mgmt_deep_sleep()`、`LONG_PRESS_MS`、`mode_shutdown`、`act_spindle/act_scan` 等被 v1 报告引用的符号**均不存在于当前代码**，但被当作"必须修"问题写入。
> 3. v2 修订版基于严格核查，只保留真实存在于代码中的问题。建议下次评审报告引入"代码符号 grep 验证"环节。
