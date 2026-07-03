/**
 * @file voice_prompt.cpp
 * @brief 语音播报 stub (P2 — V1.2+)
 */

#include "voice_prompt.h"
#include "esp_log.h"

static const char *TAG = "voice";

void voice_prompt_status(void) {
    ESP_LOGI(TAG, "Voice prompt: status (stub for V1.2)");
}

void voice_prompt_battery(void) {
    ESP_LOGI(TAG, "Voice prompt: battery (stub for V1.2)");
}

void voice_prompt_play(const char *text_id) {
    ESP_LOGI(TAG, "Voice prompt: '%s' (stub for V1.2)", text_id);
}
