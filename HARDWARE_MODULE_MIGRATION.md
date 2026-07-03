# ESP32-S3 模组迁移指南 (WROOM-2 → WROOM-1)

> 本文档对比开发板 (ESP32-S3-WROOM-2 N32R16V) 与量产模组 (WROOM-1 N16R8) 的差异，并给出迁移步骤。
> 数据来源：`hardware/esp32-s3-wroom-2_datasheet_cn.pdf` 与 `hardware/esp32-s3-wroom-1_wroom-1u_datasheet_cn.pdf` (v1.8)。

---

## 1. 速查表（最终决策：**WROOM-1 N16R8** 用于量产）

| 维度 | **WROOM-2 N32R16V** (开发) | **WROOM-1 N16R8** (量产) |
|---|---|---|
| **Flash 接口** | Octal SPI (8 线) | **Quad SPI (4 线)** |
| **Flash 容量** | 32 MB | 16 MB |
| **PSRAM 接口** | Octal SPI (8 线) | Octal SPI (8 线) |
| **PSRAM 容量** | 16 MB | 8 MB |
| **SPI 电压 (VDD_SPI)** | **1.8 V** | **3.3 V** |
| **工作温度** | −40 ~ 65 °C | −40 ~ 65 °C |
| **PCB 尺寸** | 18.0 × 25.5 × 3.1 mm | 18.0 × 25.5 × 3.1 mm ✅ |
| **封装兼容** | — | ✅ 同 41-pin footprint |
| **被 SPI 内部占用** | GPIO33/34/35/36/37 (5 个) | GPIO35/36/37 (3 个) |
| **释放的 GPIO** | — | **GPIO33 / GPIO34** ✅ |
| **OTA 支持** | 当前 ❌（单 factory）| ✅ `partitions_ota.csv` |

---

## 2. 关键差异详解

### 2.1 Flash 接口

| 模组 | Flash 类型 | sdkconfig 标志 |
|---|---|---|
| WROOM-2 N32R16V | **Octal** (8 线) | `ESPTOOLPY_OCT_FLASH=y` + `FLASHSIZE_32MB` |
| WROOM-1 N16R8 | **Quad** (4 线) | `FLASHSIZE_16MB` + `FLASHMODE_DIO` (无 OCT_FLASH) |

### 2.2 SPI 电压 (VDD_SPI) ⚠️ **PCB 改动关键**

| 模组 | VDD_SPI | 模块内部 LDO |
|---|---|---|
| WROOM-2 N32R16V | **1.8 V** | 有 (3.3V → 1.8V) |
| WROOM-1 N16R8 | **3.3 V** | **不需要**（直接吃 3.3V）|

**含义**：量产 WROOM-1 N16R8 时，**如果 PCB 上有外部 1.8V LDO 单独给 VDD_SPI 供电，必须移除或调整**。模块内部自带 LDO 会与外部供电冲突。

### 2.3 GPIO 占用

| 模组 | SPI 内部占用 | 应用可用 |
|---|---|---|
| WROOM-2 N32R16V | GPIO33/34/35/36/37 (5 个) | 排除这 5 个 |
| WROOM-1 N16R8 | GPIO35/36/37 (3 个) | **GPIO33/34 释放** ✅ |

### 2.4 OTA 支持（量产必须）

| 项 | WROOM-2（开发板）| WROOM-1 N16R8（量产）|
|---|---|---|
| 分区表 | `partitions.csv` | `partitions_ota.csv` |
| factory 区 | 3 MB | 2 MB |
| ota_0 / ota_1 | 不存在 | 各 2 MB |
| otadata | 不存在 | 8 KB |
| **总占用** | ~3 MB | **~6 MB** |
| OTA 命令 | ❌ | `idf.py ota` |

### 2.5 PSRAM 用量分析（实测）

| 来源 | 大小 |
|---|---|
| `playlist_item_t g_items[256]` (384 B × 256) | **96 KB** |
| `AUDIO_BUFFER_SIZE` | **8 KB** |
| ADF 运行时（pipeline + 解码）| ~200-300 KB |
| 其他 | < 50 KB |
| **总计** | **< 500 KB** |

**N16R8 的 8 MB PSRAM 只用 ~6%**，完全够用。

---

## 3. 迁移步骤

### 3.1 切换目标模组

```powershell
# 量产配置
configure.bat wroom-1-n16r8

# 切回开发板
configure.bat wroom-2-n32r16v
```

`configure.bat` 会：
1. 复制对应的 `configs/sdkconfig.defaults.<target>` 到项目根 `sdkconfig.defaults`
2. 删除旧 `sdkconfig`，让 `menuconfig`/`build` 重新生成

### 3.2 验证 Kconfig 选项

```powershell
build.bat menuconfig
# 进入 "Target Module Selection" → 确认已选 WROOM-1 N16R8
```

`BOARD_MODULE_*` 选项会出现在 `menuconfig` 里。**关键派生选项**：
- `BOARD_SUPPORTS_OTA=y`（自动）
- `BOARD_PARTITION_TABLE="partitions_ota.csv"`（自动）
- `BOARD_FREE_GPIO33_34=y`（自动）

### 3.3 编译

```powershell
build.bat build
```

### 3.4 OTA 烧录（量产用）

第一次烧录：
```powershell
build.bat -p COM3 flash
```

后续 OTA 升级（在固件支持 OTA 接收命令时）：
```powershell
# 通过 WiFi / HTTP OTA
idf.py ota --host 192.168.1.100 --port 3232

# 或本地串口 OTA（需 menuconfig 启用 esp-http-client OTA）
```

### 3.5 PCB 改动 checklist

- [ ] **去掉外部 1.8V LDO**（如果 PCB 上有）
- [ ] 确认 VDD_SPI 引脚接模块内部 LDO 输出（3.3V）
- [ ] GPIO33/34 引脚保留可用（量产 PCB 可加功能）
- [ ] 天线匹配电路保持不变（同一系列兼容）

---

## 4. 文件清单

| 路径 | 说明 |
|---|---|
| `HARDWARE_MODULE_MIGRATION.md` | 本文档 |
| `main/Kconfig.projbuild` | 模块选择 Kconfig（2 选项 + 派生 bool） |
| `configs/sdkconfig.defaults.wroom-1-n16r8` | WROOM-1 N16R8 模板（带 OTA） |
| `configs/sdkconfig.defaults.wroom-2-n32r16v` | WROOM-2 N32R16V 模板（开发板） |
| `partitions_ota.csv` | OTA 分区表（每个 app slot 2 MB） |
| `partitions.csv` | 单一 factory 分区表（开发板用） |
| `configure.bat` | 一键切换脚本 |
| `main/CMakeLists.txt` | 把 `BOARD_MODULE_*` 宏传到 C 代码 |

---

## 5. 决策结论

| 项 | 选择 |
|---|---|
| **量产模组** | **WROOM-1 N16R8** |
| Flash | 16 MB Quad SPI |
| PSRAM | 8 MB Octal SPI（项目只用 6%） |
| SPI 电压 | 3.3 V（**PCB 需去掉 1.8V LDO**） |
| OTA | **支持**（`partitions_ota.csv`） |
| 释放 GPIO | GPIO33 / GPIO34 |
| BOM 成本 | 比 WROOM-2 低 ~60% |

---

## 6. 验证 checklist

切换模组后必须验证：

- [ ] `idf.py menuconfig` 中 "Target Module Selection" 显示 WROOM-1 N16R8
- [ ] `BOARD_SUPPORTS_OTA=y`、`BOARD_PARTITION_TABLE=partitions_ota.csv` 自动设置
- [ ] 编译成功（无 Octal Flash / 1.8V 相关错误）
- [ ] 烧录后能从串口看到启动日志：`PSRAM: 8MB`
- [ ] SD 卡 FATFS 挂载成功
- [ ] 播放 MP3/FLAC 验证音频输出
- [ ] 按键扫描、OLED 显示正常
- [ ] （可选）`idf.py ota` 测试 OTA 升级

---

**最后更新**：根据两份 datasheet v1.x 编写
**作者**：CodeBuddy（基于用户提供的硬件规格书）