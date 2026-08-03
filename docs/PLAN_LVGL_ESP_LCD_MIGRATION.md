# 改造方案：迁移到原生 esp_lcd + LVGL

## 0. 背景与依据

- 当前 `main/display.cpp` 用**自写 ST7789 SPI 驱动** + **u8g2 离线 1bpp 渲染器**，
  其注释声称 “esp_lcd_st7789 在本工具链不存在”。
- 实测本机 IDF 为 **v5.5.3**，原生内置 `components/esp_lcd`：
  - `src/esp_lcd_panel_st7789.c`（原厂 ST7789 驱动）
  - `include/esp_lcd_panel_st7789.h` → `esp_lcd_new_panel_st7789()`
  - `spi/esp_lcd_panel_io_spi.c` → `esp_lcd_new_panel_io_spi()`
- 结论：自写驱动的前提已过时，应改用原生 `esp_lcd_spi` + `esp_lcd_panel_st7789`，
  并以其 `esp_lcd_panel_draw_bitmap()` 作为 **LVGL `flush_cb`** 的底层，原生打通 LVGL。

## 1. 硬件引脚与 SPI 复用（核查结论）

### 显示（SPI3_HOST，ST7789 2.0" 320×240）
| 信号 | GPIO | esp_lcd 配置项 |
|------|------|----------------|
| TFT_SDA/MOSI | GPIO18 | `mosi_io_num` |
| TFT_SCL/SCK  | GPIO8  | `sclk_io_num` |
| TFT_DC       | GPIO16 | `dc_gpio_num` |
| TFT_RES      | GPIO17 | `reset_gpio_num` |
| TFT_BLK 背光 | GPIO15 | LEDC PWM（独立于 esp_lcd） |
| TFT_CS       | 接地   | `cs_gpio_num = -1` |
| LCD_POW_EN   | GPIO39 | PMOS 低电平导通，初始化须拉低通电 |

### SD 卡（SPI2_HOST，独立于显示）
| 信号 | GPIO |
|------|------|
| SD_CS   | GPIO10 |
| SD_MOSI | GPIO11 |
| SD_CLK  | GPIO12 |
| SD_MISO | GPIO13 |

### 关键结论
- 显示 = **SPI3_HOST**，SD = **SPI2_HOST**：**两条总线各自独立，不共用**。
- 无需 `spi_bus_reconfigure()`；显示与 SD 可并行，互不冲突。
- `esp_lcd` 走 SPI3 初始化即可，SD 的 `sdspi` 走 SPI2 保持原状。

## 2. 目标

用 ESP-IDF 原生 `esp_lcd`（`esp_lcd_spi` IO 层 + `esp_lcd_panel_st7789` 面板驱动）
替换 `display.cpp` 里的自写 ST7789 驱动，并接入 **LVGL** 作为 GUI 框架，
替换 u8g2 单色渲染。最终：原厂驱动 + LVGL 流畅刷屏 + 无第三方 ST7789 库。

## 3. 依赖变更

- `main/idf_component.yml`：增加 `lvgl`（及推荐的 `lvgl_esp32_port` 端口封装）。
  - 注意：LVGL 经 idf component manager 拉取需联网/registry；
    若构建环境离线，需 vendoring 到 `components/lvgl`。
- `main/CMakeLists.txt` `REQUIRES`：
  - 增加 `lvgl`（及 `lvgl_esp32_port`，如选用）；`esp_lcd` 为 IDF 内置组件，无需额外声明。
  - 移除 `u8g2`（迁移完成后）。
- 停用/删除 `components/u8g2` 手动源码（确认无其它引用后）。

## 4. display.cpp 重写（核心）

删除：自写 `st7789_*` 函数、u8g2 渲染分支（`#ifdef CONFIG_USE_U8G2`）、
`CONFIG_USE_U8G2` 宏相关代码。

新初始化（SPI3）：

```c
// 1) 初始化 SPI3 总线（独立于 SD 的 SPI2）
spi_bus_config_t buscfg = {
    .sclk_io_num = DISPLAY_SCLK_IO,   // GPIO8
    .mosi_io_num = DISPLAY_MOSI_IO,   // GPIO18
    .miso_io_num = -1,                // 屏只读命令/数据，无需 MISO
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = DISPLAY_WIDTH * 40 * 2, // 一次刷 40 行 RGB565
};
spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);

// 2) esp_lcd_spi IO 层
esp_lcd_panel_io_spi_config_t io_cfg = {
    .dc_gpio_num = DISPLAY_DC_IO,     // GPIO16
    .cs_gpio_num = -1,                // CS 接地常选
    .spi_clock_hz = 40 * 1000 * 1000,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
    .spi_mode = 0,
};
esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_cfg, &io_handle);

// 3) 原生 ST7789 面板驱动
esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = DISPLAY_RESET_IO,  // GPIO17
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 16,                // RGB565
};
esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle);

// 4) 复位 + 初始化 + 横屏取向
esp_lcd_panel_reset(panel_handle);
esp_lcd_panel_init(panel_handle);
// 320x240 横屏：不交换 XY，按需镜像
esp_lcd_panel_swap_xy(panel_handle, false);
esp_lcd_panel_mirror(panel_handle, false, false);
esp_lcd_panel_invert_color(panel_handle, true); // 视屏而定

// 5) 电源与背光（独立于 esp_lcd）
gpio_set_level(LCD_POW_EN_IO, 0);   // GPIO39 拉低 -> 屏上电
// BLK(GPIO15) 维持现有 LEDC PWM 背光逻辑
```

## 5. LVGL 接入

推荐用 `lvgl_esp32_port` 封装（处理 tick/task/双缓冲）：

```c
lvgl_port_init();
lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = io_handle,
    .panel_handle = panel_handle,
    .buffer_size = DISPLAY_WIDTH * 40,        // 40 行缓冲
    .trans_size = DISPLAY_WIDTH * 40,
    .double_buffer = true,
    .hres = DISPLAY_WIDTH,                   // 320
    .vres = DISPLAY_HEIGHT,                  // 240
    .monochrome = false,
};
lvgl_port_add_disp(&disp_cfg); // flush_cb 内部调用 esp_lcd_panel_draw_bitmap
```

随后用 LVGL widget 重写现有界面函数：
`display_show_splash / display_show_no_files / display_show_no_card /
display_show_browse / display_update`。

## 6. 分辨率与方向

- 当前 `config.h`：`DISPLAY_WIDTH=320` / `DISPLAY_HEIGHT=240`（横屏）。
- 若需求为竖屏 **240×320**：`swap_xy(panel, true)` 并交换 `DISPLAY_WIDTH/HEIGHT` 宏。
- MADCTL 由 `esp_lcd_panel_st7789` 自动管理，无需像自写驱动那样手填 `0x00`。

## 7. 分阶段实施（建议）

- **Phase 1（打通驱动）**：接入 esp_lcd + LVGL，显示测试屏（确认原生驱动刷屏 OK），暂不改 UI。
- **Phase 2（重写 UI）**：用 LVGL widget 逐一替换 `display_show_*` / `display_update`。
- **Phase 3（清理）**：移除 u8g2 组件与 `CONFIG_USE_U8G2` 分支、自写 ST7789 驱动。

## 8. 风险与注意

- LVGL 经 idf component manager 拉取需 registry 访问；离线环境需 vendoring。
- 320×240 RGB565 全屏缓冲 ≈ 150 KB，双缓冲 ≈ 300 KB；
  WROOM-1 N16R8 有 8 MB Octal PSRAM，缓冲放在 PSRAM（`MALLOC_CAP_SPIRAM`）足够。
- u8g2 现有 UI 全部重写，工作量集中在 Phase 2。
- `LCD_POW_EN`（GPIO39）务必在显示初始化早期拉低，否则屏无供电（与 esp_lcd 逻辑解耦）。

## 9. 验证

- `configure.bat wroom-1-n16r8 && build.bat build`：确认原生 `esp_lcd` 链接、LVGL 编译通过。
- 运行：splash 屏出现、刷屏流畅、与 SD 卡读写并行无冲突。

## 10. 实施状态（2026-07-29）

**Phase 1 已完成，构建通过（exitCode 0）**：

| 项 | 状态 | 说明 |
|----|------|------|
| LVGL 依赖 | ✅ | `idf_component.yml` 加 `lvgl/lvgl >=9.0.0`，component manager 实际拉取 **LVGL 9.5.0** |
| esp_lcd 驱动 | ✅ | `display.cpp` 重写：`spi_bus_initialize(SPI3)` → `esp_lcd_new_panel_io_spi` → `esp_lcd_new_panel_st7789`；自写 ST7789 驱动与 u8g2 分支全部移除 |
| LVGL 配置 | ✅ | 新增 `main/lv_conf.h`（RGB565 / 16 位色深 / FreeRTOS） |
| 方向开关 | ✅ | `config.h` 新增 `DISPLAY_ORIENTATION`（0=横屏 320×240 默认，1=竖屏 240×320）；`DISPLAY_WIDTH/HEIGHT` 随宏自动切换，方向仅在 `lcd_hw_init()` 经 `swap_xy/mirror` 设置 |
| 依赖清理 | ✅ | `main/CMakeLists.txt` REQUIRES：去 `u8g2`，加 `lvgl esp_lcd`（u8g2 不再参与编译） |
| UI | ✅(Phase1) | 既有 `display_*` API 全部保留，UI 以 LVGL widget 复刻原布局 |

**实施中发现的 API 修正（IDF v5.5.3 实测）**：
- `esp_lcd_panel_spi.h` 头文件不存在 → SPI IO 函数声明在 `esp_lcd_panel_io.h`；
- `esp_lcd_panel_io_spi_config_t` 时钟字段名为 **`pclk_hz`**（非 `spi_clock_hz`，本文档 §4 示例为通用写法）；
- C++ designated initializer 须按结构体声明顺序排列。

**待办 / 状态**：
- [ ] 真机烧录验证（`build.bat flash`）：确认取向 / `invert_color` / 颜色序；
- [x] **Phase 2（UI 重写）已完成（2026-07-29）**：主播放界面与浏览/提示界面已全部用 LVGL widget 实现，修复原状态栏 `%%` 显示 bug，新增 `[SEQ/ALL/ONE]` 播放模式显示、`LV_LABEL_LONG_SCROLL_CIRCULAR` 长文件名循环滚动、`lv_bar` 进度条 + 百分比、磁带卷轴装饰、30s 无操作降亮度屏保；新增 `display_set_play_mode()` 由 `cycle_play_mode()` 与初始化加载时同步（详见 DETAILED_DESIGN.md §5.3 布局图）。
- [ ] Phase 3：删除 `components/u8g2` 目录与 `CONFIG_USE_U8G2` Kconfig 项（当前仅解除依赖）。
