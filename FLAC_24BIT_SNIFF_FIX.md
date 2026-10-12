# R132 — FLAC 24bit 仍崩（conv 错位）→ 嗅探位深 bug

## 现象
R129（FLAC 24bit→16bit 降级）烧录后，播放 `instrumental-48000hz-24bit.flac` 仍 `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)`，崩在 conv task 内部。
启动日志关键证据：
```
I (7990) audio_player: R122-FLAC-16BIT: downconvert 8bit -> 16bit (I2S 16bit path)
```
实际是 24bit 文件，却被识别为 **8bit** —— conv 用错误的 `src_bits=8` 去处理 24bit PCM，每样本步进错误导致大幅越界写 PSRAM。

## 根因
`flac_sniff_bits()`（`main/audio_player.cpp`）从 FLAC STREAMINFO 提取位深时写错字段。

FLAC STREAMINFO 布局（`d` 指向 STREAMINFO block data 起点，共 34 字节）：
- `d[10..12]` 高 20 位：sample_rate（其中 `d[12]` 高 4 位是采样率最低 4 位）
- `d[12]` bit[3:1]：**channels - 1**（声道数，**不是位深**）
- `d[12]` bit[0]：**bits-per-sample index 最高 1 位**
- `d[13]` bit[7:4]：**bits-per-sample index 低 4 位**
- `d[13]` bit[3:0]：total_samples 最高 4 位

实际位深 = 5-bit index + 1。

旧代码：
```c
int bps_idx = (d[12] >> 1) & 0x07;   // 误取声道数-1
int bps_map[] = {0, 8, 12, 16, 20, 24, 32};
int bits = (bps_idx >= 1 && bps_idx <= 6) ? bps_map[bps_idx] : 16;
```
对立体声 24bit FLAC：`channels-1 = 1` → `(d[12]>>1)&0x07 = 1` → `bps_map[1] = 8` → 误判 **8bit**。

## 修复
```c
// 正确提取 5-bit bits-per-sample 字段
int bps_index = ((d[12] & 0x01) << 4) | (d[13] >> 4);
int bits = bps_index + 1;
if (bits < 8 || bits > 32) bits = 16;   // 非法值兜底
return bits;
```
删除错误的 `bps_map` 数组。`bps_index` 直接对应 FLAC 规范定义的「bits per sample − 1」，+1 即得真实位深。

## 影响范围
- **FLAC 24/32bit**：conv `src_bits` 现在正确（24/32）→ `wav_bitdepth_conv` 正确降级 16bit，R129 的降级路径真正生效。
- **FLAC 16bit**：`bps_index=15` → `bits=16` → 不走 conv，保持直通（行为不变）。
- **FLAC 8bit**（罕见）：`bps_index=7` → `bits=8` → conv 8→16（正确）。
- **WAV / MP3 / AAC / OGG / OPUS**：不受影响（`flac_sniff_bits` 仅 FLAC 分支调用）。

## 验证
- 编译：`idf.py build` 通过（已烧录 COM7）。
- 真机确认（重点）：
  1. 播放 `instrumental-48000hz-24bit.flac` + 反复切歌 → 不再 `Guru Meditation`。
  2. 日志应显示 `downconvert 24bit -> 16bit`（旧版显示 `8bit` 即未生效）。
  3. 开启变速后重复 #1。
  4. FLAC 16bit / WAV 24·32bit / MP3 / AAC / OGG·OPUS 各播放+切歌 + 循环切歌压力测试。
- 注：启动日志 `Compile time` 仍可能滞留旧日期（见开发日志顶部 LESSON-0930），以日志中 `downconvert 24bit` 字符串 / `ELF file SHA256` 比对判新固件。

## R133 — B 止血：拒绝非16bit FLAC（conv 方案证伪后）

### 真机验证结果
R132 烧录后日志显示 `downconvert 24bit -> 16bit`（sniff 已正确），但**仍 Guru Meditation**。
addr2line 解析崩点为 `lv_obj_get_layer_type` / `lv_obj_refr`（LVGL 重绘）→ 越界发生在 **FLAC 解码器
输出 24bit PCM 写入 PSRAM 之时**（早于 conv）。查证 `esp_flac_dec_open` 文档：`cfg` 设为 NULL
（不需要）、**不支持任何配置** → esp_codec FLAC **必然输出源位深(24bit)，无法强制 16bit 输出**。
故 R129 的 conv 降级从架构上无效——越界在解码器内部，conv 在下游拦不住。

### 决策（用户选定"先B后C"）
- **B（本次）**：FLAC 只支持 16bit；非 16bit(24/32/8) 一律拒绝播放 + `display_toast` 屏上提示
  "FLAC must be 16bit / transcode pls"，并移除已废弃的 R129 conv 路径。16bit FLAC 仍直通。
- **C（后续）**：移植 dr_flac（公共域单文件 FLAC 解码器，可配置输出 16bit 整数 PCM）替换 esp_codec
  FLAC，从而保留 24bit 文件以 16bit 播放。工作量较大，排期另定。

### 影响范围
- 24/32/8bit FLAC：不再崩溃，友好提示后停在 STOPPED（跳过）。
- 16bit FLAC：正常直通播放。
- 其他格式（WAV 24/32bit via conv、MP3/AAC/OGG/OPUS）：不受影响（B 仅改 FLAC 分支）。

### 状态
编译通过（`idf.py build` app 分区 32% free）。首次烧录时 COM7 被 monitor 占用导致 esptool
校验失败（Checksum error / serial noise），关闭 monitor 后重烧即可。
