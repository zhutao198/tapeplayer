# 资源利用全局评估（修正版 · 以 BT 音箱构建为对象）

> 修订日期：2026-08-14
> 修订原因：原版以"默认构建（Wi-Fi 开 / BT 关）"为对象，得出"双无线都关 = 最大节省"，
> 与产品需求（蓝牙音箱 = BT 必须开）相反。本版改为以正确的 **BT 构建（BT 开 / Wi-Fi 关）**
> 为评估对象重评，并修正了原版的多处事实错误。
> 已落地修复：`configs/sdkconfig.bt_speaker` 现补 `CONFIG_USE_BT_SPEAKER=y`（见 §6）。

---

## 1. 量化基线（map 实测，来自默认构建产物）

| 段 | 占用 | 区域容量 | 占比 | 备注 |
|---|---:|---:|---:|---|
| IRAM（.iram0.vectors+.text） | **26 KB** | 350 KB | **7.5%** | 另有 iCache 16KB（硬件保留），链接器剩余 ~324KB |
| DRAM 静态（data+bss） | 85.5 KB | 340 KB | 25% | — |
| DRAM 通用堆（估算） | ~248 KB | — | — | 小对象关键缓存层 |
| Flash 应用段（text+rodata+appdesc） | **~1.6 MB** | 16 MB(物理) / 8MB(MMU 映射窗) | — | 注：此实测来自"Wi-Fi 开"构建，**已含 Wi-Fi 库** |
| Flash rodata | 564 KB | — | — | 含字库/位图 |
| PSRAM 运行时（估算） | **~700 KB** | 8 MB | **~9%** | LVGL 25 + 播放列表 160 + FreeType 缓存(1024)≈512 |
| PSRAM 类型 | 8 MB Octal @80MHz | — | — | sdkconfig 确认 |

> 说明：IRAM 区域长度 `0x57700`=350KB；Flash 物理 16MB，但指令映射窗 `iram0_2_seg=0x007fffe0`=8MB。

---

## 2. BT 构建的资源重算（BT 开 / Wi-Fi 关）

| 动作 | 资源影响 | 量级（估算） |
|---|---|---|
| Wi-Fi OFF | 释放 | ~1.4 MB Flash + ~200 KB DRAM |
| BT ON（Bluedroid+Classic+A2DP+AVRC） | 占用 | ~1.4 MB Flash + ~50–120 KB DRAM |
| **净变化（vs 默认构建）** | — | **Flash 基本持平（Wi-Fi 换 BT）；DRAM 反而略宽裕**（Wi-Fi 栈 > BT 栈） |

最终 BT 构建固件：
- **应用段 ≈ 1.6–2.0 MB**（原 "Wi-Fi 开" 实测 1.6MB；可行性文档估上限 ~2.2MB）。
- 落在 `factory`(3MB) 与 `ota_0/ota_1`(各 2MB) 槽内。**前提：BT 开时必须 Wi-Fi 关**，否则两者叠加 >2MB，会超出 OTA 2MB 槽。

---

## 3. 各资源逐项评估

### 3.1 IRAM —— 原版"100% 占满"是错误论断（已纠正）
- 实测 `.iram0.vectors`(1KB) + `.iram0.text`(89KB) 仅用到 `0x4038a700` = **26KB / 350KB ≈ 7.5%**。
- iCache 仅 16KB（`CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB`）。链接器在 iram0_0_seg 仍有 **~324KB** 空闲。
- **结论：IRAM 极宽松，不存在"加 IRAM_ATTR 即链接失败"风险。BT 引入的少量 IRAM（BT ISR/加密）远在其余量内。**
- 原版 §1 基线表写 25%、§3.1/§4 又写"100% 占满"——自相矛盾且无 map 依据，已删除该风险。

### 3.2 DRAM —— 相对最紧，但 BT 构建下更宽裕
- 区域 340KB，静态 85.5KB，剩余 ~254KB 供堆+任务栈；启动后通用堆 ~230–248KB。
- BT 构建因关 Wi-Fi（省 ~200KB）而比默认构建更宽松。瓶颈仍是 DRAM 通用堆，但远未触顶。

### 3.3 Flash —— 富余
- 应用段 ~1.6–2.0MB，factory 3MB 槽富余 ~1MB+；字库走独立 8MB FATFS 分区，不占应用段。
- 真正约束是 **OTA 双槽 2MB**：BT 构建须确保 Wi-Fi 关，避免 >2MB。

### 3.4 PSRAM —— 极度富余（原版"<5%"低估）
- 运行时 ~700KB（含 FreeType 1024 缓存 ≈512KB），约 **8–9%** of 8MB，余量 >7MB。
- 原版 §1 写"300–500KB(<5%)"与 §3.1"FreeType 512KB"自相矛盾，已统一为 ~700KB/9%。

### 3.5 CPU —— ADF 任务绑定 core 0，CPU1 轻载（非完全空闲）
- `DEFAULT_ELEMENT_TASK_CORE=0`：ADF 解码/读卡/I2S 任务全部绑定 core 0。
- 主任务 + LVGL 任务在 core 0 / 不绑定；CPU1 仅跑 idle + 偶发 LVGL。属"轻载"，非"完全空闲"。
- 双核未做主动负载分担，但当前负载下无性能瓶颈。

---

## 4. 做对的事 / 风险 / 可优化

### ✅ 方向正确（保留）
1. **BT 构建关 Wi-Fi**：既消 2.4GHz 共存干扰，又省 ~200KB DRAM + ~1.4MB Flash。
2. **PSRAM 接管 LVGL/字库/播放列表**：8MB 富余做缓存层，方向正确。
3. **小模块硬编码 SPIRAM + 显式 DRAM 回退** + FreeType 离线加载。
4. **诊断信息非周期化**（仅启动 + 扫描各一次）。
5. **BT 音箱功能已实现**（`bt_speaker.cpp` A2DP Sink），并有专用构建配方。

### ⚠️ 风险（修正后）
1. **构建缺口（已修复，见 §6）**：旧 `sdkconfig.bt_speaker` 只开 BT 协议栈、未开 `USE_BT_SPEAKER`，
   导致 BT 栈编入但蓝牙音箱功能本体不进固件。现已补 `CONFIG_USE_BT_SPEAKER=y`。
2. **OTA 2MB 槽约束**：BT 构建固件可能逼近 2MB，务必保持 Wi-Fi 关闭，否则超槽。

### ❌ 原版错误（已删除）
- "IRAM 100% 占满 / 禁新增 IRAM_ATTR"（实际 7.5%，余 324KB）。
- "BT 默认关闭 = 节省"（产品需要 BT 开；关的是 Wi-Fi）。
- "双无线都关省 2.8MB Flash + 320KB DRAM"（双算；正确终态是 Wi-Fi 关 + BT 开，净持平）。

### 可优化（非必要）
- LVGL partial buffer 25KB → 升全屏 150KB（PSRAM 充裕换流畅度）。P3
- FreeType 缓存 1024 → 256（省 PSRAM）。P4
- 主任务栈显式 DRAM（仿 LVGL 改法）。P2

---

## 5. 瓶颈排序（BT 构建）
**DRAM 通用堆 > Flash(应用段，但 OTA 槽有 2MB 约束) > PSRAM ≈ CPU ≈ IRAM（IRAM 健康）**

---

## 6. 已落地的构建缺口修复
`configs/sdkconfig.bt_speaker` 追加：
```
CONFIG_USE_BT_SPEAKER=y
```
联动生效：
- `main/CMakeLists.txt` 第 16-18 行 `if(CONFIG_USE_BT_SPEAKER) list(APPEND COMPONENT_REQUIRES bluetooth_service bt)` 触发 → 链接 ESP-ADF `bluetooth_service`。
- `main/bt_speaker.cpp` 守卫 `#if defined(CONFIG_USE_ESP_ADF) && defined(CONFIG_USE_BT_SPEAKER)` 满足 → A2DP Sink 编译进固件。
- 依赖链 OK：`USE_ESP_ADF` 默认 `y`，`USE_BT_SPEAKER` 依赖成立。
- `main/idf_component.yml` **无需改**：bluetooth_service 已由 CMakeLists 条件引入，不在 yml 声明。

> 使用方式：`configure.bat wroom-1-n16r8-bt`（或 `wroom-2-n32r16v-bt`）即注入本配方（BT 开 + Wi-Fi 关 + USE_BT_SPEAKER）。

---

## 7. 优化优先级（修正后）
| 优先级 | 动作 | 建议 |
|---|---|---|
| P0 | 用 `*-bt` 配方构建（BT 开 / Wi-Fi 关） | 已修复开关，直接可用 |
| P1 | 监控 BT 构建 bin 体积 ≤ 2MB（OTA 槽） | 保持 Wi-Fi 关 |
| P2 | 主任务栈显式 DRAM | 确定性提升 |
| P3 | LVGL buffer 升全屏 | 流畅度 |
| P4 | FreeType 缓存 1024→256 | 省 PSRAM |

**整体判定**：方案在"BT 开 / Wi-Fi 关"终态下资源合理且宽松。原版最大的两个问题——IRAM 误报占满、把 BT 关当成优点——均已纠正。当前真正约束是 DRAM 通用堆与 OTA 2MB 槽（后者靠"Wi-Fi 必关"保证）。
