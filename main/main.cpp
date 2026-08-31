/**
 * @file main.cpp
 * @brief ESP32-S3 鍚功鏈轰富绋嬪簭
 *
 * 涓诲惊鐜€昏緫锛?
 * 1. 鎸夐敭鎵弿 鈫?浜嬩欢鍒嗗彂锛堟敮鎸佺煭鎸?鍙屽嚮/闀挎寜/瓒呴暱鎸?HOLD/RELEASE锛?
 * 2. 纾佸甫鎺у埗鍣?tick 鈫?妗ｄ綅鍒囨崲
 * 3. 闊抽鎾斁鍣?tick 鈫?绠￠亾缁存姢/璺冲抚/浜嬩欢鐩戝惉
 * 4. 璁剧疆鑷姩淇濆瓨 鈫?姣?30 绉掍繚瀛樻柇鐐?
 * 5. 鐢垫簮绠＄悊 tick 鈫?鐢甸噺妫€娴?瀹氭椂鍏虫満
 * 6. 鏄剧ず灞忓埛鏂?
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_sleep.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

#include "config.h"
#include "button_manager.h"
#include "tape_control.h"
#include "playlist.h"
#include "display.h"
#include "audio_player.h"
#include "settings.h"
#include "power_mgmt.h"
#include "menu.h"
#include "ota_sd.h"
#if defined(CONFIG_USE_BT_SPEAKER)
#include "bt_speaker.h"
#endif
#include "esp_ota_ops.h"

#include "bookmark.h"
#include "led_strip.h"

static const char *TAG = "main";

#if defined(CONFIG_USE_BT_SPEAKER)
static char g_bt_device_name[32] = {0};
static bool g_bt_connected = false;
static bool g_bt_paused = false;

/* BT 鐘舵€佸洖璋冿細鍒锋柊杩炴帴鐘舵€佷笌璁惧鍚嶏紝渚?UI 浣跨敤 */
static void on_bt_state(bt_speaker_state_t state, const char *name)
{
    switch (state) {
    case BT_SPEAKER_STATE_CONNECTED:
        g_bt_connected = true;
        if (name) strncpy(g_bt_device_name, name, sizeof(g_bt_device_name) - 1);
        break;
    case BT_SPEAKER_STATE_DISCONNECTED:
    case BT_SPEAKER_STATE_STOPPED:
        g_bt_connected = false;
        g_bt_device_name[0] = '\0';
        break;
    default:
        break;
    }
}
#endif

/* ============================================================
 * 鎾斁妯″紡锛堜笌 DESIGN 8.2 涓€鑷达級
 * ============================================================ */
typedef enum {
    PLAY_MODE_SEQUENCE = 0,    // 椤哄簭鎾斁锛氭挱瀹屽垪琛ㄥ悗鍋滄
    PLAY_MODE_REPEAT_ALL,      // 鍏ㄩ儴寰幆
    PLAY_MODE_REPEAT_ONE,      // 鍗曟洸寰幆
} play_mode_t;

/* ============================================================
 * 鍏ㄥ眬鐘舵€侊紙涓?DESIGN 8.3 涓€鑷达級
 * ============================================================ */
typedef enum {
    APP_STATE_IDLE,            // 绌洪棽 (鏃犳枃浠?
    APP_STATE_STOPPED,         // 鍋滄
    APP_STATE_PLAYING,         // 鎾斁涓?
    APP_STATE_PAUSED,          // 鏆傚仠
    APP_STATE_FAST_FORWARD,    // 蹇繘锛堢甯︽ā寮忥級
    APP_STATE_REWIND,          // 蹇€€锛堢甯︽ā寮忥級
    APP_STATE_BROWSING,        // 鏂囦欢澶规祻瑙?
    APP_STATE_MENU,            // 缁熶竴璁剧疆鑿滃崟 (R049)
    APP_STATE_BT_SPEAKER,      // 钃濈墮闊崇 (A2DP Sink)
    APP_STATE_OTA,             // TF 鍗″浐浠跺崌绾у悜瀵?(R049c 鐪熷疄鍖?
} app_state_t;

// 鎵€鏈夊叏灞€鍙橀噺鍗曚换鍔¤闂紝鏃犻渶 volatile锛圡-9/L-8: 璁捐纭 OK锛?
static app_state_t    g_app_state = APP_STATE_IDLE;
static int            g_current_track = 0;
static uint64_t       g_last_display_update = 0;
static int64_t        g_next_loop_deadline = 0;
static int            g_vol_down_counter = 0;  // 闊抽噺鍑忛暱鎸夎鏁板櫒锛堟瘡 5 姝ヨ皟 1 绾э級
static int            g_vol_up_counter = 0;    // 闊抽噺鍔犻暱鎸夎鏁板櫒锛堟瘡 5 姝ヨ皟 1 绾э級
static int            g_seek_on_play_position = 0;  // 鏂偣鎭㈠ seek 鐩爣锛堢锛?
// g_last_auto_save_us: 涓?auto_save/settings_flush/power_mgmt 鑰﹀悎锛屽崟浠诲姟涓?OK锛圡-15: 璁捐绾э紝鍙帴鍙楋級
static uint64_t       g_last_auto_save_us = 0;
static play_mode_t    g_play_mode = PLAY_MODE_SEQUENCE;

// 寤惰繜澶勭悊锛氭洸鐩挱瀹?鈫?涓诲惊鐜鐞嗕笅涓€棣栵紙閬垮厤鍦ㄥ洖璋冨唴宓屽璋?play锛?
static bool           g_pending_track_finished = false;
static int            g_pending_track_next = -1;
static int            g_pending_track_seek = 0;

// 寤惰繜 NVS 淇濆瓨锛堥伩鍏嶅洖璋冨唴鍚屾鍐?NVS锛?
static int            g_pending_save_track = -1;
static int            g_pending_save_position = 0;

static int            g_browse_index = 0;              // 娴忚妯″紡閫変腑绱㈠紩
static app_state_t    g_state_before_browse = APP_STATE_STOPPED;
static app_state_t    g_state_before_menu   = APP_STATE_STOPPED;
static uint32_t       g_browse_repeat_ms = 0;          // 娴忚闀挎寜杩炵画绉诲姩鍩哄噯鏃跺埢 (hold_ms)

// 缁勫悎閿?REW+STOP锛氫袱閿湪 COMBO_WINDOW_US 鍐呭厛鍚?鍚屾椂鐭寜 鈫?璺冲埌褰撳墠鏇查
static uint64_t       g_combo_rew_us = 0;
static uint64_t       g_combo_stop_us = 0;

// R049c锛氫俊鎭睆瑕嗙洊锛圤TA/USB/鍏充簬绛夋々鎻愮ず锛?.5s 鍚庤嚜鍔ㄦ秷澶憋級
static char     g_info_title[32] = {0};
static char     g_info_text[64] = {0};
static uint64_t g_info_until_us = 0;
static bool     g_info_active = false;
static bool     g_ota_in_progress = false;  // OTA 鍐欏叆涓細鐙崰 SD 鍗★紝灞忚斀涓诲惊鐜彃鎷斿鐞?

/**
 * 娴忚妯″紡闀挎寜杩炵画绉诲姩鐨勯棿闅?(ms/鏇?锛氶殢鎸変綇鏃堕暱鍔犻€熺缉鐭€?
 * 鍒氳繘鍏ラ暱鎸夌敤 BROWSE_REPEAT_MS_INIT锛岃秴杩?BROWSE_HOLD_ACCEL_MS 鍚庣敤鏈€蹇棿闅斻€?
 */
static uint32_t browse_repeat_interval(uint32_t hold_ms)
{
    if (hold_ms >= BROWSE_HOLD_ACCEL_MS) {
        return BROWSE_REPEAT_MS_MIN;
    }
    if (hold_ms >= BROWSE_HOLD_ACCEL_MS / 2) {
        return BROWSE_REPEAT_MS_FAST;
    }
    return BROWSE_REPEAT_MS_INIT;
}

static sdmmc_card_t   *g_sd_card = NULL;  // SD 鍗″彞鏌?
static uint64_t    g_last_sd_check_us = 0;
static int         g_sd_read_fail_cnt = 0;  // R094: SD 健康检查连续失败计数，避免瞬时 CRC 误判移除
#define SD_READ_FAIL_THRESHOLD   2          // 连续 N 次(每次含重试)读失败才判为移除
static bool        g_sd_inserted = false; // SD 鍗″湪浣?鍘绘姈鍚庢彁浜ょ姸鎬?
static int         g_sd_cd_raw = -1;      // SD_CD 鍘熷鐢靛钩(鍘绘姈鐢?
static int         g_sd_cd_stable_cnt = 0;// 鍚岀數骞宠繛缁噰鏍锋鏁?

#define AUTO_SAVE_INTERVAL_US  (30 * 1000000)  // 30 绉掕嚜鍔ㄤ繚瀛?
#define SD_CHECK_INTERVAL_US   (5 * 1000000)   // 5 绉掓鏌?SD 鍗＄姸鎬?
#define COMBO_WINDOW_US        (250 * 1000)    // 缁勫悎閿?REW+STOP 鍒ゅ畾绐楋細250ms 鍐呬袱閿煭鎸夌畻鍚屾椂

/* ============================================================
 * 杈呭姪锛氫繚瀛樺綋鍓嶆柇鐐?
 * ============================================================ */
/* R095: NVS commit(Flash 写)期间 ESP32-S3 会全局禁用 cache；若解码任务(另一核执行 Flash
 * 中的 MP3Decode)正在运行，会触发 "Cache disabled but cached memory region accessed" 崩溃
 * (实际复现于 FF/REW seek)。故仅在解码停止(PAUSED/STOPPED/IDLE)时才真正落盘；PLAYING/FF/REW
 * 解码活跃时只写 NVS RAM(staged)，由暂停/停止/休眠等安全点统一 flush。 */
static void flush_nvs_if_safe(void)
{
    /* R098: 仅在解码活跃态(PLAYING/FF/REW)跳过落盘，避免 nvs_commit 禁用 Flash cache
       撞上并发 IROM/PSRAM 访问(解码任务)。PAUSED/STOPPED/IDLE 等解码已停态落盘，保证
       音量/位置等设置持久化(重启不丢)。崩溃根因已确认是 seek 垃圾头 nSlots(自对齐已修)，
       与落盘无关，故恢复 PAUSED 态落盘。 */
    bool decoding = (g_app_state == APP_STATE_PLAYING ||
                     g_app_state == APP_STATE_FAST_FORWARD ||
                     g_app_state == APP_STATE_REWIND);
    if (!decoding) settings_flush();
}

static void save_current_position(void)
{
    // R034-002锛氬寘鍚?FF/RW 鎬侊紝閬垮厤蹇繘/蹇€€涓帀鐢靛悗缁挱鐐瑰仠鐣欏湪涓婃鏅€氭挱鏀句綅缃?
    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
        g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
        char name[FILENAME_MAX_LEN] = "";
        playlist_get_name(g_current_track, name, sizeof(name));
        int pos = audio_player_get_position();
        int dur = audio_player_get_duration();
        if (dur > 0 && pos > dur) pos = dur;   /* R100: 钳制断点数<=曲长，防恢复点越文件尾"播放即结束" */
        settings_save_position(g_current_track, pos, name);
        // S8锛歴eek/鍒囨瓕鍚庣珛鍗?flush锛岄伩鍏嶆柇鐢典涪澶辨渶杩戜竴娆℃柇鐐?
        flush_nvs_if_safe();
    }
}

/* ============================================================
 * 杈呭姪锛氬仠姝?鎾斁/璺宠浆
 * ============================================================ */
static void stop_playback(void)
{
    save_current_position();
    // 缂撳瓨褰撳墠浣嶇疆锛屼緵 STOPPED鈫掓挱鏀?缁挱锛堜富寰幆涓嶅啀娓呴浂锛岃 BTN_ID_PLAY_PAUSE锛夈€?
    // 蹇呴』鍦?audio_player_stop() 涔嬪墠璇诲彇锛岀閬撻攢姣佸悗 get_position 澶辨晥銆?
    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
        g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
        g_seek_on_play_position = audio_player_get_position();
    }
    audio_player_stop();
    // 缁熶竴閫€鍑虹甯︽ā寮忥紙涓嶉檺 FF/RW锛?
    if (g_app_state == APP_STATE_FAST_FORWARD) {
        tape_control_ff_release();
    } else if (g_app_state == APP_STATE_REWIND) {
        tape_control_rewind_release();
    }
    g_app_state = APP_STATE_STOPPED;
}

/* 缁勫悎閿?REW+STOP锛氳烦鍒板綋鍓嶆洸棣栵紙浠庡ご锛夈€?
 * 浠呮挱鏀剧浉鍏虫€佹湁鏁堬細鎾斁/鏆傚仠/蹇繘閫€鎬佺珛鍗?seek(0) 骞惰惤鐩橈紱鍋滄鎬佸垯鎶婄画鎾綅缃涓?0銆?*/
static void jump_to_track_start(void)
{
    if (g_app_state == APP_STATE_FAST_FORWARD)      tape_control_ff_release();
    else if (g_app_state == APP_STATE_REWIND)       tape_control_rewind_release();

    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
        g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
        audio_player_set_speed(TAPE_SPEED_NORMAL);
        audio_player_seek(0);
        save_current_position();   // 钀界洏鏇查浣嶇疆锛?锛?
        if (g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
            g_app_state = APP_STATE_PLAYING;
        }
    } else if (g_app_state == APP_STATE_STOPPED) {
        g_seek_on_play_position = 0;   // 鍋滄鎬侊細涓嬫鎾斁浠庡ご
    }
    ESP_LOGI(TAG, "Combo REW+STOP: jump to track start");
}

static void play_current_track(void)
{
    /* R100: no SD card -> do not start playback (file-open fails => UI 'playing'
       but silent; leftover error pipeline can hang SD re-mount -> freeze) */
    if (!g_sd_inserted || g_sd_card == NULL) {
        display_show_no_card();
        g_app_state = APP_STATE_IDLE;
        ESP_LOGW(TAG, "Play requested but no SD mounted");
        return;
    }
    char filepath[FILENAME_MAX_LEN * 4];  // R032-001: 鎵╁埌 *4 涓?playlist 璺緞缂撳啿涓€鑷达紝閬垮厤鎺ユ敹鏃舵埅鏂?
    if (playlist_get_path(g_current_track, filepath, sizeof(filepath))) {
        if (audio_player_play(filepath)) {
            // 濡傛灉鏈夋柇鐐逛綅缃紙浠?NVS 鎭㈠鎴栧垏鎹㈡洸鐩椂鎸囧畾锛?
            if (g_seek_on_play_position > 0) {
                audio_player_seek(g_seek_on_play_position);
                g_seek_on_play_position = 0;
            }
            // R034-001锛氫緷鎹?tape_control_get_mode() 杩樺師鐘舵€侊紝閬垮厤 FF/RW 璺ㄦ洸鏃?
            // g_app_state 涓?tape mode 涓嶄竴鑷达紙琛屼负瀵逛絾鍥炬爣/鐘舵€佹満閿欎綅锛?
            tape_mode_t m = tape_control_get_mode();
            if (m == TAPE_MODE_FAST_FORWARD) {
                g_app_state = APP_STATE_FAST_FORWARD;
            } else if (m == TAPE_MODE_REWIND) {
                g_app_state = APP_STATE_REWIND;
            } else {
                g_app_state = APP_STATE_PLAYING;
            }
            g_last_auto_save_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Now playing: %s", filepath);
        }
    }
}

/* 鍒囨崲鎾斁妯″紡 */
static void cycle_play_mode(void)
{
    g_play_mode = (play_mode_t)((g_play_mode + 1) % 3);
    const char *mode_str[] = {"SEQ", "ALL", "ONE"};
    ESP_LOGI(TAG, "Play mode: %s", mode_str[g_play_mode]);
    settings_save_play_mode((int)g_play_mode);
    display_set_play_mode((int)g_play_mode);
}

/* 鐭寜璺宠浆 卤10s */
static void skip_seconds(int seconds)
{
    if (g_app_state != APP_STATE_PLAYING && g_app_state != APP_STATE_PAUSED) return;

    int cur = audio_player_get_position();
    int new_pos = cur + seconds;
    if (new_pos < 0) new_pos = 0;
    int duration = audio_player_get_duration();
    if (duration > 0 && new_pos > duration) new_pos = duration;
    audio_player_seek(new_pos);   // R099: 暂停式 seek(re-open 应用 byte_pos 可靠跳转)，单次不淹队列
    save_current_position();   // R032-104: seek 鍚庣珛鍗充繚瀛樻柇鐐瑰苟 flush锛岄伩鍏嶆柇鐢典涪澶?
    ESP_LOGI(TAG, "Skip %ds 鈫?pos=%d", seconds, new_pos);
}

/* ============================================================
 * 鏇茬洰鎾畬鍥炶皟
 * ============================================================ */
static void on_track_finished(int state, void *user_data)
{
    ESP_LOGI(TAG, "Track finished naturally");

    // 寮傛锛氫粎璁颁笅闇€瑕佷繚瀛樼殑浣嶇疆鍜屼笅涓€鏇诧紝涓诲惊鐜腑鎵ц
    g_pending_save_track = g_current_track;
    g_pending_save_position = 0;

    // 鏍规嵁鎾斁妯″紡鍐冲畾涓嬩竴棣栵紙浠呰涓嬬洰鏍囷紝涓诲惊鐜腑鎵ц璺宠浆锛?
    switch (g_play_mode) {
    case PLAY_MODE_SEQUENCE:
        if (g_current_track < playlist_count() - 1) {
            g_pending_track_next = playlist_next();
            g_pending_track_seek = 0;
            g_pending_track_finished = true;
        } else {
            g_app_state = APP_STATE_STOPPED;
            ESP_LOGI(TAG, "Playlist finished (sequence mode)");
        }
        break;
    case PLAY_MODE_REPEAT_ALL:
        g_pending_track_next = playlist_next();
        g_pending_track_seek = 0;
        g_pending_track_finished = true;
        break;
    case PLAY_MODE_REPEAT_ONE:
        g_pending_track_next = g_current_track;
        g_pending_track_seek = 0;
        g_pending_track_finished = true;
        break;
    }
}

/* ============================================================
 * 缁熶竴鑿滃崟瀹夸富鍥炶皟 (R049) 鈥?鐢?menu.cpp 璋冪敤
 * ============================================================ */
void app_menu_exit(void)
{
    menu_close();
    g_app_state = g_state_before_menu;
}

void app_enter_browse(void)
{
    if (playlist_count() == 0) {
        menu_close();
        g_app_state = g_state_before_menu;
        return;
    }
    g_state_before_browse = g_state_before_menu;
    menu_close();
    g_app_state = APP_STATE_BROWSING;
    g_browse_index = g_current_track;
    ESP_LOGI(TAG, "Enter browse via menu");
}

int app_get_play_mode(void)
{
    return (int)g_play_mode;
}

void app_set_play_mode(int m)
{
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    g_play_mode = (play_mode_t)m;
    settings_save_play_mode(m);
    display_set_play_mode(m);
}

/* R049c锛氫俊鎭睆锛堟々鍔熻兘鎻愮ず锛?*/
void app_show_info(const char *title, const char *text)
{
    strncpy(g_info_title, title ? title : "", sizeof(g_info_title) - 1);
    g_info_title[sizeof(g_info_title) - 1] = '\0';
    strncpy(g_info_text, text ? text : "", sizeof(g_info_text) - 1);
    g_info_text[sizeof(g_info_text) - 1] = '\0';
    g_info_until_us = esp_timer_get_time() + 1500000;  // 鏄剧ず 1.5s
    g_info_active = true;
    ESP_LOGI(TAG, "Info: %s", title);
}

/* R049c锛氭寜閿彁绀洪煶锛堣缃紑鍚笖闈炴挱鏀炬€佹椂鍦ㄨ彍鍗?娴忚/鍋滄鎬佹挱鏀撅級 */
void app_play_beep(void)
{
    if (settings_load_key_beep()) {
        audio_player_play_beep();
    }
}

/* R049c锛氳繘鍏?閫€鍑?TF 鍗″浐浠跺崌绾у悜瀵硷紙鐢?menu.cpp 鐨?app_ota_enter 璋冪敤锛?*/
void app_enter_ota(void)
{
    // 鍗囩骇鍓嶇‘淇濆仠姝㈡挱鏀撅紝鐙崰 SD 鍗?
    save_current_position();
    audio_player_stop();
    g_ota_in_progress = true;
    menu_close();
    g_app_state = APP_STATE_OTA;
    ota_sd_begin();
}

void app_ota_exit(void)
{
    g_ota_in_progress = false;
    g_app_state = g_state_before_menu;
}

/* ============================================================
 * 澶勭悊鎸夐敭浜嬩欢
 * ============================================================ */
static void handle_button_events(void)
{
    btn_event_info_t events[8];
    int n = button_manager_scan(events, sizeof(events) / sizeof(events[0]));
    static uint32_t scan_count = 0;
    if (n > 0) {
        scan_count++;
        if (scan_count <= 5) {
            // ESP_LOGI(TAG, "DBG: btn scan #%u got %d events, first id=%d ev=%d",
            //          (unsigned)scan_count, n, (int)events[0].id, (int)events[0].event);
        }
    }

    /* 缁熶竴鑿滃崟璺敱 (R049): 鑿滃崟鎵撳紑鏃舵墍鏈夋寜閿氦缁欒彍鍗曞鐞?*/
    if (menu_is_open()) {
        menu_handle_button(events, n);
        return;
    }

    /* R049c OTA 鍗囩骇鍚戝璺敱锛氱嫭绔嬩簬鑿滃崟锛岀‘璁?閿佹/浠绘剰閿噸鍚?*/
    if (g_app_state == APP_STATE_OTA) {
        ota_sd_handle_button(events, n);
        return;
    }

    /* 缁熶竴鑿滃崟鍏ュ彛: 闀挎寜 STOP 鍦ㄦ挱鏀剧浉鍏虫€佹墦寮€鑿滃崟 (鍙栦唬鍘?闀挎寜 STOP 杩涙祻瑙?) */
    for (int j = 0; j < n; j++) {
        if (events[j].id == BTN_ID_STOP &&
            events[j].event == BTN_EVENT_LONG_PRESS &&
            (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
             g_app_state == APP_STATE_STOPPED)) {
            g_state_before_menu = g_app_state;
            menu_open();
            g_app_state = APP_STATE_MENU;
            return;
        }
    }

    /* 缁勫悎閿?REW+STOP 璺虫洸棣栵細鍏堟娴嬫湰甯ф槸鍚︿袱閿悓鏃剁煭鎸夛紙鎺掗櫎娴忚/绌洪棽鎬侊級 */
    uint64_t now = esp_timer_get_time();
    bool in_play_ctx = (g_app_state != APP_STATE_BROWSING && g_app_state != APP_STATE_IDLE);
    bool frame_rew = false, frame_stop = false;
    if (in_play_ctx) {
        for (int j = 0; j < n; j++) {
            if (events[j].event == BTN_EVENT_SHORT_PRESS) {
                if (events[j].id == BTN_ID_REWIND) frame_rew = true;
                if (events[j].id == BTN_ID_STOP)    frame_stop = true;
            }
        }
    }
    bool combo_done = false;
    if (frame_rew && frame_stop) {
        jump_to_track_start();
        combo_done = true;
        g_combo_rew_us = 0;
        g_combo_stop_us = 0;
    }

    for (int i = 0; i < n; i++) {
        btn_event_info_t *e = &events[i];

        /* 璁板綍鐢ㄦ埛娲诲姩锛堝厛浜庨攣瀹氭鏌ワ紝閬垮厤閿佸畾鐘舵€佽瑙﹀彂浼戠湢锛?*/
        if (e->event != BTN_EVENT_NONE) {
            power_mgmt_record_activity();
        }

        /* 娴忚妯″紡锛氫笂涓€棣?涓嬩竴棣?鐭寜涓?涓嬩竴鏇诧紱闀挎寜/鎸佺画鎸変綇鍒欏姞閫熻繛缁Щ鍔紱
           鎾斁 閫夋嫨锛屽仠姝?閫€鍑猴紝鍋滄闀挎寜 缁欓€変腑鏇插姞涔︾ */
        if (g_app_state == APP_STATE_BROWSING) {
            int total = playlist_count();
            switch (e->id) {
            case BTN_ID_PREV:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_browse_index = (g_browse_index - 1 + total) % total;
                    g_browse_repeat_ms = 0;
                } else if (e->event == BTN_EVENT_LONG_PRESS) {
                    g_browse_repeat_ms = e->hold_ms;   // 閿氬畾璧风偣锛岄伩鍏嶉暱鎸夌灛闂撮噸澶嶈烦
                } else if (e->event == BTN_EVENT_HOLD ||
                           e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                    uint32_t step = browse_repeat_interval(e->hold_ms);
                    if (e->hold_ms - g_browse_repeat_ms >= step) {
                        g_browse_repeat_ms = e->hold_ms;
                        g_browse_index = (g_browse_index - 1 + total) % total;
                    }
                } else if (e->event == BTN_EVENT_RELEASE) {
                    g_browse_repeat_ms = 0;
                }
                break;
            case BTN_ID_NEXT:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_browse_index = (g_browse_index + 1) % total;
                    g_browse_repeat_ms = 0;
                } else if (e->event == BTN_EVENT_LONG_PRESS) {
                    g_browse_repeat_ms = e->hold_ms;
                } else if (e->event == BTN_EVENT_HOLD ||
                           e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                    uint32_t step = browse_repeat_interval(e->hold_ms);
                    if (e->hold_ms - g_browse_repeat_ms >= step) {
                        g_browse_repeat_ms = e->hold_ms;
                        g_browse_index = (g_browse_index + 1) % total;
                    }
                } else if (e->event == BTN_EVENT_RELEASE) {
                    g_browse_repeat_ms = 0;
                }
                break;
            case BTN_ID_PLAY_PAUSE:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_current_track = g_browse_index;
                    playlist_set_index(g_current_track);
                    g_seek_on_play_position = 0;
                    g_app_state = g_state_before_browse;
                    play_current_track();
                }
                break;
            case BTN_ID_REWIND:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    int first = 0;
                    g_browse_index = (g_browse_index - BROWSE_PAGE_STEP < first)
                                      ? first : g_browse_index - BROWSE_PAGE_STEP;
                } else if (e->event == BTN_EVENT_LONG_PRESS ||
                           e->event == BTN_EVENT_HOLD ||
                           e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                    g_browse_index = 0;          // 璺冲埌鍒楄〃澶?
                }
                g_browse_repeat_ms = 0;
                break;
            case BTN_ID_FAST_FORWARD: {
                int last = (total > 0) ? total - 1 : 0;
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_browse_index = (g_browse_index + BROWSE_PAGE_STEP > last)
                                      ? last : g_browse_index + BROWSE_PAGE_STEP;
                } else if (e->event == BTN_EVENT_LONG_PRESS ||
                           e->event == BTN_EVENT_HOLD ||
                           e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                    g_browse_index = last;        // 璺冲埌鍒楄〃灏?
                }
                g_browse_repeat_ms = 0;
                break;
            }
            case BTN_ID_STOP:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_app_state = g_state_before_browse;
                } else if (e->event == BTN_EVENT_LONG_PRESS) {
                    int bm = bookmark_add(g_browse_index, 0);
                    if (bm >= 0) ESP_LOGI(TAG, "Bookmark added at track %d (slot %d)", g_browse_index, bm);
                    else        ESP_LOGW(TAG, "Bookmark add failed at track %d", g_browse_index);
                }
                break;
            default:
                break;
            }
            continue;
        }

#if defined(CONFIG_USE_BT_SPEAKER)
        /* 钃濈墮闊崇妯″紡锛氭嫤鎴?PLAY/STOP锛屽叾浣欙紙闊抽噺绛夛級浜ょ粰涓嬫柟閫氱敤鍒嗗彂 */
        if (g_app_state == APP_STATE_BT_SPEAKER) {
            bool handled = false;
            switch (e->id) {
            case BTN_ID_PLAY_PAUSE:
                if (e->event == BTN_EVENT_SHORT_PRESS && g_bt_connected) {
                    if (!g_bt_paused) { bt_speaker_avrc_pause(); g_bt_paused = true; }
                    else              { bt_speaker_avrc_play();  g_bt_paused = false; }
                }
                handled = true;
                break;
            case BTN_ID_STOP:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    audio_player_stop_bt();
                    g_app_state = APP_STATE_STOPPED;
                }
                handled = true;
                break;
            default:
                handled = false;
                break;
            }
            if (handled) continue;   // PLAY/STOP 宸叉嫤鎴紱闊抽噺绛夌户缁蛋涓嬫柟閫氱敤澶勭悊
        }
#endif

        switch (e->id) {

        /* --- 鎾斁/鏆傚仠 --- */
        case BTN_ID_PLAY_PAUSE:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                app_play_beep();  // R049c 鎸夐敭鎻愮ず闊?
                if (g_app_state == APP_STATE_STOPPED || g_app_state == APP_STATE_IDLE) {
                    g_current_track = playlist_current_index();
                    // 涓嶅啀娓呴浂锛氭部鐢?stop_playback/init_storage 缂撳瓨鐨勪綅缃紙0 = 浠庡ご锛?
                    play_current_track();
                } else if (g_app_state == APP_STATE_PLAYING) {
                    audio_player_pause();
                    g_app_state = APP_STATE_PAUSED;
                } else if (g_app_state == APP_STATE_PAUSED) {
                    audio_player_resume();
                    g_app_state = APP_STATE_PLAYING;
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                /* 闀挎寜锛氬垏鎹㈡挱鏀炬ā寮忥紙椤哄簭 鈫?鍒楄〃寰幆 鈫?鍗曟洸寰幆锛?*/
                cycle_play_mode();
            }
            break;

        /* --- 鍋滄 --- */
        case BTN_ID_STOP:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                app_play_beep();  // R049c 鎸夐敭鎻愮ず闊?
                if (combo_done) {
                    // 鏈抚宸蹭綔涓虹粍鍚堥敭澶勭悊锛堜袱閿悓鎸夛級锛岃烦杩囧崟鐙€昏緫锛岄伩鍏嶉噸澶?stop/skip
                } else if (g_combo_rew_us && (now - g_combo_rew_us) < COMBO_WINDOW_US) {
                    // 涓庣◢鏃╃殑 REW 鐭寜鏋勬垚缁勫悎閿?鈫?璺虫洸棣?
                    jump_to_track_start();
                    g_combo_rew_us = 0;
                    g_combo_stop_us = 0;
                } else {
                    g_combo_stop_us = now;   // 璁板綍鍗曠嫭 STOP锛岀瓑寰呭彲鑳界殑 REW 缁勫悎
                    stop_playback();
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                /* 闀挎寜锛氳繘鍏ユ祻瑙堢晫闈?*/
                if (playlist_count() > 0) {
                    g_state_before_browse = g_app_state;
                    g_browse_index = g_current_track;
                    g_app_state = APP_STATE_BROWSING;
                    ESP_LOGI(TAG, "Enter browse mode, selected track %d", g_browse_index);
                }
            }
            break;

        /* --- 涓婁竴棣?--- */
        case BTN_ID_PREV:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                app_play_beep();  // R049c 鎸夐敭鎻愮ず闊?
                if (g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND)
                    break;   // 鎸変綇蹇繘/蹇€€鏈熼棿蹇界暐鍒囨瓕锛堢甯︽満浜掗攣锛?
                save_current_position();  // 鍒囨崲鍓嶄繚瀛樻棫浣嶇疆
                int prev = playlist_prev();  // R032-107: 绌哄垪琛ㄨ繑鍥?-1锛岄伩鍏嶆薄鏌?g_current_track
                if (prev >= 0) {
                    g_current_track = prev;
                    playlist_set_index(prev);
                    g_seek_on_play_position = 0;  // 鏂版洸鐩粠澶?
                    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                        play_current_track();
                    }
                }
            }
            /* R042: 闀挎寜/鎸佺画鎸変綇闊抽噺璋冭妭宸茶縼鍑鸿嚦涓撶敤 VOL卤 閿?(GPIO0/GPIO3) */
            break;

        /* --- 涓嬩竴棣?--- */
        case BTN_ID_NEXT:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                app_play_beep();  // R049c 鎸夐敭鎻愮ず闊?
                if (g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND)
                    break;   // 鎸変綇蹇繘/蹇€€鏈熼棿蹇界暐鍒囨瓕锛堢甯︽満浜掗攣锛?
                save_current_position();
                int next = playlist_next();  // R032-107: 绌哄垪琛ㄨ繑鍥?-1锛岄伩鍏嶆薄鏌?g_current_track
                if (next >= 0) {
                    g_current_track = next;
                    playlist_set_index(next);
                    g_seek_on_play_position = 0;
                    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                        play_current_track();
                    }
                }
            }
            /* R042: 闀挎寜/鎸佺画鎸変綇闊抽噺璋冭妭宸茶縼鍑鸿嚦涓撶敤 VOL卤 閿?(GPIO0/GPIO3) */
            break;

        /* --- 闊抽噺鍑?(LCK 宸︽嫧, GPIO0) --- */
        case BTN_ID_VOL_DOWN:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                g_vol_down_counter = 0;
                int vol = audio_player_get_volume();
                if (vol > 0) {
                    audio_player_set_volume(vol - 1);
                    display_show_volume(audio_player_get_volume());
                    settings_save_volume(audio_player_get_volume());
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS ||
                       e->event == BTN_EVENT_HOLD ||
                       e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                /* 鎷ㄨ疆鑷浣嶅満鏅笅 LONG_PRESS/HOLD 鏋佸皯瑙﹀彂; 鍏滃簳淇濈暀杩炵画鍑?*/
                g_vol_down_counter++;
                if (g_vol_down_counter % 5 == 0) {
                    int vol = audio_player_get_volume();
                    if (vol > 0) {
                        audio_player_set_volume(vol - 1);
                        display_show_volume(audio_player_get_volume());
                    }
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                g_vol_down_counter = 0;
                settings_save_volume(audio_player_get_volume());  // 鏉惧紑鏃朵繚瀛橀煶閲?
            }
            break;

        /* --- 闊抽噺鍔?(LCK 鍙虫嫧, GPIO3) --- */
        case BTN_ID_VOL_UP:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                g_vol_up_counter = 0;
                int vol = audio_player_get_volume();
                if (vol < VOLUME_LEVEL_MAX) {
                    audio_player_set_volume(vol + 1);
                    display_show_volume(audio_player_get_volume());
                    settings_save_volume(audio_player_get_volume());
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS ||
                       e->event == BTN_EVENT_HOLD ||
                       e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                g_vol_up_counter++;
                if (g_vol_up_counter % 5 == 0) {
                    int vol = audio_player_get_volume();
                    if (vol < VOLUME_LEVEL_MAX) {
                        audio_player_set_volume(vol + 1);
                        display_show_volume(audio_player_get_volume());
                    }
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                g_vol_up_counter = 0;
                settings_save_volume(audio_player_get_volume());
            }
            break;

        /* --- 蹇繘 --- */
        case BTN_ID_FAST_FORWARD:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                skip_seconds(SEEK_STEP_SEC);            // R045锛氱煭鎸夎烦 5 绉?
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                // 杩涘叆鍙橀€熸€侊細浠呭湪闀挎寜棣栨瑙﹀彂涓€娆★紙閬垮厤涓?HOLD 閲嶅璋冪敤 press锛?
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    if (g_app_state == APP_STATE_PAUSED) {
                        audio_player_resume();
                    }
                    skip_seconds(SEEK_STEP_SEC);            // R046锛氬厛缁ф壙鐭寜鍩哄噯璺宠繘 5 绉掞紝閬垮厤"鍒氳繃闀挎寜鍙嶈€屽€掗€€鏇村皯"鐨勬柇灞?
                    tape_control_ff_press();
                    audio_player_set_speed(tape_control_get_speed());
                    audio_player_scrub_enter();   /* R098: 暂停播放仅跳帧，decoder 停止后重置才安全 */
                    g_app_state = APP_STATE_FAST_FORWARD;
                    g_combo_rew_us = 0; g_combo_stop_us = 0;  // 杩涘叆鍙橀€熸€侊紝鏀惧純鏈畬鎴愮殑缁勫悎閿鏃?
                }
            } else if (e->event == BTN_EVENT_HOLD ||
                       e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                // 淇濇寔鎬侊細鍙橀€熸。浣嶇敱 tape_control_tick() 鎸夋寜浣忔椂闀胯嚜鍔ㄥ崌妗ｏ紝
                // 姝ゅ浠呯‘淇濋€熷害鍚屾锛坧ress 宸插湪 LONG_PRESS 璋冭繃锛屼笉閲嶅璋冪敤锛夈€?
                if (g_app_state == APP_STATE_FAST_FORWARD) {
                    audio_player_set_speed(tape_control_get_speed());
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                tape_control_ff_release();
                audio_player_set_speed(TAPE_SPEED_NORMAL);
                audio_player_scrub_exit();   /* R098: 从最后 seek 位置恢复播放 */
                g_app_state = APP_STATE_PLAYING;
                g_combo_rew_us = 0; g_combo_stop_us = 0;  // 閫€鍑哄彉閫熸€侊紝娓呯┖缁勫悎閿鏃?
            }
            break;

        /* --- 蹇€€ --- */
        case BTN_ID_REWIND:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                if (combo_done) {
                    // 鏈抚宸蹭綔涓虹粍鍚堥敭澶勭悊锛堜袱閿悓鎸夛級锛岃烦杩囧崟鐙€昏緫锛岄伩鍏嶉噸澶?stop/skip
                } else if (g_combo_stop_us && (now - g_combo_stop_us) < COMBO_WINDOW_US) {
                    // 涓庣◢鏃╃殑 STOP 鐭寜鏋勬垚缁勫悎閿?鈫?璺虫洸棣?
                    jump_to_track_start();
                    g_combo_rew_us = 0;
                    g_combo_stop_us = 0;
                } else {
                    g_combo_rew_us = now;    // 璁板綍鍗曠嫭 REW锛岀瓑寰呭彲鑳界殑 STOP 缁勫悎
                    skip_seconds(-SEEK_STEP_SEC);        // R045锛氱煭鎸夊悗閫€ 5 绉?
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                // 杩涘叆鍙橀€熸€侊細浠呭湪闀挎寜棣栨瑙﹀彂涓€娆★紙閬垮厤涓?HOLD 閲嶅璋冪敤 press锛?
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    if (g_app_state == APP_STATE_PAUSED) {
                        audio_player_resume();
                    }
                    skip_seconds(-SEEK_STEP_SEC);           // R046锛氬厛缁ф壙鐭寜鍩哄噯鍚庨€€ 5 绉掞紝閬垮厤"鍒氳繃闀挎寜鍙嶈€屽€掗€€鏇村皯"鐨勬柇灞?
                    tape_control_rewind_press();
                    audio_player_set_speed(tape_control_get_speed());
                    audio_player_scrub_enter();   /* R098: 暂停播放仅跳帧，decoder 停止后重置才安全 */
                    g_app_state = APP_STATE_REWIND;
                    g_combo_rew_us = 0; g_combo_stop_us = 0;  // 杩涘叆鍙橀€熸€侊紝鏀惧純鏈畬鎴愮殑缁勫悎閿鏃?
                }
            } else if (e->event == BTN_EVENT_HOLD ||
                       e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                // 淇濇寔鎬侊細鍙橀€熸。浣嶇敱 tape_control_tick() 鑷姩鍗囨。锛宲ress 宸茶皟杩囷紝涓嶉噸澶?
                if (g_app_state == APP_STATE_REWIND) {
                    audio_player_set_speed(tape_control_get_speed());
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                tape_control_rewind_release();
                audio_player_set_speed(TAPE_SPEED_NORMAL);
                audio_player_scrub_exit();   /* R098: 从最后 seek 位置恢复播放 */
                g_app_state = APP_STATE_PLAYING;
                g_combo_rew_us = 0; g_combo_stop_us = 0;  // 閫€鍑哄彉閫熸€侊紝娓呯┖缁勫悎閿鏃?
            }
            break;

        default:
            break;
        }
    }
}

/* ============================================================
 * 鏇存柊鏄剧ず灞?
 * ============================================================ */
static void update_display(void)
{
    // R049c锛氫俊鎭睆瑕嗙洊浼樺厛锛圤TA/USB/鍏充簬绛夋々鎻愮ず锛夛紝涓嶅彈 200ms 鑺傛祦闄愬埗
    if (g_info_active) {
        uint64_t t = esp_timer_get_time();
        if (t < g_info_until_us) {
            display_show_info(g_info_title, g_info_text);
            return;
        }
        g_info_active = false;
    }

    /* R049c OTA 鍗囩骇鐣岄潰锛氫紭鍏堟覆鏌擄紝涓嶅彈 200ms 鑺傛祦闄愬埗 */
    if (g_app_state == APP_STATE_OTA) {
        ota_sd_tick();
        ota_sd_render();
        return;
    }

    uint64_t now = esp_timer_get_time();
    if ((now - g_last_display_update) < 200000) return;
    g_last_display_update = now;

    if (g_app_state == APP_STATE_MENU) {
        return;  // 鑿滃崟鐢辫嚜韬覆鏌?(menu_render 鈫?display_show_menu)
    }

    if (g_app_state == APP_STATE_BROWSING) {
        int total = playlist_count();
        int scroll = g_browse_index - (BROWSE_VISIBLE_LINES / 2);
        if (scroll < 0) scroll = 0;
        int max_scroll = total - BROWSE_VISIBLE_LINES;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll > max_scroll) scroll = max_scroll;

        char lines[BROWSE_VISIBLE_LINES][24];
        int count = total - scroll;
        if (count > BROWSE_VISIBLE_LINES) count = BROWSE_VISIBLE_LINES;

        for (int i = 0; i < count; i++) {
            int idx = scroll + i;
            char name[FILENAME_MAX_LEN];
            playlist_get_name(idx, name, sizeof(name));
            snprintf(lines[i], sizeof(lines[i]), "%s%.21s",
                     (idx == g_browse_index) ? ">" : " ", name);
        }
        display_show_browse(g_browse_index, total, lines, count);
        return;
    }

#if defined(CONFIG_USE_BT_SPEAKER)
    if (g_app_state == APP_STATE_BT_SPEAKER) {
        display_show_bt_status(g_bt_connected ? g_bt_device_name : NULL,
                               g_bt_connected, audio_player_get_volume());
        return;
    }
#endif

    player_state_t disp_state;
    switch (g_app_state) {
    case APP_STATE_PLAYING:      disp_state = PLAYER_STATE_PLAYING;  break;
    case APP_STATE_FAST_FORWARD: disp_state = PLAYER_STATE_FAST_FORWARD; break;
    case APP_STATE_REWIND:       disp_state = PLAYER_STATE_REWIND;   break;
    case APP_STATE_PAUSED:       disp_state = PLAYER_STATE_PAUSED;   break;
    case APP_STATE_STOPPED:
    case APP_STATE_IDLE:
    default:                     disp_state = PLAYER_STATE_STOPPED;  break;
    }

    char track_name[FILENAME_MAX_LEN] = "";
    if (!playlist_get_name(g_current_track, track_name, sizeof(track_name))) {
        snprintf(track_name, sizeof(track_name), "Track %d", g_current_track + 1);
    }

    int position = audio_player_get_position();
    int duration = audio_player_get_duration();
    float speed  = tape_control_get_speed();
    int gear     = tape_control_get_gear();
    int volume   = audio_player_get_volume();
    int total    = playlist_count();

    display_update(disp_state, track_name,
                   g_current_track + 1, total,
                   position, duration,
                   speed, gear, volume);
}

/* ============================================================
 * 鎸傝浇 SD 鍗?
 * ============================================================ */
static bool mount_sd_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
        .use_one_fat = true,  // R028/M1: 鍗?FAT 鑺傜渷鍐呭瓨锛堝祵鍏ュ紡鍗曠敤鎴凤級
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;  // 鏄惧紡纭锛圫DSPI_HOST_DEFAULT 宸茶浣嗕繚鐣欐樉寮忥級

    sdspi_device_config_t device_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    device_cfg.host_id = SD_SPI_HOST;
    device_cfg.gpio_cs = SD_CS_IO;

    /* 蹇呴』鍏堝垵濮嬪寲 SD 鎵€鍦ㄧ殑 SPI2 鎬荤嚎, 鍚﹀垯 esp_vfs_fat_sdspi_mount 鍐呴儴
     * sdspi_host_init 浼氬洜 host_id not initialized(0x103) 闃诲/澶辫触,
     * 瀵艰嚧璋冪敤鏂?main 浠诲姟)鍗℃銆佹棤娉曞杺鐪嬮棬鐙楄€屽弽澶嶉噸鍚€?
     * 鑻ユ€荤嚎宸插垵濮嬪寲(閲嶅鎸傝浇)鍒欏拷鐣?ESP_ERR_INVALID_STATE銆?*/
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = SD_MOSI_IO;
    buscfg.miso_io_num = SD_MISO_IO;
    buscfg.sclk_io_num = SD_SCLK_IO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;
    esp_err_t bus_ret = spi_bus_initialize(SD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SD SPI bus init failed: 0x%x", bus_ret);
        return false;
    }

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host,
                                              &device_cfg, &mount_config,
                                              &g_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (0x%x)", ret);
        spi_bus_free(SD_SPI_HOST);
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted: %s", g_sd_card->cid.name);
    return true;
}

/* ============================================================
 * 鍞ら啋 GPIO 杈呭姪 (ESP32-S3 浠?GPIO0~21 涓?RTC GPIO, 鍙綔 ext1 鍞ら啋婧?
 * 鏂扮増鎸夐敭鏄犲皠涓?NEXT(IO47)/REW(IO42)/FF(IO41) 闈?RTC GPIO,
 * 涓嶈兘鐢ㄤ簬 light/deep sleep 鍞ら啋, 椤讳粠鍞ら啋鎺╃爜涓帓闄ゃ€?
 * ============================================================ */
static bool is_rtc_wakeup_gpio(gpio_num_t g)
{
    return (g >= GPIO_NUM_0 && g <= GPIO_NUM_21);
}

static uint64_t build_rtc_wakeup_mask(void)
{
    uint64_t mask = 0;
    const gpio_num_t btns[] = {
        BTN_PLAY_PAUSE, BTN_STOP, BTN_PREV, BTN_NEXT, BTN_REWIND, BTN_FAST_FORWARD
    };
    for (int i = 0; i < 6; i++) {
        if (is_rtc_wakeup_gpio(btns[i])) {
            mask |= (1ULL << btns[i]);
        }
    }
    return mask;
}

/* ============================================================
 * 鍒濆鍖栧璁?
 * ============================================================ */
/* ============================================================
 * WS2812 鐘舵€佹寚绀虹伅 (IO48)
 * 閲囩敤 ESP-IDF led_strip 缁勪欢锛圧MT 椹卞姩鍗曢 WS2812锛夈€?
 * WS2812 鏁版嵁绾跨粡纭欢鐢靛钩杞崲锛汳CU 渚т互 GPIO 鎺ㄦ尳杈撳嚭椹卞姩銆?
 * 棰滆壊璇箟锛氳摑=鍏呯數涓?/ 绾?鐢甸噺鏋佷綆 / 姗?鐢甸噺浣?/ 缁?鎾斁涓?/ 鐏?绌洪棽銆?
 * ============================================================ */
static led_strip_handle_t s_ws2812 = NULL;

static void indicator_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ws2812) return;
    /* 璋冭瘯锛氭殏鏃朵笉璋?refresh锛岄伩鍏嶅湪鏃犵‖浠舵椂闃诲 */
    esp_err_t r1 = led_strip_set_pixel(s_ws2812, 0, r, g, b);
    esp_err_t r2 = led_strip_refresh(s_ws2812);
    if (r1 != ESP_OK || r2 != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 set failed: %s / %s", esp_err_to_name(r1), esp_err_to_name(r2));
    }
}

static void indicator_led_init(void)
{
    if (s_ws2812) return;
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_IO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = 10 * 1000 * 1000;  // 10 MHz
    rmt_config.mem_block_symbols = 0;             // 0 = 椹卞姩榛樿
    rmt_config.flags.with_dma = false;
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_ws2812);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init failed: %s", esp_err_to_name(ret));
        return;
    }
    indicator_led_set(0, 0, 0); // 涓婄數榛樿鐔勭伃
}

static void init_hardware(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  TapeBook - Tape-Style Audiobook Player");
    ESP_LOGI(TAG, "  ESP32-S3-WROOM-1 (Octal PSRAM)");
    ESP_LOGI(TAG, "========================================");

    // 0. 鐢垫簮鑷攣 (R052): 寮€鏈哄悗绗竴浠朵簨鎷夐珮 POW_EN (IO40) 璁╃‖浠堕攣瀛?
    //    鍚﹀垯 NVS/display/audio 绛夊垵濮嬪寲鑰楁椂 (1-2 绉? 鍐呯敤鎴锋澗鎵嬩細瑙﹀彂鑷姩鍏虫満.
    //    蹇呴』鍦?power_mgmt_init() 涔嬪墠 (L1025) 绔嬪嵆鎵ц.
    {
        gpio_config_t pow_io = {};
        pow_io.pin_bit_mask = (1ULL << POW_EN_IO);
        pow_io.mode = GPIO_MODE_OUTPUT;
        pow_io.pull_up_en = GPIO_PULLUP_DISABLE;
        pow_io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        pow_io.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&pow_io);
        gpio_set_level(POW_EN_IO, 1);
        ESP_LOGI(TAG, "POW_EN (IO40) latched HIGH for hardware self-hold");
    }

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 2. Settings锛堟墦寮€ NVS handle锛?
    settings_init();

    // 3. 鏄剧ず灞?
    display_init();
    display_show_splash();

    // 4. 鎸夐敭
    button_manager_init();
    menu_init();

    // 5. 纾佸甫鎺у埗鍣?
    tape_control_init();

    // 5.5 MAX98357 SD_MODE (GPIO4 = I2S_SD 鈫?Pin4)锛氶噰鏍风巼妯″紡閫夋嫨鑴氾紝椤荤敱 MCU 鎷夊埌鍥哄畾鐢靛钩锛?
    //     鍚﹀垯鎮┖浼氬鑷撮噰鏍风巼妯″紡涓嶇‘瀹氥€佹棤澹般€?
    {
        gpio_config_t sd_mode_cfg = {
            .pin_bit_mask = (1ULL << MAX98357_SD_MODE_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&sd_mode_cfg));
        gpio_set_level(MAX98357_SD_MODE_GPIO, MAX98357_SD_MODE_LEVEL);
    }

    // 5.6 SD 鍗″湪浣嶆娴?(IO38): 杈撳叆, 澶栭儴 10K 涓婃媺(R6)鍒?3.3V, active-low(鎻掑叆=浣?
    {
        gpio_config_t cd_cfg = {
            .pin_bit_mask = (1ULL << SD_CD_IO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cd_cfg));
    }

    // 6. 闊抽鎾斁鍣?
    audio_player_init();
    audio_player_set_callback(on_track_finished, NULL);

    // 7. 鐢垫簮绠＄悊
    power_mgmt_init();

    // 7.5 鐘舵€佹寚绀虹伅 (WS2812, IO48)
    indicator_led_init();

    // 8. 涔︾妯″潡
    bookmark_init();

    // 9. 鐪嬮棬鐙楀垵濮嬪寲锛?0 绉掕秴鏃讹紝缁?callback 鍐?pipeline 鎿嶄綔鐣欎綑閲忥級
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 30000,
        .idle_core_mask = 0,
        .trigger_panic = false,  /* 改 false 防止 WDT 超时触发 panic 重启，导致屏幕永久黑屏 */
    };
    esp_err_t wdt_err = esp_task_wdt_init(&twdt_config);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_reconfigure(&twdt_config);
    } else {
        ESP_ERROR_CHECK(wdt_err);
    }
    /* main_task 璁㈤槄 task_wdt锛堝凡淇 audio_player_tick 闀挎搷浣滆鍒よ秴鏃堕棶棰橈級锛屼綔涓烘閿佸厹搴?*/
    esp_task_wdt_add(NULL);

    // 10. 鍔犺浇鎸佷箙鍖栬缃?
    int vol = settings_load_volume();
    ESP_LOGI(TAG, "DBG boot volume=%d", vol);
    audio_player_set_volume(vol);
    g_play_mode = (play_mode_t)settings_load_play_mode();
    display_set_play_mode((int)g_play_mode);

    // 11. 鏍￠獙鎵€鏈夋寜閿?GPIO 鏄惁涓哄悎娉?RTC 鍞ら啋婧?(鏂扮増鏄犲皠閮ㄥ垎鎸夐敭闈?RTC GPIO)
    const gpio_num_t wakeup_gpios[] = {
        BTN_PLAY_PAUSE, BTN_STOP, BTN_PREV, BTN_NEXT, BTN_REWIND, BTN_FAST_FORWARD
    };
    for (size_t i = 0; i < sizeof(wakeup_gpios) / sizeof(wakeup_gpios[0]); i++) {
        if (!esp_sleep_is_valid_wakeup_gpio(wakeup_gpios[i])) {
            ESP_LOGW(TAG, "GPIO %d is NOT a valid RTC wakeup source; "
                          "excluded from sleep wakeup mask", wakeup_gpios[i]);
        }
    }
}

/* ============================================================
 * 鍒濆鍖栧瓨鍌細鎸傝浇 SD 鈫?鎵弿 鈫?鎭㈠鏂偣
 * ============================================================ */
static void init_storage(void)
{
    if (!mount_sd_card()) {
        display_show_no_card();
        g_app_state = APP_STATE_IDLE;
        return;
    }

    int count = playlist_scan(SD_MOUNT_POINT);
    ESP_LOGI(TAG, "Found %d audio files on SD card", count);

    if (count == 0) {
        display_show_no_files();
        g_app_state = APP_STATE_IDLE;
    } else {
        // 灏濊瘯浠?NVS 鎭㈠鏂偣
        int saved_idx = 0, saved_pos = 0;
        if (settings_load_position(&saved_idx, &saved_pos)) {
            g_current_track = saved_idx;
            playlist_set_index(g_current_track);
            g_seek_on_play_position = saved_pos;
            g_app_state = APP_STATE_STOPPED;  // 绛夊緟鐢ㄦ埛鎸夋挱鏀?
            ESP_LOGI(TAG, "Resuming from track %d at %ds (press Play to start)", saved_idx, saved_pos);
        } else {
            g_current_track = 0;
            playlist_set_index(0);
            g_app_state = APP_STATE_STOPPED;
        }
    }
}

/* ============================================================
 * 涓讳换鍔?
 * ============================================================ */
#if defined(CONFIG_USE_BT_SPEAKER)
void app_enter_bt_speaker(void)
{
    stop_playback();                 // 鍋?SD + 娓呮柇鐐癸紙鍐呴儴 audio_player_stop 涔熶細鍋?BT锛?
    menu_close();
    if (audio_player_start_bt()) {
        bt_speaker_register_state_cb(on_bt_state);
        g_bt_connected = false;
        g_bt_paused = false;
        g_app_state = APP_STATE_BT_SPEAKER;
        ESP_LOGI(TAG, "Entered BT speaker mode");
    } else {
        display_show_info("蓝牙音箱", "初始化失败");
        g_app_state = g_state_before_menu;
    }
}
#endif

extern "C" void app_main(void)
{
    init_hardware();
    init_storage();

    /* OTA 鍚姩鍥炴粴纭锛氳嫢浠?OTA 鍒嗗尯鍚姩涓旇繍琛屽仴搴凤紝鏍囪鏈夋晥锛堥槻 boot loop 鍋囨鐮栵級 */
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running && (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
                        running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Marked OTA app valid (rollback cancelled)");
        }
    }

    // 鍒濆鍖?SD 鍗″湪浣嶇姸鎬?(閬垮厤鍚姩璇姤鎻掓嫈鎻愮ず)
    {
        int boot_lvl = gpio_get_level(SD_CD_IO);
        g_sd_cd_raw = boot_lvl;
        g_sd_cd_stable_cnt = 3;   // 瑙嗕负宸茬ǔ瀹? 涓嶈Е鍙戞彃鍏?寮瑰嚭浜嬩欢
        g_sd_inserted = (boot_lvl == SD_CD_ACTIVE_LEVEL);
        display_set_sd_present_init(g_sd_inserted);
    }

    ESP_LOGI(TAG, "System ready. Waiting for user input...");
    ESP_LOGI(TAG, ">>> SYSTEM BOOT COMPLETE <<< state=%d", (int)g_app_state);

    /* 璁?splash 鑷冲皯鍙 ~1s锛屽啀浜よ繕缁欎富寰幆鎸?g_app_state 娓叉煋鎾斁鍣?IDLE 涓荤晫闈紝
       杩欐牱 RESET 鍚庝粠涓插彛(System boot complete) + 灞忓箷(涓荤晫闈?鍗冲彲涓€鐪肩‘璁ょ郴缁熺湡姝ｈ窇璧锋潵 */
    vTaskDelay(pdMS_TO_TICKS(1000));

    g_next_loop_deadline = esp_timer_get_time();

    /* 鎵€鏈夊垵濮嬪寲鏈?LVGL 鍐?(display_init / display_show_splash / init_storage 鎻愮ず)
     * 宸插畬鎴愶紝姝ゅ埢鎵嶅惎鍔?lvgl_task锛岄伩鍏嶄笌 main_task 骞跺彂璁块棶 LVGL 瀵硅薄鏍戞閿併€?
     * 涓诲惊鐜唴鐨?display_update 宸茬敤 lv_lock/lv_unlock 淇濇姢銆?*/
    display_start_lvgl_task();

    /* 娉ㄥ唽涓诲惊鐜?tick 鍥炶皟: lvgl_task 鍦ㄨ嚜宸辩殑寰幆閲屾寔閿佽皟鐢?update_display,
     * 娑堥櫎 main_task 鐩存帴璋?LVGL API 瀵艰嚧鐨勬閿?鐪嬮棬鐙楄秴鏃躲€?*/
    display_register_main_tick(update_display);

    // R073-fix: 强制 1.5s 后调一次 display_request_main_tick 触发 player 渲染
    // （R072 实测：splash 1s 延迟结束后 lvgl_task 消费 s_msg_pending 显示 splash，
    // 但 200ms 节流的 update_display 在某种时序下没把 splash 隐藏掉，按键才进 player。
    // 此处加一个 timer 兜底 1.5s 后强制 tick 一次，让 ui_show_player 把 splash 隐藏）
    vTaskDelay(pdMS_TO_TICKS(500));   // 累计 1.5s（前面已有 vTaskDelay(1000)）
    display_request_main_tick();

    while (1) {
        // 0. 蹇冭烦锛氭瘡 N 娆℃墦鍗颁竴娆★紝纭涓诲惊鐜椿鐫€
        static uint32_t heartbeat = 0;
        heartbeat++;
        // R051: 移除 R049 加的 7 条主循环 DBG 日志 + tick_diag 残留 (line 1228-1236)
        // 1. 澶勭悊鎸夐敭浜嬩欢
        handle_button_events();

        // 2. 纾佸甫鎺у埗鍣?tick
        tape_control_tick();

        // 3. 蹇繘/蹇€€閫熷害鏇存柊
        tape_mode_t mode = tape_control_get_mode();
        if (mode != TAPE_MODE_NORMAL) {
            audio_player_set_speed(tape_control_get_speed());
        }

        // 4. 闊抽鎾斁鍣?tick锛堢閬撶淮鎶?璺冲抚/浜嬩欢鐩戝惉锛?
        audio_player_tick();
        // 5. 寮傛澶勭悊鏇茬洰鎾畬锛堥伩鍏嶅湪鍥炶皟鍐呭祵濂?pipeline 鎿嶄綔锛?
        if (g_pending_track_finished) {
            g_pending_track_finished = false;
            g_current_track = g_pending_track_next;
            playlist_set_index(g_current_track);
            g_seek_on_play_position = g_pending_track_seek;
            play_current_track();
        }

        // 5b. 寮傛淇濆瓨鎸佷箙鍖栫姸鎬侊紙鍥炶皟涓粎鏆傚瓨锛宻ettings_flush() 璐熻矗钀界洏锛?
        if (g_pending_save_track >= 0) {
            char name[FILENAME_MAX_LEN] = "";
            playlist_get_name(g_pending_save_track, name, sizeof(name));
            settings_save_position(g_pending_save_track, g_pending_save_position, name);
            flush_nvs_if_safe();   // R032-104: 鎾畬鍚庣珛鍗宠惤鐩橈紝閬垮厤 30s 鑷姩淇濆瓨绐楀彛鍐呮柇鐢典涪鏂偣
            g_pending_save_track = -1;
        }
        // 6. 姣?30 绉掕嚜鍔ㄤ繚瀛樻柇鐐?+ 鎵归噺 flush NVS锛堟挱鏀?鏆傚仠/FF/RW 鍧囦繚瀛橈紝R034-002 / R035-004锛?
        {
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_auto_save_us) >= AUTO_SAVE_INTERVAL_US) {
                g_last_auto_save_us = now;
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
                    g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
                    save_current_position();
                    flush_nvs_if_safe();
                    // R035-010锛氳嚜鍔ㄤ繚瀛樹篃绠楃敤鎴锋椿鍔紝閲嶇疆 auto-off 璁℃椂
                    power_mgmt_record_activity();
                }
            }
        }
        // 7. 瀹氭椂鍏虫満妫€鏌ワ紙鍚?FF/RW锛?

        // 5. 寮傛澶勭悊鏇茬洰鎾畬锛堥伩鍏嶅湪鍥炶皟鍐呭祵濂?pipeline 鎿嶄綔锛?
        if (g_pending_track_finished) {
            g_pending_track_finished = false;
            g_current_track = g_pending_track_next;
            playlist_set_index(g_current_track);
            g_seek_on_play_position = g_pending_track_seek;
            play_current_track();
        }

        // 5b. 寮傛淇濆瓨鎸佷箙鍖栫姸鎬侊紙鍥炶皟涓粎鏆傚瓨锛宻ettings_flush() 璐熻矗钀界洏锛?
        if (g_pending_save_track >= 0) {
            char name[FILENAME_MAX_LEN] = "";
            playlist_get_name(g_pending_save_track, name, sizeof(name));
            settings_save_position(g_pending_save_track, g_pending_save_position, name);
            flush_nvs_if_safe();   // R032-104: 鎾畬鍚庣珛鍗宠惤鐩橈紝閬垮厤 30s 鑷姩淇濆瓨绐楀彛鍐呮柇鐢典涪鏂偣
            g_pending_save_track = -1;
        }

        // 6. 姣?30 绉掕嚜鍔ㄤ繚瀛樻柇鐐?+ 鎵归噺 flush NVS锛堟挱鏀?鏆傚仠/FF/RW 鍧囦繚瀛橈紝R034-002 / R035-004锛?
        {
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_auto_save_us) >= AUTO_SAVE_INTERVAL_US) {
                g_last_auto_save_us = now;
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
                    g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
                    save_current_position();
                    flush_nvs_if_safe();
                    // R035-010锛氳嚜鍔ㄤ繚瀛樹篃绠楃敤鎴锋椿鍔紝閲嶇疆 auto-off 璁℃椂
                    power_mgmt_record_activity();
                }
            }
        }

        // 7. 瀹氭椂鍏虫満妫€鏌ワ紙鍚?FF/RW锛?
        if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
            g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND
#if defined(CONFIG_USE_BT_SPEAKER)
            || g_app_state == APP_STATE_BT_SPEAKER
#endif
            ) {
            if (power_mgmt_auto_off_expired()) {
                ESP_LOGI(TAG, "Auto-off timer expired, stopping playback");
                audio_player_stop();
                g_app_state = APP_STATE_STOPPED;
                power_mgmt_set_auto_off(0);
                // R034-007锛氳Е鍙戝悗钀界洏娓呴浂 NVS锛岄伩鍏嶉噸鍚?power_mgmt_init 閲嶆柊姝﹁
                settings_save_auto_off(0);
                flush_nvs_if_safe();
            }
        }

        // 7b. 鐢垫簮绠＄悊 tick锛?Hz 鍛ㄦ湡鎬т换鍔★級
        {
            static uint64_t last_power_tick = 0;
            uint64_t now = esp_timer_get_time();
            if ((now - last_power_tick) >= 1000000ULL) {  /* #7b 电源管理 tick (1Hz): ADC 采样 + LED 指示 + 低电关机 */
                last_power_tick = now;
                power_mgmt_tick();

                // WS2812 鐘舵€佹寚绀虹伅棰滆壊鏇存柊
                if (power_mgmt_is_charging()) {
                    indicator_led_set(0, 0, 255);            // 钃濓細鍏呯數涓?
                } else {
                    bat_state_t st = power_mgmt_get_state();
                    if (st == BAT_STATE_CRITICAL) {
                        indicator_led_set(255, 0, 0);        // 绾細鐢甸噺鏋佷綆
                    } else if (st == BAT_STATE_LOW) {
                        indicator_led_set(255, 80, 0);      // 姗欙細鐢甸噺浣?
                    } else if (g_app_state == APP_STATE_PLAYING ||
                               g_app_state == APP_STATE_PAUSED ||
                               g_app_state == APP_STATE_FAST_FORWARD ||
                               g_app_state == APP_STATE_REWIND) {
                        indicator_led_set(0, 255, 0);        // 缁匡細鎾斁涓?
                    } else {
                        indicator_led_set(0, 0, 0);          // 鐏細绌洪棽
                    }
                }

                // 鐢甸噺鏋佷綆鏃朵繚瀛樼姸鎬佸苟杞叧鏈?(鑴夊啿 POW_EN 纭柇鐢?
                if (power_mgmt_should_shutdown()) {
                    ESP_LOGE(TAG, "Battery critical, saving state and powering off");
                    audio_player_stop();
                    save_current_position();
                    flush_nvs_if_safe();
                    // 浠?RTC GPIO 鍙綔鍞ら啋婧? 璁剧疆鎺╃爜渚?power_off 鐨?deep-sleep 鍏滃簳
                    uint64_t wakeup_mask = build_rtc_wakeup_mask();
                    if (wakeup_mask) {
                        esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
                    }
                    power_mgmt_power_off();   // 鑴夊啿 POW_EN 閲婃斁鐢垫簮閿佸瓨 (鍚?deep-sleep 鍏滃簳)
                }
            }
        }

        // 7c. 鑷姩浼戠湢锛? 鍒嗛挓鏃犳搷浣滆繘鍏?light sleep锛屾寜閿?GPIO 鍞ら啋锛?
        // S2锛氭挱鏀句腑锛圥LAYING/PAUSED锛変笉浼戠湢锛屽惁鍒欏惉涔︿細琚墦鏂?
        // S3锛歴leep 鍓嶉噴鏀剧甯︾姸鎬佹満锛岄伩鍏嶅敜閱掑悗妗ｄ綅娈嬬暀
        if ((g_app_state == APP_STATE_STOPPED || g_app_state == APP_STATE_IDLE) &&
            power_mgmt_should_sleep()) {
            ESP_LOGI(TAG, "Idle timeout, entering light sleep");
            // ESP_LOGI(TAG, "DBG: before esp_light_sleep_start");

            save_current_position();   // R032-103: sleep 鍓嶄繚瀛樻柇鐐癸紙FF/RW 涓嶄細杩涘叆姝ゅ垎鏀紝宸插湪姝ゅ墠閲婃斁锛?
            flush_nvs_if_safe();
            audio_player_stop();
            g_app_state = APP_STATE_IDLE;

            {
                uint64_t wakeup_mask = build_rtc_wakeup_mask();
                if (wakeup_mask) {
                    esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
                }
            }
            esp_light_sleep_start();

            ESP_LOGI(TAG, "Woke from light sleep");
            g_next_loop_deadline = esp_timer_get_time();

            // 鍞ら啋鍚庢仮澶嶄负 STOPPED 鐘舵€侊紙淇濇寔鏇茬洰閫変腑锛岀敤鎴锋寜 Play 缁х画锛?
            g_app_state = APP_STATE_STOPPED;

            // L4: 鍞ら啋鍚庢仮澶嶆柇鐐逛綅缃紙浠呭綋 saved_track == g_current_track锛?
            {
                int saved_idx = 0, saved_pos = 0;
                if (settings_load_position(&saved_idx, &saved_pos) &&
                    saved_idx == g_current_track) {
                    g_seek_on_play_position = saved_pos;
                    ESP_LOGI(TAG, "Wakeup resume: track %d at %ds", saved_idx, saved_pos);
                }
            }

            power_mgmt_record_activity();
        }

        // 8. SD 鍗℃彃鎷旂鐞?(鍩轰簬 SD_CD 鏈烘寮€鍏冲疄鏃舵娴? 杞欢鍘绘姈)
        //    OTA 鍐欏叆鏈熼棿鐙崰 SD 鍗★細璺宠繃鎻掓嫈/璇绘牎楠屽鐞嗭紝閬垮厤涓庡崌绾у啓绔炰簤
        if (!g_ota_in_progress) {
            int lvl = gpio_get_level(SD_CD_IO);
            // 鍘绘姈: 鍚屼竴鐢靛钩闇€杩炵画绋冲畾鑻ュ共娆?鈮?0ms)鎵嶆彁浜?
            if (lvl != g_sd_cd_raw) {
                g_sd_cd_raw = lvl;
                g_sd_cd_stable_cnt = 0;
            } else if (g_sd_cd_stable_cnt < 255) {
                g_sd_cd_stable_cnt++;
            }

            if (g_sd_cd_stable_cnt >= 3) {
                bool present = (lvl == SD_CD_ACTIVE_LEVEL);
                if (present && (g_sd_card == NULL)) {
                    // 鎻掑叆浜嬩欢: 鎸傝浇 + 鎵弿
                    ESP_LOGI(TAG, "SD card inserted");
                    if (mount_sd_card()) {
                        int count = playlist_scan(SD_MOUNT_POINT);
                        ESP_LOGI(TAG, "Found %d audio files on SD card", count);
                        if (count > 0) {
                            g_current_track = 0;
                            playlist_set_index(0);
                            g_app_state = APP_STATE_STOPPED;
                            display_clear_msg();   /* R100: 清除"SD card not detected"并返回播放器 */
                        } else {
                            display_show_no_files();
                            g_app_state = APP_STATE_IDLE;
                        }
                    } else {
                        display_show_no_card();
                        g_app_state = APP_STATE_IDLE;
                    }
                    display_set_sd_present(true);
                } else if (!present && g_sd_inserted) {
                    // 寮瑰嚭浜嬩欢: 鍋滄 + 鍗歌浇
                    ESP_LOGI(TAG, "SD card removed");
                    if (g_sd_card != NULL) {
                        audio_player_stop();
                        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_sd_card);
                        spi_bus_free(SD_SPI_HOST);
                        g_sd_card = NULL;
                    }
                    display_show_no_card();
                    g_app_state = APP_STATE_IDLE;
                    display_set_sd_present(false);
                }
                g_sd_inserted = present;
            }

            // 鍚庡: 宸叉寕杞芥椂姣?5 绉掕鎵囧尯 0 鎺㈡祴寮傚父鎷斿嚭 (鑴忔嫈/璇婚敊璇?
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_sd_check_us) >= SD_CHECK_INTERVAL_US) {
                g_last_sd_check_us = now;
                if (g_sd_card != NULL) {
                    uint32_t buf;
                    // R094: 单次读失败(如偶发 data CRC)不应直接判移除并停播。
                    // 先原地重试几次过滤瞬态错误；仅当本次检查(含重试)全失败才累加计数，
                    // 连续 SD_READ_FAIL_THRESHOLD 次检查都失败才真正判为移除（与真实拔卡区分）。
                    esp_err_t ret = ESP_FAIL;
                    for (int attempt = 0; attempt < 3; attempt++) {
                        ret = sdmmc_read_sectors(g_sd_card, (uint8_t *)&buf, 0, 1);
                        if (ret == ESP_OK) break;
                        vTaskDelay(pdMS_TO_TICKS(2));  // 短暂退避后重试
                    }
                    if (ret != ESP_OK) {
                        g_sd_read_fail_cnt++;
                        ESP_LOGW(TAG, "SD health read fail (cnt=%d), ret=0x%x", g_sd_read_fail_cnt, ret);
                        if (g_sd_read_fail_cnt >= SD_READ_FAIL_THRESHOLD) {
                            ESP_LOGW(TAG, "SD card removed (read fail)!");
                            audio_player_stop();
                            display_show_no_card();
                            g_app_state = APP_STATE_IDLE;
                            esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_sd_card);
                            spi_bus_free(SD_SPI_HOST);
                            g_sd_card = NULL;
                            g_sd_read_fail_cnt = 0;
                            // 涓嶆敼鍔?g_sd_inserted: 鐢?SD_CD 鍘绘姈閫昏緫鍐冲畾鍚庣画(閲嶆柊鎸傝浇鎴栫疆鐏?
                        }
                    } else {
                        g_sd_read_fail_cnt = 0;  // 读成功，清零连续失败计数
                    }
                }
            }
        }

        // 9. 鐪嬮棬鐙楀浣?
        esp_task_wdt_reset();

        // 10. 璇锋眰鏇存柊鏄剧ず灞忥紙瀹為檯鐢?lvgl_task 鎸侀攣璋冪敤 update_display锛岄伩鍏?main 琚?LVGL mutex 闃诲瓒呮椂锛?
        display_request_main_tick();
        // ESP_LOGI(TAG, "DBG: after request_tick (hbt=%u)", (unsigned)heartbeat);

        // 11. 浼戠湢锛屾帶鍒跺惊鐜鐜囷紙鍩轰簬缁濆鏃堕棿瀵归綈锛岃ˉ鍋垮墠搴忚€楁椂锛?
        {
            int64_t now = esp_timer_get_time();
            if (now < g_next_loop_deadline) {
                int64_t delay_us = g_next_loop_deadline - now;
                /* 上限保护：单轮等待不超过一个扫描间隔，防止异常大值睡死 */
                if (delay_us > (int64_t)BTN_SCAN_INTERVAL * 1000) {
                    delay_us = (int64_t)BTN_SCAN_INTERVAL * 1000;
                }
                uint32_t delay_ms = (uint32_t)(delay_us / 1000);
                vTaskDelay(delay_ms ? pdMS_TO_TICKS(delay_ms) : 1);
            } else {
                /* 本轮耗时已超过扫描间隔：至少让出 1 tick，避免独占 CPU 饿死其他任务 */
                vTaskDelay(1);
            }
            /* 基于当前时间重新对齐（不再无条件累加）：
               否则本轮耗时一旦超过 BTN_SCAN_INTERVAL，deadline 会永久落后于真实时间，
               deadline-now 变负数并在 pdMS_TO_TICKS 的无符号运算中被解释成巨大 tick 数，
               导致 main 任务近乎无限期睡死、不再喂 task_wdt。 */
            g_next_loop_deadline = esp_timer_get_time() + (int64_t)BTN_SCAN_INTERVAL * 1000;
        }
    }
}
