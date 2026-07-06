#include "voice_prompt.h"
#include "power_mgmt.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>

static const char *TAG = "voice";

void voice_prompt_status(void)
{
    // V1.2: play "/voice/status.wav" from SD card when ADF is enabled
    // Current: log-based announcement ready for ADF integration
    ESP_LOGI(TAG, "[V1.2] Voice prompt: status announcement (requires ADF + WAV files)");
}

void voice_prompt_battery(void)
{
    int pct = power_mgmt_get_battery_percent();
    // V1.2: play "/voice/battery_<pct>.wav" or "/voice/battery_low.wav"
    ESP_LOGI(TAG, "[V1.2] Voice prompt: battery %d%% (requires ADF + WAV files)", pct);
}

void voice_prompt_play(const char *text_id)
{
    if (!text_id) return;
    // V1.2: play "/sdcard/voice/<text_id>.wav"
    // Implementation note: must save current playback state, play WAV,
    // then restore. Requires audio_player API extension for state save/restore.
    ESP_LOGI(TAG, "[V1.2] Voice prompt: play '%s' (requires ADF + WAV files)", text_id);
}
