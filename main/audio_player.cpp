/**
 * @file audio_player.cpp
 * @brief 音频播放引擎实现
 *
 * 核心设计变更（评审修正）：
 * 1. 每次 play() 重建 pipeline，避免 terminate 后复用失败 (S-09)
 * 2. WAV 使用 wav_decoder，不回退到 mp3_decoder (M-09)
 * 3. seek/tick 使用毫秒级精度 (M-03/M-04)
 * 4. 跳帧仅在 ≥4x 高档位执行，1.5x/2.5x 仅变速不跳帧 (M-10)
 * 5. 移除未使用的 opus_decoder.h (L-01)
 */

#include "audio_player.h"
#include "config.h"
#include "tape_control.h"
#include "esp_log.h"
#include <string.h>

#ifdef CONFIG_USE_ESP_ADF

#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "flac_decoder.h"
#include "ogg_decoder.h"
#include "wav_decoder.h"
#include "filter_resample.h"
// 2026-07-03 R003: 注释 board.h（项目用 MAX98357 + SSD1306 非 ADF 开发板，未配置 audio_board Kconfig，
//   而代码未实际使用 board.h 中任何 API）
// #include "board.h"

static const char *TAG = "audio_player";

/* --- 全局状态 --- */
static audio_pipeline_handle_t  g_pipeline = NULL;
static audio_element_handle_t   g_fatfs_reader = NULL;
static audio_element_handle_t   g_decoder = NULL;
static audio_element_handle_t   g_i2s_writer = NULL;   // 跨曲目复用

static bool         g_is_playing = false;
static bool         g_is_paused = false;
static int          g_volume = AUDIO_OUTPUT_VOL;
static float        g_current_speed = TAPE_SPEED_NORMAL;
static int          g_total_duration_ms = 0;           // 总时长(ms)
static char         g_current_file[256] = "";

static audio_status_cb_t g_status_cb = NULL;
static void              *g_user_data = NULL;

/* ============================================================
 * 辅助：根据文件扩展名选择解码器
 * ============================================================ */
static const char *get_file_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

static audio_element_handle_t create_decoder(const char *path)
{
    const char *ext = get_file_ext(path);

    mp3_decoder_cfg_t  mp3_cfg  = DEFAULT_MP3_DECODER_CONFIG();
    aac_decoder_cfg_t  aac_cfg  = DEFAULT_AAC_DECODER_CONFIG();
    flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
    ogg_decoder_cfg_t  ogg_cfg  = DEFAULT_OGG_DECODER_CONFIG();
    wav_decoder_cfg_t  wav_cfg  = DEFAULT_WAV_DECODER_CONFIG();

    if (strcasecmp(ext, ".mp3") == 0) {
        ESP_LOGI(TAG, "Using MP3 decoder");
        return mp3_decoder_init(&mp3_cfg);
    } else if (strcasecmp(ext, ".aac") == 0 || strcasecmp(ext, ".m4a") == 0) {
        ESP_LOGI(TAG, "Using AAC decoder");
        return aac_decoder_init(&aac_cfg);
    } else if (strcasecmp(ext, ".flac") == 0) {
        ESP_LOGI(TAG, "Using FLAC decoder");
        return flac_decoder_init(&flac_cfg);
    } else if (strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext, ".opus") == 0) {
        ESP_LOGI(TAG, "Using OGG/OPUS decoder");
        return ogg_decoder_init(&ogg_cfg);
    } else if (strcasecmp(ext, ".wav") == 0) {
        ESP_LOGI(TAG, "Using WAV decoder");
        return wav_decoder_init(&wav_cfg);
    }

    ESP_LOGW(TAG, "Unknown format %s, trying MP3 decoder", ext);
    return mp3_decoder_init(&mp3_cfg);
}

/* ============================================================
 * 初始化（仅创建 I2S 输出流，pipeline 在 play() 中重建）
 * ============================================================ */
void audio_player_init(void)
{
    ESP_LOGI(TAG, "Initializing audio subsystem...");

    // 创建 I2S 输出流（跨曲目复用，无需每次重建）
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    g_i2s_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "Audio subsystem initialized (I2S writer ready)");
}

/* ============================================================
 * 播放（每次重建 pipeline + 元素，避免 terminate 后复用 Bug）
 * ============================================================ */
bool audio_player_play(const char *filepath)
{
    if (!filepath || !*filepath) return false;

    ESP_LOGI(TAG, "Playing: %s", filepath);
    audio_player_stop(); // 确保上一个管道已销毁

    // 1. 创建 pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    g_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!g_pipeline) {
        ESP_LOGE(TAG, "Failed to create audio pipeline");
        return false;
    }

    // 2. 创建 FATFS 文件读取器
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    g_fatfs_reader = fatfs_stream_init(&fatfs_cfg);

    // 3. 创建解码器
    g_decoder = create_decoder(filepath);

    // 4. 注册元素到管道
    audio_pipeline_register(g_pipeline, g_fatfs_reader, "file");
    audio_pipeline_register(g_pipeline, g_decoder, "decoder");
    audio_pipeline_register(g_pipeline, g_i2s_writer, "i2s");

    // 5. 链接管道: file → decoder → i2s
    const char *link_tags[3] = {"file", "decoder", "i2s"};
    audio_pipeline_link(g_pipeline, link_tags, 3);

    // 6. 设置文件 URI
    audio_element_set_uri(g_fatfs_reader, filepath);

    // 7. 设置音量
    audio_element_set_volume(g_i2s_writer, g_volume);

    // 8. 启动管道
    audio_pipeline_run(g_pipeline);

    strncpy(g_current_file, filepath, sizeof(g_current_file) - 1);
    g_is_playing = true;
    g_is_paused = false;
    g_current_speed = TAPE_SPEED_NORMAL;

    // 9. 获取总时长（从解码器元数据）
    // 注：ESP-ADF 无 audio_element_get_total_time API，
    // 需通过 AEL_MSG_CMD_REPORT_MUSIC_INFO 事件获取或播放后估算
    g_total_duration_ms = 0; // 初始未知，通过事件回调更新

    return true;
}

void audio_player_pause(void)
{
    if (g_is_playing && !g_is_paused && g_pipeline) {
        audio_pipeline_pause(g_pipeline);
        g_is_paused = true;
        ESP_LOGI(TAG, "Paused");
    }
}

void audio_player_resume(void)
{
    if (g_is_playing && g_is_paused && g_pipeline) {
        audio_pipeline_resume(g_pipeline);
        g_is_paused = false;
        ESP_LOGI(TAG, "Resumed");
    }
}

void audio_player_stop(void)
{
    if (g_pipeline) {
        audio_pipeline_stop(g_pipeline);
        audio_pipeline_wait_for_stop(g_pipeline);

        // 销毁 per-track 元素（fatfs_reader + decoder）
        if (g_fatfs_reader) {
            audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
            audio_element_deinit(g_fatfs_reader);
            g_fatfs_reader = NULL;
        }
        if (g_decoder) {
            audio_pipeline_unregister(g_pipeline, g_decoder);
            audio_element_deinit(g_decoder);
            g_decoder = NULL;
        }
        // i2s_writer 从管道注销但不销毁（跨曲目复用）
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);

        // 销毁管道本身
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
    }

    g_is_playing = false;
    g_is_paused = false;
    g_current_speed = TAPE_SPEED_NORMAL;
    g_total_duration_ms = 0;
}

/* ============================================================
 * Seek（毫秒级内部实现）
 * ============================================================ */
void audio_player_seek(int seconds)
{
    audio_player_seek_ms(seconds * 1000);
}

void audio_player_seek_ms(int ms)
{
    if (!g_pipeline || !g_is_playing || !g_decoder) return;

    // ESP-ADF seek：通过 audio_element_set_pos 设置毫秒位置
    // 注：不是所有解码器都支持 seek，MP3/AAC 支持，FLAC/WAV 可能不支持
    audio_pipeline_pause(g_pipeline);
    audio_element_set_pos(g_decoder, ms);
    audio_pipeline_resume(g_pipeline);
}

int audio_player_get_position_ms(void)
{
    if (g_pipeline && g_is_playing && g_decoder) {
        int pos_ms = 0;
        // ESP-ADF 中通过 audio_element_get_pos 获取毫秒位置
        audio_element_get_pos(g_decoder, &pos_ms);
        return pos_ms;
    }
    return 0;
}

int audio_player_get_position(void)
{
    return audio_player_get_position_ms() / 1000;
}

int audio_player_get_duration(void)
{
    return g_total_duration_ms / 1000;
}

bool audio_player_is_playing(void)
{
    return g_is_playing && !g_is_paused;
}

bool audio_player_is_paused(void)
{
    return g_is_paused;
}

void audio_player_set_speed(float speed)
{
    g_current_speed = speed;

    // 通过调整 I2S 时钟实现变速（变调效果，模拟磁带机）
    if (g_i2s_writer) {
        if (speed > 0) {
            int sample_rate = (int)(AUDIO_SAMPLE_RATE * speed);
            if (sample_rate < 8000) sample_rate = 8000;
            if (sample_rate > 96000) sample_rate = 96000;
            i2s_stream_set_clk(g_i2s_writer, sample_rate, 16, 2);
        } else {
            // 快退：保持原始采样率（快退通过 seek 后退模拟）
            i2s_stream_set_clk(g_i2s_writer, AUDIO_SAMPLE_RATE, 16, 2);
        }
    }
}

void audio_player_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_volume = volume;

    if (g_i2s_writer) {
        audio_element_set_volume(g_i2s_writer, volume);
    }
}

int audio_player_get_volume(void)
{
    return g_volume;
}

/* ============================================================
 * Tick — 处理管道状态 + 快进/快退跳帧
 *
 * 跳帧策略（与 DESIGN 5.2.3 一致）：
 * - 1.5x / 2.5x：仅变速（I2S 采样率），不跳帧
 * - 4.0x / 8.0x：变速 + 每 50ms seek 跳帧
 * - 快退：所有档位都通过向后 seek 模拟
 * ============================================================ */
void audio_player_tick(void)
{
    if (!g_pipeline || !g_is_playing) return;

    // 检查管道状态
    audio_pipeline_state_t state = audio_pipeline_get_state(g_pipeline);
    if (state == AEL_STATE_FINISHED) {
        ESP_LOGI(TAG, "Track finished");
        g_is_playing = false;
        if (g_status_cb) {
            g_status_cb(0, g_user_data); // 0 = finished
        }
    }

    // 快进/快退跳帧处理
    tape_mode_t mode = tape_control_get_mode();
    if (mode == TAPE_MODE_NORMAL) return;

    float speed = tape_control_get_speed();
    float abs_speed = (speed > 0) ? speed : -speed;

    // 仅高档位（≥4x）执行跳帧；1.5x/2.5x 仅靠 I2S 变速
    // 快退所有档位都跳帧（因为没有"倒放"能力，只能断续 seek）
    bool need_seek = (abs_speed >= 4.0f) || (mode == TAPE_MODE_REWIND);

    if (!need_seek) return;

    static uint64_t last_scrub_us = 0;
    uint64_t now = esp_timer_get_time();

    if ((now - last_scrub_us) < 50000) return; // 50ms 间隔
    last_scrub_us = now;

    // 毫秒级跳帧计算：50ms × 速度倍率 = 跳过的毫秒数
    int skip_ms = (int)(50.0f * abs_speed);

    int cur_ms = audio_player_get_position_ms();
    int target_ms;

    if (speed > 0) {
        // 快进：向前跳
        target_ms = cur_ms + skip_ms;
        int duration_ms = g_total_duration_ms > 0 ? g_total_duration_ms : 3600000; // 默认上限 1h
        if (target_ms > duration_ms) target_ms = duration_ms;
    } else {
        // 快退：向后跳
        target_ms = cur_ms - skip_ms;
        if (target_ms < 0) target_ms = 0;
    }

    audio_player_seek_ms(target_ms);
}

void audio_player_set_callback(audio_status_cb_t cb, void *user_data)
{
    g_status_cb = cb;
    g_user_data = user_data;
}

#else // 不使用 ESP-ADF 的简易占位实现

#include "esp_log.h"
static const char *TAG = "audio_player";

void audio_player_init(void) {
    ESP_LOGI(TAG, "Audio player init (stub)");
}

bool audio_player_play(const char *filepath) {
    ESP_LOGI(TAG, "Play (stub): %s", filepath);
    return true;
}

void audio_player_pause(void) {}
void audio_player_resume(void) {}
void audio_player_stop(void) {}
void audio_player_seek(int seconds) {}
void audio_player_seek_ms(int ms) {}
int  audio_player_get_position_ms(void) { return 0; }
int  audio_player_get_position(void) { return 0; }
int  audio_player_get_duration(void) { return 0; }
bool audio_player_is_playing(void)  { return false; }
bool audio_player_is_paused(void)   { return false; }
void audio_player_set_speed(float speed) {}
void audio_player_set_volume(int volume) {}
int  audio_player_get_volume(void) { return 70; }
void audio_player_tick(void) {}
void audio_player_set_callback(audio_status_cb_t cb, void *user_data) {}

#endif // CONFIG_USE_ESP_ADF
