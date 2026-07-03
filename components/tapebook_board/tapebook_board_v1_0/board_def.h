#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_

#include "driver/gpio.h"
#include "driver/i2s_std.h"

/* I2S - MAX98357A */
#define FUNC_AUDIO_CODEC_EN         0  /* No I2C codec; MAX98357A is I2S-only */
#define CODEC_ADC_I2S_PORT          ((i2s_port_t)0)
#define CODEC_ADC_BITS_PER_SAMPLE   ((i2s_data_bit_width_t)I2S_DATA_BIT_WIDTH_16BIT)
#define CODEC_ADC_SAMPLE_RATE       (44100)

/* I2C - SSD1306 OLED */
#define AW2013_I2C_PORT             I2C_NUM_0

/* SD card (SPI mode) */
#define FUNC_SDCARD_EN              1
#define SDCARD_OPEN_FILE_NUM_MAX    5
#define SDCARD_INTR_GPIO            -1
#define SDCARD_PWR_CTRL             -1
#define ESP_SD_PIN_CLK              GPIO_NUM_13
#define ESP_SD_PIN_CMD              GPIO_NUM_11
#define ESP_SD_PIN_D0               GPIO_NUM_12
#define ESP_SD_PIN_D1               -1
#define ESP_SD_PIN_D2               -1
#define ESP_SD_PIN_D3               GPIO_NUM_10
#define ESP_SD_PIN_D4               -1
#define ESP_SD_PIN_D5               -1
#define ESP_SD_PIN_D6               -1
#define ESP_SD_PIN_D7               -1
#define ESP_SD_PIN_CD               -1
#define ESP_SD_PIN_WP               -1

/* No headphone detect / PA enable on MAX98357A */
#define HEADPHONE_DETECT            -1
#define PA_ENABLE_GPIO              -1
#define BOARD_PA_GAIN               0

/* Button mapping */
#define INPUT_KEY_NUM               6
#define BUTTON_PLAY_ID              0
#define BUTTON_STOP_ID              1
#define BUTTON_PREV_ID              2
#define BUTTON_NEXT_ID              3
#define BUTTON_REW_ID               4
#define BUTTON_FF_ID                5

#define INPUT_KEY_DEFAULT_INFO() {                      \
    {                                                   \
        .type = PERIPH_ID_BTN,                          \
        .user_id = INPUT_KEY_USER_ID_PLAY,              \
        .act_id = BUTTON_PLAY_ID,                       \
    },                                                  \
    {                                                   \
        .type = PERIPH_ID_BTN,                          \
        .user_id = INPUT_KEY_USER_ID_SET,               \
        .act_id = BUTTON_STOP_ID,                       \
    },                                                  \
}

#endif
