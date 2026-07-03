set(COMPONENT_REQUIRES)
set(COMPONENT_PRIV_REQUIRES audio_sal audio_hal esp_dispatcher esp_peripherals display_service)

if(CONFIG_AUDIO_BOARD_CUSTOM)
    message(STATUS "Current board name is TAPEBOOK_BOARD (custom)")
    list(APPEND COMPONENT_ADD_INCLUDEDIRS ./tapebook_board_v1_0)
    set(COMPONENT_SRCS
        ./tapebook_board_v1_0/board.c
        ./tapebook_board_v1_0/board_pins_config.c
    )
endif()

register_component()

idf_component_get_property(audio_board_lib audio_board COMPONENT_LIB)
set_property(TARGET ${audio_board_lib} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${COMPONENT_LIB})
