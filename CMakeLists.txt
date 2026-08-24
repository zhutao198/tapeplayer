# ESP-IDF 项目顶层配置
cmake_minimum_required(VERSION 3.16)

# 启用 ESP-ADF：把 ADF components 加进 build
# 优先用环境变量 ADF_PATH，否则回退到本地默认路径 D:\esp\esp-adf
if(DEFINED ENV{ADF_PATH} AND EXISTS "$ENV{ADF_PATH}/components/audio_pipeline")
    set(ADF_COMPONENTS_DIR "$ENV{ADF_PATH}/components")
    message(STATUS "ESP-ADF components enabled from $ENV{ADF_PATH}/components")
elseif(EXISTS "D:/esp/esp-adf/components/audio_pipeline")
    set(ADF_COMPONENTS_DIR "D:/esp/esp-adf/components")
    message(STATUS "ESP-ADF components enabled from D:/esp/esp-adf/components (default path)")
else()
    message(WARNING "ADF_PATH not set or audio_pipeline not found; ADF components disabled")
endif()
if(ADF_COMPONENTS_DIR)
    set(EXTRA_COMPONENT_DIRS "${ADF_COMPONENTS_DIR}")
endif()

# ADF v2.8 用了若干 IDF v5.5 标记 deprecated 的 API
# (-Werror=all 把所有 warning 升级为 error；这里强制 -Wno-error=deprecated-declarations)
add_compile_options(-Wno-error=deprecated-declarations)

# 允许 CMake 自动确认批量删除未使用组件 (managed_components 中无用依赖),
# 避免非交互编译时因 SAFE_DELETE_BULK_CONFIRM_REQUIRED 中断。
set(IDF_SAFE_DELETE_BULK_CONFIRM ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audiobook_player)

# R076-CODEC-7: libesp_processing.a (PV-MP3) 与 libesp_audio_codec.a (开源 MP3) 都内嵌了
# minimp3 的 mp3dec_init/mp3dec_decode_frame，导致 multiple definition。
# 已彻底改用 esp_mp3_dec_* (开源版)，PV-MP3 的 mp3_decoder_open 不再被调用，
# 故允许链接器取首个定义 (开源版) 即可消除冲突。
idf_build_get_property(project_elf EXECUTABLE)
if(TARGET ${project_elf})
    target_link_options(${project_elf} PRIVATE "-Wl,--allow-multiple-definition")
    message(STATUS "R076-CODEC-7: added --allow-multiple-definition to ${project_elf}")
endif()