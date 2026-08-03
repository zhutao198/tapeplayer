set(COMPONENT_ADD_INCLUDEDIRS ./include)

# Component requirements.
# NOTE: This component overrides the one shipped with ESP-ADF (same directory name
# "audio_board" inside project components/ takes priority over EXTRA_COMPONENT_DIRS).
# The tapebook board only provides compile-only stubs because the project uses its
# own I2S/LCD/SD drivers and never calls audio_board_init().
set(COMPONENT_REQUIRES )
set(COMPONENT_PRIV_REQUIRES esp_peripherals audio_sal audio_hal esp_dispatcher display_service)

if (CONFIG_ESP_TAPEBOOK_BOARD)
message(STATUS "Current board name is tapebook (custom)")
list(APPEND COMPONENT_ADD_INCLUDEDIRS ./tapebook)
set(COMPONENT_SRCS
./tapebook/board.c
./tapebook/board_pins_config.c
)
endif()

register_component()
