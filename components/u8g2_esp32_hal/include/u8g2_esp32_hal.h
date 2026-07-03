#pragma once
#include "u8g2.h"

typedef struct { int sda; int scl; } u8g2_esp32_hal_t;

void u8g2_esp32_hal_init(u8g2_esp32_hal_t cfg);
uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
