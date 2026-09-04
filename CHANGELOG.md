# CHANGELOG — 简洁变更记录

> 规则：每次修改追加一条，含 版本号 / 日期 / 改动文件 / 关键点 / 风险。可读即可追溯。

---

## R102 [2026-09-04] 点阵字库中文显示（最小验证）
- 文件: `main/cjk_font.c/.h`(新增, 从 `debug/cjk_font_bak` 复制 simsun 16x16),
  `main/CMakeLists.txt`, `main/display.cpp`, `main/display.h`, `main/main.cpp`
- 关键点:
  - 绕开 LVGL 中文字体路径(`lv_font_chinese_*` 真机渲染不稳/Cache error), 改用点阵直接绘制
  - 曲名(`lbl_track`)+ 菜单(`display_show_menu`)改走点阵
  - **关键修复**: 点阵不能在各处直接调 `esp_lcd_panel_draw_bitmap`(会抢 SPI 总线→BREAK 崩溃,
    首版已崩)。改为 `cjk_request_text()` 入队, 由 `lvgl_flush_cb` 在本帧写屏后统一绘制
    (LVGL 持 SPI 总线时安全)
  - 菜单退出: `app_menu_exit()`→`display_menu_closed()` 清缓存, 恢复 player 渲染
- 范围: 仅曲名+菜单两处; 其余中文 label(A-B/OTA/蓝牙/info)仍为 LVGL 路径, 待全面铺开
- 状态: 已构建成功, 待烧录(COM7)实测
- 风险: 菜单"卡死不能退出"旧 bug 未单独处理(本次只保证 player 恢复)

## 教训 [2026-09-04] 换模型未留版本记录
- 之前多轮修改无 commit/记录, 导致无法追溯"哪个版本中文正常"
- 结论: 中文显示正确路径=点阵字库; LVGL 中文字体在你的硬件上不稳, 禁用
- 行动: 此后每次改动必做带 R 编号 commit + 写本条 CHANGELOG

---

## R102 实测结果 [2026-09-04]（补充）
- 实测: 烧录后**反复重启** `rst:0x8 TG1WDT_SYS_RST`(中断看门狗), 崩在首次 flush
- 根因: "点阵 shadow 帧缓冲 + flush 内掩码覆盖 px_map" 机制破坏 SPI/DMA 传输
- 处置: 已回退该机制(`lvgl_flush_cb` 移除 `cjk_flush_overlay()` + mask 覆盖), 恢复纯净 flush → 稳定
- 结论: **点阵两个落点均已被证伪**
  1. flush 外直接 `esp_lcd_panel_draw_bitmap` → BREAK 崩溃(R102 首版)
  2. flush 内覆盖 px_map → TG1WDT 反复重启(本次)
- 下一步: 改走 LVGL 原生位图(canvas/img), 由 LVGL 自己 flush, 不碰 SPI / 不碰 draw_buf

## R103 [2026-09-04] 修复菜单进/退卡死 30s(task_wdt)
- 文件: `main/display.cpp`
- 现象: 长按 STOP 进菜单 → main 任务卡死 30 秒 → `task_wdt: main (CPU 0)` 触发
- 根因: R102 的 `display_show_menu()` 是**唯一未加锁却直接操作 LVGL** 的显示函数。
  调用链 `handle_button_events → menu_open → menu_render → display_show_menu`
  (main 任务, **无 lv_lock**) 直接调 `lv_obj_add_flag`/`lv_obj_invalidate`,
  与 CPU1 的 lvgl_task 并发竞争 → LVGL 内部结构损坏 → `lv_refr`/`lv_obj_pos`
  遍历死循环 → main 卡 30s。
  同类函数 `display_show_ab_menu`(1664行)/`display_update`(1365行) 都有 `lv_lock()`,
  **唯独 R102 新增的这个漏了**。
- 修复: 沿用 R063/R097/R100 既定范式(**标志位化**, 而非简单加锁
  ——R097 教训: lv_lock 也无法解决的深层并发冲突)
  - `display_show_menu()` 只缓存数据 + 置 `s_menu_pending`, **不碰任何 LVGL**
  - 新增 `menu_apply_nolock()`: 真正的 LVGL 绘制(隐藏各屏+点阵入队+invalidate), 仅在持锁区执行
  - lvgl_task 持锁消费区新增 `s_menu_pending` / `s_menu_close_pending` 消费
  - `display_menu_closed()` 去掉直接 `lv_obj_invalidate`, 改置 `s_menu_close_pending`
  - `display_update()` 持锁路径直接调 `menu_apply_nolock()`(不绕标志, 无延迟)
- 实测: ✅ **卡死消失**(menu_open ENTER→EXIT 正常, 无 task_wdt), 歌曲播放正常, 菜单可进
- 遗留: 菜单中文仍空白(点阵已禁用); browse 子菜单走 LVGL 中文路径渲染出"二维码"乱码

---

## R104 [2026-09-04] 主菜单中文显示成功(方案C: LVGL canvas) + 修复退出菜单黑屏
- 文件: `main/display.cpp`, `main/menu.cpp`; 新增工具 `debug/_check_font_cover.py`
- **中文显示落点确定(方案 C)**: 前两个落点均已被证伪——
  (1) flush 外直接 `esp_lcd_panel_draw_bitmap` → BREAK 崩溃(抢 SPI 总线)
  (2) flush 内覆盖 px_map(shadow 帧缓冲+掩码) → TG1WDT 反复重启
  最终方案: **点阵画进 `lv_canvas` 的 PSRAM buffer(纯内存写), 由 LVGL 当普通
  图像对象 flush 出去**。不碰 SPI / 不碰 draw_buf 内部 / 不额外调 draw_bitmap。
- 实现要点:
  - 新增 `cjk_canvas_init/clear/text()`, canvas 320x240 RGB565, 在 `ui_create()` 之后
    创建以保证位于最顶层
  - **字节序**: canvas buffer 存 LVGL **原生** RGB565(不做 SWAP16),
    由 `flush_cb` 对整帧统一 SWAP, 与 LVGL 其它内容一致
  - `menu_apply_nolock()` 改画 canvas; 非菜单态/退出菜单时隐藏 canvas
- **字库覆盖率校验**(`debug/_check_font_cover.py`): 字库 6870 字符
  (ASCII/符号 95 + 汉字 6775, U+0020~U+9FA0)。菜单文案 107 种字符**汉字 100% 覆盖**,
  仅缺 4 个符号 `» ● ± ·` → 已替换为 `* > +`(否则显示空心方框)
- **修复退出菜单黑屏**: `menu_apply_nolock()` 会把 `g_player` 设为 HIDDEN,
  退出时若只隐藏 canvas 则屏幕无可见对象。且不能依赖 `display_update` 里的
  `ui_show_player()`——它有**指纹节流**, 退出菜单时状态未变(同曲/同位/仍 STOPPED)
  指纹相同直接 return, player 会一直黑到按 PLAY 改变状态为止。
  改为在 lvgl_task 消费 `s_menu_close_pending` 时**显式调 `ui_show_player()`**。
- 实测: ✅ 主菜单中文正确显示; ✅ 无反复重启/无卡死; 退出黑屏修复待烧录验证
- 待办: 铺开到曲名 `lbl_track` / browse 文件名 / A-B 复读 / OTA 等其余中文 label
  (browse 目前仍走 LVGL 字体路径 -> 中文渲染为"二维码"乱码)

---

## R105 [2026-09-04] 铺开方案C: 修复曲名回归 + browse 中文点阵化
- 文件: `main/display.cpp`
- **修复曲名回归 bug(重要)**: R102 把曲名 `lbl_track` 改走"点阵入队 +
  flush_cb 消费", 但该队列机制因导致 TG1WDT 反复重启已**被禁用**
  → 队列无人消费 → **曲名完全不显示**(日志 `track_vis=0` 可印证)。
  现改为曲名专用小 canvas, 修复回归。
- **browse 中文点阵化**: 原 `display_show_browse()` 把文件列表拼成大串交给
  `ui_show_msg()`(LVGL 字体) → 中文文件名渲染成"二维码"乱码。
  改为转调 `display_show_menu()` 复用点阵 canvas 路径(标题/行/提示结构一致)。
- 实现要点:
  - 新增**曲名专用小 canvas** `s_track_canvas` (304x18 @ 8,52),
    只覆盖曲名区域, 避免遮挡 player 屏的其它 LVGL 元素(状态栏/进度条/时间)。
    全屏 canvas 仅用于菜单/browse 独占态。
  - **层级关键**: LVGL 中后创建的对象在上层, 故 `cjk_canvas_init()` 中
    **先创建曲名 canvas, 后创建全屏 canvas**, 菜单态全屏 canvas 才能盖住曲名。
  - 重构点阵绘制为通用 `cjk_blit_text(fb, w, h, x, y, utf8, fg, bg)`,
    全屏 canvas 与曲名 canvas 共用。
  - `cjk_canvas_clear()` / `cjk_track_clear()` 改为**填充背景色 0x0a0e17**
    (原 memset 纯黑, 会与 player 背景形成色块)。
  - browse 退出: 消费 `s_clear_msg_pending` 时清 `s_menu_visible` + 隐藏全屏
    canvas, 否则 canvas 会一直盖住 player 屏。
- 状态: 已编译通过, 待烧录实测
- 待办: A-B 复读 / OTA / info 弹窗等其余中文 label 点阵化

---

## R106 [2026-09-04] R105 实测两修复: 汉字方框(FATFS编码) + browse退出卡住

- 文件: `main/main.cpp`(browse 退出补 `display_clear_msg()`), `sdkconfig.defaults`, `sdkconfig`(FATFS 编码)
- **问题1 — 曲名/文件名中文方框**: R105 烧录后菜单中文正常(源码 UTF-8), 但
  **SD 卡中文文件名/曲名显示方框**。根因: FATFS 原 `FATFS_API_ENCODING_ANSI_OEM`
  (CP437), `f_gets`/`scandir` 返回的文件名**不是 UTF-8**, 点阵按 UTF-8 解码得
  错误 unicode → 字库缺字 → 画空心方框。菜单文本是编译期 UTF-8 常量故不受影响,
  差异正源于此。
- **修复1**: `sdkconfig.defaults` 与 `sdkconfig` 改
  `CONFIG_FATFS_API_ENCODING_UTF_8=y`(CODEPAGE 保留 437, **不引入 180KB cc936
  简体中文表**, 避免撑爆 2MB app 分区)。改 FATFS 编码**必须重编**才能生效。
- **问题2 — browse 选曲后停在浏览界面不返回**: `BTN_ID_PLAY_PAUSE` 退出 browse 时
  只切 `g_app_state`, 没清 `s_menu_visible`, 导致 `display_update` 继续走
  `menu_apply_nolock()` 画 browse canvas 盖住 player 屏(日志 `ui_show_player #1/#2`
  被调用却看不见即此因)。
- **修复2**: `main.cpp:558` 退出 browse 处补 `display_clear_msg()`。
- **遗漏补丁**: 同路径 `BTN_ID_STOP` 退 browse 也只切状态、漏清独占态 → 一并补
  `display_clear_msg()`(main.cpp:589)。
- 实测(烧录后真机): ✅ 曲名/文件名中文不再方框; ✅ browse 选曲按 PLAY 返回播放界面;
  ✅ browse 按 STOP 退出也返回。三项全部通过。
- 固件 1,796,304 B(≈1.71MB) < 2MB app slot 上限, 空间充足。
