# SESSION_SUMMARY.md — TapeBook 关键决策与经验

> **最后更新**：2026-07-10（R015 — 硬件设计修复 6 项 + LE Audio 方案文档）

---

## 1. 会话主线（按时间）

| 时间 | 主题 | 结果 |
|---|---|---|
| 2026-07-03 上午 | 阅读 README + DEVELOP_STATUS + HARDWARE_MIGRATION | 摸清项目状态：V1.0 MVP 8/10，3 个 P0 阻塞 |
| 2026-07-03 中午 | 用户确认方向：先解 P0 阻塞 + 初始化 git | 创建 5 个任务 |
| 2026-07-03 中午 | 用户补充 GitHub 仓库地址 | 调整计划：本地 → GitHub `zhutao198/tapeplayer` |
| 2026-07-03 下午 | git init + .gitignore + 3 类文件 + 首次 commit | ✅ baseline commit `938abbe` |
| 2026-07-03 下午 | R001 启用 ESP-ADF（追认 sdkconfig 已配） | ✅ commit `c0c67e4` |
| 2026-07-03 下午 | R002 启用 u8g2（删 334M 手动源码 + 备份 + idf component 路径） | ✅ commit `d773f05` |
| 2026-07-03 下午-晚上 | R003 build 验证（多次失败 + 修 4 个子模块 + 暴露 ADF 5.5 引用方式变化） | ⚠️ commit `333e44e`（build 未通过） |
| 2026-07-03 傍晚-凌晨 | R004+R007 build 全线修复（custom board / audio_player API / u8g2_hal 兼容性） | ✅ 首次成功构建！`.bin` 718KB，分区 77% 剩余 |
| 2026-07-06 | R009-R010 代码审查全部 38 项清零 | ✅ 审查完成，代码稳定 |
| 2026-07-06 | R008 代码审查修复（核实 38 条发现，修 33 项） | ✅ 构建通过，二进制 0xaf9c0 |
| 2026-07-06 | R009 审查剩余 9 项修复（SD 热插拔/脏区/屏保/light sleep/锁定态/button/采样率） | ✅ 构建通过，二进制 0xb26b0 |
| 2026-07-06 | R010 审查剩余 8 项清零（bookmark NVS/voice_prompt/M-2 timeout/M-3 init/设计确认） | ✅ 构建通过，二进制 0xb2660 |
| 2026-07-07 | R011 修复 R010 引入的 6 个 bug（SD 检测/light sleep/pause-resume/截断/溢出/命名歧义）+ H-8 ADC 桩 + L-1 bookmark 集成 | ✅ 构建通过，二进制 0xb2660 |
| 2026-07-07 | R012 实现文件夹浏览（滚动列表 + Prev/Next/Play/STOP 导航）— **V1.0 MVP 全部 11 项完成！** | ✅ 构建通过，二进制 0xb2a40 |
| 2026-07-07 | R013 R012 review 修复（scroll clamp + API cleanup） | ✅ 构建通过 |
| 2026-07-09 | R014 PRD 审查 5 项修复（OLED/音量/书签/电源/休眠）+ 原理图设计 | ✅ 构建通过，二进制 0xb9b70 |
| 2026-07-10 | R015 硬件设计修复（B2/N1/N2/N3/N4/N5）+ LE Audio 方案文档 | ✅ 全部闭环；新建 BT_AUDIO_PLAN.md |

---

## 2. 关键决策

### 决策 D001：本地项目上传为 GitHub 新仓库
- **背景**：本地 `D:\zhutao\audio_player` 无 .git；用户指向 `https://github.com/zhutao198/tapeplayer`
- **决定**：本地是主源，GitHub 为新仓库
- **实施**：git init → 首次 commit (baseline) → R001-R003 → （待 push）

### 决策 D002：3 类核心文件立即建
- **背景**：全局 CLAUDE.md 9.2 强制要求；项目根原本缺失
- **决定**：在首次 commit 前建好 CONTEXT/SESSION_SUMMARY/开发日志
- **理由**：避免 R 节点机制空转

### 决策 D003：.gitignore 屏蔽 build 日志 + 第三方源码
- **背景**：根目录有大量 `*.log` / `*.err` / `*.out`（编译产物）+ 334M `components/u8g2/`（手动 clone）
- **决定**：全部忽略；`sdkconfig` 忽略但 `sdkconfig.defaults` 保留
- **理由**：仓库只留源码 + 文档 + 模板

### 决策 D004：R002 删 334M components/u8g2 + 走 idf component
- **背景**：手动 clone 的 334M 源码难维护
- **决定**：备份后删 + 改用 `idf.py add-dependency lfengineering/u8g2_esp32`
- **结果**：R002 commit 后发现 `lfengineering/u8g2_esp32` 在 registry **不存在** → R003 暂禁用

### 决策 D005：R003 暂禁用 u8g2 + 保留 ADF 修复
- **背景**：R002 用了错的 u8g2 component 名 → build fail；同时发现 R001 漏 ADF REQUIRES → 4 文件修复
- **决定**：u8g2 暂禁用，ADF 修复 + 子模块 fix 走 R003 commit（即使 build 仍 fail）
- **理由**：保留进度，避免代码丢失

### 决策 D006：ESP-IDF 子模块 4 个修复不入仓
- **背景**：micro-ecc / lib_esp32 / lib_esp32c2 / lib_esp32c3_family 都坏（HEAD bad object）
- **决定**：直接修复 ESP-IDF 仓库子模块（环境维护），不入 audio_player 仓
- **记录**：在开发日志 R003 节点留 T-003 教训 + 修复命令模式

### 决策 D007：评审文档归档不建 R 节点
- **背景**：用户要求评审 `docs/HARDWARE_PIN_WIRING.md`
- **决定**：输出 `docs/HARDWARE_PIN_WIRING_REVIEW.md`（V1.0），**不**建 R 节点（评审是调研/审计，不是代码修改）
- **理由**：按规范 8.1"触发时机"严格性，评审归档不属于 R 节点触发范围；用普通 commit 保留 git history

---

## 3. 关键成就

- ✅ **仓库基线建立**（baseline commit + tag，172 文件入仓）
- ✅ **3 类核心文件齐备**（CONTEXT.md / SESSION_SUMMARY.md / 开发日志.md）
- ✅ **R001 完成**（ESP-ADF 启用，已在 sdkconfig 配 + 注释追认）
- ✅ **R002 完成**（u8g2 改用 idf component 路径 + 删 334M 手动源码 + 备份到 D 盘）
- ✅ **R003 完成**（commit `333e44e` 含 5 文件修复 + sdkconfig 清理；build 验证未通过但发现关键阻塞点）
- ✅ **ESP-IDF 4 子模块修复**（micro-ecc / lib_esp32 / lib_esp32c2 / lib_esp32c3_family）
- ✅ **R004 完成**（CMakeLists.txt 启用 ADF：EXTRA_COMPONENT_DIRS 移到项目根）
- ✅ **R005 完成**（修 HARDWARE_PIN_WIRING.md 5 处错误 + 补 MAX98357A 规格书）
- ✅ **R006 完成**（修 HARDWARE_PIN_WIRING.md 5 处错误：SD_MODE 公式/GPIO47-48 拆分 R8V-R16V/GPIO19-20 D+/D- 标反/GPIO45 strapping + 补 SSD1315 规格书 + pdf_search 提取脚本）
- ✅ **R007 完成——首次成功构建！**（`audiobook_player.bin` 生成，718KB，分区 77% 剩余）
  - 新建 `components/tapebook_board/` 组件（ADF 自定义板级支持）
  - 修复 `audio_player.cpp` 6 处无效 ADF API 调用
  - 修复 `u8g2_esp32_hal` 编译兼容性（REQUIRES + ets_delay_us → esp_rom_delay_us）
- ✅ **R008 完成——代码审查 33 项修复！**（核实 38 条发现，3 条不属实）
  - CRITICAL：seek 字节换算、position 时间戳、NULL 检查、WDT 增大 + 回调异步化
  - HIGH：PSRAM 分配、NVS 返回值检查 + 降低 commit 频率、auto_off 集成到主循环
  - MEDIUM：删 unused 变量、stop_playback 语义修正、playlist_set_index 补缺
- ✅ **R009 完成——代码审查剩余 9 项修复！**
  - HIGH：SD 热插拔 stat() 轮询、display 脏区 + 屏保、锁定态 activity 记录、power_mgmt tick + light sleep
  - MEDIUM：DBL_DEBOUNCE 去抖、GPIO 返回值检查、删 I2S_MCLK_IO、button 配置/状态分离
  - LOW：采样率缓存、g_count 类型（已在 R008 修）
- ✅ **R010 完成——代码审查全部 38 项清零！**
  - MEDIUM：wait_for_stop 超时保护、sdspi mount init 警告修复
  - LOW：bookmark NVS 书签实现、voice_prompt V1.2 预备
  - 设计确认：M-9/M-10/M-15/L-3/L-8 单任务安全，加注释说明
- ✅ **R011 完成——修复 R010 引入的 6 个 bug + 补 H-8/L-1**
- ✅ **R012 完成——文件夹浏览实现，V1.0 MVP 11/11 全部完工！**
  - 长按 STOP 进入浏览模式
  - Prev/Next 滚动，Play 选中播放，STOP 退出
  - OLED 滚动列表（5×8 字体，6 条可见，`>` 标记选中行）
- ✅ **R013 完成——R012 review 修复：scroll overflow clamp + API cleanup**
- ✅ **R014 完成——PRD 审查 5 项修复（OLED/音量/书签/电源/休眠）+ 原理图设计**
  - OLED：根因分析 3 层（Kconfig 符号缺失 → CMake 无 REQUIRES → C++ name mangling）
  - 音量：ADF 的 `audio_element_set_volume()` 不存在，改用 `i2s_alc_volume_set()`
  - Bookmark：满 10 时环形覆盖（slot 0 丢弃，前移，新值写末尾）
  - 定时关机：`power_mgmt_tick()` 内轮询 `power_mgmt_should_shutdown()`
  - Light sleep：唤醒后统一 `APP_STATE_STOPPED`
  - 原理图：6 页规格书 + Protel 网表 + CSV BOM（30 种物料）
  - H-8: ADC 电池检测桩代码（换算公式注释）
  - L-1: bookmark 接入 STOP 双击事件
- ✅ **R015 完成——硬件设计修复 6 项闭环（B2/N1/N2/N3/N4/N5）+ LE Audio 方案文档**
  - B2：ASCII 引脚图重画为 WROOM-1 物理编号（消除 Pin 22-27 冲突）
  - N1：ME6211C33 SOT-89→SOT-23-5（M5G-N，带 CE 使能引脚）
  - N2：泄放电阻 1kΩ→100kΩ
  - N3：GPIO45/46/47/48 ≥5mm PCB 净空注释
  - N4：HARDWARE_PIN_WIRING.md 电源树同步（AMS1117→ME6211C33 + BAT直供）
  - N5：附录 A AMS1117→ME6211C33M5G-N
  - I3：§3.4 "A2DP"→"LE Audio"
  - BOM/网表同步更新
  - 新建 `docs/BT_AUDIO_PLAN.md`——LE Audio 蓝牙耳机方案（11 节，含 IDF 升级前提/API 流程/12 步实施计划）

---

## 4. 经验教训

### L001：WebFetch / WebSearch 网络受限
- **现象**：WebFetch github.com / components.espressif.com / WebSearch 多个域失败
- **应对**：依赖本地目录 + git ls-remote 探测 + 询问用户澄清意图
- **未来**：涉及网络查询时，**直接 `git ls-remote`** 或让用户提供信息

### L002：项目名"audio_player"（本地）/ "tapeplayer"（GitHub）/ "TapeBook"（README）三者不同
- **现象**：路径 / 仓库名 / 文档项目名 3 个不同
- **建议**：所有引用统一用"TapeBook"或"tapeplayer"

### L003：tar 备份 + rm -rf 必须分两步 + 验证（教训 T-001）
- **现象**：第一次 `tar czf D:/u8g2_backup.tar.gz components/u8g2/` 失败（Git Bash 把 D:/ 当 host），但 `rm -rf` 仍执行
- **正确用法**：
  ```bash
  # 错误：tar czf D:/backup.tar.gz src/    ← D:/ 被 Git Bash 当 host
  # 正确：tar czf /d/backup.tar.gz src/    ← POSIX 路径 /d/ = D:\
  # 验证：tar tzf /d/backup.tar.gz | head -3
  # 再删：rm -rf src/   （验证成功后再删）
  ```
- **教训**：删前必须先验证 tar 成功；两命令分开跑，不要 `&&` 链式

### L004：ESP-IDF build 在 Git Bash 下跑不通（教训 T-002）
- **现象**：./build.bat 不被 bash 解释；cmd /c 开新窗口；cmd //c 路径转换失败
- **正确用法**：
  ```bash
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; & 'D:\esp\v5.5.3\esp-idf\export.ps1' | Out-Null; \$env:ADF_PATH='D:\esp\esp-adf'; idf.py build"
  ```
- **关键点**：
  1. 必须在 **PowerShell 内部** `Remove-Item Env:MSYSTEM`（bash 子 shell `unset` 不传给 PowerShell）
  2. `export.ps1` 通过 `&` dot-source 调，保留环境变量
  3. 不要用 cmd /c "build.bat" 跑 build.bat（开新窗口）

### L005：ESP-IDF 子模块 fix 模式（教训 T-003）
- **现象**：ESP-IDF 5.5.3 install 时多个子模块只 clone 元数据没 fetch objects
- **修复 3 步**：
  1. `cd <submodule_path>`
  2. `git fetch origin`
  3. `git reset --hard <ESP-IDF 锁定的 commit>`（从 `.gitmodules` sbom-hash 或 `HEAD` 文件读）
- **建议**：建立 `tools/fix_idf_submodules.sh` 一键修复

### L006：ADF 5.5 组件引用方式变化（教训 T-004）
- **现象**：`main/CMakeLists.txt REQUIRES audio_pipeline` 被弃用
- **正确**：`main/idf_component.yml dependencies: espressif/audio_pipeline: "*"`
- **教训**：遇到 "The component X could not be found" + 提示"moved to IDF component manager" 时，**改用 idf_component.yml 引用**

### L007：idf_component.yml YAML 空 dict 也报错
- **现象**：`dependencies:` 后只有 `# 注释` → "Input should be a valid dictionary"
- **解决**：整个 `dependencies:` 块注释掉（包括 key）

### L008：删 334M 前必须先备份 + 验证
- 见 L003（备份失败的教训）
- **未来**：所有"先备份后删"操作必须分两步：① tar + `tar tzf` 验证 ② 再 rm

### L009：评审报告也可能有误，必须经 datasheet 核实
- **现象**：评审报告误说 GPIO17/18 是 USB-JTAG（实为 GPIO19/20）、GPIO1/2 是 UART0（实为 GPIO43/44）
- **教训**：所有评审发现必须经官方 datasheet 核实后才可采纳，不盲信评审结论

### L010：MAX98357 SD_MODE 是四级电压阈值，非二值 GND/VDD

### L011：评审必须基于规格书，不凭经验
- **现象**：V1.0 评审凭经验误判 GPIO17/18 为 USB-JTAG、GPIO1/2 为 UART0；V2.0 基于规格书核实全部正确
- **教训**：评审质量取决于数据来源。**必须有规格书原文支撑**，经验性评审只能作初步筛选

### L012：WROOM-1 和 WROOM-2 的 GPIO47/48 引出规则不同
- WROOM-1：仅 R16V 芯片有 GPIO47/48（脚注 c）
- WROOM-2：R8V 和 R16V 都有 GPIO47/48（脚注 2）
- **教训**：同一模组系列不同封装/型号可能引脚不同，必须逐型号看规格书脚注
- **现象**：SD_MODE 不是简单的 GND/VDD 二值，而是 **Shutdown(0V) / Mono(0.16-0.77V) / Right(0.77-1.4V) / Left(>1.4V)** 四级
- **教训**：必须用电阻分压获得 Mono 模式，直连 VDD 只输出 Left channel

### L013：ADF 自定义 board 必须手动创建 INTERFACE_LINK_LIBRARIES 组件
- **现象**：`CONFIG_AUDIO_BOARD_CUSTOM=y` 时 ADF 的 `esp_peripherals` 无条件 include `<board.h>`，但 ADF 不提供默认 board
- **解决**：创建 `components/tapebook_board/`，通过 CMake `INTERFACE_LINK_LIBRARIES` 将包含路径注入到 `audio_board` 库
- **模板**：参考 ADF 示例 `examples/player/pipeline_bt_player/` 中的 `my_board` 目录结构
- **教训**：ADF 自定义板级支持不是通过 menuconfig 配置完成，而是通过 CMake 组件注入

### L014：audio_player.cpp 6 处 API 从未被编译过
- **现象**：该文件包含 `audio_element_set_volume()`、`audio_element_set_pos()`、`audio_pipeline_get_state()` 等不存在于 ADF 的 API
- **原因**：文件基于 ADF 文档手册编写，但实际 API 不同；从未构建验证过
- **教训**：`audio_element_set_volume` → 直接调 `i2s_set_sample_rate`（无音量控制 API）
- **教训**：`audio_element_set_pos/get_pos` → 用 `audio_element_get_byte_pos` 近似
- **教训**：`audio_pipeline_state_t`/`get_state` → 换 `audio_element_get_state`
- **教训**：所有新加调用的文件必须编译验证，不能仅靠文档审查

### L015：ets_delay_us → esp_rom_delay_us（IDF 5.x 弃用）

### L016：`stat()` 在 FATFS VFS 挂载点永远返回 0
- **现象**：`stat("/sdcard", &st)` 即使卡被拔掉也返回 0（VFS 伪目录持久存在）
- **正确做法**：用 `sdmmc_read_sectors(g_sd_card, buf, 0, 1)` 读 MBR 扇区，卡移除返回 `ESP_ERR_TIMEOUT`
- **教训**：VFS 层函数不能用于物理设备存在性检测

### L017：FF/RW 高速跳帧中 pause/resume 是反模式
- **现象**：FF 8x 每 50ms 跳 400ms + 每次 pause/resume 引起 I2S underrun 杂音
- **正确做法**：抽出 `audio_player_seek_ms_internal()` 跳过 pipeline lifecycle，跳帧 tick 直接调用
- **教训**：高速重复调用中有副作用的函数前必须考虑累积效应

### L018：`"%.21s"` 比手动分支更安全简洁
- **现象**：手动 `if (len <= 21) { %-21s } else { %s }` 的 else 分支遗忘截断
- **教训**：snprintf 的 `%.*s` 或 `"%.21s"` 格式字符串比手动分支更不容易出错
- **现象**：`components/u8g2_esp32_hal/u8g2_esp32_hal.c` 使用 `ets_delay_us()`，IDF 5.5 报错"implicit declaration"
- **解决**：替换为 `esp_rom_delay_us()`（来自 `<esp_rom_sys.h>`）
- **教训**：IDF 从 v5.0 起逐步弃用 `ets_*` ROM 函数，推荐使用 `esp_rom_*` 替代
- WROOM-1：仅 R16V 芯片有 GPIO47/48（脚注 c）
- WROOM-2：R8V 和 R16V 都有 GPIO47/48（脚注 2）
- **教训**：同一模组系列不同封装/型号可能引脚不同，必须逐型号看规格书脚注
- **现象**：SD_MODE 不是简单的 GND/VDD 二值，而是 **Shutdown(0V) / Mono(0.16-0.77V) / Right(0.77-1.4V) / Left(>1.4V)** 四级
- **教训**：必须用电阻分压获得 Mono 模式，直连 VDD 只输出 Left channel

### L019：C++ 代码调用 C 函数必须加 extern "C"（R014 关键教训）
- **现象**：`display.cpp`（C++）调用 `u8g2_esp32_hal_init()`（C 实现），链接器报 undefined reference
- **根因**：C++ name mangling 使编译器寻找 `_Z19u8g2_esp32_hal_init...`（mangled），但 C 编译产生 `u8g2_esp32_hal_init`（unmangled）
- **修复**：`u8g2_esp32_hal.h` 加 `extern "C" { }` 包裹
- **教训**：所有可能被 C++ 引用的 C 头文件必须加 `extern "C"` 守卫（即使现在不用，将来可能被 C++ 调用）

### L020：ESP32-S3 仅支持 BLE 5.0（无 BT Classic），LE Audio 是唯一蓝牙音频方案
- **现象**：用户期望 A2DP 蓝牙耳机，但 ESP32-S3 硬件不支持 BT Classic（无 BR/EDR 控制器）
- **解决**：LE Audio（LC3 编解码，BLE ISO 等时通道），通过 `esp-ble-audio` 组件实现
- **教训**：ESP32 系列双模（BT Classic + BLE）仅限 ESP32、ESP32-C3；ESP32-S3、C6、H2 只有 BLE 5.0

### L021：ME6211C33 实际封装为 SOT-23-5（非 SOT-89）
- **现象**：早期设计假定 SOT-89，但实际采购 ME6211C33M5G-N 为 SOT-23-5（带 CE 使能引脚）
- **引脚**：1=VIN, 2=CE, 3=VOUT, 4=NC, 5=GND
- **教训**：相同型号不同后缀封装不同，必须根据实际采购后缀确定封装

### L022：静态库链接顺序——依赖库必须出现在使用者之后
- **现象**：`libu8g2_esp32_hal.a` 定义了符号，`libmain.a` 引用这些符号，但链接失败
- **根因**：GNU ld 从左到右处理静态库；如果库在前面，且前面没有对象引用其符号，库里的 .o 被丢弃
- **解决**：把 `u8g2_esp32_hal.c` 源码直接编入 `main` 组件，避免静态库链接
- **教训**：ESP-IDF 中组件 `REQUIRES` 不一定保证正确链接顺序；把强依赖源的源码直接放到使用组件是最确定的做法

### L023：Kconfig 符号必须配套 CMake 条件才生效
- **现象**：`sdkconfig.defaults` 中有 `CONFIG_USE_U8G2=y`，但 cmake 未`#ifdef`使用，实际被静默忽略
- **修复**：在 `Kconfig.projbuild` 中定义 `config USE_U8G2` + `main/CMakeLists.txt REQUIRES u8g2`
- **教训**：menuconfig 符号要生效需要 3 步：① Kconfig 定义 ② CMake 引用 ③ C 代码 `#ifdef`

---

## 5. 性能指标

| 指标 | R013 (MVP) | R014 (PRD fix + OLED) |
|---|---|---|
| Build 错误 | 0 | 0 |
| Binary 大小 | 0xb2a40 (731 MB) | 0xb9b70 (762 MB) |
| 分区空闲 | 77% | 76% |
| OLED 驱动 | ❌ 黑屏 | ✅ u8g2 + I2C |
| 音量控制 | ⚠️ 存值无效 | ✅ i2s_alc_volume_set |

---

## 6. 未来方向

### 下次会话
1. **烧录验证硬件**：`build.bat -p COMx flash` 确认 SD/OLED/I2S/按键全部跑通
2. **V1.1 起步**：定时关机（ADC 实装）、A-B 复读、按键提示音
3. **提交原理图到 KiCad/Altium**：手工输入 SCH_TapeBook_V1.3.md → PCB layout
4. **LE Audio 实施**：按 BT_AUDIO_PLAN.md 12 步计划——先验证 IDF master 构建

### 短期
- V1.1 体验增强：定时关机、按键音、屏幕保护
- LE Audio 可行性验证（IDF master + esp-ble-audio 编译）

### 中期
- 量产前：OTA 接收代码（HTTP/HTTPS）
- V1.2 进阶：书签、语音、电量、设置菜单
- LE Audio CIS 连接 + LC3 编解码集成

### 长期
- V2.0 远期：蓝牙音频（LE Audio）、EQ、速度微调

---

## 7. 持久化资源

| 资源 | 路径 / 链接 |
|---|---|
| GitHub 仓库 | https://github.com/zhutao198/tapeplayer（待 push）|
| 本地仓库 | D:\zhutao\audio_player |
| ESP-IDF v5.5.3 | D:\esp\v5.5.3\esp-idf（**4 子模块已修复**）|
| ESP-ADF v2.7 | D:\esp\esp-adf |
| u8g2 备份 | D:\u8g2_backup_20260703.tar.gz（282 MB）|
| ESP-IDF v5.3 离线安装器 | https://dl.espressif.com/dl/esp-idf/ |
| ESP-ADF v2.7 | https://github.com/espressif/esp-adf |
| WROOM-1 datasheet | hardware/esp32-s3-wroom-1_wroom-1u_datasheet_cn.pdf |
| WROOM-2 datasheet | hardware/esp32-s3-wroom-2_datasheet_cn.pdf |
| MAX98357A 规格书 | hardware/C910544_MAX98357A...PDF |
| SSD1315 OLED 规格书 | hardware/OLED_SSD1315.pdf |
| PDF 搜索脚本 | tools/pdf_search.py |
| MAX98357A 提取文本 | tools/_max98357a.txt |
| WROOM 提取文本 | tools/_wroom.txt |

---

## 8. R 节点 Git 状态

```
d54d0ed R015: 硬件设计修复 6 项（B2/N1/N2/N3/N4/N5）+ LE Audio 方案文档
eca38cc R014: PRD 审查 5 项修复（OLED/音量/书签/电源/休眠）+ 原理图设计
4f3b25e R013: post-R012 review fixes （scroll clamp + API cleanup + CODE_REVIEW.md sync）
1d95d12 R012: 实现文件夹浏览（V1.0 MVP 最后功能补齐）
df11f0d R011: 修复 R010 引入的 6 个 bug + H-8 ADC 桩 + L-1 bookmark 按键集成
```

**17 个 R 节点**（含 baseline + R001-R015）全部 committed + tagged（annotated）。

---

**作者**：Claude（6 R 节点完成：R010 → R015）  
**更新规则**：每次 R 节点 commit 后同步更新
