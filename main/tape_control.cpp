/**
 * @file tape_control.cpp
 * @brief 磁带机式快进快退控制实现
 *
 * 核心机制：
 * 1. 快进时，解码正常进行，但通过调整 I2S 输出采样率实现变调加速
 * 2. 在多档加速之间平滑切换，模拟磁带机加速感
 * 3. 松开按键后立即恢复到当前解码位置继续正常播放
 */

#include "tape_control.h"
#include "config.h"
#include "esp_timer.h"

static tape_mode_t g_mode = TAPE_MODE_NORMAL;
static float       g_speed = TAPE_SPEED_NORMAL;
static int         g_gear = 0;
static uint64_t    g_mode_start_us = 0;    // 进入当前模式的时间戳

/* 加速档位阶梯 */
typedef struct {
    uint32_t threshold_ms;  // 超过此时长进入该档
    float    speed;         // 该档速度
} speed_step_t;

static const speed_step_t g_speed_steps[] = {
    { TAPE_ACCEL_STEP1_MS, TAPE_SPEED_1 },  // 档位1: 1.5x
    { TAPE_ACCEL_STEP2_MS, TAPE_SPEED_2 },  // 档位2: 2.5x
    { TAPE_ACCEL_STEP3_MS, TAPE_SPEED_3 },  // 档位3: 4x
    { TAPE_ACCEL_STEP4_MS, TAPE_SPEED_4 },  // 档位4: 8x
};
#define NUM_SPEED_STEPS (sizeof(g_speed_steps) / sizeof(g_speed_steps[0]))

/* ============================================================
 * 初始化
 * ============================================================ */
void tape_control_init(void)
{
    g_mode = TAPE_MODE_NORMAL;
    g_speed = TAPE_SPEED_NORMAL;
    g_gear = 0;
    g_mode_start_us = 0;
}

/* ============================================================
 * 按键事件处理
 * ============================================================ */
static void enter_mode(tape_mode_t mode)
{
    g_mode = mode;
    g_speed = TAPE_SPEED_NORMAL;
    g_gear = 0;
    g_mode_start_us = esp_timer_get_time();
}

static void exit_mode(void)
{
    g_mode = TAPE_MODE_NORMAL;
    g_speed = TAPE_SPEED_NORMAL;
    g_gear = 0;
    g_mode_start_us = 0;
}

void tape_control_ff_press(void)
{
    enter_mode(TAPE_MODE_FAST_FORWARD);
}

void tape_control_ff_release(void)
{
    exit_mode();
}

void tape_control_rewind_press(void)
{
    enter_mode(TAPE_MODE_REWIND);
}

void tape_control_rewind_release(void)
{
    exit_mode();
}

/* ============================================================
 * 速度/状态查询
 * ============================================================ */
float tape_control_get_speed(void)
{
    return g_speed;
}

tape_mode_t tape_control_get_mode(void)
{
    return g_mode;
}

int tape_control_get_gear(void)
{
    return g_gear;
}

/* ============================================================
 * 定时 Tick：根据按住时长切换加速档位
 * ============================================================ */
void tape_control_tick(void)
{
    if (g_mode == TAPE_MODE_NORMAL) {
        return;
    }

    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - g_mode_start_us) / 1000);

    // 按阶梯逐级加速
    int new_gear = 0;
    float new_speed = TAPE_SPEED_NORMAL;

    for (int i = 0; i < NUM_SPEED_STEPS; i++) {
        if (elapsed_ms >= g_speed_steps[i].threshold_ms) {
            new_gear = i + 1;
            new_speed = g_speed_steps[i].speed;
        } else {
            break;
        }
    }

    // 快退用负速度标识方向
    if (g_mode == TAPE_MODE_REWIND) {
        new_speed = -new_speed;
    }

    g_gear = new_gear;
    g_speed = new_speed;
}
