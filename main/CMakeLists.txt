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
)

# 传递目标模块选择为编译宏
# (在 C 代码里可用 BOARD_MODULE_WROOM_1_N16R8 / _WROOM_2_N32R16V)
if(CONFIG_BOARD_MODULE_WROOM_1_N16R8)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_MODULE_WROOM_1_N16R8=1)
endif()
if(CONFIG_BOARD_MODULE_WROOM_2_N32R16V)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_MODULE_WROOM_2_N32R16V=1)
endif()

# 启用 ESP-ADF：把 ADF components 加进 build（audio_player.cpp 走真实路径需要）
set(EXTRA_COMPONENT_DIRS $ENV{ADF_PATH}/components)
