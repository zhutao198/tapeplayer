# 计划：为项目建立自定义 ADF board（tapebook）

## 目标

在项目内以 `components/audio_board` **覆盖** ADF 自带的 `audio_board` 组件，新增自定义 `tapebook` 板（桩实现），替换当前仅用于“让 ADF 编译通过”的 `esp32_s3_box_3` 占位板，使构建不再依赖任何现成开发板。

## 为什么用“项目内覆盖”而不是改 ADF 源码

项目根 `CMakeLists.txt` 通过 `EXTRA_COMPONENT_DIRS = $ENV{ADF_PATH}/components` 引入 ADF。
IDF 的组件解析顺序**优先**搜索项目自身 `components/`，因此：

- 在 `components/audio_board` 建同名组件即可**覆盖** ADF 自带版本；
- 不改动全局 ADF 安装（`D:\esp\esp-adf`），方案可随仓库版本化、不受 ADF 更新影响。

## 关键事实（已探查确认）

- `esp_peripherals/lib/sdcard/sdcard.c` **无条件**引用 13 个 `ESP_SD_PIN_*` 宏
  （CLK/CMD/D0-D7/CD/WP）→ `tapebook/board_def.h` **必须**全部定义，值仅用于编译。
- `audio_stream` 组件仅 `REQUIRES audio_board` 作为构建依赖，对 `board_def.h` 的
  `AUDIO_CODEC_*` 宏**无任何引用** → board_def.h 不强制含 codec 宏。
- `CONFIG_ESP32_S3_BOX_3_BOARD` 仅被 `audio_board/CMakeLists.txt` 引用（移除安全）；
  `CONFIG_ESP_LYRAT_V4_3_BOARD` 虽在 `audio_hal/driver/es8388/*` 出现，但 box_3 模式下本就为
  false，切换后行为不变（无回归）。
- 项目 `main` 未调用 `audio_board_init()`（I2S/LCD/SD 均由自写驱动接管）→
  `tapebook/board.c` 只需编译通过的桩函数，无需真实 codec/LCD 逻辑，可去除对
  `esp_lcd_ili9341`/`tca9554` 等重依赖。

## 实施步骤（5 步）

### 1. 骨架
- `components/audio_board/CMakeLists.txt`：基础 `COMPONENT_ADD_INCLUDEDIRS ./include` +
  `COMPONENT_PRIV_REQUIRES esp_peripherals audio_sal audio_hal esp_dispatcher display_service`，
  仅保留 `CONFIG_ESP_TAPEBOOK_BOARD` 分支（含 `tapebook/board.c` 与 `tapebook/board_pins_config.c`）。
- `components/audio_board/Kconfig.projbuild`：`choice AUDIO_BOARD` 内新增
  `config ESP_TAPEBOOK_BOARD bool "Tapebook (custom)"`，并设为 default。
- `components/audio_board/include/board_pins_config.h`：沿用 ADF 通用共享头（声明 `get_*()` 等，无引脚值）。

### 2. 实现 tapebook 板
- `tapebook/board.h`：复制 box_3 的 API 头（声明 `audio_board_init()` 等）。
- `tapebook/board_def.h`：**必须**定义 13 个 `ESP_SD_PIN_*` 宏（全部置 -1，因项目用自写 SD 驱动，运行时不生效）；
  另定义 `SDCARD_*`、`HEADPHONE_DETECT`、`PA_ENABLE_GPIO`、`BUTTON_*_ID` 供 `board_pins_config.c` 使用。
- `tapebook/board.c`：最小桩函数（`audio_board_init()` 等返回 NULL/ESP_OK），不依赖 codec/LCD。
- `tapebook/board_pins_config.c`：实现 `get_*()` 桩，返回宏值或 -1。

### 3. 切换选板
- `configs/sdkconfig.defaults.wroom-1-n16r8`：改为
  `CONFIG_ESP_LYRAT_V4_3_BOARD=n` / `CONFIG_ESP32_S3_BOX_3_BOARD=n` / `CONFIG_ESP_TAPEBOOK_BOARD=y`，
  并更新注释说明自定义板用途。

### 4. 验证构建
- `configure.bat wroom-1-n16r8` → `build.bat build`
- 确认 box_3 退出构建、覆盖生效、固件重新生成、无 `ESP_SD_PIN_*` 未定义错误。

### 5. 文档
- 在 `DESIGN.md` / `README` 记录“项目内覆盖 ADF audio_board”的方案与原因。

## 风险与对策

- **旧 sdkconfig 残留**：`configure.bat` 已删除旧 `sdkconfig` 并重新生成，旧 `BOX_3=y` 不会残留。
- **覆盖未生效**：若 IDF 仍选 ADF 版，检查 `components/audio_board` 目录名与组件名一致为 `audio_board`，
  且项目 `components/` 在 `EXTRA_COMPONENT_DIRS` 之前被搜索（IDF 默认如此）。
- **链接缺失符号**：桩 `board.c` 不调用外部 codec/LCD 函数，故无外部未定义引用。

## 验收标准

- `idf.py build` 成功，`audio_board` 组件来自 `D:\zhutao\audio_player\components\audio_board`。
- 构建日志出现 `Current board name is tapebook (custom)`。
- 不再出现 `esp32_s3_box_3` 相关编译 / 宏报错。
