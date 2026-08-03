/*
 * Tapebook custom ADF board - minimal stub implementation.
 *
 * The project uses its own I2S/LCD/SD drivers and never calls
 * audio_board_init(), so every function here is a no-op stub that only needs
 * to compile and link. No codec/LCD/touch dependencies are pulled in.
 */

#include "board.h"

static audio_board_handle_t board_handle = NULL;

audio_board_handle_t audio_board_init(void)
{
    return NULL;
}

audio_hal_handle_t audio_board_codec_init(void)
{
    return NULL;
}

audio_hal_handle_t audio_board_adc_init(void)
{
    return NULL;
}

void *audio_board_lcd_init(esp_periph_set_handle_t set, void *cb)
{
    return NULL;
}

display_service_handle_t audio_board_blue_led_init(void)
{
    return NULL;
}

esp_err_t audio_board_key_init(esp_periph_set_handle_t set)
{
    return ESP_OK;
}

esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set, periph_sdcard_mode_t mode)
{
    return ESP_OK;
}

audio_board_handle_t audio_board_get_handle(void)
{
    return board_handle;
}

esp_err_t audio_board_deinit(audio_board_handle_t audio_board)
{
    return ESP_OK;
}
