/*
 * build_info.c  (R124)
 * 打印精准构建信息：真实构建时间 + git 版本 + 短 hash。
 * build_info.h 由 gen_build_info.cmake 在每次构建时重新生成，故这些信息始终准确，
 * 不受 ESP-IDF esp_app_desc.c 增量编译不重编导致的 "Compile time" 滞留旧日期影响。
 */
#include "build_info.h"
#include "esp_log.h"

static const char *TAG = "BUILD";

void build_info_print(void)
{
    ESP_LOGI(TAG, "BUILD version=%s hash=%s time=%s",
             BUILD_INFO_VERSION, BUILD_INFO_GIT_HASH, BUILD_INFO_TIME);
}
