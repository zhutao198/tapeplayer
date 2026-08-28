#include "font_partition.h"

#include "lvgl.h"
#include "libs/freetype/lv_freetype.h"   /* LVGL 组件 INCLUDE_DIRS 只到 src 层，需带子路径 */
#include "esp_partition.h"
#include "esp_vfs.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static const char * TAG = "font_part";

/* 由 gen_font.py -> tools/cjk.ttf 烧录进 font 分区后得到，符号在 lv_conf.h 中声明 */
extern lv_font_t lv_font_chinese_12;
extern lv_font_t lv_font_chinese_14;
extern lv_font_t lv_font_chinese_16;

static const esp_partition_t * s_font_part = NULL;
static lv_font_t * s_cjk_font = NULL;
static uint32_t s_offset = 0;   /* 单文件 VFS 的当前读偏移 */
static bool     s_font_ready = false;  /* freetype + 回退字体是否就绪 */
/* R096: font 分区 mmap 映射指针（cacheable, XIP 安全）。改用 mmap 读取字形数据，
   避免 esp_partition_read 禁用 Flash cache 导致并发运行 IROM 的解码任务 Cache error 崩溃。 */
static const void * s_font_mmap_ptr = NULL;
static esp_partition_mmap_handle_t s_font_mmap_handle = 0;

/* ------------------------------------------------------------------ *
 * 自定义 newlib VFS：把整个 font 分区直接当作 /font/cjk.ttf 文件暴露。
 * 这样 freetype 的 FT_New_Face("/font/cjk.ttf") 即可按标准路径打开，
 * 无需 FAT 文件系统、无需主机端制作 FAT 镜像，烧录时直接 write_flash 原样写入即可。
 * 换 SD 卡不影响中文显示（字库在设备自带 flash 分区）。
 * ------------------------------------------------------------------ */
static int font_open_p(void * ctx, const char * path, int flags, int mode)
{
    (void)ctx; (void)path; (void)flags; (void)mode;
    s_offset = 0;
    return 0;   /* 单文件，固定本地 fd=0 */
}

static ssize_t font_read_p(void * ctx, int fd, void * dst, size_t size)
{
    (void)fd;
    const esp_partition_t * part = (const esp_partition_t *)ctx;
    uint32_t part_size = part->size;
    if (s_offset >= part_size) return 0;
    size_t to_read = (s_offset + size > part_size) ? (part_size - s_offset) : size;
    if (to_read > 0) {
        if (s_font_mmap_ptr) {
            /* R096: 从 mmap 的 cacheable 区读取，不禁用 Flash cache，可与解码任务并发 */
            memcpy(dst, (const uint8_t *)s_font_mmap_ptr + s_offset, to_read);
        } else {
            esp_partition_read(part, s_offset, dst, to_read);
        }
    }
    s_offset += to_read;
    return (ssize_t)to_read;
}

static off_t font_lseek_p(void * ctx, int fd, off_t offset, int mode)
{
    (void)fd;
    const esp_partition_t * part = (const esp_partition_t *)ctx;
    uint32_t part_size = part->size;
    switch (mode) {
        case SEEK_SET: s_offset = (uint32_t)offset; break;
        case SEEK_CUR: s_offset += (uint32_t)offset; break;
        case SEEK_END: s_offset = part_size + (uint32_t)offset; break;
        default: break;
    }
    if (s_offset > part_size) s_offset = part_size;
    return (off_t)s_offset;
}

static int font_close_p(void * ctx, int fd)
{
    (void)ctx; (void)fd;
    return 0;
}

static int font_fstat_p(void * ctx, int fd, struct stat * st)
{
    (void)fd;
    const esp_partition_t * part = (const esp_partition_t *)ctx;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    st->st_size = part->size;
    return 0;
}

static int font_stat_p(void * ctx, const char * path, struct stat * st)
{
    (void)path;
    return font_fstat_p(ctx, 0, st);
}

void font_partition_init(void)
{
    s_font_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           ESP_PARTITION_SUBTYPE_ANY, "font");
    if (!s_font_part) {
        ESP_LOGE(TAG, "未找到 label=\"font\" 的分区，请检查 partitions*.csv 是否已添加 font 分区");
        return;
    }

    /* R096: 将 font 分区 mmap 到 cacheable 地址，字形读取走 XIP（不禁用 cache），
       方可与解码任务并发执行 IROM 代码。否则 esp_partition_read 会禁用 Flash cache，
       解码任务 MP3Decode 撞 Cache error 崩溃。 */
    esp_err_t mmap_err = esp_partition_mmap(s_font_part, 0, s_font_part->size,
                                           ESP_PARTITION_MMAP_DATA,
                                           &s_font_mmap_ptr, &s_font_mmap_handle);
    if (mmap_err != ESP_OK) {
        ESP_LOGE(TAG, "font 分区 mmap 失败 (%s)，回退 esp_partition_read（并发解码可能 Cache error）",
                 esp_err_to_name(mmap_err));
        s_font_mmap_ptr = NULL;
    }

    static esp_vfs_t s_vfs = {};
    s_vfs.flags   = ESP_VFS_FLAG_CONTEXT_PTR;
    s_vfs.lseek_p = font_lseek_p;
    s_vfs.read_p  = font_read_p;
    s_vfs.open_p  = font_open_p;
    s_vfs.close_p = font_close_p;
    s_vfs.fstat_p = font_fstat_p;
    s_vfs.stat_p  = font_stat_p;
    esp_err_t err = esp_vfs_register("/font", &s_vfs, (void *)s_font_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "VFS /font 注册失败: %s", esp_err_to_name(err));
        return;
    }

#if LV_USE_FREETYPE
    /* freetype 按需渲染，字形缓存走 PSRAM（LVGL 内存已切到 malloc/PSRAM） */
    lv_result_t ft_res = lv_freetype_init(1024);
    if (ft_res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "lv_freetype_init 失败 (lv_result=%d)，中文缺字回退不可用", (int)ft_res);
        /* 失败时必须 uninit 释放半初始化的 ft_ctx，否则后续 LVGL 渲染中文时
         * 会误入 freetype 路径（library 无效）导致死循环 + task_wdt */
        lv_freetype_uninit();
        s_font_ready = false;
        return;
    }

    s_cjk_font = lv_freetype_font_create("/font/cjk.ttf",
                                         LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                         16,
                                         LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!s_cjk_font) {
        ESP_LOGE(TAG, "创建中文回退字体失败（确认 cjk.ttf 已烧录到 font 分区）");
        return;
    }

    /* 挂到子集字体之后作为缺字回退链末端：
       菜单/UI 用清晰子集，任意中文文件名缺字时自动回退到 freetype 全量中文字体 */
    lv_font_chinese_16.fallback = s_cjk_font;
    lv_font_chinese_14.fallback = s_cjk_font;
    lv_font_chinese_12.fallback = s_cjk_font;

    s_font_ready = true;
    ESP_LOGI(TAG, "中文回退字体就绪（/font/cjk.ttf, 分区大小 %u KB）",
             (unsigned)(s_font_part->size / 1024));
#else
    /* LV_USE_FREETYPE=0：不注册 freetype，避免 LVGL 对任何缺失字形 query
     * 半瘫 freetype 导致 lv_timer_handler 死循环 + task_wdt。中文文件名会
     * 显示为空白/豆腐块，但不会卡死。将来要支持中文时再开启并修复初始化。 */
    ESP_LOGW(TAG, "LV_USE_FREETYPE=0，中文渲染未启用（仅 ASCII UI）");
    s_font_ready = false;
#endif
}

/* 中文回退字体（freetype）是否就绪。未就绪时上层应避免渲染任何可能
 * 触发 freetype 路径的文本（如启动 splash 用 ASCII 兜底），否则未初始化的
 * freetype 会在 LVGL 渲染时死循环并触发 task_wdt。 */
bool font_partition_ready(void)
{
    return s_font_ready;
}
