# CONTEXT.md — TapeBook 30 秒恢复指南

> **项目**：ESP32-S3 听书机（磁带机风格音频播放器）  
> **仓库**：`zhutao198/tapeplayer`（GitHub）  
> **本地**：`D:\zhutao\audio_player`  
> **最后更新**：2026-08-03（R046 — 方案1：长按进入变速态先补 ±5 秒基准跳退，消除"刚过长按倒退反少"断层）

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
| **显示** | `main/display.cpp` | SPI TFT ST7789（原生 esp_lcd + LVGL v9，`lv_conf.h` 配置） |
| **设置** | `main/settings.cpp` | NVS 持久化（断点续播） |
| **书签** | `main/bookmark.cpp` | NVS 持久化书签（浏览中停止长按添加） |
| **电源** | `main/power_mgmt.cpp` | 定时关机 / 电量检测 stub |
| **配置** | `main/config.h` | GPIO 引脚定义 |
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
| **R024** | **2026-07-17** | **`907923b`** | **Claude 对照审核 R023（用户 + Claude 独立审核合并）** | **✅** |
| **R025** | **2026-07-17** | **`22f1540`** | **完整全项目代码审计（26 文件全覆盖）** | **✅** |
| **R026** | **2026-07-17** | **`110c238`** | **R025 全项目审计合并到 docs/CODE_REVIEW_R023.md（阶段三）** | **✅** |
| **R027** | **2026-07-17** | **`3242c60`** | **团队评审反馈修正：M4/M5 撤回** | **✅** |
| **R028** | **2026-07-17** | **`e64c6d6`** | **R025 P0/P1/L1/L6 修复（4 项 + build 通过）** | **✅** |
| **R029** | **2026-07-17** | **`f15ec83`** | **H1 显示一致性微调（用户评审建议）** | **✅** |
| **R030** | **2026-07-20** | **`8e8490f`** | **批量修复合并评审 17 项（S1+S2+S3+C08+C02+C07）** | **✅** |
| **R031** | **2026-07-29** | **—** | **ADF 选板迁移：项目内 `components/audio_board` 覆盖 ADF 组件（tapebook 桩板，`CONFIG_ESP_TAPEBOOK_BOARD=y`），移除 `tapebook_board`/`u8g2_esp32_hal` 旧组件** | **✅** |
| **R032** | **2026-07-29** | **—** | **显示层迁移 Phase 1：自写 ST7789 驱动 + u8g2 → 原生 esp_lcd(`esp_lcd_panel_st7789`, SPI3) + LVGL 9.5.0；新增 `DISPLAY_ORIENTATION` 方向开关与 `main/lv_conf.h`；构建通过，待真机验证** | **✅** |
| **R033** | **2026-07-29** | **—** | **UI 重写（Phase 2）完成：主播放/浏览/提示三界面全部 LVGL widget 实现；`display.cpp` 重写 `ui_create`/`display_update`/`display_show_browse`，修复状态栏 `%%` bug，新增 `[SEQ/ALL/ONE]` 模式显示、长文件名循环滚动、进度条百分比、磁带卷轴装饰、30s 无操作降亮度屏保；`display_set_play_mode()` 由 `cycle_play_mode()` 与初始化加载同步** | **✅** |
| **R034** | **2026-07-29** | **—** | **UI 三点改进（全中文 / 磁带动画 / 按键便捷）：① 新增中文子集字体 `main/ui_font_12/14/16.c`（`tools/gen_font.py` 用 Windows 黑体经 `lv_font_conv` 生成），`lv_conf.h` 以 `LV_FONT_CUSTOM_DECLARE` 挂载、`LV_FONT_DEFAULT=chinese_14`；全 UI 文案中文化（状态词/模式/按键/提示/浏览/启动/无卡/无文件）② 磁带卷轴加偏心标记 + `reel_anim_cb` 定时器按状态旋转（播放正向匀速 / 快进快速正向 / 快退反向 / 暂停停止锁定静止）③ 底部按键按物理分组 `<播放控制>`｜`磁带控制` 且当前主操作高亮，浏览页附导航提示。`docs/ui_preview.html`+`ui_preview.png` 已刷新为 10 个全中文界面** | **✅** |
| **R035** | **2026-07-29** | **—** | **UI 三项优化（依据用户建议）：① 修正快退动画速度——HTML 预览 `.reel.rw` 由 `3s reverse` 改为 `0.6s reverse`，与快进同速（设备端 `reel_anim_cb` 中 FF/RW 本就对称 `±200*(1+gear)`）② 电量/音量改为**图形**——状态栏右侧新增 `batt_frame/batt_fill/batt_charge`（外框+填充，<20% 变红，充电显「充」）与 `vol_bars[4]`（4 格竖条随音量点亮），移除原文字百分比；快进/快退时新增居中醒目 `lbl_seek` 读秒「快进/快退 N.Nx → mm:ss」③ 调研磁带单放机布局后，**按键提示按物理位置左→右重排**：`快退 播放 快进 停止 ｜ 上一首 下一首`（走带四键居左、选曲两键在右），并新增第 ⑪ 屏「按键位置参考」图（组合键以固件为准：播放长按=切换模式 / 停止长按=进入浏览 / 浏览中停止长按=加书签；后于 R036 移除锁定与双击）。`docs/ui_preview.html`+`ui_preview.png` 刷新为 11 个界面，`README.md`/`DETAILED_DESIGN.md` §5.3.1 同步** | **✅** |
| **R036** | **2026-07-29** | **—** | **按键映射重构（按用户要求）：① 移除「按键锁定」功能——删除枚举 `APP_STATE_LOCKED`、全局 `g_key_locked`/`g_state_before_lock` 及 `main.cpp` 锁定拦截块、播放超长按锁定分支、播完逻辑中的锁定判断；`display.h`/`display.cpp` 移除 `PLAYER_STATE_LOCKED` 与「已锁定」文案；HTML 第⑥屏由「已锁定」改为「音量调节」演示；`DETAILED_DESIGN.md` §7.4 改为「锁定功能已移除」说明 ② 取消不友好的双击操作——「播放双击=切换模式」改为 **播放长按=切换模式**；「停止双击=书签」改为 **浏览界面中停止长按=给选中曲加书签**；停止长按仍为进入浏览 ③ 因不再使用双击，`button_manager.cpp` 将播放/停止键 `dbl_click_en` 置 `false`，使短按即时响应 ④ 同步 `README.md`、`DETAILED_DESIGN.md`（§5.3.1 / §7.4）、`CONTEXT.md`（R035 组合键描述亦修正），`docs/ui_preview.html`+`ui_preview.png`（409 KB，11 屏）刷新** | **✅** |
| **R037** | **2026-07-29** | **—** | **浏览模式选曲体验增强（按用户要求）：** `main.cpp` 浏览态 `BTN_ID_PREV/NEXT` 在原有「短按上/下移一曲」基础上，新增 **`LONG_PRESS`/`HOLD`/`EXTRA_LONG_PRESS` 长按连续移动**——长按超过 500ms 后进入连续移动，间隔随按住时长加速缩短（`BROWSE_REPEAT_MS_INIT` 120ms → `BROWSE_REPEAT_MS_FAST` 50ms → `BROWSE_REPEAT_MS_MIN` 30ms，阈值 `BROWSE_HOLD_ACCEL_MS` 1500ms，见 `config.h`）；新增 `g_browse_repeat_ms` 基准时刻与 `browse_repeat_interval()` 辅助函数，松开 `RELEASE` 复位。非浏览态 PREV/NEXT 长按仍为音量 −/+，二者不冲突（浏览态 `continue` 不落入主 switch）。同步 `README.md`、`DETAILED_DESIGN.md`（§5.3.1）按键映射说明，明确「浏览内长按=连续移曲、非浏览长按=音量」。** | **✅** |

| **R038** | **2026-07-30** | **—** | **浏览模式快退/快进赋予功能 + 按键名按功能显示（按用户要求）：** `main.cpp` 浏览态新增 `BTN_ID_REWIND`/`BTN_ID_FAST_FORWARD` 处理——短按 = 上/下翻页（跳 `BROWSE_PAGE_STEP`=6 曲，钳制在 [0,total-1]），长按/`HOLD`/`EXTRA_LONG` = 跳到列表头/尾；`config.h` 新增 `BROWSE_PAGE_STEP`。同步固件 `display.cpp` 浏览提示文案（上翻页/下翻页/确认/退出）。`docs/ui_preview.html` 浏览屏（⑦）按键名改为功能名（快退→上翻页、播放→确认、快进→下翻页、停止→退出），导航提示更新；⑪ 参考补充浏览态翻页说明；`README.md`、`DETAILED_DESIGN.md` 浏览映射同步。** | **✅** |
| **R039** | **2026-07-30** | **—** | **停止键改为「安全停止/续播」+ 新增「快退+停止」组合键（按用户需求）：** 学习英语场景需一句话反复听，误触停止会打乱节奏、难找回位置。改动：① `stop_playback()` 在销毁解码管道前把当前位置读入 `g_seek_on_play_position` 缓存；② `STOPPED→播放` 去掉 `g_seek_on_play_position = 0`，改为沿用缓存位置续播——同时修复了此前该清零会把**上电/唤醒 NVS 断点一并清零**的隐患，使停止/暂停/关机/唤醒四者统一为续播模型（磁带机式体验）；③ 新增组合键 **快退+停止**（250ms `COMBO_WINDOW_US` 窗口内同按/先后按）→ `jump_to_track_start()` 跳到当前曲首（从头重听），单独按键零延迟、未命中安全降级。按键数量与位置不变。`README.md`、`DETAILED_DESIGN.md` §2.3 同步。** | **✅** |
| **R040** | **2026-07-30** | **—** | **FF/RW 态按键互锁 + 组合键时间戳清理（按用户需求，明确磁带机模型）：** 快进/快退为「按住态」，用户确认其间其他键应忽略、继续走带，松手才恢复正常续播。改动：① `PREV`/`NEXT` 短按在 `APP_STATE_FAST_FORWARD`/`APP_STATE_REWIND` 态直接 `break`，不再静默改 `g_current_track`/`playlist_set_index` 导致「索引变、声音未变」的状态错位；`PLAY` 本就无对应分支，自然忽略——三者与「按住期间不响应」模型完全自洽；② FF/RW **进入与退出**均清零 `g_combo_rew_us`/`g_combo_stop_us`，杜绝变速态残留时间戳在 250ms 窗口内误触 `REW+STOP` 组合键。按键数量与位置不变。`README.md`、`DETAILED_DESIGN.md` 同步。** | **✅** |
| **R042** | **2026-08-03** | **—** | **新增 GPIO0/GPIO3 专用音量+/-键（LCK-TG001A-G1 拨轮开关）：** 按硬件组合开关：公共端 `a` 接 GND、`b`/`d` 各经 10kΩ 上拉到 3.3V；下按 `a-c` 为纯机械电源开关（不进 GPIO），左拨 `a-b` = 音量-，右拨 `a-d` = 音量+。改动：① `main/config.h` 新增 `BTN_VOL_DOWN=GPIO_NUM_0`、`BTN_VOL_UP=GPIO_NUM_3`，并说明 GPIO0/GPIO3 为 Strapping 引脚、10kΩ 上拉保证上电高电平不破坏 SPI Boot / 默认 JTAG 信号源；② `main/button_manager.{h,cpp}` 枚举与配置表增 `BTN_ID_VOL_DOWN`/`BTN_ID_VOL_UP`（沿用 `dbl_click_en=false`，短按 0ms 响应）；③ `main/main.cpp` `handle_button_events()` 新增两 case：SHORT_PRESS = `audio_player_set_volume(vol±1)`、RELEASE = `settings_save_volume`、`LONG_PRESS`/`HOLD` 兜底连续 ±（复用 `g_vol_hold_counter`，每 100ms/级）。LCK 自复位 + 按下/拨动机械互锁，开机时无长按风险；**同步移除** `PREV`/`NEXT` 长按/持续按住/释放上的音量调节逻辑——音量专用键已接管，避免双路径冲突。** | **✅** |
| **R043** | **2026-08-03** | **—** | **长按全检修复（Major-1 / Minor-2 / Minor-3）：** ① **Major-1（FF/REW 重复 press）**：原 `LONG_PRESS || HOLD` 合并条件会在进入变速态后每 20ms 的 `HOLD` 事件都调一次 `tape_control_ff_press()`/`tape_control_rewind_press()`（设计上 press 只需进入时一次，档位升档由 `tape_control_tick()` 自动完成）。修复：`LONG_PRESS` 分支负责「进入变速态 + 调一次 press + 置 `APP_STATE_FAST_FORWARD/REWIND`」；新增 `HOLD`/`EXTRA_LONG_PRESS` 分支仅做「保持态速度同步」（`audio_player_set_speed(tape_control_get_speed())`），不再重复 press。② **Minor-2（VOL 计数器串扰）**：原全局 `g_vol_hold_counter` 被 VOL-/VOL+ 共用，跨键切换时残留计数会错级。拆分为 `g_vol_down_counter`/`g_vol_up_counter` 两个独立变量。③ **Minor-3（EXTRA_LONG 覆盖）**：VOL± 分支的条件补上 `BTN_EVENT_EXTRA_LONG_PRESS`，避免超长按事件在 HOLD 分支里被静默丢弃。`main/main.cpp` 已改，编译通过（`0x10c960`，48% free）。 | **✅** |
| **R044** | **2026-08-03** | **—** | **变速档位由 4 档改为 3 档（2x/4x/8x）：** 原 1.5x/2.0x/3.0x/8.0x 四档中 1.5~3x 偏慢（真实磁带倒带为 4~8x），且慢档过多。按用户选择（方案 B）改为 3 档：① `config.h` 的 `TAPE_SPEED_1/2/3` 改为 `2.0f/4.0f/8.0f`，删除 `TAPE_SPEED_4`；② 时序阈值 `TAPE_ACCEL_STEP1/2/3_MS` 改为 `1000/6000/9000`（递进：按住 1s 进 2x、再 5s 进 4x、再 3s 进 8x，累计 1/6/9s），删除 `TAPE_ACCEL_STEP4_MS`；③ `tape_control.cpp` 的 `g_speed_steps[]` 由 4 项减为 3 项；④ `tape_control.h` 头部档位说明同步。2x/4x 走 I2S 变调快放（4x 正好到采样率上限），8x 为跳帧模式（`tape_control_is_scrub_mode()` 自动判定最高档）。编译通过（`0x10c950`，48% free）。 | **✅** |
| **R045** | **2026-08-03** | **—** | **短按阈值 800ms + 进入变速态即 2x + 短按跳 5 秒：** ① `config.h` 的 `BTN_LONG_PRESS_MS` 由 500→**800**（所有按键短按/长按分界；≤800ms 松开=短按，≥800ms=长按进变速态）；② 变速态去掉 1x 缓冲：`TAPE_ACCEL_STEP1_MS` 由 1000→**0**（进入变速态瞬间直接 2x），`STEP2/3_MS` 改为 **5000/8000**（累计 5s 进 4x、8s 进 8x）；`tape_control.cpp` 的 `enter_mode()` 初速由 `TAPE_SPEED_NORMAL`(1x) 改为 `TAPE_SPEED_1`(2x)，进即 2x；③ `main.cpp` 快进 `SHORT_PRESS` 由 `skip_seconds(10)`→`skip_seconds(5)`、快退由 `skip_seconds(-10)`→`skip_seconds(-5)`（短按微调跳 5 秒）。判定链路：≤800ms 松开=短按跳 5 秒（不打变速）；≥800ms 触发 LONG_PRESS 才进变速态（直接 2x，再按累计时长升 4x/8x）。编译通过（`0x10c950`，48% free）。 | **✅** |
| **R046** | **2026-08-03** | **—** | **修复"刚过长按倒退反少于短按"的断层（方案1）：** R045 后短按/长按为互斥分支——799ms 短按走 `skip_seconds(±5)` 跳 5 秒，801ms 长按走变速态却**不继承** ±5 秒、仅 2x 倒带 1ms（≈2ms），导致"按越久倒越少"的反直觉断层。修复：`main.cpp` 快进/快退 `LONG_PRESS` 进入变速态时**先**补 `skip_seconds(5)` / `skip_seconds(-5)`（继承短按基准跳退），再 `tape_control_*_press()` 开始 2x 变速走带。现 801ms 长按 = 5 秒 + 2x×1ms，与 799ms 短按平滑衔接；继续按住即在 5 秒基础上叠加 2x/4x/8x 倒带（按越久倒越多）。`HOLD` 分支不重复调 skip（已在 LONG_PRESS 调一次）。编译通过（`0x10c950`，48% free）。 | **✅** |
> 详细变更见 `开发日志.md`，回滚命令：`git checkout <tag>`

---

## 5. 关键决策速查

| 决策 | 选择 | 理由 | 文档 |
|---|---|---|---|
| 主控 | ESP32-S3-WROOM-1 N16R8（量产） | BOM 低、Octal PSRAM 8MB 够用、3.3V VDD_SPI 简化 PCB | HARDWARE_MODULE_MIGRATION.md |
| 音频框架 | ESP-ADF v2.7 | 多格式解码（MP3/AAC/FLAC/OGG/Opus） | README.md |
| 存储 | MicroSD SPI | 简单可靠 | README.md |
| 显示 | ST7789 2.0寸 SPI TFT 320×240（SPI3_HOST 独立总线） | 原理图 V1 定型；方向可切换（`DISPLAY_ORIENTATION`） | DESIGN.md §5.6 |
| 断点续播 | NVS 命名空间 `tapebook` | 不占 SD 写寿命 | DETAILED_DESIGN.md |
| 音量控制 | I2S ALC (i2s_alc_volume_set) | ADF 内置 ALC 音量，-96~+12dB 范围 | audio_player.cpp |
| 显示驱动 | 原生 esp_lcd（`esp_lcd_panel_st7789`，IDF v5.5.3 内置）+ LVGL v9 | 零第三方 ST7789 库；`draw_bitmap` 直接作 LVGL flush_cb；u8g2/自写 SPI 驱动废弃 | docs/PLAN_LVGL_ESP_LCD_MIGRATION.md |
| ADF 选板 | 项目内 `components/audio_board` 覆盖 ADF 组件（tapebook 桩板） | 不依赖现成开发板、方案随仓库版本化 | docs/PLAN_TAPEBOOK_ADF_BOARD.md |
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
