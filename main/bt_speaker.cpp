/**
 * @file bt_speaker.cpp
 * @brief 蓝牙音箱 (A2DP Sink) 实现 (R050-BT)
 *
 * 依赖：CONFIG_USE_ESP_ADF + CONFIG_USE_BT_SPEAKER
 * 复用 audio_player 的 g_i2s_writer（I2S/MAX98357 输出），链路：
 *     A2DP Sink 解码 PCM (bluetooth_service 元素) → i2s_stream_writer
 *
 * 注意：ESP-ADF bluetooth_service 的 API 随版本略有差异，本文件基于 ADF v2.8。
 *   若编译报错，请对照 D:\esp\esp-adf\components\bluetooth_service\include\bluetooth_service.h
 *   微调回调函数签名 / esp_bluetooth_service_create 的返回类型。
 */

#include "bt_speaker.h"

#if defined(CONFIG_USE_ESP_ADF) && defined(CONFIG_USE_BT_SPEAKER)

#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "audio_pipeline.h"
#include "bluetooth_service.h"
#include "config.h"

static const char *TAG = "bt_speaker";

/* ---- 内部状态 ---- */
typedef struct {
    audio_pipeline_handle_t        pipeline;
    audio_element_handle_t         bt_element;   // A2DP Sink 解码元素
    audio_element_handle_t         i2s_writer;   // 由 audio_player 传入并复用
    bool                           initialized;
    bool                           connected;
} bt_speaker_t;

static bt_speaker_t s_bt = {0};

static bt_speaker_state_cb_t  g_state_cb = NULL;
static bt_speaker_volume_cb_t g_vol_cb    = NULL;

/* ============================================================
 * 回调：GAP / A2DP / AVRCP
 * ============================================================ */
static void bt_gap_evt_handler(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    /* Just-Works 配对在当前场景无需 PIN 输入；如需定制可在此处理 PIN_REQ */
    (void)event; (void)param;
}

static void bt_a2d_evt_handler(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_bt.connected = true;
            ESP_LOGI(TAG, "A2DP connected");
            if (g_state_cb) g_state_cb(BT_SPEAKER_STATE_CONNECTED, NULL);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_bt.connected = false;
            ESP_LOGI(TAG, "A2DP disconnected");
            if (g_state_cb) g_state_cb(BT_SPEAKER_STATE_DISCONNECTED, NULL);
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state: %d", param->audio_stat.state);
        break;
    default:
        break;
    }
}

static void bt_avrc_ct_evt_handler(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_VOLUME_CHANGE_EVT: {
        uint8_t vol = param->volume.volume;   // AVRCP 绝对音量 0..127
        ESP_LOGI(TAG, "Phone volume: %u/127", vol);
        if (g_vol_cb) g_vol_cb(vol);
        break;
    }
    default:
        break;
    }
}

/* ============================================================
 * 初始化 / 反初始化
 * ============================================================ */
esp_err_t bt_speaker_init(const char *device_name)
{
    if (s_bt.initialized) return ESP_OK;

    esp_bluedroid_config_t cfg = BT_DEFAULT_BLUECROID_CONFIG();
    esp_err_t ret = esp_bluedroid_init_with_cfg(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init failed: 0x%x", ret);
        return ret;
    }
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable failed: 0x%x", ret);
        return ret;
    }

    if (device_name) {
        esp_bt_dev_set_device_name(device_name);
    }
    // 设为可发现 + 可连接：手机才能扫描到并配对
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    s_bt.initialized = true;
    return ESP_OK;
}

esp_err_t bt_speaker_deinit(void)
{
    bt_speaker_stop();
    if (s_bt.initialized) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        s_bt.initialized = false;
    }
    return ESP_OK;
}

/* ============================================================
 * 启动 / 停止 音频管线
 * ============================================================ */
esp_err_t bt_speaker_start(audio_element_handle_t i2s_writer)
{
    if (!i2s_writer) return ESP_ERR_INVALID_ARG;
    if (bt_speaker_init(BT_SPEAKER_DEVICE_NAME) != ESP_OK) return ESP_FAIL;

    s_bt.i2s_writer = i2s_writer;

    // 创建 A2DP Sink 服务（回调用于连接状态 / 手机音量）
    bt_app_config_t bt_cfg = {
        .device_name      = BT_SPEAKER_DEVICE_NAME,
        .mode             = BT_A2DP_SINK,
        .callback         = NULL,
        .gap_callback     = bt_gap_evt_handler,
        .a2d_callback     = bt_a2d_evt_handler,
        .avrc_ct_callback = bt_avrc_ct_evt_handler,
    };
    esp_bluetooth_service_handle_t svc = esp_bluetooth_service_create(&bt_cfg);
    if (!svc) {
        ESP_LOGE(TAG, "esp_bluetooth_service_create failed");
        return ESP_FAIL;
    }

    // 取 A2DP 解码元素，接入 pipeline
    s_bt.bt_element = periph_bluetooth_get_element(svc);
    if (!s_bt.bt_element) {
        ESP_LOGE(TAG, "periph_bluetooth_get_element failed");
        return ESP_FAIL;
    }

    audio_pipeline_cfg_t pcfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_bt.pipeline = audio_pipeline_init(&pcfg);
    if (!s_bt.pipeline) {
        ESP_LOGE(TAG, "audio_pipeline_init failed");
        return ESP_FAIL;
    }

    audio_pipeline_register(s_bt.pipeline, s_bt.bt_element, "bt");
    audio_pipeline_register(s_bt.pipeline, i2s_writer, "i2s");
    const char *tags[2] = {"bt", "i2s"};
    audio_pipeline_link(s_bt.pipeline, tags, 2);
    audio_pipeline_run(s_bt.pipeline);

    ESP_LOGI(TAG, "BT speaker pipeline started (waiting for phone connection)");
    if (g_state_cb) g_state_cb(BT_SPEAKER_STATE_INITIALIZED, NULL);
    return ESP_OK;
}

esp_err_t bt_speaker_stop(void)
{
    if (s_bt.pipeline) {
        audio_pipeline_stop(s_bt.pipeline);
        audio_pipeline_terminate(s_bt.pipeline);
        if (s_bt.bt_element) audio_pipeline_unregister(s_bt.pipeline, s_bt.bt_element);
        if (s_bt.i2s_writer)  audio_pipeline_unregister(s_bt.pipeline, s_bt.i2s_writer);
        audio_pipeline_deinit(s_bt.pipeline);
        s_bt.pipeline = NULL;
    }
    s_bt.bt_element = NULL;
    s_bt.connected  = false;
    if (g_state_cb) g_state_cb(BT_SPEAKER_STATE_STOPPED, NULL);
    return ESP_OK;
}

void bt_speaker_register_state_cb(bt_speaker_state_cb_t cb) { g_state_cb = cb; }
void bt_speaker_set_volume_cb(bt_speaker_volume_cb_t cb)     { g_vol_cb = cb; }

void bt_speaker_report_volume(uint8_t vol_0_127)
{
    // 把本地音量回传手机（AVRCP 绝对音量），使两端一致
    esp_a2dp_set_volume(vol_0_127);
}

bool bt_speaker_is_connected(void) { return s_bt.connected; }

void bt_speaker_avrc_play(void)  { esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY); }
void bt_speaker_avrc_pause(void) { esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE); }

#endif // CONFIG_USE_ESP_ADF && CONFIG_USE_BT_SPEAKER
