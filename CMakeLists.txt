# ESP-IDF 项目顶层配置
cmake_minimum_required(VERSION 3.16)

# 启用 ESP-ADF：把 ADF components 加进 build
# (ADF_PATH 必须在 build 环境中已 export：set ADF_PATH=D:\esp\esp-adf)
if(DEFINED ENV{ADF_PATH} AND EXISTS "$ENV{ADF_PATH}/components/audio_pipeline")
    set(EXTRA_COMPONENT_DIRS "$ENV{ADF_PATH}/components")
    message(STATUS "ESP-ADF components enabled from $ENV{ADF_PATH}/components")
else()
    message(WARNING "ADF_PATH not set or audio_pipeline not found; ADF components disabled")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audiobook_player)