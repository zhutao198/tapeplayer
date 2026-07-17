#include "esp_log.h"
#include "board.h"
#include "audio_mem.h"

static const char *TAG = "TAPEBOOK_BOARD";
static audio_board_handle_t board_handle = NULL;

audio_board_handle_t audio_board_init(void)
{
    if (board_handle) {
        ESP_LOGW(TAG, "Board already initialized");
        return board_handle;
    }
    board_handle = (audio_board_handle_t) audio_calloc(1, sizeof(struct audio_board_handle));
    AUDIO_MEM_CHECK(TAG, board_handle, return NULL);
    board_handle->audio_hal = NULL;
    board_handle->adc_hal = NULL;
    ESP_LOGI(TAG, "TapeBook custom board initialized");
    return board_handle;
}

audio_hal_handle_t audio_board_codec_init(void)
{
    ESP_LOGI(TAG, "No I2C codec; MAX98357A driven via I2S directly");
    return NULL;
}

audio_hal_handle_t audio_board_adc_init(void)
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
    if (!audio_board) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_OK;
    if (audio_board->audio_hal) {
        ret |= audio_hal_deinit(audio_board->audio_hal);
    }
    free(audio_board);
    board_handle = NULL;
    return ret;
}
