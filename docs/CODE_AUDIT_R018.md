# Code Audit Report (R018 批次)

> **审计范围**：仅代码（C/C++ 源码），不修改任何代码
> **审计日期**：2026-07-11
> **审计对象**：`main/*.cpp|.h` + `components/tapebook_board/*` + `main/CMakeLists.txt` + 顶层 `CMakeLists.txt` + `main/Kconfig.projbuild`
> **构建版本基线**：`R017` commit `49f6421`
> **审计者**：Claude（按全局 CLAUDE.md 第 5/8/9 节规范）

---

## 0. 摘要

| 严重等级 | 数量 | 关键发现 |
|---|---|---|
| **Critical**（必修，影响功能/可靠性） | 6 | deep_sleep 无唤醒源导致永久睡眠；position 计算累加暂停时长错误；环形书签覆盖逻辑错乱；CMake 引用不存在的路径；display HAL 形参传递可能失效；板级组件与 main 引脚冲突 |
| **High**（应修，影响正确性/可维护性） | 7 | fingerprint 缺 speed 参数；EXT1 wakeup 范围未核实；音量映射整数精度损失；NVS 错误处理只 LOG；tick 周期不稳定；DT_REG 在 FAT 上不可靠；auto_off NVS 启动恢复缺失 |
| **Medium**（建议修） | 11 | 全局 `-Wno-error`；长按状态 RELEASE 丢失；多 API 返回值未检查；自动保存无差别 flush；回调内同步 NVS 写；等 |
| **Low**（可优化） | 9 | dead code；栈变量重复；#pragma 抑制范围过大；flash 磨损；负数处理；等 |

**总体结论**：项目主体功能（V1.0 MVP）可构建运行，但存在 **6 个 Critical 问题**必须在量产前修复，其中 `audio_player.cpp` 的 position 计算和 `main.cpp` 的 deep_sleep 路径属于"测试不易暴露但运行中必现"的隐患。

---

## 1. Critical（必修）

### C-1：`main.cpp:643` deep_sleep 调用无任何唤醒源 → 系统永久睡眠

**位置**：`main/main.cpp:637-644`
```cpp
if (power_mgmt_should_shutdown()) {
    ESP_LOGE(TAG, "Battery critical, saving state and shutting down");
    save_current_position();
    settings_flush();
    audio_player_stop();
    esp_deep_sleep_start();   // <-- 无 esp_sleep_enable_* 调用
}
```

**问题**：进入 deep sleep 前**从未调用任何 `esp_sleep_enable_*`**，esp_deep_sleep_start() 默认仅启用 RTC Timer wakeup（未配置），其他 wakeup 源为零。设备将**永久无法唤醒**，需人工按 RESET。

**影响**：电量低自动关机 → 用户次日长按也开不了机器（电池持续耗电 → 最终彻底无电）。

**修复建议**：
```cpp
esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
// 或：esp_sleep_enable_timer_wakeup(3600 * 1000000ULL);  // 1h 后尝试唤醒做状态报告
esp_deep_sleep_start();
```

---

### C-2：`audio_player.cpp:227,316-320` `g_play_offset_us` 累加的语义错误

**位置**：`main/audio_player.cpp:223-233, 311-321`
```cpp
void audio_player_resume(void)
{
    if (g_is_playing && g_is_paused && g_pipeline) {
        // 累计暂停前的播放时间     ← 注释是错的
        g_play_offset_us += (int64_t)(esp_timer_get_time() - g_pause_start_us);
        ...
    }
}

int audio_player_get_position_ms(void)
{
    int64_t total = g_play_offset_us;
    if (g_is_playing && !g_is_paused && g_play_start_us > 0) {
        total += (int64_t)(esp_timer_get_time() - g_play_start_us);
    }
    return (int)(total / 1000);
}
```

**问题**：`g_play_offset_us` 累加的是 `(now - g_pause_start_us)`，这是**暂停时长**而非"已播放时长"。

**场景复现**：
- 播放 10s → position = 10s ✓
- 暂停 5s → position = 0s ✓
- 恢复后再播 1s → 实际：`g_play_offset_us = 0 + 5s`，`g_play_start_us` 重置 → `position = 5 + 1 = 6s`
- **期望**：`position = 11s`（实际已播放时间）

**影响**：每次暂停后，UI 显示的进度会比真实进度**减少"暂停时长"秒数**；书签 save 写入 NVS 后下次恢复会跳到错误位置。

**修复建议**：
```cpp
// pause 时
g_pause_start_us = esp_timer_get_time();

// resume 时
g_play_offset_us = (int64_t)(esp_timer_get_time() - g_play_start_us);  // 不是 +=
g_play_start_us = esp_timer_get_time();
g_pause_start_us = 0;
```
或者增加独立 `g_played_before_pause_us` 变量。

---

### C-3：`bookmark.cpp:47-65` 环形覆盖移位逻辑错乱

**位置**：`main/bookmark.cpp:34-77`
```cpp
int32_t slots[BOOKMARK_MAX_PER_FILE];
int occupied = 0;
for (int i = 0; i < BOOKMARK_MAX_PER_FILE; i++) {
    ...
    if (err == ESP_OK) {
        slots[occupied++] = val;          // 按遍历顺序填充
    } else if (slot < 0) {
        slot = i;
    }
}

if (slot < 0) {
    // 满：擦除最旧（slot 0），向前移位，新书签放末尾
    for (int i = 0; i < BOOKMARK_MAX_PER_FILE - 1; i++) {
        char key_old[24], key_new[24];
        make_key(file_idx, i, key_old, sizeof(key_old));
        make_key(file_idx, i + 1, key_new, sizeof(key_new));
        err = nvs_set_i32(g_bm_handle, key_old, slots[i + 1]);  // <-- 用 i+1 写 i
        if (err != ESP_OK) break;
    }
    slot = BOOKMARK_MAX_PER_FILE - 1;
    ...
}
```

**问题**：
- `slots[]` 数组按 **NVS 遍历顺序**填充（与物理 slot 号无关），可能 `slots[0] = bm_3 的值`（若 bm_0 不存在）
- 循环 `nvs_set_i32(key=bm_i, value=slots[i+1])` 是 **shuffle forward**：所有 slot 都被覆盖
- "擦除 slot 0" 的注释与实际行为不符

**影响**：满时再添加书签，**原有数据被复制到不同 slot，没有真正丢弃**。书签列表显示混乱，且 NVS 写入次数被翻倍（10 次写而非 1 次 commit）。

**修复建议**：
```cpp
if (slot < 0) {
    // 真正"删除最旧（slot 0）"语义
    nvs_erase_key(g_bm_handle, "bm_X_0");
    for (int i = 1; i < BOOKMARK_MAX_PER_FILE; i++) {
        // 读 slot i → 写到 slot i-1
        ...
    }
    slot = BOOKMARK_MAX_PER_FILE - 1;
    nvs_set_i32(...);
}
```

---

### C-4：`main/CMakeLists.txt:14` 硬编码路径在删除的目录上

**位置**：`main/CMakeLists.txt:13-18`
```cmake
# R014: 直接把 u8g2_esp32_hal.c 编译进 main 组件（避免静态库链接顺序问题）
"${CMAKE_CURRENT_LIST_DIR}/../components/u8g2_esp32_hal/u8g2_esp32_hal.c"
INCLUDE_DIRS
    "."
    # R014: u8g2_esp32_hal.h 头文件路径
    "${CMAKE_CURRENT_LIST_DIR}/../components/u8g2_esp32_hal/include"
```

**问题**：
- `components/u8g2_esp32_hal/` 在文件系统下**确实存在**（审计中确认）
- 但 git status 显示它是**未跟踪**（untracked）状态，且未在 `idf_component.yml` 中声明
- 如未来清理 `components/u8g2_esp32_hal/`（按 R017 的修改方向），CMake 会**静默使用空路径**
- 路径**与 `idf_component.yml` 机制重复**——一个文件既出现在 main SRCS 又在子目录组件 SRCS，会产生链接冲突

**影响**：build 顺序隐患；维护时易遗忘该耦合；CMake 路径失效不会报清晰错误。

**建议**：
- 把 `u8g2_esp32_hal/` 整合为 `managed_component` 或独立 `components/` 子目录组件
- main 组件移除直接 SRCS 引用

---

### C-5：`display.cpp:58-61` u8g2_esp32_hal_init 形参传递

**位置**：`main/display.cpp:56-71`
```cpp
void display_init(void)
{
    u8g2_esp32_hal_t u8g2_esp32_hal;             // 栈变量
    u8g2_esp32_hal.sda = DISPLAY_SDA_IO;
    u8g2_esp32_hal.scl = DISPLAY_SCL_IO;
    u8g2_esp32_hal_init(u8g2_esp32_hal);         // 按值传参！
    ...
}
```

**问题**：
- 按值传 struct，函数内部**复制**了结构体
- 如果 `u8g2_esp32_hal_init()` 内部**只保存指针或引用**到传入的 struct（多数 HAL 实现都是保存一份到模块全局），这里 OK
- 但如果它保存的是**对栈地址的引用**，函数返回后该引用**失效**
- 同时**后续任何对 HAL 的重新配置**都不会生效

**风险**：依赖 `u8g2_esp32_hal` 实现细节；如果它是一个 ESP-IDF component 提供的库（status: from component），行为不透明。

**建议**：将 HAL 结构体升级为全局静态：
```cpp
static u8g2_esp32_hal_t s_hal_config = {
    .sda = DISPLAY_SDA_IO,
    .scl = DISPLAY_SCL_IO,
};
u8g2_esp32_hal_init(s_hal_config);
```
并查证 HAL init 的内部语义。

---

### C-6：`board_pins_config.c:33-39` 与 `config.h:17` MCLK 引脚冲突

**位置**：
- `main/config.h:13-17`：`MAX98357A 不需要 MCLK，I2S_MCLK_IO 删除`
- `components/tapebook_board/tapebook_board_v1_0/board_pins_config.c:33-39`：
  ```c
  i2s_config->mck_io_num = GPIO_NUM_7;   // 与 main 注释矛盾
  ```

**问题**：
- 两个文件对同一物理信号的描述矛盾
- 如果 `tapebook_board` 组件**实际未被使用**（main.cpp 注释掉 `#include "board.h"`），则 board_pins_config 的设置**不会生效**
- 但若未来某 PR 启用 board 组件，**MCLK 会被驱动到 GPIO7**——而 ESP32-S3 GPIO7 是 **Strapping Pin（用于启动模式选择）**！

**影响**：启用 board 组件后启动失败/卡 bootloader；GPIO7 输出 MCLK 时钟会干扰启动时序。

**建议**：
- 在 `board_pins_config.c` 显式设 `mck_io_num = -1` 与 main 对齐
- 或：从 build 中彻底移除 `tapebook_board` 组件（确认实际未被调用）

---

## 2. High（应该修复）

### H-1：`display.cpp:39-52` 屏保 fingerprint 缺少 speed 参数

```cpp
static uint32_t calc_fingerprint(player_state_t state,
                                  int track_idx, int total,
                                  int current_sec, int total_sec,
                                  int gear, int volume) {
    // 缺 speed
}
```

**问题**：用户长按 FF 时速度从 1.0x → 1.5x → 2.5x，**屏幕指纹不变**，UI 不刷新（屏保判断错），且底部小字"<<Prev..." 状态行不更新。

**建议**：把 `tape_control_get_speed()` 也加入 fingerprint；或独立维护一个低频状态行刷新流程。

---

### H-2：`main.cpp:656-662` EXT1 wakeup GPIO 范围未严格核对 ESP32-S3 RTC IO

```cpp
uint64_t wakeup_mask = 0;
wakeup_mask |= (1ULL << BTN_PLAY_PAUSE) | (1ULL << BTN_STOP);
wakeup_mask |= (1ULL << BTN_PREV) | (1ULL << BTN_NEXT);
wakeup_mask |= (1ULL << BTN_REWIND) | (1ULL << BTN_FAST_FORWARD);
esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
```

**问题**：
- ESP32-S3 的 EXT1 wakeup **仅支持 RTC GPIO**（GPIO0-21）
- 当前按键 GPIO：1, 2, 8, 9, 14, 15 — **均在 RTC IO 范围内** ✓（初步 OK）
- 但 ESP-IDF v5.5+ 增加了 `esp_sleep_is_valid_wakeup_gpio()` 校验，**未调用此函数**验证
- 实际 ESP32-S3 die 版本（ESP32-S3 v0.1 / v0.2 / v0.4）的 RTC IO 列表略不同

**建议**：
- 在 `init_hardware()` 末尾调用 `esp_sleep_is_valid_wakeup_gpio()` 做断言
- 在 `CONTEXT.md` 记录所用的 ESP32-S3 die 版本

---

### H-3：`audio_player.cpp:375-382` set_volume 整数除法精度损失

```cpp
if (volume <= 0) {
    alc_vol = -96;
} else if (volume <= 50) {
    alc_vol = (volume - 50) * 48 / 50;       // 整数除法
} else {
    alc_vol = (volume - 50) * 12 / 50;       // 整数除法
}
```

**问题**：
- `volume=51`：(51-50) × 12 / 50 = **0**（整数除法）
- `volume=58`：(58-50) × 12 / 50 = 96 / 50 = **1**
- volume 51..58 全部映射到 alc_vol=0..1，**有效档位严重浪费**
- 类似 volume=1..7 段：alc_vol = -47..-43（间隔 1），但 volume=8..10 也是 -43..-41

**建议**：
```cpp
alc_vol = (int)(((volume - 50) * 12.0f) / 50.0f);
```
或查表查找。

---

### H-4：`settings.cpp:46-59` NVS 错误处理只 LOG 不中止

```cpp
err = nvs_set_u16(...);
if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u16(%s) failed: 0x%x", ...);
// 继续写下一个，不 return
```

**问题**：
- `nvs_commit` 失败不会重试，**已写入的 set 可能未提交到 flash**
- 多个 SET 之间部分失败无法定位，调试困难
- 静默失败：调用方无法感知状态不一致

**建议**：
- 第一个 error 就 return false（让调用方决定是否重试）
- 或：批量收集到本地结构，逐一 try，最终一次 commit，commit 失败再 LOG

---

### H-5：`main.cpp:583-700` 主循环周期不稳定（非 RT）

```cpp
while (1) {
    handle_button_events();           // 耗时不稳定
    tape_control_tick();
    audio_player_tick();              // 主要耗时在 NVS / pipeline tick
    ...
    vTaskDelay(pdMS_TO_TICKS(BTN_SCAN_INTERVAL));   // 最后 delay，无前段补偿
}
```

**问题**：
- 实际循环周期 = `BTN_SCAN_INTERVAL + handle_total_time`，通常比 20ms 多 5-15ms
- WDT reset 在末尾，**主循环阻塞时间受 audio_player_tick 影响**
- 长时间阻塞会触发 TWDT panic（即使 timeout=10s，单次回调阻塞 8s 也会触发）

**建议**：
- 把 vTaskDelay 改为基于 esp_timer 的**绝对时间对齐**：
  ```cpp
  int64_t next_deadline = esp_timer_get_time() + BTN_SCAN_INTERVAL * 1000;
  while (1) {
      ...
      int64_t now = esp_timer_get_time();
      if (now < next_deadline) vTaskDelay(pdMS_TO_TICKS((next_deadline - now) / 1000));
      next_deadline += BTN_SCAN_INTERVAL * 1000;
  }
  ```
- audio_player_tick 中的阻塞操作（注册回调内 NVS 写入）应异步化

---

### H-6：`playlist.cpp:99,115` `DT_REG`/`DT_DIR` 在 FAT 上不可靠

```cpp
if (entry->d_type == DT_REG && playlist_is_audio_file(entry->d_name)) {
    ...
} else if (entry->d_type == DT_DIR) {
    ...
}
```

**问题**：
- FATFS（`esp_vfs_fat`）的 `dirent.d_type` **可能填 0（DT_UNKNOWN）**
- ESP-IDF FATFS 实现的确切行为需查源码（不同版本表现不一致）

**建议**：
```cpp
struct stat st;
if (stat(full_path, &st) == 0) {
    if (S_ISREG(st.st_mode)) { ... }
    else if (S_ISDIR(st.st_mode)) { ... }
}
```

---

### H-7：`power_mgmt.cpp` 启动未从 NVS 恢复 auto_off

**问题**：
- `settings_save_auto_off()` / `settings_load_auto_off()` 在 settings.cpp 存在
- `power_mgmt_init()` **未调用** `settings_load_auto_off()`，auto_off 分钟数永远 = 0
- `power_mgmt_set_auto_off()` 在 main.cpp 中**未被调用**

**影响**：用户在设置里改了定时关机值，重启后定时关机**永久失效**。

**修复**：
```cpp
void power_mgmt_init(void) {
    g_auto_off_min = settings_load_auto_off();
    ...
}
```

---

## 3. Medium（建议修复）

| ID | 位置 | 问题 | 建议 |
|---|---|---|---|
| M-1 | `CMakeLists.txt:16` | `add_compile_options(-Wno-error=deprecated-declarations)` 全局抑制 | 限定到 ADF 组件 `target_compile_options(adf_target PRIVATE ...)` |
| M-2 | `button_manager.cpp:194-200` | `BTN_STATE_LONG_PRESS` → IDLE 时不发 RELEASE | 长按 < HOLD 阈值（< 20ms）松开时音量保存不触发 |
| M-3 | `bookmark.cpp:104-105` | `label="BM%d", i+1` 与 bm_idx=0..9 不一致 | 改为 `i` 或在调用方文档明确 |
| M-4 | `audio_player.cpp:113,140,148` | `i2s_stream_init`/`fatfs_stream_init`/`create_decoder` 返回值未 NULL 检查 | 已有部分检查，建议统一错误处理宏 |
| M-5 | `audio_player.cpp:165,181` | `audio_pipeline_link` / `set_uri` 返回值未检查 | ESP_LOG 至少记录 |
| M-6 | `audio_player.cpp:288-298` | `set_byte_pos` 在 VBR MP3 上精度低（按字节/总字节比例） | 用 ADF 的 time-based seek API（如果有）或接受限制并在文档声明 |
| M-7 | `audio_player.cpp:449` | playback 中 `seek_ms_internal` 不 pause/resume pipeline，**潜在爆音** | 商业做法：mute → seek → unmute |
| M-8 | `main.cpp:608-617` | auto_save 每 30 秒 flush **无论状态** | 仅 PLAY/PAUSED flush，或在 STOPPED 时跳过 |
| M-9 | `main.cpp:162-194` | `on_track_finished` 回调内同步 `settings_save_position()`（NVS write）阻塞回调 | 移到 `g_pending_*` 异步执行 |
| M-10 | `main.cpp:282-284` | `bookmark_add` 返回 -1 静默 | 调用方应感知失败 |
| M-11 | `main.cpp:660` | EXT1 wakeup mask 无 RTC IO 校验 | 见 H-2 |

---

## 4. Low（可优化）

| ID | 位置 | 问题 |
|---|---|---|
| L-1 | `tape_control.cpp` | 4 档加速阶梯硬编码，无单位测试；用户期望 0.8/2/4/7s 与实际 tick 调度一致性需 stress test |
| L-2 | `voice_prompt.cpp` | 整模块无 main.cpp 调用（dead code），但保留 stub 是为 V1.2 准备。建议加注释 `#if defined(V1_2_ENABLE)` |
| L-3 | `bookmark.cpp:99,116` | `bookmark_jump()` 在 main.cpp **未调用**（dead code） |
| L-4 | `power_mgmt.cpp` | `power_mgmt_set_auto_off()` 在 main.cpp **未调用**（dead code） |
| L-5 | `bookmark.cpp:62,74` | `nvs_commit()` 每次 add 立即执行 → 同一曲目连续添加触发 N 次 flash write |
| L-6 | `playlist.cpp:17-21` | `#pragma GCC diagnostic ignored "-Wformat-truncation"` **整文件**抑制 |
| L-7 | `playlist.cpp:75` | `qsort` 按 display_name 排序，子目录文件名带 prefix，导致"目录分组排序"非用户预期（混合排序） |
| L-8 | `playlist.cpp:96-97` | `System Volume Information` 硬编码白名单，但 Windows 上其他隐藏目录（RECYCLE.BIN、$RECYCLE.BIN）未屏蔽 |
| L-9 | `bookmark.cpp:36,52,68` | 循环内反复声明 `char key[24]`，可提到外层或 declare once |
| L-10 | `button_manager.cpp:110-113` | 每次 scan 重新计算时间常量（轻微性能） |
| L-11 | `display.cpp:130-139` | `format_time()` 未处理负数（虽然外部已 clamp，但防御性缺失） |
| L-12 | `display.cpp:62,67` | `u8x8_SetI2CAddress` 在 `Setup_ssd1306_i2c_128x64_noname_f` **之后**调用，顺序依赖 u8g2 内部 API 行为 |
| L-13 | `main.cpp:316-318,341-343` | PREV/NEXT RELEASE 时保存音量，但每次都写 NVS（依懒 settings.cpp flush） |
| L-14 | `CMakeLists.txt:6-12` | `ADF_PATH` 未设置时仅 WARN 不中止，CONFIG_USE_ESP_ADF 仍 y → 编译失败而非优雅降级 |
| L-15 | `Kconfig.projbuild` | `CONFIG_USE_U8G2` 与 `CONFIG_USE_ESP_ADF` 互不感知，但任何为 n 时依赖组件缺失需手动处理 |

---

## 5. 正面观察（不修，仅记录）

- **多层状态机设计**：button_manager 的 PRESSED→LONG_PRESS→HOLD→RELEASE 状态完整，extra_long 用于按键锁定复用合理
- **PSRAM 回退 malloc**：playlist.cpp 在 PSRAM 失败时回退到普通 DRAM，**降级而非失败**
- **NVS namespace 分桶**：key 前缀 `book_X_name` / `bm_X_Y` 避免不同域冲突
- **CRC-free 位置恢复校验**：用文件名 strcasecmp 比对避免 CRC 开销
- **WDT 双订阅**：TWDT add(NULL) + main loop reset 模式 OK
- **统一 flush 策略**：settings_flush() 集中提交降低 flash 磨损
- **audio_pipeline 每次重建**：避免 ADF 的 terminate 复用 Bug（S-09）
- **错误恢复路径**：`nvs_flash_init` 失败自动 erase 重试

---

## 6. 测试覆盖度观察

| 模块 | 单元测试 | 集成测试 | 建议 |
|---|---|---|---|
| audio_player | ❌ | ⚠️ (仅构建通过) | 需要 PCM/MP3 样本 |
| button_manager | ❌ | ⚠️ | 写状态机表驱动测试 |
| tape_control | ❌ | ❌ | 加时长到档位映射的 unit test |
| playlist | ❌ | ⚠️ | mock dirent 测试 qsort 边界 |
| power_mgmt | ❌ | ❌ | auto_off 边界测试 |
| settings | ❌ | ⚠️ | NVS 满/坏块测试 |
| bookmark | ❌ | ⚠️ | 循环覆盖场景测试（修 C-3 后必做） |
| display | ❌ | ⚠️ | I2C 总线错误注入 |

---

## 7. 修复优先级建议

1. **本批次必修**（4 项）：C-2（position）、C-3（书签环形覆盖）、C-6（MCLK 冲突）、C-1（deep sleep 唤醒源）
2. **下次合并必修**（2 项）：C-4（CMake 路径）、C-5（HAL 形参）
3. **V1.1 必修**：H-7（auto_off NVS 恢复）、H-3（音量精度）
4. **持续清理**：H-1、Medium 部分（按代码触及范围排序）

---

## 8. 审计方法论说明

- 本次**仅静态代码审计**，未运行真实设备测试
- 重点关注：状态机正确性、错误处理、并发安全、资源泄露、API 用法正确性
- 未涉及：性能 profiling、功耗测量、PCB 验证、音频质量主观评价
- 未审计：文档（已通过独立 R 节点管理）、硬件设计评审（走 HARDWARE_REVIEW 系列）

---

**审计结论**：项目主体可构建运行（基于 R017 commit `49f6421`），V1.0 MVP 11/11 功能闭环（R012）。但存在 **6 个 Critical** 隐患必须修复后才能进入 V1.1/V1.2 阶段。建议在 **R018 节点**一次性修复 C-1/C-2/C-3/C-6，并在 R019 修复 C-4/C-5。

