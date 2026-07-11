/**
 * @file playlist.cpp
 * @brief 播放列表实现
 *
 * 使用结构体数组保证 display_name 与 full_path 排序后仍一一对应。
 * 大数组放在 PSRAM（EXT_RAM_ATTR）以减少 DRAM 占用。
 */

#include "playlist.h"
#include "config.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>

// GCC 14 的 -Wformat-truncation 静态分析 snprintf 会截断，
// 这里 buffer 是有意做大，但 GCC 看不到边界。
// 局部禁用这个 warning，不影响其他警告。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* --- 数据结构：名称与路径绑定，排序后不错乱 --- */
typedef struct {
    char display_name[FILENAME_MAX_LEN];       // 显示名（可含子目录前缀）
    char full_path[FILENAME_MAX_LEN * 2];       // 完整路径（用于打开文件）
} playlist_item_t;

static playlist_item_t *g_items = NULL;
static int   g_count = 0;
static int   g_current = 0;
static char  g_base_path[64] = "";

/* ============================================================
 * 辅助函数
 * ============================================================ */
static const char *get_ext(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "";
    return dot;
}

static int strcicmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0 || !*a) return d;
    }
}

bool playlist_is_audio_file(const char *filename)
{
    const char *ext = get_ext(filename);
    if (*ext == '\0') return false;

    const char *supported[] = {
        ".mp3", ".wav", ".flac", ".aac", ".m4a",
        ".ogg", ".opus", NULL
    };

    for (int i = 0; supported[i]; i++) {
        if (strcicmp(ext, supported[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* --- 结构体整体排序（按 display_name），名称与路径始终对应 --- */
static int qsort_item_cmp(const void *a, const void *b)
{
    const playlist_item_t *ia = (const playlist_item_t *)a;
    const playlist_item_t *ib = (const playlist_item_t *)b;
    return strcasecmp(ia->display_name, ib->display_name);
}

/* ============================================================
 * 扫描 SD 卡（递归扫描子目录，最多 MAX_SUBDIR_DEPTH 层）
 * ============================================================ */
#define MAX_SUBDIR_DEPTH  3

static void scan_dir_recursive(const char *path, const char *prefix, int depth)
{
    if (depth > MAX_SUBDIR_DEPTH) return;

    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        /* 跳过隐藏文件和系统目录 */
        if (entry->d_name[0] == '.' ||
            strcmp(entry->d_name, "System Volume Information") == 0 ||
            strcasecmp(entry->d_name, "$RECYCLE.BIN") == 0)
            continue;

        char full_entry_path[FILENAME_MAX_LEN * 2];
        snprintf(full_entry_path, sizeof(full_entry_path), "%s/%s", path, entry->d_name);

        bool is_reg = (entry->d_type == DT_REG);
        bool is_dir = (entry->d_type == DT_DIR);
        if (!is_reg && !is_dir && entry->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(full_entry_path, &st) == 0) {
                is_reg = S_ISREG(st.st_mode);
                is_dir = S_ISDIR(st.st_mode);
            }
        }

        if (is_reg && playlist_is_audio_file(entry->d_name)) {
            if (g_count < PLAYLIST_TRACK_MAX) {
                /* display_name：如有前缀则加前缀 */
                if (prefix && *prefix) {
                    snprintf(g_items[g_count].display_name, FILENAME_MAX_LEN,
                             "%s/%s", prefix, entry->d_name);
                } else {
                    strncpy(g_items[g_count].display_name, entry->d_name,
                            FILENAME_MAX_LEN - 1);
                    g_items[g_count].display_name[FILENAME_MAX_LEN - 1] = '\0';
                }
                /* full_path：完整路径 */
                strncpy(g_items[g_count].full_path, full_entry_path,
                        sizeof(g_items[0].full_path) - 1);
                g_items[g_count].full_path[sizeof(g_items[0].full_path) - 1] = '\0';
                g_count++;
            }
        } else if (is_dir) {
            char sub_prefix[FILENAME_MAX_LEN];
            if (prefix && *prefix) {
                snprintf(sub_prefix, sizeof(sub_prefix), "%s/%s", prefix, entry->d_name);
            } else {
                strncpy(sub_prefix, entry->d_name, FILENAME_MAX_LEN - 1);
                sub_prefix[FILENAME_MAX_LEN - 1] = '\0';
            }
            scan_dir_recursive(full_entry_path, sub_prefix, depth + 1);
        }
    }
    closedir(dir);
}

int playlist_scan(const char *base_path)
{
    strncpy(g_base_path, base_path, sizeof(g_base_path) - 1);
    g_base_path[sizeof(g_base_path) - 1] = '\0';

    // 分配 PSRAM（优先）或普通 DRAM
    if (g_items) {
        free(g_items);
        g_items = NULL;
    }
    g_items = (playlist_item_t *)heap_caps_malloc(
        sizeof(playlist_item_t) * PLAYLIST_TRACK_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_items) {
        // PSRAM 失败时回退到普通 DRAM
        g_items = (playlist_item_t *)malloc(sizeof(playlist_item_t) * PLAYLIST_TRACK_MAX);
        if (!g_items) return 0;
    }

    g_count = 0;
    g_current = 0;

    /* 递归扫描根目录及所有子目录 */
    scan_dir_recursive(base_path, NULL, 0);

    /* 整体排序（结构体绑定，名称-路径不会错乱） */
    qsort(g_items, g_count, sizeof(g_items[0]), qsort_item_cmp);

    return g_count;
}

/* ============================================================
 * 播放列表操作
 * ============================================================ */
int playlist_count(void)
{
    return g_count;
}

bool playlist_get_name(int index, char *buffer, size_t buf_size)
{
    if (index < 0 || index >= g_count) return false;
    strncpy(buffer, g_items[index].display_name, buf_size - 1);
    buffer[buf_size - 1] = '\0';
    return true;
}

bool playlist_get_path(int index, char *buffer, size_t buf_size)
{
    if (index < 0 || index >= g_count) return false;
    strncpy(buffer, g_items[index].full_path, buf_size - 1);
    buffer[buf_size - 1] = '\0';
    return true;
}

int playlist_current_index(void)
{
    return g_current;
}

void playlist_set_index(int index)
{
    if (index >= 0 && index < g_count) {
        g_current = index;
    }
}

int playlist_next(void)
{
    if (g_count == 0) return -1;
    g_current = (g_current + 1) % g_count;
    return g_current;
}

int playlist_prev(void)
{
    if (g_count == 0) return -1;
    g_current = (g_current - 1 + g_count) % g_count;
    return g_current;
}

bool playlist_is_empty(void)
{
    return g_count == 0;
}

#pragma GCC diagnostic pop
