# BT 音箱 (A2DP Sink) 需要 ESP-ADF 的 bluetooth_service 组件
set(COMPONENT_REQUIRES
    driver
    nvs_flash
    esp_timer
    fatfs
    esp_adc
    audio_pipeline
    audio_stream
    esp-adf-libs
    esp_audio_codec          # R076-CODEC-17: 单路径用 component manager 拉的 v2.6.2
    lvgl
    esp_lcd
    freetype
    led_strip
    app_update
    )
if(CONFIG_USE_BT_SPEAKER)
    list(APPEND COMPONENT_REQUIRES bluetooth_service bt)
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "menu.cpp"
        "ota_sd.cpp"
        "button_manager.cpp"
        "tape_control.cpp"
        "playlist.cpp"
        "display.cpp"
        "font_partition.cpp"
        "audio_player.cpp"
        "mp3_decoder_esp_codec.c"   # R076-CODEC-7: 开源 MP3 解码器 wrapper (替代闭源 PV-MP3)
        "mp3_decoder_libhelix.c"   # R080: Helix MP3 解码器 wrapper (替代闭源 PV-MP3, 不崩+坏帧跳过)
        "flac_decoder_esp_codec.c"  # R122-FLAC-CODEC: 开源 FLAC 解码器 wrapper (替代闭源 flac_decoder, 崩)
        "flac_decoder_drflac.c"     # R-FLAC-DRFLAC: 自托管 dr_flac 解码器 (内存可控, 不踩 LVGL)
        "aac_decoder_esp_codec.c"   # R122-AAC-CODEC: 开源 AAC 解码器 wrapper (替代闭源 aac_decoder, 与 FLAC 同构)
        "wav_bitdepth_conv.c"       # R123-WAV: WAV 原始 PCM(16/24/32bit) -> 16bit 转换 element(规避 32bit I2S 卡死)
        "settings.cpp"
        "bookmark.cpp"
        "power_mgmt.cpp"
        # R101: 中文字体已修复并重新生成。之前乱码与"LVGL v8 字体不兼容 v9"无关
        # （已核对 v9 的 lv_font_t / cmap / glyph_dsc 与 v8 完全一致），真正根因是
        # gen_font.py 的两个 bug：
        #   1) lv_font_conv 对离散字符集生成错误 cmap(min->max 巨型区间)，脚本据此
        #      反推 unicode，导致 1689 字符里 1433 个错位；
        #   2) 缺 v9 新增的 stride 字段，且 base_line 写死 0。
        # 详见 tools/gen_font.py，无需升级 lv_font_conv。
        "ui_font_12.c"
        "ui_font_14.c"
        "ui_font_16.c"
        "cjk_font.c"   # 点阵字库(simsun 16x16, 替代 LVGL 中文路径; 硬件上 LVGL lv_font_chinese_* 渲染不稳)
        "bt_speaker.cpp"
        "build_info.c"           # R124: 精准构建信息(每次构建重新生成, 含真实构建时间+git版本)
    INCLUDE_DIRS
        "."
    REQUIRES ${COMPONENT_REQUIRES}
)

# ----------------------------------------------------------------------------
# R124: 精准构建信息
# 每次构建都重新生成 build_info.h（含真实构建时间 + git 版本），并确保 build_info.c
# 重新编译，使启动日志里的构建时间/版本始终准确（替代 IDF 的 Compile time，后者因
# 增量编译不重编 esp_app_desc.c 而易滞留旧日期）。
#
# R158 修复接线缺陷: 原实现把生成命令放在 OUTPUT 型 add_custom_command 里、
# DEPENDS 指向无输出的 phony 目标 —— CMake 对目标的 DEPENDS 解析为该目标的输出
# 文件列表, phony 无输出 => 依赖为空 => 生成命令只在 build_info.h 缺失时跑过一次
# (实测 2026-09-30 之后从未重新生成, 版本串一直停留在 R118-10-ge53e1f9-dirty)。
# 改为: 生成命令直接放进自定义目标(build_info_force_phony)。自定义目标每次构建
# 都会执行(旧接法中该目标的 echo 也确实每次都跑, 见构建日志 [1/N]), 且组件库
# add_dependencies 于它 => 先生成后编译, 顺序有保证。
# ----------------------------------------------------------------------------
set(BUILD_INFO_H "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")
add_custom_target(build_info_force_phony
    COMMAND ${CMAKE_COMMAND} -D OUT_FILE=${BUILD_INFO_H} -D SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_build_info.cmake
    COMMENT "Generating build_info.h (fresh build timestamp)"
    VERBATIM
)
# 让 main 组件库的编译排在生成之后，且 build_info.c 在头文件更新时重编
add_dependencies(${COMPONENT_LIB} build_info_force_phony)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_source_files_properties(build_info.c PROPERTIES OBJECT_DEPENDS ${BUILD_INFO_H})

# 传递目标模块选择为编译宏
# (在 C 代码里可用 BOARD_MODULE_WROOM_1_N16R8 / _WROOM_2_N32R16V)
if(CONFIG_BOARD_MODULE_WROOM_1_N16R8)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_MODULE_WROOM_1_N16R8=1)
endif()
if(CONFIG_BOARD_MODULE_WROOM_2_N32R16V)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_MODULE_WROOM_2_N32R16V=1)
endif()

# 显示诊断模式: 已确认颜色修复 (RGB + LV_COLOR_16_SWAP=1), 关闭诊断恢复正常 UI
# target_compile_definitions(${COMPONENT_LIB} PRIVATE DISPLAY_DIAG=1)

# 注意：ESP-ADF components 通过项目根目录的 EXTRA_COMPONENT_DIRS 引入
# 这里不再重复设置（CMake 规范要求 EXTRA_COMPONENT_DIRS 必须在 include(project.cmake) 之前）

# ----------------------------------------------------------------------------
# LVGL freetype 桥接 (R050)
# LVGL 组件自身不依赖 espressif/freetype，但其 src/libs/freetype/lv_freetype.c
# 需要 freetype 提供的 ft2build.h 与 FT_* 头；链接则靠 main -> freetype 组件
# 依赖链自动完成。此处手动把 freetype 的 include 目录暴露给 lvgl 编译目标，
# 否则 LVGL 编译 freetype.c 时会报 "lv_freetype_private.h: ft2build.h: No such file"。
# ----------------------------------------------------------------------------
set(FREETYPE_INC "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__freetype/freetype/include")
if(EXISTS "${FREETYPE_INC}")
    # LVGL 的真实组件库 target 形如 __idf_lvgl__lvgl（注册名 lvgl__lvgl），
    # 直接用目标名探测，避免 idf_component_get_property 的组件名解析差异。
    foreach(_lvgl_tgt __idf_lvgl__lvgl lvgl lvgl__lvgl)
        if(TARGET ${_lvgl_tgt})
            target_include_directories(${_lvgl_tgt} PRIVATE "${FREETYPE_INC}")
            break()
        endif()
    endforeach()
endif()

# 严格编译警告: 仅作用于本组件源码, 提前暴露潜在问题 (review #33)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

# R076-CODEC-14: 回退到 ADF release/v2.x 官方方案，全部使用 libesp_processing.a (PV-MP3 闭源)
# 不再手工 link esp_audio_codec.a / esp_audio_simple_dec.a，
# 不再需要 --allow-multiple-definition / --start-group 链接顺序。
# PV-MP3 由 esp-adf-libs 顶层组件自动拉入 main 链接图。
message(STATUS "R076-CODEC-14: using ADF release/v2.x stock PV-MP3 (libesp_processing.a via esp-adf-libs)")

# R076-CODEC-14: 回退到 ADF release/v2.x 官方 PV-MP3 方案
#   - 不再 include esp_audio_codec (改用 ADF mp3_decoder.h, 由 audio_stream 组件已自动 expose)
#   - 不再手工 link esp_audio_codec.a (PV-MP3 由 esp-adf-libs 顶层组件自动 link)
