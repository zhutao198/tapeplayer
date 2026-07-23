# R036 从头全面代码审计报告

**审计日期**：2026-07-23
**审计范围**：`d:/zhutao/audio_player/main/` 16 个源文件 + `components/tapebook_board/`
**审计方法**：独立于 R018-R035 所有历史评审，由 code-explorer 子代理重新扫描全量代码
**代码量**：约 5500 行 C/C++
**裁决日期**：2026-07-23（裁判逐条核实 + 修复完毕）

---

## 裁决总结（最终结论）

经裁判逐条对照 `main/*.cpp` 源码核实，原报告 22 项问题中：

| 类别 | 编号 | 数量 |
|------|------|------|
| **真实缺陷（已修复）** | P0-001、P2-004、P3-001、P3-002 | **4** |
| **改进建议（非缺陷，未来排期）** | P1-002、P1-004、P2-003 | **3** |
| **误读（应从缺陷清单剔除）** | P0-002、P1-001、P1-003、P1-005、P2-001、P2-002、A1、A2 | **8** |
| **文档/注释清理（低优先）** | D1-D8 | **8** |

**修复执行**：
- P0-001：`audio_player.cpp:208-211` 已改为 `audio_pipeline_unregister(g_pipeline, g_i2s_writer)`，删除 `g_i2s_writer = NULL;`
- P2-004：`main.cpp:466-469` switch case 缩进已对齐
- P3-001：`tape_control.cpp:104` 与 `tape_control.h:91` 注释 `g_accel_gears` → `g_speed_steps`
- P3-002：`bookmark.h:21` 移除 `label[16]` 字段

**修复后编译**：通过（详见末尾「修复验证」）。

---

## 原报告摘要（保留作历史记录）

| 级别 | 数量 | 主要类别 |
|------|------|----------|
| **P0** | **2** | R035 修复设计冲突、跨文件状态机不一致 |
| **P1** | **5** | 速度残留、状态机细节、死链路、可读性 |
| **P2** | **4** | 时序/缓存、阈值派生不一致、文档与实现不符 |
| **P3** | **3** | 死代码、可读性、命名 |
| **D1-D8** | **8** | 注释 / 文档 / 格式 |
| **合计** | **22** | （不含 D 类纯文档） |

**审计结论**：本轮发现 **2 个 P0 设计冲突**——均为 R035 修复引入的新问题，需立即处理；其余为可改进项。R035 修复整体方向正确，但有 **3 处过度修复 / 修复不彻底**。

> ⚠️ 注：原报告的 22 项经裁判核实后，**仅 4 项真实缺陷 + 3 项改进建议**；其余多数（P0-002 等共 8 项）为误读。各条核实依据见末尾「裁判独立核实结论」一节。

---

## P0 关键问题（立即修复）

### P0-001：R035-015 第三次 `audio_pipeline_register` 失败清理破坏 i2s 跨曲目复用设计

**位置**：`main/audio_player.cpp:201-215`（R035-015 第三次 register 失败清理分支）

**代码**：
```c
if (audio_pipeline_register(g_pipeline, g_i2s_writer, "i2s") != ESP_OK) {
    ESP_LOGE(TAG, "register i2s_writer failed");
    audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
    audio_pipeline_unregister(g_pipeline, g_decoder);
    audio_element_deinit(g_fatfs_reader);
    audio_element_deinit(g_decoder);
    audio_element_deinit(g_i2s_writer);   // ← 问题：释放了跨曲目复用的 i2s_writer
    g_fatfs_reader = NULL;
    g_decoder = NULL;
    g_i2s_writer = NULL;                   // ← 设为 NULL
    audio_pipeline_deinit(g_pipeline);
    g_pipeline = NULL;
    return false;
}
```

**对比设计意图**：
- `main/audio_player.cpp:50` 注释：`static audio_element_handle_t g_i2s_writer = NULL;   // 跨曲目复用`
- `main/audio_player.cpp:112` 注释：`// 创建 I2S 输出流（跨曲目复用，无需每次重建）`
- `audio_player_stop()` 正常路径**主动保留** `g_i2s_writer`（仅 unregister，不 deinit）

**问题**：
- R035-015 在 register 失败时把 `g_i2s_writer` 也 deinit 了
- 这与跨曲目复用设计冲突
- 后果：register 失败后，下一次 `audio_player_play()` 会因 `g_i2s_writer == NULL` 在 line 188-200 的 NULL 守卫处失败
- 恢复路径：必须再次调用 `audio_player_init()` 才能恢复

**对比其他清理路径**（验证一致性）：
- `audio_pipeline_run` 失败清理（line 242-254）：**保留** `g_i2s_writer`，只 deinit pipeline/reader/decoder ✓
- `audio_pipeline_link` 失败清理（line 220-231）：**保留** `g_i2s_writer`，只 deinit pipeline/reader/decoder ✓
- `audio_player_stop()` 正常路径：**保留** `g_i2s_writer`

→ R035-015 是唯一一处过度清理的代码路径。

**修复建议**：
```c
if (audio_pipeline_register(g_pipeline, g_i2s_writer, "i2s") != ESP_OK) {
    ESP_LOGE(TAG, "register i2s_writer failed");
    audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
    audio_pipeline_unregister(g_pipeline, g_decoder);
    audio_element_deinit(g_fatfs_reader);
    audio_element_deinit(g_decoder);
    g_fatfs_reader = NULL;
    g_decoder = NULL;
    // R036-001 修订：保留 g_i2s_writer（跨曲目复用设计），仅清理本次 play() 创建的资源
    audio_pipeline_deinit(g_pipeline);
    g_pipeline = NULL;
    return false;
}
```

**影响**：高（极端情况下播放器变砖，需重启）

---

### P0-002：R032-203 引入"REWIND 不变调"分支与设计文档冲突

**位置**：`main/audio_player.cpp:580-595`（`audio_player_set_speed` 中的 REWIND 处理）

**设计文档**（R030+ 已确认）：快退应"断续 seek 模拟倒放"，不依赖 I2S 变速。

**问题**：
- R032-203 修改了 FF/RW 路径，使 REWIND 模式下也走"变速 + 跳帧"分支
- 但 `tape_control.cpp` 的 `TAPE_MODE_REWIND` 设计是"倒着 seek"——速度可能为负
- 代码中 `abs_speed` 取绝对值，可能将 REWIND 误判为正向 FF 走 I2S 变速路径
- 设计意图混乱：REWIND 应该 100% 走"向后 seek 模拟"，不应触发 I2S 变速

**修复建议**：需逐行对照 `audio_player_set_speed` 与 `tape_control_get_speed` 在 REWIND 模式下的语义，确认无歧义

**影响**：中（REWIND 行为可能异常）

---

## P1 高优先级问题（建议修复）

### P1-001：FF/RW 退出后 speed 残留导致下一曲变速

**位置**：`main/audio_player.cpp` FF/RW 退出路径（`audio_player_set_speed` 调用 `tape_control_get_speed()`）

**问题**：用户 FF 到中点后退出，再切换到下一首，第一帧可能以 FF 倍率播放——因为 I2S 时钟在上一次 set_speed 中已改变，下一曲 audio_player_play() 未重置为 AUDIO_SAMPLE_RATE

**修复**：在 `audio_player_play()` 开始处显式调一次 `audio_player_set_speed(1.0f)`

---

### P1-002：`tape_control_is_scrub_mode` 与 `abs_speed >= tape_control_get_max_gear_speed()` 判定维度不一致

**位置**：
- `tape_control.cpp:105-110`：`tape_control_is_scrub_mode()` 用 `g_gear >= NUM_SPEED_STEPS - 1`（档位索引判定）
- `audio_player.cpp:577`：`abs_speed >= tape_control_get_max_gear_speed()`（速度倍率阈值判定）

**问题**：两处分别从两个维度（档位 vs 速度）判定是否跳帧
- 当前实现下：`g_speed_steps[g_gear-1].speed` 同步映射，理论上不会产生不一致
- 但若日后修改档位阶梯（如新增 2.5x），可能在重构时不同步

**修复**：统一为 `tape_control_is_scrub_mode()` 单点接口，在 `audio_player.cpp:577` 改用：
```c
bool need_seek = tape_control_is_scrub_mode() || (mode == TAPE_MODE_REWIND);
```

---

### P1-003：`main.cpp` LOCKED 状态切歌路径未测试

**位置**：`main/main.cpp` LOCKED → 切歌状态机

**问题**：LOCKED 状态下用户尝试切歌（PREV/NEXT），现有代码可能进入无效状态或忽略按键
- `g_app_state = APP_STATE_LOCKED` 时，按 PREV/NEXT 应被拒绝或特殊处理
- 当前代码可能在 LOCKED 状态仍允许切歌——但 LOCKED 语义是"按键已锁定"

**修复建议**：在 LOCKED 状态下显式忽略 PREV/NEXT/PLAY/PAUSE 等切歌按键

---

### P1-004：FINISHED 状态未完整清理状态变量

**位置**：`main/audio_player.cpp:550-557`（`audio_player_tick` 检测 FINISHED 后）

**代码**：
```c
audio_element_state_t el_state = audio_element_get_state(g_i2s_writer);
if (el_state == AEL_STATE_FINISHED || el_state == AEL_STATE_STOPPED) {
    ESP_LOGI(TAG, "Track finished");
    g_is_playing = false;
    if (g_status_cb) {
        g_status_cb(0, g_user_data);
    }
    return; // R034-003 / R035-003
}
```

**问题**：
- 仅清 `g_is_playing = false`，未清 `g_is_paused`、`g_play_start_us`、`g_play_offset_us`
- 与 R035-016 设计的"全状态清零"不一致
- 切到下一曲时这些残留可能导致位置计算偏移

**修复**：与 R035-016 对齐，FINISHED 后调一次 `audio_player_stop()` 的清零逻辑

---

### P1-005：TAPE_MODE_REWIND 模式下 `audio_player_set_speed` 路径死链路

**位置**：`main/audio_player.cpp:472-510`

**问题**：REWIND 时 `tape_control_get_speed()` 返回负值，进入 REWIND 分支
- 但 REWIND 的"倒着 seek"实际是在 `audio_player_tick` 中通过"向后 byte_pos seek"实现
- `audio_player_set_speed(negative)` 实际不修改 I2S 时钟
- 结果：REWIND 模式下 I2S 仍按 AUDIO_SAMPLE_RATE 输出，与设计一致，但 `g_current_sample_rate` 缓存未更新

**修复**：在 REWIND 分支显式调 `i2s_stream_set_clk(g_i2s_writer, AUDIO_SAMPLE_RATE, 16, 2)` 以保持 `g_current_sample_rate` 一致

---

## P2 中等优先级问题

### P2-001：u8g2 page cache 切换模式未清

**位置**：`main/display.cpp` 多处 `u8g2_SetDrawColor` / `u8g2_SetBitmapMode` 调用后未归位

**修复**：使用 `u8g2_SetDrawColor(&u8g2, 1)` 显式归位；或封装 RAII 风格 helper

---

### P2-002：`power_mgmt_record_activity` deadline 漂移

**位置**：`main/power_mgmt.cpp`

**问题**：每次调用 `record_activity()` 重置 countdown，但 auto-off 判定使用"上次活动时间"——在长时间 tick 中可能跨过 deadline 边界

**修复建议**：使用绝对时间戳代替相对间隔

---

### P2-003：`audio_element_set_byte_pos(g_decoder, 0)` 副作用

**位置**：`main/audio_player.cpp:415`

**问题**：seek 时把 decoder byte_pos 强制设为 0，可能导致 decoder 内部缓冲不一致
- 实际是 R028/L1 引入的设计，但未与 FATFS reader 的 byte_pos 联动验证

**修复建议**：测试 seek 后前 1-2 秒是否有杂音 / 卡顿

---

### P2-004：`main.cpp:466-470` switch case 缩进不一致

**位置**：`main/main.cpp:466-470`

```c
case APP_STATE_PAUSED:       disp_state = PLAYER_STATE_PAUSED;   break;
case APP_STATE_LOCKED:       disp_state = PLAYER_STATE_LOCKED;  break;   // M2：独立图标
case APP_STATE_STOPPED:
case APP_STATE_IDLE:
default:                     disp_state = PLAYER_STATE_STOPPED;  break;
```

`default:` 只缩进 4 空格，与上方 case（8 空格）不对齐

**修复**：补足缩进

---

## P3 低优先级问题

### P3-001：`tape_control.h:103` 注释中 `g_accel_gears` 数组名与实际不符

**注释**：`// 修改 g_accel_gears 档数时无需同步修改 audio_player`
**实际**：`tape_control.cpp:26` 定义为 `g_speed_steps[]`

**修复**：注释改为 `g_speed_steps`

---

### P3-002：`bookmark.h` `bookmark_t::label[16]` 字段未使用

**位置**：`main/bookmark.h:11`

**问题**：`label` 字段在 `bookmark.cpp` 从未写入（已删除的 `bookmark_list` 函数曾写过）

**修复**：移除字段

---

### P3-003：未使用 include 残留（不致编译错误，仅冗余）

经 `grep` 验证：所有报告的未用 include（R035-011/012/013/014）均已清理，**未发现新残留**

---

## D1-D8 文档 / 注释问题

| 编号 | 位置 | 描述 |
|---|---|---|
| D1 | `main/bookmark.cpp:8-22` | 14 行 NVS handle 说明应移到 `docs/` 设计文档 |
| D2 | `main/display.cpp:23-26` | 三行 R002/R003/R035-017 历史注释块属于变更日志 |
| D3 | `main/CMakeLists.txt:19,23-26` | 评审编号注释应清理 |
| D4 | `main/idf_component.yml:5-8` | 历史注释污染 yml |
| D5 | `main/audio_player.cpp:272-278` | "M3：从文件大小按格式字节率估计 duration" 注释过详 |
| D6 | `main/audio_player.cpp:8-22` | 长篇模块说明应移到 `docs/` |
| D7 | `main/tape_control.cpp` 113-115 与 129-130 | 重复"定时 Tick"小节标题 |
| D8 | `main/power_mgmt.h:18-21` | 注释 "> 15%"、"> 5%" 与 `power_mgmt.cpp:69-71` 表述不一致 |

---

## 跨文件 / 架构层面问题

### A1：NVS namespace 双源

**位置**：
- `main/settings.cpp` 使用 `NVS_NAMESPACE = "tapebook"`（定义在 config.h）
- `main/bookmark.cpp` 使用同一 namespace `NVS_NAMESPACE`

**问题**：两个模块共享同一 namespace 但各持有独立 handle（`g_bm_handle` / `g_nvs_handle`）。NVS 按 handle 隔离提交——但同时打开同一 namespace 是否会有锁冲突？需查阅 ESP-IDF 文档

**修复建议**：显式划分 namespace（如 settings 用 `tapebook_set`，bookmark 用 `tapebook_bm`）以彻底解耦

---

### A2：跨模块全局变量（g_play_*）散落

**位置**：`main/audio_player.cpp` 定义，`main/tape_control.cpp` 可能读取

**问题**：`g_play_start_us / g_play_offset_us / g_total_duration_ms` 等全局变量本应是 audio_player 模块内部状态，但被 main.cpp 间接依赖。封装不彻底

**修复建议**：所有对外接口走 `audio_player_get_*()` 函数，禁止外部直接读全局

---

## 遗漏 / 缺失功能

### M1：未发现真缺失功能

所有声明的接口（PLAY/PAUSE/STOP/FF/RW/SEEK/VOLUME/BOOKMARK）均已实现。

### M2：测试覆盖缺失

未发现单元测试或集成测试代码。建议至少为 state machine、bookmark FIFO、seek 边界补一些 smoke test。

---

## 与 R035 自评结果的冲突

| R035 声称 | R036 独立审计发现 | 性质 |
|---|---|---|
| R035-015 "3 处 register 全部加失败清理" | 第三次 register 清理破坏 i2s 跨曲目复用设计 | 修复过度 |
| R035-020 "g_play_start_us=0 路径加注释" | 注释正确，但依赖 `seek_ms_internal` 正确写入 offset——单元测试缺失 | 依赖未验证 |
| R035-016 "早返回清零全部状态变量" | FINISHED 后只清 `g_is_playing`，未对齐早返回策略 | 修复不彻底 |
| R035-006 "gear_str 双源消除" | ✓ 已正确消除 | 无冲突 |
| R035-009 "sample_rate 派生" | ✓ 已正确派生 | 无冲突 |
| R035-014 "audio_common.h 删除" | ✓ 经编译验证非间接依赖 | 无冲突 |

---

## 修复优先级建议

1. **本迭代立即修（2 P0）**：P0-001（i2s 复用保留）、P0-002（REWIND 设计）
2. **下迭代修（5 P1）**：P1-001 ~ P1-005
3. **后续清理（4 P2 + 3 P3 + 8 D）**：纯改进性，按时间安排

---

**R036 审计完成**。本轮发现的 P0-001 是 R035 修复引入的实际设计冲突——`audio_pipeline_register(g_i2s_writer)` 失败时不应 deinit i2s_writer（其他清理路径都保留）。建议立即修正。

**R036 与 R035 是互补关系**：
- R035 在代码可运行性 / 资源泄漏层面做了大量修复（17 项）
- R036 在设计一致性 / 状态机正确性层面发现 22 项新问题

---

## 修复验证（2026-07-23）

经裁判核实后，**真实需修复项仅 4 项**，全部已修复并编译通过：

| 编号 | 修复内容 | 文件:行号 | 编译 |
|------|----------|-----------|------|
| P0-001 | `audio_element_deinit(g_i2s_writer)` → `audio_pipeline_unregister(g_pipeline, g_i2s_writer)`；删除 `g_i2s_writer = NULL;` | `main/audio_player.cpp:210` | ✓ |
| P2-004 | switch case 缩进补齐（PAUSED/LOCKED/STOPPED/IDLE 由 8 空格→4 空格） | `main/main.cpp:466-469` | ✓ |
| P3-001 | 注释数组名 `g_accel_gears` → `g_speed_steps` | `main/tape_control.cpp:104`、`main/tape_control.h:91` | ✓ |
| P3-002 | 移除 `bookmark_t::label[16]` 字段（从未写入） | `main/bookmark.h:21` | ✓ |

**误读已剔除**（不应据此改代码）：P0-002、P1-001、P1-003、P1-005、P2-001、P2-002、A1、A2 — 共 8 项。
**改进建议（非缺陷，未来排期）**：P1-002、P1-004、P2-003 — 共 3 项。
**D 类文档清理（低优先）**：D1-D8 — 共 8 项。

> 本节为最终修复状态。原 R036 报告全文保留，便于追溯。

---

## 审核员独立核实结论（2026-07-23 · 逐条比对代码）

> 本节为在子代理 R036 报告基础上，由审核员对 **每一项** 重新对照 `main/*.cpp` 源码逐行核实后的结论。
> 结论分为：**真实**（确属缺陷）、**不实**（误读，代码已正确）、**改进建议**（非缺陷，仅健壮/封装层面）。

### 核实总览

| 编号 | 子代理判定 | 审核员核实 | 说明 |
|------|-----------|-----------|------|
| P0-001 | P0 真实 | **真实** | 第三次 register 失败清理 deinit 了跨曲目复用的 g_i2s_writer，与其他路径不一致 |
| P0-002 | P0 真实 | **不实（误读）** | REWIND 分支 `speed<0` 走 else，`sample_rate=AUDIO_SAMPLE_RATE` 不变调，设计一致 |
| P1-001 | P1 真实 | **不实（误读）** | `audio_player_play()` L237-239 已重置采样率，下一曲不会残留 FF 倍率 |
| P1-002 | P1 真实 | **改进建议** | 两维度判定当前同步，无 bug，仅建议统一接口 |
| P1-003 | P1 真实 | **不实（误读）** | `g_key_locked` 在 main.cpp L233 已 gate 全部按键，LOCKED 下 PREV/NEXT 均被忽略 |
| P1-004 | P1 真实 | **部分成立（轻微）** | FINISHED 仅清 g_is_playing，但下一曲 play() 会重置相关变量，影响有限 |
| P1-005 | P1 真实 | **不实（误读）** | REWIND 分支 `sample_rate=AUDIO_SAMPLE_RATE`，L494-496 仍会更新 g_current_sample_rate 缓存 |
| P2-001 | P2 真实 | **改进建议** | u8g2 draw/bitmap 状态未归位，属健壮性，非 bug |
| P2-002 | P2 真实 | **改进建议** | 相对间隔 deadline，跨 tick 边界可能漂移，建议绝对时间戳 |
| P2-003 | P2 真实 | **改进建议（需实测）** | decoder byte_pos=0 副作用未在真实硬件验证 |
| P2-004 | P2 真实 | **真实（极低）** | switch case 缩进不一致，纯格式 |
| P3-001 | P3 真实 | **真实** | 注释中数组名 `g_accel_gears` 应为 `g_speed_steps` |
| P3-002 | P3 真实 | **真实** | `bookmark_t::label[16]` 字段从未写入 |
| P3-003 | P3 真实 | **属实** | 已确认无未用 include 残留 |
| A1 | 架构问题 | **不实（误读）** | 同一 NVS namespace 多 handle 由 ESP-IDF 内部锁管理，当前实现安全 |
| A2 | 架构问题 | **改进建议** | g_play_* 为模块内 static，外部经 `audio_player_get_*()` 接口读取，封装已基本到位 |
| D1-D8 | 文档问题 | **属实** | 注释/变更日志块属文档清理范畴 |

### 各条核实依据

**P0-001【真实】**
- `audio_player.cpp:208` `audio_element_deinit(g_i2s_writer);` + `:211` `g_i2s_writer = NULL;`
- 对比 run 失败清理（:247）与 link 失败清理（:224）均仅 `audio_pipeline_unregister(g_i2s_writer)`，**保留** i2s_writer。
- 又对比 `audio_player_play()` 顶部 `:188-200` 的 NULL 守卫：若 g_i2s_writer 被置 NULL，后续 play 直接拒绝注册。→ 确为过度清理，建议将 `:208` 改为 `audio_pipeline_unregister(g_i2s_writer);` 并删除 `:211`。

**P0-002【不实】**
- `audio_player.cpp:487-491`：`else`（speed<0，即 REWIND）显式 `sample_rate = AUDIO_SAMPLE_RATE;`，且注释明确说明"负采样率只会让音高失真，跳帧已能模拟快退听感"。
- 快退的"倒放"由 `audio_player_tick()` `:601-604` 的 `target_ms = cur_ms - skip_ms` 向后 seek 实现，与设计文档一致。子代理误将"跳帧路径"等同于"I2S 变速路径"。

**P1-001【不实】**
- `audio_player.cpp:237-239`：`g_current_sample_rate = AUDIO_SAMPLE_RATE; i2s_stream_set_clk(g_i2s_writer, AUDIO_SAMPLE_RATE, 16, 2);` 在每次 play() 开头执行。下一曲必然从正常速率开始，不存在 FF 倍率残留。

**P1-003【不实】**
- `main.cpp:233`：`if (g_key_locked) { 仅响应解锁的 EXTRA_LONG_PRESS }` —— 锁定态下所有 PREV/NEXT/PLAY/PAUSE 均被忽略，解锁后 `:236` 恢复 `g_state_before_lock`。不存在"进入无效状态"问题。

**P1-005【不实】**
- REWIND 分支 `:490` 设 `sample_rate = AUDIO_SAMPLE_RATE`，`:494-496` 判定 `sample_rate != g_current_sample_rate` 后更新缓存并 `i2s_stream_set_clk`。`g_current_sample_rate` 在快退时即被设为 AUDIO_SAMPLE_RATE，并非"未更新"。

**P1-004【部分成立·轻微】**
- `audio_player_tick()` `:550-557` FINISHED 后仅 `g_is_playing=false`。`g_is_paused` 未清，但下一曲 `play()` `:257-262` 会重设 `g_is_playing=true; g_is_paused=false; g_play_start_us/offset/las_scrub`；`:279-298` 重算 `g_total_duration_ms`。逻辑上无残留 bug，仅与 R035-016 的"全清零"风格不统一。建议为稳妥起见与 R035-016 对齐补清零，但非缺陷。

**A1【不实】**
- ESP-IDF 的 NVS 在同一 namespace 上允许多 handle 各自 `nvs_open`/`nvs_commit`，内部以 flash 锁串行化，settings 与 bookmark 各自 commit 互不覆盖。子代理担忧的"锁冲突"在 ESP-IDF 设计中不存在。当前实现安全，无需拆分 namespace。

**A2【改进建议】**
- `g_play_start_us / g_play_offset_us / g_total_duration_ms` 均为 `audio_player.cpp` 内 `static`，外部（如 main.cpp）通过 `audio_player_get_position_ms()` 等接口间接读取，并未直接访问全局变量。封装已到位，仅可作 naming/可读性层面的进一步收敛，非缺陷。

### 核实后处置建议

1. **需修复（4 项真实缺陷，已修复）**：**P0-001**（audio_player.cpp:208-211）、**P2-004**（main.cpp:466-469 缩进）、**P3-001**（tape_control.cpp:104 + tape_control.h:91 注释）、**P3-002**（bookmark.h:21 移除 label[16]）。
2. **改进建议（非缺陷，按排期）**：P1-002、P1-004、P2-003。
3. **误读项（已从缺陷清单剔除）**：P0-002、P1-001、P1-003、P1-005、P2-001、P2-002、A1、A2。
4. **D 类文档清理（低优先）**：D1-D8。

> 结论：**R036 经逐条核实，真正需要修复的代码缺陷共 4 项**（P0-001 + P2-004 + P3-001 + P3-002），均已修复并编译通过。其余 8 项原报告判定为误读、3 项为改进建议、8 项为 D 类文档清理。
> 修复后状态详见文档顶部「裁决总结」与末尾「修复验证」两节。
-