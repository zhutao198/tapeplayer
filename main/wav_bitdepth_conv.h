#pragma once

#include <stdbool.h>
#include "audio_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/* R123-WAV: 把直通的 WAV 原始 PCM(16/24/32bit) 转成 16bit PCM 输出。
   目的：彻底规避 ESP32-S3 直配 24/32bit I2S 在 96k/32bit 下 i2s_set_clk 卡死
   (导致 play() 阻塞 ~10s + main 看门狗 30s 死机)。下游 i2s_writer 仍是已验证的
   16bit 路径，软件音量缩放(16bit)也正确。 */
typedef struct {
    int src_bits;   /* 16 / 24 / 32，WAV 真实位深 */
    bool is_float;  /* true = 32-bit IEEE float 源(如音效 sfx)，需转 int16；false = 整数 PCM */
    int sample_rate;/* WAV 真实采样率(供 INRATE 等诊断正确计算需求带宽) */
} wav_conv_cfg_t;

audio_element_handle_t wav_bitdepth_conv_init(wav_conv_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
