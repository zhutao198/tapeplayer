# TapeBook 开发状态对照 (vs PRD V1.x)

> 本文档对照 PRD (V1.0/V1.1/V1.2/V2.0) 跟踪每个功能模块的实现状态。
> 最后更新：2026-08-12（R047 评审基线 + R048 已提交 + R049 统一菜单：R049a 框架 / R049b A-B 复读 / R049c 按键提示音 + OTA(SD卡固件升级) 均已完成并通过编译）

## 速查图

```
PRD V1.0 MVP    ██████████  完成 (R047: 11 模块齐备 + 全量代码评审通过; 仅"按键锁定"被主动移除)
PRD V1.1 增强   ██████████  完成 (屏幕保护✅ / OGG-Opus✅ / 定时关机✅ / A-B✅ / 提示音✅ / EC11 用专用音量键替代)
PRD V1.2 进阶   ██░░░░░░░░  部分 (书签✅ / 电量stub / 语音stub / 菜单部分 / OTA(SD)✅ / 低电告警✗)
PRD V2.0 扩展   ░░░░░░░░░░  规划 (蓝牙方案已出 BT_AUDIO_PLAN.md; 录音/EQ/USB 未实现)
```

---

## V1.0 — MVP（最小可行产品）

| 功能 | PRD 章节 | 状态 | 实现位置 | 备注 |
|---|---|---|---|---|
| 播放/暂停 | FR-001 4.2.1 | ✅ 完成 | main.cpp + button_manager.cpp | |
| 停止（安全停止/续播）| FR-001 4.2.2 | ✅ 完成 | main.cpp stop_playback | **R039 改为续播模型**：停止不清零、位置缓存续播（与暂停/关机/唤醒统一），不再"位置归零" |
| 上一首/下一首 | FR-001 4.2.3 | ✅ 完成 | main.cpp handle_button_events | 循环；停止态仅切换不自动播 |
| 音量调节（专用键 + 15 档逻辑音量）| FR-001 4.2.4 | ✅ 完成 | main.cpp + audio_player.cpp | **R042** GPIO0/GPIO3 专用音量键；**R048(WIP)** 由 0-100 改为 15 档逻辑音量（dB 线性 -96..+12） |
| 磁带机快进（3 档 2x/4x/8x）| FR-002 4.2.5 | ✅ 完成 | tape_control.cpp | **R044** 由 4 档改 3 档；**R045** 进态即 2x；**R046** 进态先补 ±5s 基准跳退 |
| 磁带机快退（3 档 2x/4x/8x）| FR-002 4.2.6 | ✅ 完成 | tape_control.cpp | 同快进，反向 |
| 跳帧（≥8x scrub）| FR-002 4.2.7 | ✅ 完成 | audio_player.cpp | 最高档 8x 跳帧 |
| 断点续播 | FR-003 4.2.8 | ✅ 完成 | settings.cpp | NVS 命名空间 `tapebook`，每文件独立记忆 |
| 播放列表自动扫描 | FR-004 | ✅ 完成 | playlist.cpp | 递归扫描 SD 卡音频 |
| 文件夹浏览 | FR-004 | ✅ 完成 | main.cpp + display.cpp | **R012** 基础浏览；**R037** 长按连续移曲；**R038** FF/RW 翻页+跳头尾 |
| 播放模式（顺序/单曲/全部）| FR-005 | ✅ 完成 | main.cpp + settings.cpp | 播放长按切换（R036 由双击改长按）|
| 按键锁定（超长按 3s）| FR-009 | ❌ 已移除 | — | **R036** 按用户要求主动移除 |
| 快跳（短按 ±5s）| FR-010 | ✅ 完成 | main.cpp skip_seconds | **R045/R046** 短按 ±5s（非 PRD 原 ±10s），与长按变速平滑衔接 |
| 显示（ST7789 LVGL 全中文 UI）| FR-006 | ✅ 完成 | display.cpp + lv_conf.h | **R032** 原生 esp_lcd + LVGL9 迁移（替代 u8g2 OLED）；**R033/R034/R035** 全中文 + 磁带动画 + 图形电量/音量 + 快进快退读秒 |
| SD 卡 FATFS | — | ✅ 完成 | main.cpp mount_sd_card | 自动挂载 + 热插拔检测（R048 WIP 增强 SD_CD 极性 + 状态栏图标）|
| 音频播放（ESP-ADF）| 核心 | ✅ 完成 | audio_player.cpp | **R031** 项目内 `components/audio_board` 覆盖 ADF 选板，已启用 |
| **总进度** | | **14/15（功能项）** | | 仅"按键锁定"被主动移除；其余 V1.0 功能均实现 |

---

## V1.1 — 体验增强（P2）

| 功能 | 状态 | 备注 |
|---|---|---|
| 定时关机（15/30/60/90 分钟）| ✅ 完成 | **R049a** 已补菜单 UI 入口（`系统`→`播放`→`定时关机` toggle）并真正"武装"引擎（设置即落盘 NVS + 启动倒计时）；之前仅缺 UI 入口，引擎早已就绪 |
| A-B 区间复读 | ✅ 完成 | **R049b** audio_player 标记 A/B 点 + tick 循环 seek；菜单 `系统`→`A-B 复读` 子菜单（标记A/标记B/复读开关/清除）|
| 屏幕保护/自动变暗 | ✅ 完成 | R033：30s 无操作降亮度 |
| 按键提示音（"滴"声）| ✅ 完成 | **R049c** 复用空闲 I2S 通道播放 60ms/880Hz PCM（raw_stream→i2s）；菜单 `系统`→`按键提示音` toggle，默认关；**仅非播放态**（菜单/浏览/停止）触发，避免打断音乐 |
| OGG/Opus 解码 | ✅ 完成 | ADF 启用后自动可用（已在 build 中包含）|
| EC11 旋转编码器（音量）| ❌ 未实现 | GPIO 预留未用；**已用专用音量键(GPIO0/3)替代**，V1.0 即满足 |
| **总进度** | **3/6**（定时关机引擎已就绪，差一个 UI 入口即可用）| |

---

## V1.2 — 进阶功能（P2）

| 功能 | 状态 | 实现位置 | 备注 |
|---|---|---|---|
| 书签管理（每文件 10 个）| ✅ 完成 | bookmark.cpp | R011 集成；浏览中停止长按添加书签 |
| 语音播报（预录 WAV）| ❌ stub | voice_prompt.cpp | 全 stub |
| 电量检测 + 显示 | ⚠️ 部分 | power_mgmt.cpp + config.h | 硬件 GPIO（CHRG_DET_IO=GPIO2, BAT_ADC 待定）已定义，软件 ADC 读取为 stub |
| 电池低电告警 | ❌ 未实现 | — | 依赖电量检测 |
| 设置菜单（OLED 导航）| ✅ 完成 | menu.cpp + display.cpp | **R049a/b/c** 统一菜单框架 + 完整树（浏览文件/播放/书签/系统）；含 A-B 复读、按键提示音、OTA/USB 入口（桩）；语音播报/EQ 为持久化 stub |
| 固件升级（OTA）| ✅ R049c 已实现 (SD 卡) | ota_sd.cpp + display.cpp + main.cpp | 分区表 `partitions_ota.csv`；菜单 `系统`→`固件升级` 走 SD-OTA 向导：扫描 `TAPEBOOK.BIN` → 版本防降级 + SHA256 清单校验 + 电量保护 → `esp_ota` 写 ota_0/1 → 启动回滚(`BOOTLOADER_APP_ROLLBACK_ENABLE`)；USB 存储仍为桩（本机无 USB OTG 固件）|
| **总进度** | **~2/6** | | |

---

## V2.0 — 扩展生态（P3 远期）

| 功能 | 状态 |
|---|---|
| LE Audio 蓝牙耳机（V1.1） | 📄 已规划 | `docs/BT_AUDIO_PLAN.md` |
| 线路输入录音 | ❌ |
| 速度微调（0.5x ~ 2.0x 不变调）| ❌ |
| EQ 均衡器 | ❌ |
| USB 大容量存储 | ❌ |

---

## 🔍 R047 全量代码评审结论（2026-08-03，基准 commit R047 v3）

- ✅ 代码与硬件设计**完全一致**（电源锁存、按键矩阵、I2S 音频、SD 卡、LCD 显示各模块）
- ✅ 无"必须立刻修"的致命 bug
- ✅ O6 已在 R047 修复
- ⚠️ **遗留建议项**：
  - **S1**：IO4 重名宏加注释（低风险，R048 已补 `SD_CD` 相关注释）
  - **S2**：GPIO0(VOL_DOWN) 上电稳定性需硬件确认（软件已加内部上拉 + 注释，待硬件验证）
  - **B2**：feed 抵消与跨模块句柄耦合重构（中等风险，留待 R048+）

---

## 📋 待补完清单（按优先级）

### P0 - 评审闭环
- [x] S1 加注释（R048 已补）
- [ ] S2 硬件确认 GPIO0 启动上拉
- [ ] B2 feed 抵消重构（R048+）

### P1 - 完善 V1.0（收尾）
- [x] 显示迁移 LVGL（R032~R035）
- [x] 音量专用键 + 15 档重构（R042/R048）
- [x] SD 检测增强 + 状态栏图标（R048 已完成）
- [ ] 真机验证（音频出声 / 变速变调 / 续航）

### P2 - V1.1
- [x] 定时关机（R049a：UI 入口 + 武装引擎）
- [x] A-B 复读（R049b）
- [x] 按键提示音（R049c：PCM 片段 + I2S）

### P3 - V1.2 / V2.0
- [ ] 电量 ADC 实际读取 + 低电告警
- [x] 设置菜单 UI（R049a/b/c：完整树 + A-B + 提示音 + OTA/USB 入口）
- [x] OTA 接收代码（SD 卡升级，R049c 已实现：SHA256 清单校验 + 版本防降级 + 启动回滚；USB 存储仍为桩）
- [ ] 蓝牙 A2DP / LE Audio
- [ ] EQ 均衡器

---

## 🎯 当前执行顺序建议

1. ✅ **已完成**：提交 R048（音量 15 档 + SD 检测 + 硬件 PCB 微调 + 评审报告 v3）并打标签推送（2026-08-12）
2. ✅ **已完成**：更新 CONTEXT.md / 开发日志.md（补齐 R031~R048 记录）
3. ✅ **已完成**：R049 统一设置菜单
   - R049a 框架：长按 STOP 入口、通用树/栈式导航、播放模式/定时关机 toggle（已编译验证）
   - R049b A-B 区间复读：audio_player 标记 A/B + tick 循环 seek，菜单 `系统→A-B 复读` 子菜单（标记A/标记B/复读开关/清除）
   - R049c 按键提示音（I2S PCM）+ 固件升级（SD 卡真实升级向导：扫描/TAPEBOOK.BIN/进度/结果）/USB 存储入口（桩）；其余 系统 项（语音播报/EQ/书签）已就位或持久化 stub
4. **真机验证**：首次上电验证音频/显示/变速（当前仅有 build + 代码评审，未真机验证；R049a 已通过 `idf.py build` 编译验证，固件可烧录）
5. **量产前**：电量检测、OTA、完整测试

---

**作者**：CodeBuddy（由 R047 评审 + working-tree 现状核对生成）  
**数据来源**：PRD.md、main/ 源代码、git log（R001~R047）、R047 全量代码评审报告

---

## R109 — 卷轴动画流畅化 + UI v2 重构（2026-09-05）

### 卷轴动画卡顿修复（已验收"动画还可以"）
- **根因排查链**：逐一排除异步 flush（回退阻塞）、双缓冲（回退单缓冲）、LVGL tick 驱动（改硬件 esp_timer @1ms）、跨任务锁竞争（flag-consumption 模式）、全屏 invalidate、电池 ADC（加 5 秒缓存）、SPI 速度（10→20MHz）、任务优先级（5→8）
- **最终方案**：卷轴动画从 LVGL 定时器移到 lvgl_task 循环，用 esp_timer_get_time() 硬件时间戳驱动，在 lv_timer_handler() 之前切帧；帧率 30fps（33ms），48 帧/64px/step 恒±1
- **已验证做不通**：异步 flush、双缓冲（均回退后仍卡）、FreeType 运行时渲染（死机）、I2S ALC 音量（崩溃）、闭源 PV-MP3 解码器（确定性崩溃）

### UI v2 重构（对齐 docs/ui_preview_v2.html）
- **P1 盒壳+布局**：PIL 生成 cassette_bg.h（296x96 RGB565 小端，浅蓝紫渐变壳+花生跑道+轮毂窗+磁带窗，已移除突兀线圈）；卷轴 48→64px（红圈+6辐条+金属轴心+螺丝孔）；全屏布局重排（磁带区 y=42~138，格式行 y=144，时间行 y=164，进度条 y=182）；隐藏 Now Playing 副标题和百分比
- **P1 修复**：盒壳 RGB565 字节序（高字节在前→低字节在前）；状态栏 label 加宽 168→214px 防换行；percent 残留"0"（设空文本）
- **P2 底部 6 键指示条**：快退/播放/快进/停止/上首/下首，纯几何图标（lv_line 闭合三角形 + lv_obj 矩形/竖线），无文字；当前状态深绿底(#0d3b1e)+亮绿图标(#4ade80)+绿边框高亮；播放/暂停图标显隐切换
- **P2 待做**：状态栏图标化（音量/循环/NOR 徽章）

### 关键参数
- 显示缓冲：40 行 partial，单缓冲阻塞 flush，max_transfer_sz=32752
- lvgl_task：优先级 8，Core 1，栈 16384 字，5ms 循环
- SPI 时钟：20MHz（验证稳定无花屏）
- 电池读取：5 秒缓存

### 变更文件
- main/display.cpp — 动画驱动+UI重构+按键条
- main/reel_img.h — 64px 48帧卷轴
- main/cassette_bg.h — 盒壳背景（新增）
- main/power_mgmt.cpp — 电池 ADC 5秒缓存
- tools/gen_reel.py — 卷轴生成器
- tools/gen_cassette_bg.py — 盒壳生成器（新增）

---

## R110 — 卷轴橙色化 + 全图2x超采样抗锯齿 + 布局重排（2026-09-05）

### 卷轴与盒壳抗锯齿
- 卷轴 gen_reel.py: 128px 高分辨率生成+旋转, LANCZOS 缩小到 64px (2xSSAA), 消除边缘锯齿
- 盒壳 gen_cassette_bg.py: 592x192 高分辨率生成, LANCZOS 缩小到 296x96 (2xSSAA), 轮毂圆环边缘平滑
- 卷轴颜色: 红 -> 黄(试) -> 棕(试) -> 橙 (#FF8000, 最终)

### 布局重排 (曲名+格式行移到盒壳上方)
- 曲名 canvas: y=56(覆盖盒壳) -> y=24 (盒壳上方)
- 格式行 lbl_fmt: y=144 -> y=44 (曲名下方, 盒壳上方)
- 盒壳 tape_bg: y=42 -> y=64 (下移, 96px高 -> y=64~160)
- 卷轴 reel_l/r: y=58 -> y=80
- 时间行: y=164 (盒壳下方)
- 进度条: y=186
- 图标栏: y=204 (不变)

### 状态栏迭代 (最终回退到简洁方案)
- 尝试: 喇叭图标+音量数字 -> 横屏空间不足导致文字换行, 回退
- 尝试: NOR 紫色徽章(透明底+紫边框) -> 用户要求改回纯文字
- 尝试: SD卡图标重设计(金属触点+写保护缺口) -> 效果不好, 恢复最初版本(矩形+缺角+TF文字)
- percent "0" 残留: 移到屏幕外 (-100,-100) 彻底消除

### 变更文件
- main/display.cpp — 布局重排+状态栏迭代+percent修复
- main/reel_img.h — 橙色卷轴 2xSSAA
- main/cassette_bg.h — 盒壳背景 2xSSAA
- tools/gen_reel.py — 橙色+2xSSAA生成器
- tools/gen_cassette_bg.py — 2xSSAA生成器


---

## R111 — A-B复读混合式 + UI优化 + Bug修复 (2026-09-09)

### A-B复读混合式改造 (核心功能)
- 长按PLAY键改为A-B标记状态机: 无标记→标记A; 有A无B→标记B(≥1s自动开循环); 循环中→清除标记
- 标记时底部toast提示3秒 (A: 00:58 / B: 01:07 -> LOOP / Too short / A-B Cleared)
- 状态栏A标记 + 格式行A-B信息 (MP3 | A:58->B:07 x3) + 进度条A/B标记点 + AB徽章
- 原长按PLAY切换播放模式移至菜单
- 菜单→A-B复读保留精细微调 (Mark A/Mark B/Loop/Clear)

### A-B复读Bug修复 (多轮排查)
- task_wdt死机: display_show_ab_menu()直接在main任务调lv_lock()与lvgl_task竞争→改为缓存+标志范式
- PLAY键无响应: 进入子菜单时s_edit未重置→进入时s_edit=false
- 界面被覆盖: s_menu_visible未生效 + ab_menu_apply_nolock()未隐藏s_cjk_canvas→修复
- 中文方块: g_ab_menu用montserrat_14仅ASCII→A-B子菜单改用英文 (Mark A/Mark B/Loop/Clear)
- 暂停态标记B后界面仍暂停: 标记B后自动恢复播放
- 状态不一致(按键指示暂停但磁带轮转): 快进/快退退出无条件设PLAYING→保存进入前状态并恢复
- 暂停态快进导致管道混乱: 暂停态进入scrub不resume + audio_player_scrub_exit(resume)参数控制
- 标记B后无声: 在B点resume后立即触发AB循环seek回A, RESUME/PAUSE碰撞→暂停态先seek到A点再resume

### UI优化
- 音量OSD中央浮层 (去掉喇叭图标, 高50px)
- TAPEBOOK启动画面
- 格式行: 移除静态44KHZ/16bit/320kbps, 改为动态A-B复读信息
- 移除底部lbl_ab (y=202与按键重叠), A-B文字移至格式行
- SQ徽章化 + 总时长右对齐修复
- 进入菜单自动暂停播放, 退出菜单自动恢复

### 功能修复
- 停止状态FF/REW RELEASE状态检查 (假播放bug)
- 快进/快退响应优化 (BTN_LONG_PRESS_MS 800→500ms)
- 停止键重置播放位置 (从开始播放而非继续)
- FF/REW时间显示移至进度条上方, 与倍速指示同一行
- RW/FF倍速指示与读秒重叠修复
- NOR和TF卡图标重叠修复

### 变更文件
- main/main.cpp — A-B标记状态机 + 快进/快退状态恢复 + 标记B seek-to-A
- main/display.cpp — toast机制 + 格式行动态A-B信息 + 移除lbl_ab + A-B菜单缓存范式
- main/display.h — display_toast()声明
- main/menu.cpp — s_edit重置 + A-B子菜单英文label
- main/audio_player.cpp — A-B遍数计数 + 标记B自动开循环 + scrub_exit(resume)参数
- main/audio_player.h — ab_loop_count() + scrub_exit()签名变更
- main/config.h — BTN_LONG_PRESS_MS=500


## R112 (2026-09-10) 菜单UI设计稿对齐 + 浏览文件交互修复

### 菜单UI改进
- 顶部状态栏: 标题(紫色) + NOR徽章 + 电量图标(低电量橙色)
- 选中项高亮背景 (#1d2740 深蓝块) + 白色文字
- 序号列: 选中项青色">", 未选中灰色数字
- 子菜单指示箭头 (右侧青色">")
- TOGGLE值右对齐 + 青色高亮
- 未选中项灰色文字, 层次分明
- 菜单数据结构化: menu_disp_item_t (label/kind/value) 替代预格式化字符串
- 新增 cjk_canvas_fill_rect() 点阵矩形绘制

### 浏览文件交互修复
- 从菜单进入浏览时保留菜单状态, STOP返回一级菜单 (不再直接回播放界面)
- 浏览中PLAY选曲后自动关闭菜单
- 浏览中VOL+/VOL- 用于上下移动 (与菜单一致)
- VOL方向统一: 往上拨=向上移动, 往下拨=向下移动 (菜单和浏览同步)
- 浏览翻页后高亮跟随选中项 (修复: 选中行索引 vs 全局索引)
- 浏览序号随滚动变化 (全局索引, 如7-12而非固定1-6)
- 浏览序号列宽32px支持两位数显示
- 去掉文件名前的">"前缀 (由渲染层统一画选中指示)
- 按键分发: 浏览模式优先于菜单处理 (菜单保持打开但不拦截浏览按键)
- 新增 menu_refresh() 公开函数 (从浏览返回菜单时重渲染)

### 变更文件
- main/display.cpp - menu_apply_nolock重写(设计稿风格) + cjk_canvas_fill_rect + menu_cache_item_t(字符数组缓存) + display_show_browse修复
- main/display.h - menu_disp_item_t定义 + display_show_menu签名变更 + menu_refresh声明
- main/menu.cpp - menu_render结构化输出 + VOL方向修正 + menu_refresh实现
- main/menu.h - menu_refresh声明
- main/main.cpp - 浏览从菜单进入保留菜单状态 + STOP返回菜单 + VOL导航 + 按键分发优先级

## R113 (2026-09-14) A-B菜单移除 + ASCII字体重构 + 按键音死机修复

### A-B菜单移除
- 菜单中移除"A-B 复读"入口(根菜单从5项变为4项: 浏览文件/书签/播放模式/系统设置)
- 移除A-B二级菜单及微调状态机
- 保留播放界面A-B功能: 长按PLAY键标记A/B, 自动进入循环, 进度条标记线+区间高亮+格式行动态A-B信息

### ASCII字体重构
- 全部95个ASCII字模(0x20-0x7E)从原点阵字库替换为Arial 15px渲染
- 下对齐(baseline=13): 符合人类书写习惯, 所有字符baseline一致
- 左对齐(x=1): 字符从同一列开始, 水平位置一致
- 解决原点阵字库大量字符字形错误(A左上缺像素/2/7/V/X等缺陷)及高低不平问题

### 字符间距优化
- cjk_blit_text: ASCII字符步进从16px改为12px, 中文字符保持16px
- 解决ASCII字符间距过大问题

### 按键音死机修复
- 根因: 停止态按STOP/PREV/NEXT键时, app_play_beep()创建raw+i2s管道播放提示音, 触发task_wdt死机(main任务)
- 修复: app_play_beep()改为空函数, 禁用所有按键提示音
- 验证: 启动后直接按任意键不再死机

### 菜单UI
- 移除顶部状态栏"NOR"徽章(无意义)

### 变更文件
- main/cjk_font.c - 全部95个ASCII字模替换为Arial 15px(baseline=13, x=1)
- main/display.cpp - cjk_blit_text ASCII步进12px + 移除菜单NOR徽章
- main/main.cpp - app_play_beep()禁用 + A-B菜单移除相关清理
- main/menu.cpp - 移除A-B子菜单及相关代码
- main/display.h - 移除A-B菜单相关声明

## R114 (2026-09-14) 书签功能实现

### 书签功能
- 菜单→书签: 动态子菜单, 第一项"+ 添加当前位置", 后续为当前文件的书签列表(时间点)
- 添加书签: 播放/暂停态用当前位置, 停止态用NVS中最后保存的位置
- 跳转播放: 选中书签按PLAY → 关闭菜单, 从书签位置开始播放
- 每文件最多10个书签, 满了自动覆盖最旧
- NVS持久化存储, 重启不丢失

### 变更文件
- main/bookmark.h - 新增 bookmark_get_all() / bookmark_delete()
- main/bookmark.cpp - 实现 bookmark_get_all() / bookmark_delete()
- main/menu.cpp - 书签动态子菜单(bookmark_fill_and_enter) + PLAY键特殊处理
- main/main.cpp - app_get_current_track_idx() / app_bookmark_add_current() / app_bookmark_jump()

## R115 (2026-09-15) 格式行中文化 + 关机系统完善 + 开机体验优化

### 格式行canvas化（中文播放模式）
- 格式行从LVGL label改为canvas渲染(s_fmt_canvas, 274x18 at (8,44)), 用cjk_blit_text渲染中文
- 显示内容: 无A-B时 MP3|顺序播放, 有A-B时 MP3|A01:23 B02:45x3
- A/B字符用黄色(0xf59e0b)高亮, 其余紫色(0xb8a4dc)
- 取消原AB橙色徽章(被字体覆盖且意义不大)
- ASCII字模步进12px(M字符能完整显示)

### 播放模式列表选择
- 菜单→播放模式→动态子菜单列出 顺序播放/列表循环/单曲循环
- 当前模式默认选中高亮, 按PLAY确认返回
- 播放界面格式行显示中文模式名

### 书签删除功能
- 书签子菜单中: 短按PLAY=跳转播放, 长按PLAY=删除当前选中书签
- 修复关键bug: bookmark_delete期望NVS的slot编号, 之前传列表索引idx, 书签不连续存储时删错位置
- bookmark_t新增slot字段, bookmark_get_all填充实际slot, 删除时用bms[idx].slot

### 菜单清理
- 系统设置菜单移除未实现的桩功能: 按键提示音(已禁用beep)、语音播报(仅NVS持久化)、USB存储(仅提示功能未开放)、EQ(回滚)
- 系统设置剩余3项: 固件升级、关于、定时关机
- 修复系统设置child_count bug(原7/8, 实际3/4, 导致数组越界LoadProhibited死机)
- EQ功能尝试与回滚: ESP-ADF equalizer元件插入管道导致Guru Meditation Error死机, 已完全移除

### 开机体验优化
- 开机花屏修复: LCD初始化后立即用esp_lcd_panel_draw_bitmap清屏(全黑, 分6块), 不依赖LVGL异步渲染; 背光初始化移到首次渲染之后
- splash顶部黑条修复: s_splash容器添加bg_opa=LV_OPA_COVER(完全不透明), 避免透出下层player UI; 显示splash时隐藏g_msg/g_player/g_ota并move_foreground

### 关机系统完善
- 恢复真正硬件关机: TAPEBOOK_POWER_LATCH_BYPASSED从1改为0, power_mgmt_power_off()恢复拉低POW_EN 2秒释放锁存 + deep-sleep兜底
- 低电量关机倒计时: 电量临界(<5%)且未充电→启动30秒倒计时, 屏幕显示Low Battery / Shutdown in XX s; 倒计时期间接上充电→自动取消; 倒计时结束→保存状态后关机
- 定时关机改为无动作计时: 任何按键操作→重置计时(持续无操作才到期); 无操作到时间→停止播放+启动30秒关机倒计时, 屏幕显示Auto Off / Shutdown in XX s; 倒计时期间按任意键→取消关机并重新计时; 倒计时结束→清除定时设置后关机

### 变更文件
- main/display.cpp - 格式行canvas + splash bg_opa修复 + LCD清屏 + 背光延后 + display_show_shutdown_countdown() + AB徽章隐藏
- main/display.h - display_show_shutdown_countdown()声明
- main/menu.cpp - 播放模式列表选择 + 书签长按删除 + 移除EQ/语音播报/USB存储/按键提示音 + 系统设置child_count=3/4
- main/main.cpp - 低电量倒计时逻辑 + 定时关机无动作计时 + 倒计时到期关机 + 按键取消倒计时
- main/power_mgmt.h - 关机倒计时类型/函数声明 + power_mgmt_reset_auto_off_timer()
- main/power_mgmt.cpp - 关机倒计时实现 + record_activity重置定时关机计时
- main/config.h - TAPEBOOK_POWER_LATCH_BYPASSED=0
- main/bookmark.h - bookmark_t添加slot字段
- main/bookmark.cpp - bookmark_get_all填充slot字段
- main/audio_player.cpp/h - EQ回滚(移除equalizer相关代码)
