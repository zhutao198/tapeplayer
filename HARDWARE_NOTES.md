# 硬件事实记录（避免 AI 反复误判）

> **建立时间**：2026-08-19
> **目的**：把用户明确说明的硬件事实记录下来，AI 不再凭默认假设判断。
> **违反下面的事实 = 错误，必须纠正后才有可信输出**。

---

## 1. USB 转串口芯片引脚映射（COM7）

- **未接 RTS/DTR 引脚到 ESP32 自动下载电路**
- 后果：`esptool --before default_reset` / `Hard resetting via RTS pin` **完全无效**，芯片不会因此进入或退出下载模式
- 进入下载模式**只能人工操作**（按住 BOOT → 按 RESET → 松开 BOOT）
- 烧录完成后 `Hard resetting via RTS pin` 这一行**没有任何效果**，设备**不会自动复位跑新固件**
- 烧录完成后要测新固件，**必须用户自己按 RESET 键**

## 2. 供电与开机自锁

- 用户已用跳线**旁路开关机电路**
- 意味着 IO40 自锁（GPIO 拉高维持供电）在硬件上**不再是必要条件**
- 设备**掉不掉电与 IO40 拉高无关**
- 但代码里仍保留 IO40 拉高逻辑（兼容正常供电模式），**不影响功能**

### 2.1 跳线旁路后的 IO40 安全规则（重要）

跳线旁路后，IO40 任何后续操作（重复初始化、重复拉低/拉高）都**可能让硬件 latch 误触为关机脉冲**造成意外断电。代码必须遵守：

- **app_main 拉高 IO40 一次后**，禁止任何后续 `gpio_set_direction` / `gpio_set_level(IO40, ...)`
- `power_mgmt_power_off()` 必须禁用 IO40 拉低（即使"电池低要关机"）
- 通过 `TAPEBOOK_POWER_LATCH_BYPASSED` 编译宏控制：
  - `=0`（默认）: 正常工作模式，power_mgmt_power_off 拉低 IO40 释放 latch
  - `=1`（当前用户配置）: 跳线旁路模式，power_mgmt_power_off 只打警告不操作 IO40
- 真正"关机"在旁路模式下由用户在外部断电完成；电池低仅警告不强行断电（避免损坏电池）
- 未来若恢复硬件锁存电路，把 config.h 里 `TAPEBOOK_POWER_LATCH_BYPASSED` 改回 `0` 即可，**不需要改其它代码**

## 3. 其他硬件事实

- SD 卡插槽：SPI2 总线（已修复 mount_sd_card 缺 spi_bus_initialize 的 0x103 问题）
- LCD：ST7789，SPI3 总线，320x240
- 音频：I2S（BCLK=IO6, WS=IO7, DIN=IO5）
- PSRAM：8MB Octal（AP_3v3）
- Flash：16MB（DIO 80MHz）
- 串口日志引脚：GPIO 44 (TX) / GPIO 43 (RX)——与 USB 转串口芯片相连

---

## 4. 烧录后必须告诉用户的事

烧录完成后给用户的消息**不能**写以下内容：

- ❌ "设备已被 Hard resetting via RTS pin 自动复位"
- ❌ "应该已经在跑新固件"
- ❌ "重启后会显示 X"

正确的话术：
- ✅ "烧录完成（4 个分区 Hash verified）。由于硬件未接 RTS，需要你手动按 RESET 键复位，然后查看串口日志。"
- ✅ "请按 RESET 让设备从新烧的 0x10000 启动"

---

## 5. SHA 比对规则（避免反复误判"烧错了"）

### 三个相关 SHA 的来源

| 文件 | SHA 来源 | 用途 |
|------|---------|------|
| `audiobook_player.elf` | 编译器产物（GCC 链接后） | 编译时算 SHA 并嵌入 app image header |
| `audiobook_player.bin` | esptool 生成的 flash image | 实际写进设备 flash 的内容 |
| 设备串口 `ELF file SHA256` | 从 flash image header 读出 | 来自本地 ELF，不是从 flash 算的 |

### 唯一正确的比对规则

**A. 验证烧录是否成功（一次完整校验）：**
```
设备串口 ELF SHA256 == 本地 audiobook_player.elf 的 SHA256
```
两个值**完全一致**，因为设备读的 ELF SHA 就是本地 ELF 算出来嵌入到 bin header 的。

**B. 验证写入 flash 的内容是否正确（最强校验）：**
```
回读设备 flash 0x10000 的 SHA == 本地 audiobook_player.bin 的 SHA
```

### ❌ 错误比对（已多次误判，停止使用）

- ❌ `设备 ELF SHA == 本地 audiobook_player.bin 的 SHA` — 永远不等（不同文件）
- ❌ `本地 bin 的 SHA vs 本地 elf 的 SHA` — 永远不等（不同文件）

### 反思

- 之前对话里多次"ELF SHA 不匹配"的告警都是这个错。原因是把 bin 和 elf 当成同一个东西比 SHA。
- 实际烧录链路：ELF 编译 → 嵌入 SHA 进 image header → esptool 生成 bin → 烧入 flash → 设备读 header 显示 ELF SHA。
- 任何时候要报"烧错了"，必须做**正确的 A 比对**，否则不准信。