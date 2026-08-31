# CONTEXT.md — TapeBook 30 秒恢复指南

> **项目**：ESP32-S3 听书机（磁带机风格音频播放器）  
> **仓库**：`zhutao198/tapeplayer`（GitHub）  
> **本地**：`D:\zhutao\audio_player`  
> **最后更新**：2026-08-26（**🏁 里程碑 v1.0-stable（基于 R094）**：libhelix 根治 PV-MP3 崩溃 + 坏帧跳曲保护 + I2S 时钟按文件真实采样率设置（低采样率 24000Hz 不再变快）+ R084 修栈溢出 + R085 进度条/计时器重叠 + R086 seek rb 重置 + R087 pause/resume 跳曲 + R089 回退 R088 + R090 屏蔽自动 light-sleep（屏幕常亮）+ R091 音量 decoder 软件缩放（弃 ALC）+ R092/R093 音量曲线（线性 gain=level/14）+ R094 修复 FF/REW 跳曲（seek 清 decoder 残留输入缓冲）+ SD 误报移除（健康检查重试+连续失败阈值）。SD 热拔插为已知待办）

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
| **蓝牙音箱** | `main/bt_speaker.cpp/.h` | A2DP Sink（手机推流到设备出声），菜单「蓝牙音箱」入口 |
| **字体分区** | `main/font_partition.cpp/.h` | 中文 TTF 烧录分区 + freetype 初始化（LVGL 中文渲染） |
| **统一菜单** | `main/menu.cpp/.h` | 设置/功能统一入口（A-B 复读/按键音/蓝牙/OTA 等） |
| **MP3 解码** | `main/mp3_decoder_libhelix.c/.h` | R080 起 .mp3 主解码器：Helix MP3(libhelix)，绕开闭源 PV-MP3；坏帧跳曲保护 |
| **TF 卡 OTA** | `main/ota_sd.cpp/.h` | 从 SD 卡读取固件做 OTA 升级 |
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

### R049–R076 阶段（功能扩展 + 崩溃攻坚，2026-08-20 ~ 08-24）

| R 节点 | 日期 | commit | 内容 | 状态 |
|---|---|---|---|---|
| R049–R051 | 2026-08-20 | `02db091` `4ded1fb` `2ee61b9` | 统一设置菜单框架 + A-B 复读 + 按键提示音 + OTA/USB 入口 + **TF 卡(SD) 固件 OTA 升级** | ✅ |
| R052/55/56/58 | 2026-08-20 | `4c11716` | TWDT / 屏保 / 音量键 / 按键响应修复 | ✅ |
| R059-stage-end | 2026-08-20 | — | 阶段收尾基线（后续 fix-r053-audio 分支起点） | ✅ |
| R061 | 2026-08-21 | — | 给 IDF 打 `idf_v5.5_freertos.patch`，让 ADF MP3 decoder 的 `xTaskCreateRestrictedPinnedToCore` 编入（否则无声音） | ✅ |
| R062 | 2026-08-21 | — | 播放无限重启修复（`audio_element_reset_state` 拉回 i2s）+ 关疯狂 DBG | ✅ |
| R063 | 2026-08-21 | — | P0 LVGL 死锁修复（main 设标志 + lvgl_task 异步消费，禁 main 直接调 LVGL）+ 无声音 H2 偏置诊断（后推翻） | ✅ |
| R064 | 2026-08-21 | — | SD CRC 触发规律：长按 seek 几次后必现（非偶发） | ✅ |
| R065 | 2026-08-21 | `59e746f` | **首次开机无声根因**：`get_i2s_pins()` 返回 -1 被 ADF memcpy 覆盖 → IO6/7/5 从未配成 I2S → 修复返回真实引脚 | ✅ |
| R066 | 2026-08-21 | — | 暂停切歌野指针崩溃（R062 跨曲目复用 g_i2s_writer 引入）→ resume→stop 修复 | ✅ |
| R067 | 2026-08-21 | `3ee8208` | 应用层 ID3v2 skip（heldec 混合流崩）+ seek 公式修正 | ✅ |
| R068 | 2026-08-21 | `3ee8208` | stop 改用 `terminate` + i2s_writer 每次重建（放弃跨曲目复用） | ✅ |
| R069–R071 | 2026-08-22 | — | 崩因定位在 ESP-ADF 静态库 MP3 decoder（项目代码无法修）；多种修复尝试失败；保留 R067+R068+R071(display 噪声清理) | ✅ |
| R072 | 2026-08-22 | `3ee8208` | 清理无效修复（回退 R066/R070），保留 R067+R068+R071 | ✅ |
| R073 | 2026-08-22 | `3ee8208` | splash 卡住 UX 修复（boot 后强制 tick 进 player 界面） | ✅ |
| R074 | 2026-08-22 | — | 切歌黑屏（实为崩溃重启表象，非纯 UX） | 🚧 |
| R075 | 2026-08-22 | `dd3e93e` | **double-free 根因**：stop 手动 deinit element + `audio_pipeline_deinit` 二次 deinit → 改只调一次 pipeline_deinit | ✅ |
| R076 | 2026-08-22~24 | `c53662e` | **MP3 解码器崩溃根除**：coredump 反解确认 PV-MP3 闭源库 `mp3_decoder_open` 崩 → 切换开源 `esp_audio_codec`（`esp_mp3_dec`/`simple_dec`，mpeg_parser 自动切帧绕开异常帧）；同时引入蓝牙音箱(A2DP Sink) + 中文 TTF 字体分区(freetype) + 统一菜单 | 🚧 |
| R078 | 2026-08-25 | `5f109df` | 删 `decoder_event_cb`（误把 i2s_writer 当 rsp_filter 句柄踩内存）+ 删 `g_rsp_filter` 死链（崩未根除，仅清理 UB 死链） | ✅ |
| R079 | 2026-08-25 | `d9d428f` | .mp3 回退 PV-MP3 恢复声音；栈 internal 32K+16K rb 消噪音；崩溃根因后证为 PV-MP3 对特定合法 MP3 确定性崩溃（转码 128k/320k 均复现），转码不能根治 → 引 R080 | ✅ |
| R080 | 2026-08-25 | `e234fc5` | **根治 MP3 崩溃**：.mp3 解码器换 Helix MP3(libhelix, Apache-2.0)，彻底绕开闭源 PV-MP3；坏帧/连续错误>50→返回 AEL_IO_DONE 触发 audio_player_tick 自动跳下一首（跳曲保护）| ✅ |
| R081 | 2026-08-25 | `2eb0d94` | **修复首帧误报采样率**：解码器由"首帧上报一次"改为"每帧比对采样率/声道/位宽变化才上报"，纠正 Helix 首帧误报高采样率 | ✅ |
| R082 | 2026-08-25 | `ebf5fcb` | **修复低采样率 MP3 变快变尖（理论误判）**：改解码器逐帧解析 MPEG 帧头上报 i2s；真机验证无效——证明 i2s 运行时忽略解码器上报，根因在 play() 硬锁时钟 → 引 R083 | ⚠️ |
| R083 | 2026-08-26 | `ab26ae5` | **真因修复**：根因是 `play()` 用固定 `AUDIO_SAMPLE_RATE`(48000) 硬锁 I2S 时钟，`i2s_stream.c` 无 REPORT_MUSIC_INFO 回调故解码器上报无效；新增 `mp3_sniff_sample_rate` 在 play() 嗅探文件真实采样率并据此设 I2S 时钟（speed 倍率改以文件基准速率计） | ✅ |
| R084 | 2026-08-26 | `948b9b9` | **修复 R083 栈溢出崩溃**：`mp3_sniff_sample_rate` 栈缓冲 8192→1024，消除 main 任务栈溢出（R083 后播放任意歌曲即崩） | ✅ |
| R085 | 2026-08-26 | `b2eeb9b` | **修复 FF/REW 跳曲 + 进度条不匹配 + 计时器与转轮重叠（两轮）**：① 根因=Helix `err_cnt` 只增不减→跨曲累积>50 误判曲终跳下一首，成功帧复位 `err_cnt`；② seek 落点 `mp3_frame_align` 帧对齐；③ 进度条改真实码率算时长；④ `display.cpp` 计时标签 y=122→130 | ⏳ |
| R086 | 2026-08-26 | `3c75e83` | **修复 FF/REW seek 后跳曲（残留 done 标志层）**：ADF `pause→resume` 重开元素不清 ringbuffer done 标志；seek 路径新增 `audio_player_pause_seek_resume` 在 resume 前 `audio_element_reset_input/output_ringbuf(g_decoder)` 清两端 done 标志 | ✅(部分) |
| R087 | 2026-08-26 | `0ae3ef1` | **真正根因修复 pause/resume（及 seek）一恢复即跳曲**：`mp3_decoder_libhelix.c` 在 UNDERFLOW(`out_total==0`) 时返回 `AEL_IO_OK`(0)，ADF `audio_element_process_running` 将 `AEL_IO_OK` 与 `AEL_IO_DONE` 同等对待→立即 set_ringbuf_done+finish 误判曲终；改为返回 `AEL_IO_TIMEOUT`（稍后再试），曲终仍走 eos 的 `AEL_IO_DONE`；`audio_player_resume` 同样加 rb 重置双保险 | ✅ |
| R088 | 2026-08-26 | `a021cf2`+`18da374` | **修复 FF/REW（及恢复）非帧边界落点连续坏帧误判曲终**：真因=落点非帧边界时 Helix 撞大量假同步字(`0xFFE`)，`MP3FindSyncWord` 被带偏，`err_cnt` 单个缓冲内爆表>50 误触跳曲保护。`audio_player.cpp` 新增 `mp3_valid_frame_header` + 重写 `mp3_frame_align`(32KB 扫合法真帧) + resume 也帧对齐；`mp3_decoder_libhelix.c` 坏帧重同步改扫**合法帧头**、找到真帧即 `err_cnt=0` 续播 | ❌(回归,WDT) |
| R089 | 2026-08-26 | `b1fe453` | **回退 R088**：decoder 坏帧重同步 `mp3_find_valid_sync` 在数据头通过简单校验但 Helix 拒绝时 `off==0` 原地死循环 → decoder 任务不 yield → WDT 播放回归；`git checkout 0ae3ef1` 完全回退到 R087。恢复可播放 | ✅ |
| R090 | 2026-08-26 | `3a6d3e6` | **屏蔽自动 light-sleep 息屏**：`power_mgmt_should_sleep()` 恒 false（屏幕常亮）。音量 ALC 尝试因崩溃由 R091 推翻 | ✅(屏幕) |
| R091 | 2026-08-26 | `52f9c70` | **音量改 decoder 软件 PCM 缩放**：i2s ALC（use_alc=true）在 IDF5.x 下 `alc_volume_setup_process` BREAK 崩溃（PC 0x403743c0）→ 弃用；decoder 新增 `g_vol_gain_q15`+`mp3_decoder_set_volume`+ 输出前 Q15 缩放钳位；`apply_volume_alc` 改调 decoder setter；`use_alc` 恢复 false | ✅ |
| R092 | 2026-08-26 | `b37bdc4` | **音量曲线 0..-50dB**：R091 的 0..-96dB 低档静音、高档步进大(6.9dB)；改 0..-50dB 恒定 3.6dB 步进 | ❌(最低三档仍无声) |
| R093 | 2026-08-26 | `697c92f` | **音量改线性增益 gain=level/14**：最低三档可闻(1档≈-23dB)、高档步进小，全量程逐档线性可辨 | ✅(用户满意) |
| R094 | 2026-08-26 | `e50e09e` | **修复 FF/REW 偶发跳曲**（seek 前清 decoder 残留输入缓冲）+ **SD 误报移除**（健康检查重试+连续失败阈值） | ⏳ |

> **🏁 里程碑 `v1.0-stable`（2026-08-26，基于 R094）**：libhelix 根治 PV-MP3 崩溃 + 坏帧跳曲保护 + I2S 时钟按文件真实采样率设置 + R084 修栈溢出 + R085 进度条/计时器重叠 + R086 seek rb 重置 + R087 pause/resume 跳曲 + R089 回退 R088 + R090 屏蔽 light-sleep（屏幕常亮）+ R091 音量 decoder 软件缩放（弃 ALC）+ R092/R093 音量曲线（线性 gain=level/14）+ R094 修复 FF/REW 跳曲（seek 清 decoder 残留缓冲）+ SD 误报移除（健康检查重试）。SD 热拔插为已知待办。回滚：`git checkout v1.0-stable` / `git checkout R094`。

> 注：R061–R075 多节点在开发日志中详细记录，但**历史未全部建 tag**（仅 R030/R048/R059-stage-end/R076-* 有 tag）；R076 系列已建 `R076-CODEC-*` annotated tag。
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
| 蓝牙音箱 | A2DP Sink（Bluedroid + BT Classic）| 用户要"手机推流到设备出声"，A2DP Sink 最直接；ESP32-S3 仅 BLE 但 Bluedroid 支持 A2DP Sink | bt_speaker.cpp / BT_SPEAKER_FEASIBILITY.md |
| MP3 解码器 | **libhelix（chmorgan/esp-libhelix-mp3, Apache-2.0）**替换闭源 PV-MP3（esp_audio_codec 为备选 wrapper `mp3_decoder_esp_codec.c`）| PV-MP3 闭源黑盒，对特定合法 MP3 确定性崩溃@0x403743c0（转码 128k/320k 均复现），无法 patch；libhelix 坏帧返回负码不崩 | mp3_decoder_libhelix.c |
| 中文显示 | ST7789 + LVGL + freetype + TTF 字体分区 | 中文字库烧录到独立 font 分区，freetype 动态渲染 | font_partition.cpp |
| 固件升级 | TF 卡(SD) OTA | 量产免联机，用户插卡即升 | ota_sd.cpp |

---

## 6. 服务 / 工具信息

| 项 | 值 |
|---|---|
| 串口（烧录） | `COM7`（**手动进下载模式**，esptool 直写，**禁用 `idf.py flash`**）|
| ESP-IDF 版本 | v5.5.3（实际） |
| ESP-ADF 版本 | v2.7 |
| 模组切换脚本 | `configure.bat wroom-1-n16r8` / `configure.bat wroom-2-n32r16v` |
| 构建脚本 | `build.bat build`（PowerShell 需 `cmd /c` 包裹）|
| 烧录文档 | `docs/BUILD_FLASH.md`（esptool 直写完整命令）|
| 构建运行器 | `tools/_run_in_clean_cmd.py` | 干净环境 build/flash/coredump（剥离 MSYSTEM，避免 cmd/c 误判"用户取消"）|
| 蓝牙构建 | `configure.bat wroom-1-n16r8-bt` | 注入 `CONFIG_USE_BT_SPEAKER=y` + 关 Wi-Fi |
| 字体烧录 | `flash_font.bat COM7` | font 分区 @0x620000 烧录 cjk.ttf |
| coredump 抓取 | `tools/_run_in_clean_cmd.py coredump_save` | esptool 直读 coredump 分区（RTS 未接，须 no_reset）|
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


---

## R095 里程碑（2026-08-28）
- 修复"按播放即崩"Cache error（根因：seek 落点给 Helix 解析出巨大 nSlots -> mp3dec.c:380 超大 memcpy 越界，并非并发 Flash 操作）。
- FF/REW 全面重构：严格帧头校验 + decoder 自对齐；FF/REW 静音、进度不计播放流逝（g_scrub_active），skip=速度x实际流逝时间（线性自校正）；释放时一次暂停式 seek 物理跳转。
- 音量持久化：settings_save_volume 立即 nvs_commit + 每次调整即存。
- 待办：一曲播完自动播下一首异常（见开发日志 R095 待办）。


## R096 里程碑（2026-08-28）
- 修复恢复点越界导致"播放即结束"（seek/保存断点钳制到曲长）。
- 修复进度条时长不准（MP3 bitrate 嗅探 layer 索引反了，128kbps 读成 352kbps）。
- 进度条/恢复点/FF/REW/音量/模式切换均经实测验证正常。
