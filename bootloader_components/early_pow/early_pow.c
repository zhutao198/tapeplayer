/* bootloader hook: 在二级 bootloader 初始化完成后、app 加载前拉高 POW_EN (IO40)
 *
 * 必须在 bootloader 阶段完成(早于 app 的 ~1s 启动), 否则用户松手即关机。
 *
 * 关键修正: 仅写 GPIO_ENABLE1 / GPIO_OUT1 寄存器不足以让 pad 真正输出高电平,
 * 还必须:
 *   1) 把 IO_MUX 功能切到 GPIO (MCU_SEL = 0)
 *   2) 通过 GPIO 矩阵把输出信号路由到该 pad (func_out_sel = SIG_GPIO_OUT_IDX)
 * 否则 pad 不驱动 —— 这正是 app 侧 gpio_config() 会做的几步。
 *
 * 注: bootloader 构建不含 components/hal/include, 故只用 soc 层头文件直接操作寄存器。
 * 参考: components/hal/esp32s3/include/hal/gpio_ll.h
 */
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"

/* 必须定义此哑函数: bootloader_start 组件通过 "-u bootloader_hooks_include"
 * (见 $IDF_PATH/components/bootloader/subproject/main/CMakeLists.txt) 强制链接器
 * 把本 .o 拉入 bootloader elf。否则因 bootloader_after_init 是 weak 符号且弱引用,
 * 链接器会整体 GC 丢弃本文件, 钩子永不执行 (此前两版都失效的真正原因)。
 * 参考 $IDF_PATH/examples/custom_bootloader/bootloader_hooks */
void bootloader_hooks_include(void)
{
}

void bootloader_after_init(void)
{
    gpio_dev_t *const hw = &GPIO;
    const int pin = 40;
    const uint32_t bit = (uint32_t)(pin - 32);  /* IO40 -> GPIO1 组 bit8 */

    /* 1) IO_MUX: 将 IO40 选为 GPIO 功能。
     *    注意 ESP32-S3 上 PIN_FUNC_GPIO = 1 (即 FUNC_MTDO_GPIO40),
     *    MCU_SEL 必须写成 1, 写成 0 会变成 MTDO 外设功能, pad 不接 GPIO 矩阵。 */
    PIN_FUNC_SELECT(IO_MUX_GPIO0_REG + pin * 4, PIN_FUNC_GPIO);
    /* 2) 使能 pad 输出: GPIO_ENABLE1_W1TS bit8 */
    hw->enable1_w1ts.data = (1u << bit);
    /* 3) GPIO 矩阵输出路由到该 pad: func_out_sel = SIG_GPIO_OUT_IDX */
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + pin * 4, SIG_GPIO_OUT_IDX);
    /* 4) 拉高: GPIO_OUT1_W1TS bit8, 建立电源自锁 */
    hw->out1_w1ts.val = (1u << bit);
}
