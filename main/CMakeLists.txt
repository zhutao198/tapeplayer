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
        "settings.cpp"
        "bookmark.cpp"
        "power_mgmt.cpp"
        "ui_font_12.c"
        "ui_font_14.c"
        "ui_font_16.c"
        "bt_speaker.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES ${COMPONENT_REQUIRES}
)

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
