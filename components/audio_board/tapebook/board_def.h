/*
 * Tapebook custom ADF board - compile-only pin definition.
 *
 * The project uses its own I2S/LCD/SD drivers and never calls
 * audio_board_init(); this file exists only so that the ADF audio_board
 * component (required by audio_stream) compiles. The only hard requirement is
 * that esp_peripherals/sdcard.c references all ESP_SD_PIN_* macros, so they
 * must be defined. Pin values are inert (set to -1) and never used at runtime.
 */

#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_

/**
 * @brief SDCARD Function Definition
 */
#define FUNC_SDCARD_EN              (1)
#define SDCARD_OPEN_FILE_NUM_MAX    (5)
#define SDCARD_INTR_GPIO            (-1)
#define SDCARD_PWR_CTRL             (-1)

/* Required by esp_peripherals/lib/sdcard/sdcard.c (13 macros) */
#define ESP_SD_PIN_CLK              (-1)
#define ESP_SD_PIN_CMD              (-1)
#define ESP_SD_PIN_D0               (-1)
#define ESP_SD_PIN_D1               (-1)
#define ESP_SD_PIN_D2               (-1)
#define ESP_SD_PIN_D3               (-1)
#define ESP_SD_PIN_D4               (-1)
#define ESP_SD_PIN_D5               (-1)
#define ESP_SD_PIN_D6               (-1)
#define ESP_SD_PIN_D7               (-1)
#define ESP_SD_PIN_CD               (-1)
#define ESP_SD_PIN_WP               (-1)

/**
 * @brief Audio / IO placeholders
 */
#define HEADPHONE_DETECT            (-1)
#define PA_ENABLE_GPIO              (-1)

/* Required by ADF audio_hal codec drivers (BOARD_PA_GAIN). Inert for this board. */
#define BOARD_PA_GAIN               (0)

/**
 * @brief Button Function Definition
 */
#define BUTTON_VOLUP_ID             (-1)
#define BUTTON_VOLDOWN_ID           (-1)
#define BUTTON_SET_ID               (-1)
#define BUTTON_PLAY_ID              (-1)
#define BUTTON_MODE_ID              (-1)
#define BUTTON_REC_ID               (-1)

#endif
