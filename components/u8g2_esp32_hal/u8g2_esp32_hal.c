// Minimal u8g2 ESP32 HAL stub - adapt as needed
#include "u8g2_esp32_hal.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "u8g2_hal"
#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 400000
#define ACK_CHECK_EN 1

static int s_sda = -1, s_scl = -1;

void u8g2_esp32_hal_init(u8g2_esp32_hal_t cfg) {
    s_sda = cfg.sda; s_scl = cfg.scl;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = s_sda,
        .scl_io_num = s_scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_PORT, &conf);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    ESP_LOGI(TAG, "u8g2 I2C init sda=d", s_sda, s_scl);
}

uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    static uint8_t buf[128]; static uint8_t buf_idx; static uint8_t data;
    switch(msg) {
        case U8X8_MSG_BYTE_INIT: buf_idx = 0; break;
        case U8X8_MSG_BYTE_SEND: buf[buf_idx++] = arg_int; break;
        case U8X8_MSG_BYTE_SET_DC: data = arg_int; break;
        case U8X8_MSG_BYTE_START_TRANSFER:
            buf_idx = 0; i2c_master_write_to_device(I2C_PORT, u8x8_GetI2CAddress(u8x8)>>1, NULL, 0, 1000/portTICK_PERIOD_MS);
            break;
        case U8X8_MSG_BYTE_END_TRANSFER:
            i2c_master_write_to_device(I2C_PORT, u8x8_GetI2CAddress(u8x8)>>1, buf, buf_idx, 1000/portTICK_PERIOD_MS);
            break;
        default: return 0;
    }
    return 1;
}

uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch(msg) {
        case U8X8_MSG_GPIO_RESET: break;
        case U8X8_MSG_DELAY_MILLI: vTaskDelay(arg_int/portTICK_PERIOD_MS); break;
        case U8X8_MSG_DELAY_10MICRO: ets_delay_us(10*arg_int); break;
        case U8X8_MSG_DELAY_100NANO: ets_delay_us(1); break;
        default: return 0;
    }
    return 1;
}
