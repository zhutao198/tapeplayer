#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>
#include "board.h"
#include "audio_error.h"
#include "audio_mem.h"

static const char *TAG = "TAPEBOOK_BOARD";

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config)
{
    AUDIO_NULL_CHECK(TAG, i2c_config, return ESP_FAIL);
    if (port == I2C_NUM_0) {
        i2c_config->sda_io_num = GPIO_NUM_17;
        i2c_config->scl_io_num = GPIO_NUM_18;
    } else {
        i2c_config->sda_io_num = -1;
        i2c_config->scl_io_num = -1;
        ESP_LOGE(TAG, "i2c port %d is not supported", port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config)
{
    AUDIO_NULL_CHECK(TAG, i2s_config, return ESP_FAIL);
    if (port == 0) {
        i2s_config->bck_io_num = GPIO_NUM_4;
        i2s_config->ws_io_num = GPIO_NUM_5;
        i2s_config->data_out_num = GPIO_NUM_6;
        i2s_config->data_in_num = GPIO_NUM_NC;
        i2s_config->mck_io_num = GPIO_NUM_NC;
    } else {
        memset(i2s_config, -1, sizeof(board_i2s_pin_t));
        ESP_LOGE(TAG, "i2s port %d is not supported", port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t get_spi_pins(spi_bus_config_t *spi_config, spi_device_interface_config_t *spi_device_interface_config)
{
    AUDIO_NULL_CHECK(TAG, spi_config, return ESP_FAIL);
    AUDIO_NULL_CHECK(TAG, spi_device_interface_config, return ESP_FAIL);

    spi_config->mosi_io_num = -1;
    spi_config->miso_io_num = -1;
    spi_config->sclk_io_num = -1;
    spi_config->quadwp_io_num = -1;
    spi_config->quadhd_io_num = -1;

    spi_device_interface_config->spics_io_num = -1;

    ESP_LOGW(TAG, "SPI interface is not used (SD uses SPI, pin config in main code)");
    return ESP_OK;
}

int8_t get_sdcard_intr_gpio(void)       { return SDCARD_INTR_GPIO; }
int8_t get_sdcard_open_file_num_max(void) { return SDCARD_OPEN_FILE_NUM_MAX; }
int8_t get_sdcard_power_ctrl_gpio(void)  { return SDCARD_PWR_CTRL; }

int8_t get_headphone_detect_gpio(void)   { return HEADPHONE_DETECT; }
int8_t get_pa_enable_gpio(void)          { return PA_ENABLE_GPIO; }
int8_t get_es7243_mclk_gpio(void)        { return -1; }

int8_t get_input_rec_id(void)            { return -1; }
int8_t get_input_mode_id(void)           { return -1; }
int8_t get_input_set_id(void)            { return BUTTON_STOP_ID; }
int8_t get_input_play_id(void)           { return BUTTON_PLAY_ID; }
int8_t get_input_volup_id(void)          { return -1; }
int8_t get_input_voldown_id(void)        { return -1; }

int8_t get_green_led_gpio(void)          { return -1; }
int8_t get_blue_led_gpio(void)           { return -1; }
int8_t get_reset_codec_gpio(void)        { return -1; }
int8_t get_reset_board_gpio(void)        { return -1; }
int8_t get_auxin_detect_gpio(void)       { return -1; }
int8_t get_adc_detect_gpio(void)         { return -1; }
