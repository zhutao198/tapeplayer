idf_component_register(
    SRCS
        "main.cpp"
        "button_manager.cpp"
        "tape_control.cpp"
        "playlist.cpp"
        "display.cpp"
        "audio_player.cpp"
        "settings.cpp"
        "bookmark.cpp"
        "power_mgmt.cpp"
        "voice_prompt.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        nvs_flash
        esp_timer
        fatfs
        # 2026-07-03 R003: 补 R001 漏掉的 ADF 组件（audio_player.cpp 走真实 ADF 路径需要）
        audio_pipeline
        audio_stream
        esp-adf-libs
        # R014: u8g2 OLED 显示（display.cpp #ifdef CONFIG_USE_U8G2 路径需要）
        u8g2
        # u8g2_esp32_hal I2C 适配层（独立组件，自动搜索 extra_components）
        u8g2_esp32_hal
)

# 传递目标模块选择为编译宏
# (在 C 代码里可用 BOARD_MODULE_WROOM_1_N16R8 / _WROOM_2_N32R16V)
if(CONFIG_BOARD_MODULE_WROOM_1_N16R8)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_MODULE_WROOM_1_N16R8=1)
endif()
if(CONFIG_BOARD_MODULE_WROOM_2_N32R16V)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_MODULE_WROOM_2_N32R16V=1)
endif()

# 注意：ESP-ADF components 通过项目根 CMakeLists.txt 的 EXTRA_COMPONENT_DIRS 引入
# 这里不再重复设置（CMake 规范要求 EXTRA_COMPONENT_DIRS 必须在 include(project.cmake) 之前）
