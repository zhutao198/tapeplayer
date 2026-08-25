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
        "mp3_decoder_esp_codec.c"   # R076-CODEC-7: 开源 MP3 解码器 wrapper (替代闭源 PV-MP3)
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

# R076-CODEC-7/10: 手动链接 esp_audio_codec 的预编译库 (含开源 esp_mp3_dec_* 符号)
# esp-adf-libs 把各子组件编成独立 .a，但只有 esp_codec (PV-MP3) 被自动链接；
# esp_audio_codec 的 libesp_audio_codec.a 与 libesp_audio_simple_dec.a 需显式链接
# 才能让 main 用到 esp_mp3_dec_* / esp_audio_simple_dec_*。
#
# 关键链接顺序问题：libesp_processing.a (PV-MP3) 与 libesp_audio_codec.a (开源 MP3)
# 都内嵌了 minimp3 的 mp3dec_init/mp3dec_decode_frame，产生 multiple definition。
# 我们用 --allow-multiple-definition 让链接器取「首个定义」，并用 --start-group/--end-group
# 把开源库排在闭源库之前，确保 esp_mp3_dec_* 内部调用的 minimp3 解析到【开源版】，
# 彻底避开 PV-MP3 闭源 minimp3 的崩溃点。
set(ESP_ACODEC_LIB_BASE "D:/esp/esp-adf/components/esp-adf-libs/esp_audio_codec")
set(ESP_ACODEC_LIB "${ESP_ACODEC_LIB_BASE}/lib/${CONFIG_IDF_TARGET}/libesp_audio_codec.a")
set(ESP_SDEC_LIB   "${ESP_ACODEC_LIB_BASE}/lib/${CONFIG_IDF_TARGET}/libesp_audio_simple_dec.a")
if(EXISTS "${ESP_ACODEC_LIB}")
    # libesp_audio_simple_dec.a 内部依赖 libesp_audio_codec.a (esp_audio_dec_* vtable)
    # 因此两组都要把 acodec 放进 start-group，让 sdec 链接时也能找到 acodec 符号
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        # 组 1: main 直接调用的 acodec (esp_mp3_dec_*) + sdec (esp_audio_simple_dec_register_default)
        # sdec 也在组 1 是因为 CODEC-11 在 main 直接调 esp_audio_simple_dec_register_default
        "-Wl,--start-group"
        "${ESP_ACODEC_LIB}"
        "${ESP_SDEC_LIB}"
        "-Wl,--end-group"
        # 组 2: sdec 内部引用 acodec 符号 (esp_audio_dec_* vtable dispatch)
        "-Wl,--start-group"
        "${ESP_SDEC_LIB}"
        "${ESP_ACODEC_LIB}"
        "-Wl,--end-group")
    # 允许重复定义 (组内的 mp3dec_* 由开源库优先)
    target_link_options(${COMPONENT_LIB} PRIVATE "-Wl,--allow-multiple-definition")
    message(STATUS "R076-CODEC-11: linked ${ESP_ACODEC_LIB} + ${ESP_SDEC_LIB} (open-source minimp3 first, simple_dec reg default)")
else()
    message(WARNING "R076-CODEC-11: libesp_audio_codec.a NOT FOUND at ${ESP_ACODEC_LIB}")
endif()

# R076-CODEC-7/10: 让 main 能直接 include esp_audio_codec 的 MP3 解码器头文件
# (esp_mp3_dec.h 位于 esp-adf-libs/esp_audio_codec/include/decoder/impl/，
#  simple_dec/esp_audio_simple_dec.h 位于 include/simple_dec/，
#  这些目录是 esp_audio_codec 组件的内部 include，未对依赖者公开，需手动暴露)
# 需同时暴露整条依赖链: impl -> decoder -> simple_dec -> include
set(ESP_ACODEC_BASE "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_audio_codec")
if(NOT EXISTS "${ESP_ACODEC_BASE}/include/decoder/impl/esp_mp3_dec.h")
    # 回退到本地 ADF 路径
    set(ESP_ACODEC_BASE "D:/esp/esp-adf/components/esp-adf-libs/esp_audio_codec")
endif()
if(EXISTS "${ESP_ACODEC_BASE}/include/decoder/impl/esp_mp3_dec.h")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${ESP_ACODEC_BASE}/include"
        "${ESP_ACODEC_BASE}/include/decoder"
        "${ESP_ACODEC_BASE}/include/decoder/impl"
        "${ESP_ACODEC_BASE}/include/simple_dec")  # R076-CODEC-10: esp_audio_simple_dec.h 所在目录
    message(STATUS "R076-CODEC-10: esp_audio_codec include -> ${ESP_ACODEC_BASE}/include (+simple_dec)")
else()
    message(WARNING "R076-CODEC-10: esp_mp3_dec.h NOT FOUND in ${ESP_ACODEC_BASE}")
endif()
