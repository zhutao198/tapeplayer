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
