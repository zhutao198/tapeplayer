/**
 * @file audio_player.cpp
 * @brief 音频播放引擎实现
 *
 * 核心设计变更（评审修正）：
 * 1. 每次 play() 重建 pipeline，避免 terminate 后复用失败 (S-09)
 * 2. WAV 使用 wav_decoder，不回退到 mp3_decoder (M-09)
 * 3. seek/tick 使用毫秒级精度 (M-03/M-04)
 * 4. 跳帧仅在 ≥8x 最高档位执行，1.5x/2.0x/3.0x 仅变速不跳帧（M-10，S11 修正）
 * 5. 移除未使用的 opus_decoder.h (L-01)
 * 6. M3：估算时长按格式选 bytes/ms 系数（MP3/AAC/OGG=16, FLAC=64, Opus=12, WAV=176）
 * 7. S5：seek_ms 保留原暂停态
 * 8. S6：负速度按 |speed| 拉高 I2S 采样率实现变调快退
 * 9. M5：i2s element register 前 NULL 守卫
 */

#include "audio_player.h"
#include "config.h"
#include "tape_control.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include "display.h"  // R074: 调 display_request_main_tick() 强制切歌后 player 渲染

#ifdef CONFIG_USE_ESP_ADF

#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"   // R076-CODEC-18: 监听 decoder REPORT_MUSIC_INFO 调 i2s_stream_set_clk
// R035-014：审计确认 audio_common.h 不是间接依赖（注释后构建通过 exit 0），正式删除
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "mp3_decoder.h"
#include "mp3_decoder_esp_codec.h"   // R076-CODEC-7: 开源 MP3 解码器替代闭源 PV-MP3
#include "aac_decoder.h"
#include "flac_decoder.h"
#include "ogg_decoder.h"
#include "wav_decoder.h"
#include "esp_timer.h"
#include "filter_resample.h"
#include "driver/i2s.h"        // 遗留 I2S 驱动: i2s_set_pin 显式绑定 GPIO
#include <sys/stat.h>
#include <stdlib.h>
#include <math.h>
// 2026-07-03 R003: 注释 board.h（项目用 MAX98357 + SSD1306 非 ADF 开发板，未配置 audio_board Kconfig，
//   而代码未实际使用 board.h 中任何 API）
// #include "board.h"

#if defined(CONFIG_USE_BT_SPEAKER)
#include "bt_speaker.h"
static bool g_bt_active = false;           // BT 音箱模式是否激活（复用 g_i2s_writer）
#endif

static const char *TAG = "audio_player";

/* --- 全局状态 --- */
static audio_pipeline_handle_t  g_pipeline = NULL;
static audio_element_handle_t   g_fatfs_reader = NULL;
static audio_element_handle_t   g_decoder = NULL;
static audio_element_handle_t   g_i2s_writer = NULL;   // R068：每次 play 重建（弃用 R036-001 跨曲目复用）

// R076-CODEC-18: decoder element event callback 同步 i2s sample rate
// 当 decoder 上报 AEL_MSG_CMD_REPORT_MUSIC_INFO 时调 i2s_stream_set_clk
// (PV-MP3 mp3_decoder 元素内部自动同步; 我们的 esp_audio_simple_dec wrapper 不自动)
//
// R076-CODEC-18c: 不能在 RUNNING state 直接 set_clk (i2s channel reconfig 时崩),
// 必须先 pause i2s, 再 set_clk, 再 resume. 否则崩在 i2s_driver 内部.
static esp_err_t decoder_event_cb(audio_element_handle_t el, audio_event_iface_msg_t *event, void *ctx)
{
    if (event->cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO && ctx != NULL) {
        audio_element_info_t music_info = {0};
        audio_element_getinfo(el, &music_info);
        if (music_info.sample_rates > 0) {
            audio_element_handle_t i2s = (audio_element_handle_t)ctx;
            ESP_LOGI(TAG, "R076-CODEC-18c: music info %d Hz, %d ch, %d bit -> pause+reconfig+resume i2s",
                     music_info.sample_rates, music_info.channels, music_info.bits);
            // 关键: i2s 必须在 PAUSED 状态才能 reconfig
            audio_element_state_t st = audio_element_get_state(i2s);
            if (st == AEL_STATE_RUNNING) {
                audio_element_pause(i2s);  // 不需要 wait, callback 是同步调用
            }
            i2s_stream_set_clk(i2s, music_info.sample_rates, music_info.bits, music_info.channels);
            if (st == AEL_STATE_RUNNING) {
                audio_element_resume(i2s, 0, 0);
            }
        }
    }
    return ESP_OK;
}

static bool         g_is_playing = false;
static bool         g_is_paused = false;
static int          g_volume = AUDIO_OUTPUT_VOL;

/* V1.2 音量 dB 线性映射边界 (MAX98357A ALC 范围) */
#define VOL_DB_MIN  (-96)   // 静音
#define VOL_DB_MAX  (12)    // 最大增益
static int          g_total_duration_ms = 0;
static uint32_t     g_total_file_bytes = 0;
// R067：当前曲目 ID3v2 标签字节数（0 = 无 ID3 或非 MP3）。
// seek byte_pos = id3_skip + (ms * audio_bytes / duration_ms)，
// audio_bytes = total_file_bytes - id3_skip_bytes。
static int          g_id3_skip_bytes = 0;
static int          g_current_sample_rate = AUDIO_SAMPLE_RATE;  // R076-CODEC: I2S 当前采样率缓存 (48kHz via AUDIO_SAMPLE_RATE 定义)

// R076-CODEC-18: 监听 pipeline 事件, decoder 上报 MUSIC_INFO 时调 i2s_stream_set_clk 同步采样率
// (PV-MP3 mp3_decoder 元素内部自动同步; 我们的 esp_audio_simple_dec wrapper 不自动)
static audio_event_iface_handle_t g_evt      = NULL;
static TaskHandle_t               g_evt_task = NULL;

static void audio_event_task(void *arg)
{
    audio_event_iface_msg_t msg;
    while (1) {
        if (audio_event_iface_listen(g_evt, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO
            && msg.source != NULL) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo((audio_element_handle_t)msg.source, &music_info);
            if (g_i2s_writer && music_info.sample_rates > 0
                && (music_info.sample_rates != g_current_sample_rate
                    || music_info.bits != 16
                    || music_info.channels != 2)) {
                ESP_LOGI(TAG, "R076-CODEC-18: music info %d Hz, %d ch, %d bit -> reconfig i2s",
                         music_info.sample_rates, music_info.channels, music_info.bits);
                i2s_stream_set_clk(g_i2s_writer,
                                    music_info.sample_rates,
                                    music_info.bits,
                                    music_info.channels);
                g_current_sample_rate = music_info.sample_rates;
            }
        }
        if (msg.need_free_data && msg.data) {
            free(msg.data);
        }
    }
}
static uint64_t     g_play_start_us = 0;               // 本次播放起始（pause/resume 时重置）
static int64_t      g_play_offset_us = 0;              // pause 时锁存的已播放时长，resume 时叠加
static uint64_t     g_last_scrub_us = 0;               // M1: 上次跳帧时间戳（模块级全局）

/* R049b：A-B 区间复读状态（ms，-1=未标记） */
static int  g_ab_a_ms = -1;
static int  g_ab_b_ms = -1;
static bool g_ab_enabled = false;

/* --- R067-fix：ID3v2 跳过工具 ---
 * 问题：ESP-ADF v5.5 + esp_audio_codec 静态库的 mp3 decoder 在含 ID3v2
 *       标签的 MP3 文件上稳定崩（Guru Meditation BREAK，
 *       CODEC_ELEMENT_HELPER: reserve data 2 is 0x0）。
 *       现象：1/4/5 有 ID3v2 → 必崩；2/3/6 无 ID3v2 → 正常。
 * 解决：fatfs_stream 不会自动跳 ID3v2。在 set_uri 前我们手动 peek
 *       前 10 字节 → 解析 syncsafe size → 让 audio_element_set_byte_pos
 *       把 reader 起点设到 MP3 frame 开始。decoder 收到的就是纯音频数据。
 * 注意：seek/resume 走 g_total_file_bytes 比例映射，ID3 size 必须计入
 *       g_total_file_bytes 才能正确换算 ms ↔ byte_pos（见 seek path）。*/

// 计算 MP3 文件前部的 ID3v2 总长度（syncsafe size 解析）。返回值：
//   -1：未检测到 ID3v2 (非 mp3 / 无标签 / 文件 < 10B)
//    N：ID3v2 标签总长度（含 10B header），seek 到 file[N] 就是首个 MP3 frame
static int id3v2_total_size(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    uint8_t hdr[10];
    size_t n = fread(hdr, 1, 10, fp);
    fclose(fp);
    if (n < 10) return -1;
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') return -1;
    if (hdr[3] == 0 || hdr[4] == 0xFF) return -1;       // v2.2/2.4 暂不处理
    // syncsafe: 4 字节 each 用 7 位
    uint32_t sz = ((uint32_t)(hdr[6] & 0x7F) << 21)
                | ((uint32_t)(hdr[7] & 0x7F) << 14)
                | ((uint32_t)(hdr[8] & 0x7F) <<  7)
                | ((uint32_t)(hdr[9] & 0x7F));
    int total = 10 + (int)sz;
    return total > 0 ? total : -1;
}

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
        // R076-CODEC-17: 用 v2.6.2 simple_dec 路径 (mpeg_parser + esp_mp3_dec_decode)
        // 必须用 32K task_stack (实测 2K/8K 都栈溢出崩, esp_audio_simple_dec_process
        // 内部 esp_es_parse_frame + esp_mp3_dec_decode + minimp3 帧解码嵌套深)
        // stack_in_ext=false 避免 PSRAM+Harvard 冲突
        mp3_decoder_esp_codec_cfg_t cfg = DEFAULT_MP3_DECODER_ESP_CODEC_CONFIG();
        cfg.task_stack   = 32 * 1024;
        cfg.out_rb_size  = 16 * 1024;
        cfg.stack_in_ext = false;
        ESP_LOGI(TAG, "Using espressif/esp_audio_codec v2.6.2 simple_dec MP3 (CODEC-17, stack=32K)");
        return mp3_decoder_esp_codec_init(&cfg);
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
// R068-fix：抽 helper 让 init 和 play() 都能复用 i2s_writer 创建
static audio_element_handle_t create_i2s_writer(void)
{
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.std_cfg.gpio_cfg.bclk = I2S_BCK_IO;
    i2s_cfg.std_cfg.gpio_cfg.ws   = I2S_WS_IO;
    i2s_cfg.std_cfg.gpio_cfg.dout = I2S_DOUT_IO;
    i2s_cfg.std_cfg.gpio_cfg.din  = GPIO_NUM_NC;
    ESP_LOGI(TAG, "i2s_stream_init enter (BCLK=IO%d WS=IO%d DIN=IO%d)",
             I2S_BCK_IO, I2S_WS_IO, I2S_DOUT_IO);
    vTaskDelay(pdMS_TO_TICKS(5));
    audio_element_handle_t h = i2s_stream_init(&i2s_cfg);
    ESP_LOGI(TAG, "i2s_stream_init returned %p", (void *)h);
    if (h) {
        ESP_LOGI(TAG, "I2S pins bound: BCLK=IO%d WS=IO%d DIN=IO%d",
                 I2S_BCK_IO, I2S_WS_IO, I2S_DOUT_IO);
    } else {
        ESP_LOGE(TAG, "i2s_stream_init failed");
    }
    return h;
}

void audio_player_init(void)
{
    ESP_LOGI(TAG, "Initializing audio subsystem...");
    vTaskDelay(pdMS_TO_TICKS(5));  /* 强制 flush 串口，避免阻塞前日志丢失 */

    // R076-CODEC-18: init event_iface + 启动 event task
    // 用于把 decoder REPORT_MUSIC_INFO 转成 i2s_stream_set_clk 调用
    // (实际方案 B: 用 audio_element_set_event_callback 直接注册 callback, 不需要 event_iface)
    if (!g_evt) {
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        g_evt = audio_event_iface_init(&evt_cfg);
        if (g_evt) {
            ESP_LOGI(TAG, "R076-CODEC-18: event_iface init OK");
        }
    }
    if (!g_evt_task && g_evt) {
        xTaskCreate(audio_event_task, "audio_evt", 4096, NULL, 4, &g_evt_task);
    }

    // R068-fix：i2s_writer 不再"跨曲目复用"——每次 play() 都重建。
    // 原因：R036-001 的复用策略在跨曲目时与旧 element task 状态耦合，
    // 导致 R066/R067 修复未根除 BREAK（0x403743bd）。代价：~100ms 重建开销。
    g_i2s_writer = create_i2s_writer();

    ESP_LOGI(TAG, "Audio subsystem initialized (I2S writer %s)",
             g_i2s_writer ? "ready" : "FAILED-but-ignored");
}

/* ============================================================
 * 播放（每次重建 pipeline + 元素，避免 terminate 后复用 Bug）
 * ============================================================ */
bool audio_player_play(const char *filepath)
{
    if (!filepath || !*filepath) return false;

    ESP_LOGI(TAG, "Playing: %s", filepath);
    audio_player_stop(); // 确保上一个管道已销毁
    // R062-fix：复用 g_i2s_writer 跨 play 时，上一轮 stop 已将其置为
    // AEL_STATE_FINISHED，若不 reset 直接重新 register/run，i2s 元素 resume
    // 时仍为 finished 态 → pipeline 误判播放完成 → 无限重启（听不到声音）。
    // 这里在重建 pipeline 前将其强制拉回 INIT 态。
    if (g_i2s_writer) {
        audio_element_reset_state(g_i2s_writer);
    }

    // R049b：新曲目清空 A-B 标记（避免跨文件失效）
    g_ab_a_ms = -1;
    g_ab_b_ms = -1;
    g_ab_enabled = false;

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
    if (!g_fatfs_reader) {
        ESP_LOGE(TAG, "Failed to create FATFS reader");
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 3. 创建解码器
    g_decoder = create_decoder(filepath);
    if (!g_decoder) {
        ESP_LOGE(TAG, "Failed to create decoder");
        audio_element_deinit(g_fatfs_reader);
        g_fatfs_reader = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 4. 注册元素到管道
    // R035-015：audio_pipeline_register 失败时清理已注册的元素，避免句柄泄漏
    if (audio_pipeline_register(g_pipeline, g_fatfs_reader, "file") != ESP_OK) {
        ESP_LOGE(TAG, "register fatfs_reader failed");
        audio_element_deinit(g_fatfs_reader);
        g_fatfs_reader = NULL;
        audio_element_deinit(g_decoder);
        g_decoder = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }
    if (audio_pipeline_register(g_pipeline, g_decoder, "decoder") != ESP_OK) {
        ESP_LOGE(TAG, "register decoder failed");
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_element_deinit(g_fatfs_reader);
        g_fatfs_reader = NULL;
        audio_element_deinit(g_decoder);
        g_decoder = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }
    // R076-DBG：decoder 调试日志已在其创建分支拉满（esp_log_level_set DEBUG），
    // 让 minimp3 在主动 abort 前打印内部错误（帧头非法/采样率越界等）。
    // R068-fix：每首播放前重建 i2s_writer（放弃 R036-001 跨曲目复用）。
    // audio_player_stop() 已 deinit 并置 NULL；这里若还 NULL（boot 后第一首 / init失败）
    // 就重建。代价：~100ms 重建开销（i2s_driver_install + DMA buffer），换零状态耦合。
    if (!g_i2s_writer) {
        ESP_LOGI(TAG, "R068: i2s_writer not present, creating now");
        g_i2s_writer = create_i2s_writer();
        if (!g_i2s_writer) {
            ESP_LOGE(TAG, "R068: create_i2s_writer failed");
            audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
            audio_pipeline_unregister(g_pipeline, g_decoder);
            audio_element_deinit(g_fatfs_reader);
            audio_element_deinit(g_decoder);
            g_fatfs_reader = NULL;
            g_decoder = NULL;
            audio_pipeline_deinit(g_pipeline);
            g_pipeline = NULL;
            return false;
        }
    }
    // R035-015：第三次 audio_pipeline_register 添加返回值检查 + 失败清理
    // R036-001（已弃用，R068 改为每次重建）：i2s_writer 不再"跨曲目复用"——失败清理 deinit + 置 NULL
    if (audio_pipeline_register(g_pipeline, g_i2s_writer, "i2s") != ESP_OK) {
        ESP_LOGE(TAG, "register i2s_writer failed");
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_pipeline_unregister(g_pipeline, g_decoder);
        audio_element_deinit(g_fatfs_reader);
        audio_element_deinit(g_decoder);
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);
        audio_element_deinit(g_i2s_writer);  // R068：失败也 deinit
        g_fatfs_reader = NULL;
        g_decoder = NULL;
        g_i2s_writer = NULL;                  // R068：失败置 NULL
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 5. 链接管道: file → decoder → i2s
    const char *link_tags[3] = {"file", "decoder", "i2s"};
    esp_err_t link_err = audio_pipeline_link(g_pipeline, link_tags, 3);
    if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link pipeline (0x%x)", link_err);
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_pipeline_unregister(g_pipeline, g_decoder);
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);
        audio_element_deinit(g_fatfs_reader);
        audio_element_deinit(g_decoder);
        audio_element_deinit(g_i2s_writer);  // R068：失败也 deinit
        g_fatfs_reader = NULL;
        g_decoder = NULL;
        g_i2s_writer = NULL;                  // R068：失败置 NULL
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 6. 设置文件 URI
    audio_element_set_uri(g_fatfs_reader, filepath);

    // R076-CODEC: 严格按 ADF release/v2.x examples/player/pipeline_sdcard_mp3_control 重构
    // - 去掉手动 ID3v2 字节跳过 (R067/R076-EXP)，让 DEFAULT_MP3_DECODER_CONFIG 自己处理
    // - i2s 时钟改为 48000Hz（MAX98357A 支持 8k-96kHz；PV-MP3 在 44.1kHz 路径可能有 bug 触发 BREAK）
    g_id3_skip_bytes = 0;  // R076-CODEC: 保留用于 seek 修正，但不再手动跳过
    if (strcasecmp(get_file_ext(filepath), ".mp3") == 0) {
        const char *mp3_path = strstr(filepath, "://");
        const char *real_path = mp3_path ? mp3_path + 3 : filepath;
        if (real_path[0] == '/' && real_path[1] == '/') real_path++;
        int id3_sz = id3v2_total_size(real_path);
        if (id3_sz > 0) {
            g_id3_skip_bytes = id3_sz;
            // R076-CODEC: 不再调 audio_element_set_byte_pos 手动跳
            ESP_LOGI(TAG, "R076-CODEC: ID3v2 detected (%d bytes), skipping manual override", id3_sz);
        }
    }

    // 7. 设置 I2S 时钟 — 改用 48000Hz 替代 44100Hz (R076-CODEC: AUDIO_SAMPLE_RATE 现已为 48000)
    g_current_sample_rate = AUDIO_SAMPLE_RATE;
    i2s_stream_set_clk(g_i2s_writer, AUDIO_SAMPLE_RATE, 16, 2);

    // R076-CODEC-18: 给 decoder 元素设 event callback, 同步 i2s sample rate
    // (PV-MP3 mp3_decoder 元素内部自动同步; 我们的 simple_dec wrapper 不自动)
    audio_element_set_event_callback(g_decoder, decoder_event_cb, g_i2s_writer);

    // 8. 启动管道（R032-209: 检查返回值，失败即终止，避免进入播放态却无声）
    if (audio_pipeline_run(g_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed");
        // R034-004：复用 link 失败时的清理路径，避免 pipeline + 元素句柄泄漏
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_pipeline_unregister(g_pipeline, g_decoder);
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);
        audio_element_deinit(g_fatfs_reader);
        audio_element_deinit(g_decoder);
        g_fatfs_reader = NULL;
        g_decoder = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    g_is_playing = true;
    g_is_paused = false;

    g_play_start_us = esp_timer_get_time();
    g_play_offset_us = 0;
    g_last_scrub_us = 0;  // M1: 跨曲目重置跳帧时间戳

    // 9. 计算文件字节数（用于 seek/位置换算）
    // R067：去掉 ID3v2 头部，让后续 seek/duration 估算按"音频数据"算，
    // 避免把 ID3 标签字节错算成音频时长。
    struct stat st;
    if (stat(filepath, &st) == 0) {
        uint32_t total = (uint32_t)st.st_size;
        g_total_file_bytes = (total > (uint32_t)g_id3_skip_bytes)
                           ? total - (uint32_t)g_id3_skip_bytes : 0;
    } else {
        g_total_file_bytes = 0;
    }

    // M3：从文件大小按格式字节率估计 duration
    // 估算字节率表（bytes/ms，仅用于进度条显示）：
    //   MP3/AAC/OGG ≈ 128kbps → 16
    //   Opus ≈ 96kbps → 12
    //   FLAC ≈ 512kbps → 64（损失编码前样本率）
    //   WAV 44.1k/16bit/stereo 1411.2kbps → 176
    // 注意：实际编码比特率与文件有关，进度条仅作粗略展示，不用于精确 seek。
    g_total_duration_ms = 0;
    if (g_total_file_bytes > 0) {
        const char *ext = get_file_ext(filepath);
        int bytes_per_ms;
        if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".aac") == 0 ||
            strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".ogg") == 0) {
            bytes_per_ms = 16;
        } else if (strcasecmp(ext, ".opus") == 0) {
            bytes_per_ms = 12;
        } else if (strcasecmp(ext, ".flac") == 0) {
            bytes_per_ms = 64;
        } else if (strcasecmp(ext, ".wav") == 0) {
            bytes_per_ms = 176;
        } else {
            bytes_per_ms = 16;  // fallback 同 MP3
        }
        g_total_duration_ms = g_total_file_bytes / bytes_per_ms;
        ESP_LOGD(TAG, "Duration estimated from file size: %d ms (bytes/ms=%d, ext=%s)",
                 g_total_duration_ms, bytes_per_ms, ext);
        ESP_LOGW(TAG, "Estimated duration is approximate; progress bar/seek may be imprecise");
    }

    // 11. 应用当前音量
    audio_player_set_volume(g_volume);

    // R074-fix: 切歌后强制 player 重新渲染。某些文件切歌后 LVGL dirty tracking
    // 失效（player 对象未标 dirty），屏幕卡在切前状态或黑屏。强制 tick 一次
    // 让 lvgl_task 在持锁回调中调 ui_show_player 重新绘制。
    // 与 R073 修复（boot 1.5s 后强制 tick）机制对称——保证 play 路径也有强制刷新。
    display_request_main_tick();

    return true;
}

void audio_player_pause(void)
{
    if (g_is_playing && !g_is_paused && g_pipeline) {
        audio_pipeline_pause(g_pipeline);
        g_play_offset_us += (int64_t)(esp_timer_get_time() - g_play_start_us);
        g_is_paused = true;
        ESP_LOGI(TAG, "Paused");
    }
}

void audio_player_resume(void)
{
    if (g_is_playing && g_is_paused && g_pipeline) {
        g_play_start_us = esp_timer_get_time();
        audio_pipeline_resume(g_pipeline);
        g_is_paused = false;
        ESP_LOGI(TAG, "Resumed");
    }
}

#define AUDIO_STOP_TIMEOUT_MS   200   /* I2S writer 等待终态超时 */
#define AUDIO_STOP_POLL_MS      10    /* 超时轮询间隔 */

void audio_player_stop(void)
{
#if defined(CONFIG_USE_BT_SPEAKER)
    // BT 音箱模式复用 g_i2s_writer，必须先停 BT 管线再继续
    if (g_bt_active) audio_player_stop_bt();
#endif

    // R032-211：pipeline/writer 未就绪（OOM 或初始化失败）时直接重置状态返回，
    // 避免访问已释放/未创建的音频元素。
    if (!g_pipeline || !g_i2s_writer) {
        // R035-016：早返回分支统一清零全部时间相关状态变量，避免后续 play() 残留旧值
        g_is_playing = false;
        g_is_paused = false;
        g_play_start_us = 0;
        g_play_offset_us = 0;
        g_total_duration_ms = 0;
        g_id3_skip_bytes = 0;  // R067
        g_last_scrub_us = 0;
        return;
    }

    if (g_pipeline) {
        // R075-fix：不再手动 deinit 各 element！
        // 旧逻辑先 audio_element_deinit(fatfs/decoder/i2s) 释放内存，再调
        // audio_pipeline_deinit()，而后者内部会遍历 el_list 对【同一个已释放的
        // element】再 deinit 一次 → double-free → 堆损坏 → 表现为切歌后下一首
        // play() 在 Pipeline started 后崩溃（0x403743c0 BREAK / DoubleException）。
        // 崩溃与具体歌曲相关，是因为不同解码路径踩中损坏堆元数据的概率不同。
        //
        // 修复：只调一次 audio_pipeline_deinit()，它内部已统一 terminate + deinit
        // el_list 中所有 element（含 i2s_writer 的 destroy→i2s_driver_uninstall），
        // 完全满足 R068 "i2s_writer 每次重建" 的意图，且杜绝 double-free。
        //
        // R076-CODEC-9：先 audio_pipeline_stop() 让各 element 任务走完输入循环
        // 并【自然关闭 fatfs 文件描述符】再 terminate/deinit。否则直接 terminate
        // 会等 2s 超时强制删 file 任务 → fd 泄漏累积 → 连播数首后
        // "Too many open files" 无法再开文件（实测第 5 首起 vfs_fat 报错）。
        ESP_LOGI(TAG, "R068: stop pipeline (let elements close fds gracefully)");
        audio_pipeline_stop(g_pipeline);
        // 短暂等待 element 任务退出（file 任务关闭 fd），避免 terminate 超时强删
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "R068: terminate pipeline (force-stop all elements, 2s timeout)");
        audio_pipeline_terminate_with_ticks(g_pipeline, pdMS_TO_TICKS(2000));

        // 销毁管道 + 内部所有 element（仅此一次 deinit）
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
    }
    // element 内存已由 audio_pipeline_deinit 释放，此处仅清句柄，避免悬空指针
    g_fatfs_reader = NULL;
    g_decoder = NULL;
    g_i2s_writer = NULL;   // R068：每次 play() 重建

    g_is_playing = false;
    g_is_paused = false;

    g_total_duration_ms = 0;
    g_id3_skip_bytes = 0;  // R067：切歌时重置，新曲目重新探测
    g_play_offset_us = 0;
    g_last_scrub_us = 0;  // M1: 停止时重置跳帧时间戳
}

/* ============================================================
 * Seek（毫秒级内部实现）
 * ============================================================ */
void audio_player_seek(int seconds)
{
    audio_player_seek_ms(seconds * 1000);
}

static void audio_player_seek_ms_internal(int ms)
{
    if (!g_pipeline || !g_is_playing || !g_decoder || !g_fatfs_reader) return;

    if (g_total_duration_ms > 0 && g_total_file_bytes > 0) {
        // R067：g_total_file_bytes 已经是音频字节数（去掉 ID3v2）；
        // seek 目标 = id3_skip + ms * audio_bytes / duration。
        // R028/L1: int64_t 中转避免 uint64 隐式截断
        int64_t byte_pos = g_id3_skip_bytes
                         + (int64_t)ms * g_total_file_bytes / g_total_duration_ms;
        // R032-002 复审修订：ADF audio_element_set_byte_pos 入参为 int（32-bit），
        // 必须钳位到 INT32_MAX，避免 >2.1 GB 文件隐式窄化截断导致 seek 失准/跳轨。
        if (byte_pos > INT32_MAX) byte_pos = INT32_MAX;
        audio_element_set_byte_pos(g_fatfs_reader, (int)byte_pos);
    } else if (g_total_file_bytes > 0) {
        int64_t byte_pos = g_id3_skip_bytes
                         + (int64_t)ms * g_total_file_bytes / 3600000;
        // R032-002 复审修订：ADF 入参为 int，必须钳位避免窄化截断。
        if (byte_pos > INT32_MAX) byte_pos = INT32_MAX;
        audio_element_set_byte_pos(g_fatfs_reader, (int)byte_pos);
    }

    // C1: 重置 decoder byte_pos，使其从 reader 新位置重新开始解码
    audio_element_set_byte_pos(g_decoder, 0);

    g_play_start_us = esp_timer_get_time();
    g_play_offset_us = (int64_t)ms * 1000;
}

void audio_player_seek_ms(int ms)
{
    if (!g_pipeline || !g_is_playing) return;
    // S5：保留原暂停态——暂停时 seek 不再静默 resume
    bool was_paused = g_is_paused;
    if (!was_paused) {
        audio_pipeline_pause(g_pipeline);
    }
    audio_player_seek_ms_internal(ms);
    if (!was_paused) {
        audio_pipeline_resume(g_pipeline);
    } else {
        // R035-020：保持 paused：清掉内部函数的 start_us 赋值，避免 get_position_ms 在暂停态累积。
        // 注意此处依赖 audio_player_seek_ms_internal 已写入正确的 g_play_offset_us，
        // 否则 seek 后的位置计算会偏移。如有疑问，请同时审计 seek_ms_internal。
        g_play_start_us = 0;
    }
}

int audio_player_get_position_ms(void)
{
    if (!g_pipeline) return 0;

    // 累计播放时间 = 暂停前已累计 + 当前段播放时间（暂停期间不增加）
    int64_t total = g_play_offset_us;
    if (g_is_playing && !g_is_paused && g_play_start_us > 0) {
        total += (int64_t)(esp_timer_get_time() - g_play_start_us);
    }
    return (int)(total / 1000);
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
    if (!g_i2s_writer) return;

    int sample_rate;
    if (speed > 0) {
        // C3: 跳帧模式 — I2S 正常速率，seek 跳帧提供"快进"感（R034-011）
        if (tape_control_is_scrub_mode()) {
            sample_rate = AUDIO_SAMPLE_RATE;
        } else {
            sample_rate = (int)(AUDIO_SAMPLE_RATE * speed);
            // R035-009: sample_rate limits derived from AUDIO_SAMPLE_RATE * {0.5, 4.0}
            if (sample_rate < (AUDIO_SAMPLE_RATE / 2)) sample_rate = AUDIO_SAMPLE_RATE / 2;
            if (sample_rate > (AUDIO_SAMPLE_RATE * 4)) sample_rate = AUDIO_SAMPLE_RATE * 4;
        }
    } else {
        // R032-203：快退（speed<0）方向由 audio_player_tick 的跳帧向后 seek 实现，
        // 此处保持正常音高不变调（负采样率只会让音高失真，且跳帧已能模拟快退听感）。
        sample_rate = AUDIO_SAMPLE_RATE;
    }

    // 缓存命中则跳过冗余的 i2s_set_clk 调用
    if (sample_rate != g_current_sample_rate) {
        g_current_sample_rate = sample_rate;
        i2s_stream_set_clk(g_i2s_writer, sample_rate, 16, 2);
    }
}

/* 内部：仅设置 g_volume 并应用 I2S ALC（不触达 BT 回传，避免音量循环） */
static void apply_volume_alc(int volume)
{
    g_volume = volume;
    if (g_i2s_writer) {
        int alc_vol;
        if (volume <= 0) {
            alc_vol = VOL_DB_MIN;                          // -96 dB 静音
        } else {
            // 线性 dB：level 0..14 映射到 -96..+12，四舍五入
            alc_vol = VOL_DB_MIN + (volume * (VOL_DB_MAX - VOL_DB_MIN) + VOLUME_LEVEL_MAX / 2) / VOLUME_LEVEL_MAX;
        }
        // 安全钳位 (i2s_alc_volume_set 仅接受 -96..+12)
        if (alc_vol < VOL_DB_MIN) alc_vol = VOL_DB_MIN;
        if (alc_vol > VOL_DB_MAX) alc_vol = VOL_DB_MAX;
        i2s_alc_volume_set(g_i2s_writer, alc_vol);
    }
}

void audio_player_set_volume(int volume)
{
    // V1.2 音量模型：15 档逻辑音量 (level 0..VOLUME_LEVEL_MAX)，线性 dB 映射 -96..+12 dB
    // 覆盖 MAX98357A ALC 全动态范围 (i2s_alc_volume_set 范围 -96..+12 dB)。
    // dB(level) = -96 + level * (12 - (-96)) / 14，四舍五入。
    //   level 0  → -96 dB（静音）
    //   level 14 → +12 dB（最大增益，约每档 7.7 dB，等感知步进）
    if (volume < 0) volume = 0;
    if (volume > VOLUME_LEVEL_MAX) volume = VOLUME_LEVEL_MAX;
    apply_volume_alc(volume);

#if defined(CONFIG_USE_BT_SPEAKER)
    // BT 模式下把本地音量回传手机（AVRCP 绝对音量 0..127），使两端一致
    if (g_bt_active) {
        bt_speaker_report_volume((uint8_t)((uint32_t)volume * 127 / VOLUME_LEVEL_MAX));
    }
#endif
}

int audio_player_get_volume(void)
{
    return g_volume;
}

/* ============================================================
 * Tick — 处理管道状态 + 快进/快退跳帧
 *
 * 跳帧策略（档位 1.5/2.0/3.0 I2S 变速 + 8.0x 跳帧模式）：
 * - 1.5x / 2.0x / 3.0x：仅变速（I2S 采样率），不跳帧
 * - 8.0x（跳帧模式）：正常 I2S + 每 50ms seek 跳帧（跳 7/8 音频）
 * - 快退：所有档位都通过向后 seek 模拟
 * ============================================================ */
void audio_player_tick(void)
{
#if defined(CONFIG_USE_BT_SPEAKER)
    // BT 模式无 SD pipeline，跳过走带/跳帧/A-B 复读逻辑
    if (g_bt_active) return;
#endif
    if (!g_pipeline || !g_is_playing) return;

    // 检查管道状态（通过 I2S writer 元素状态判断）
    audio_element_state_t el_state = audio_element_get_state(g_i2s_writer);
    if (el_state == AEL_STATE_FINISHED || el_state == AEL_STATE_STOPPED) {
        ESP_LOGI(TAG, "Track finished");
        g_is_playing = false;
        if (g_status_cb) {
            g_status_cb(0, g_user_data); // 0 = finished
        }
        return; // R034-003 / R035-003：终止本帧，避免后续 FF/RW 跳帧代码在 FINISHED pipeline 上 pause/resume
    }

    // C2: duration fallback — 从文件大小估计（ADF 无 direct duration API）
    if (g_total_duration_ms <= 0 && g_total_file_bytes > 0) {
        // 128kbps 估计：文件字节 / 16 ≈ 毫秒
        g_total_duration_ms = g_total_file_bytes / 16;
        ESP_LOGD(TAG, "Duration estimated from file size: %d ms", g_total_duration_ms);
    }

    // R049b：A-B 区间复读循环（仅在播放中、已开且 A<B 时生效）
    if (g_ab_enabled && g_ab_a_ms >= 0 && g_ab_b_ms > g_ab_a_ms) {
        int cur = audio_player_get_position_ms();
        if (cur >= g_ab_b_ms) {
            ESP_LOGD(TAG, "AB loop: seek back to A (%d ms)", g_ab_a_ms);
            audio_pipeline_pause(g_pipeline);
            audio_player_seek_ms_internal(g_ab_a_ms);
            audio_pipeline_resume(g_pipeline);
        }
    }

    // 快进/快退跳帧处理
    tape_mode_t mode = tape_control_get_mode();
    if (mode == TAPE_MODE_NORMAL) return;

    float speed = tape_control_get_speed();
    float abs_speed = (speed > 0) ? speed : -speed;

    // 仅高档位（≥最高档位速度）执行跳帧；1.5x/2.0x/3.0x 仅靠 I2S 变速
    // 快退所有档位都跳帧（因为没有"倒放"能力，只能断续 seek）
    // R034-011：阈值由硬编码 4.0f 改为派生 tape_control_get_max_gear_speed()，
    // 避免修改 g_speed_steps[] 后此处 magic number 漂移
    bool need_seek = (abs_speed >= tape_control_get_max_gear_speed()) || (mode == TAPE_MODE_REWIND);

    if (!need_seek) return;

    uint64_t now = esp_timer_get_time();

    if ((now - g_last_scrub_us) < 50000) return; // 50ms 间隔
    g_last_scrub_us = now;

    // C3: 跳帧模式 — 跳 7/8（skip 350ms/50ms），其他档位 50ms × abs_speed（R034-011）
    int skip_ms;
    if (tape_control_is_scrub_mode() && mode == TAPE_MODE_FAST_FORWARD) {
        skip_ms = (int)(50.0f * (abs_speed - 1.0f));  // 跳帧模式：50 × (8-1) = 350ms
    } else {
        skip_ms = (int)(50.0f * abs_speed);           // 常规跳帧：50 × speed
    }

    int cur_ms = audio_player_get_position_ms();
    int target_ms;

    if (speed > 0) {
        target_ms = cur_ms + skip_ms;
        int duration_ms = g_total_duration_ms > 0 ? g_total_duration_ms : 3600000;
        if (target_ms > duration_ms) target_ms = duration_ms;
    } else {
        target_ms = cur_ms - skip_ms;
        if (target_ms < 0) target_ms = 0;
    }

    // M2 + C1: pause 确保 reader idle，seek 后 resume
    audio_pipeline_pause(g_pipeline);
    audio_player_seek_ms_internal(target_ms);
    audio_pipeline_resume(g_pipeline);
}

void audio_player_set_callback(audio_status_cb_t cb, void *user_data)
{
    g_status_cb = cb;
    g_user_data = user_data;
}

/* ============================================================
 * R049b：A-B 区间复读
 * ============================================================ */
void audio_player_mark_a(void)
{
    if (!g_is_playing) {
        ESP_LOGW(TAG, "AB mark A: not playing");
        return;
    }
    g_ab_a_ms = audio_player_get_position_ms();
    g_ab_b_ms = -1;   // 重新标记 B
    ESP_LOGI(TAG, "AB mark A = %d ms", g_ab_a_ms);
}

void audio_player_mark_b(void)
{
    if (g_ab_a_ms < 0) {
        ESP_LOGW(TAG, "AB mark B: A not set");
        return;
    }
    g_ab_b_ms = audio_player_get_position_ms();
    if (g_ab_b_ms <= g_ab_a_ms) g_ab_b_ms = g_ab_a_ms + 1000; // 保证 B>A
    ESP_LOGI(TAG, "AB mark B = %d ms (span %d ms)", g_ab_b_ms, g_ab_b_ms - g_ab_a_ms);
}

void audio_player_clear_ab(void)
{
    g_ab_a_ms = -1;
    g_ab_b_ms = -1;
    g_ab_enabled = false;
    ESP_LOGI(TAG, "AB cleared");
}

void audio_player_set_ab_enabled(bool en)
{
    // 未标记 A/B 时强制关闭，避免无效循环
    if (en && (g_ab_a_ms < 0 || g_ab_b_ms <= g_ab_a_ms)) {
        ESP_LOGW(TAG, "AB enable ignored: A/B not set");
        return;
    }
    g_ab_enabled = en;
    ESP_LOGI(TAG, "AB enabled = %d", g_ab_enabled);
}

bool audio_player_is_ab_enabled(void) { return g_ab_enabled; }
int  audio_player_ab_a_ms(void) { return g_ab_a_ms; }
int  audio_player_ab_b_ms(void) { return g_ab_b_ms; }

/* R051：菜单内 A-B 微调 —— 直接设置 A/B 点到任意毫秒位置 */
void audio_player_set_ab_a_ms(int ms)
{
    if (ms < 0) return;
    if (g_ab_b_ms >= 0 && ms >= g_ab_b_ms) {
        g_ab_b_ms = ms + 1000;   // A 越过 B，则 B 顺延 1s
    }
    g_ab_a_ms = ms;
    ESP_LOGI(TAG, "AB set A = %d ms", g_ab_a_ms);
}

void audio_player_set_ab_b_ms(int ms)
{
    if (ms <= 0) return;
    if (g_ab_a_ms >= 0 && ms <= g_ab_a_ms) ms = g_ab_a_ms + 1000;  // 保证 B>A
    g_ab_b_ms = ms;
    ESP_LOGI(TAG, "AB set B = %d ms", g_ab_b_ms);
}

/* ============================================================
 * R049c：按键提示音
 * 复用空闲的 g_i2s_writer 播放一段内存 PCM（raw_stream → i2s），
 * 与音乐互斥（仅非播放态调用），避免 I2S 冲突。
 * ============================================================ */
static bool g_beep_busy = false;

void audio_player_play_beep(void)
{
    if (g_beep_busy)   return;
    if (g_is_playing)  return;     // 播放音乐时不提示（避免打断音乐）
    if (!g_i2s_writer) return;
    g_beep_busy = true;

    const int rate = 44100, ch = 2, bits = 16, ms = 60;
    const int n = rate * ch * (bits / 8) * ms / 1000;
    static uint8_t *buf = NULL;
    static int      buf_cap = 0;
    if (!buf || buf_cap < n) {
        if (buf) free(buf);
        buf = (uint8_t *)malloc(n);
        buf_cap = n;
    }
    if (!buf) { g_beep_busy = false; return; }

    int16_t *pcm = (int16_t *)buf;
    int nsamp = n / 2;
    float PI = 3.14159265f;
    int fade = (int)(0.002f * rate);   // 2ms 淡入淡出防爆音
    if (fade < 1) fade = 1;
    for (int i = 0; i < nsamp; i++) {
        float t = (float)i / (float)rate;
        float env = 1.0f;
        if (i < fade)            env = (float)i / fade;
        else if (i > nsamp - fade) env = (float)(nsamp - i) / fade;
        pcm[i] = (int16_t)(sinf(2.0f * PI * 880.0f * t) * 6000.0f * env);
    }

    audio_pipeline_cfg_t pcfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t p = audio_pipeline_init(&pcfg);
    if (!p) { g_beep_busy = false; return; }

    raw_stream_cfg_t rcfg = RAW_STREAM_CFG_DEFAULT();
    rcfg.type = AUDIO_STREAM_WRITER;
    audio_element_handle_t raw = raw_stream_init(&rcfg);
    if (!raw) { audio_pipeline_deinit(p); g_beep_busy = false; return; }

    audio_pipeline_register(p, raw, "raw");
    audio_pipeline_register(p, g_i2s_writer, "i2s");
    const char *tags[2] = {"raw", "i2s"};
    audio_pipeline_link(p, tags, 2);
    i2s_stream_set_clk(g_i2s_writer, rate, bits, ch);
    audio_pipeline_run(p);

    int off = 0, left = n;
    while (left > 0) {
        int w = raw_stream_write(raw, (char *)buf + off, left);
        if (w <= 0) break;
        off += w; left -= w;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    audio_element_set_ringbuf_done(raw);

    vTaskDelay(pdMS_TO_TICKS(ms + 60));   // 等播放完

    audio_pipeline_stop(p);
    audio_pipeline_unregister(p, raw);
    audio_pipeline_unregister(p, g_i2s_writer);  // 保留 g_i2s_writer 不销毁
    audio_pipeline_deinit(p);
    audio_element_deinit(raw);

    g_beep_busy = false;
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
int  audio_player_get_volume(void) { return AUDIO_OUTPUT_VOL; }  // R032-303：使用默认音量常量，消除硬编码耦合
void audio_player_tick(void) {}
void audio_player_set_callback(audio_status_cb_t cb, void *user_data) {}

/* R049b / R049c stub */
void audio_player_mark_a(void) {}
void audio_player_mark_b(void) {}
void audio_player_clear_ab(void) {}
void audio_player_set_ab_enabled(bool en) { (void)en; }
bool audio_player_is_ab_enabled(void) { return false; }
int  audio_player_ab_a_ms(void) { return -1; }
int  audio_player_ab_b_ms(void) { return -1; }
void audio_player_set_ab_a_ms(int ms) { (void)ms; }
void audio_player_set_ab_b_ms(int ms) { (void)ms; }
void audio_player_play_beep(void) {}

#endif // CONFIG_USE_ESP_ADF

/* ============================================================
 * 蓝牙音箱 (A2DP Sink) — 仅 ADF + USE_BT_SPEAKER 编译真实实现
 * 复用 audio_player 的 g_i2s_writer（I2S/MAX98357 输出），链路：
 *   A2DP Sink 解码 PCM (bluetooth_service 元素) → i2s_stream_writer
 * ============================================================ */
#if defined(CONFIG_USE_ESP_ADF) && defined(CONFIG_USE_BT_SPEAKER)

/* 手机端音量回调：映射到 level 并应用本地 ALC（不回传手机，避免循环） */
static void bt_phone_vol_cb(uint8_t vol_0_127)
{
    int level = (int)((uint32_t)vol_0_127 * VOLUME_LEVEL_MAX / 127);
    if (level > VOLUME_LEVEL_MAX) level = VOLUME_LEVEL_MAX;
    apply_volume_alc(level);
}

bool audio_player_start_bt(void)
{
    if (!g_i2s_writer) return false;
    audio_player_stop();   // 释放 SD 管道（保留 g_i2s_writer 跨模式复用）

    if (bt_speaker_start(g_i2s_writer) != ESP_OK) return false;

    bt_speaker_set_volume_cb(bt_phone_vol_cb);
    g_bt_active = true;
    // 同步当前音量到手机 (AVRCP 绝对音量 0..127)
    bt_speaker_report_volume((uint8_t)((uint32_t)g_volume * 127 / VOLUME_LEVEL_MAX));
    return true;
}

void audio_player_stop_bt(void)
{
    if (!g_bt_active) return;
    bt_speaker_stop();
    g_bt_active = false;
}

bool audio_player_is_bt_active(void) { return g_bt_active; }

#else  /* 非 ADF 或 未开 USE_BT_SPEAKER：桩实现，保证链接 */

bool audio_player_start_bt(void) { return false; }
void audio_player_stop_bt(void) {}
bool audio_player_is_bt_active(void) { return false; }

#endif
