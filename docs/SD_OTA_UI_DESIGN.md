# SD 卡固件升级 —— UI 入口设计

> 衔接：`docs/SD_OTA_EVALUATION.md`（可行性/安全）。本篇只谈「入口与界面怎么摆、按键怎么映射」。
> 现有框架：LVGL v9 单屏；`g_player` 播放容器 + `g_msg` 居中标签（`display.cpp`）；菜单是叠加态，`menu_is_open()` 在 `main.cpp:341` 拦截全部按键；应用态机 `app_state_t`（`main.cpp:57`）；升级入口现为 `g_system_sub` 的 `MI_ACTION`，落在 `app_ota_enter`（`menu.cpp:110/132`）。

## 1. 入口位置（菜单树落点）

**保持嵌套，不新增顶层入口**（避免误触变砖操作）：

```
根菜单
 ├ 浏览文件
 ├ 播放
 ├ 书签
 └ 系统
     ├ A-B 复读
     ├ 固件升级   ← 入口（MI_ACTION，on_enter = app_ota_enter）
     ├ USB 存储   ← 仍桩
     ├ 按键提示音
     ├ 语音播报
     ├ EQ
     └ 关于
```

理由：固件升级属「设备维护」，频率极低，遵循手机惯例（设置→关于→系统更新）放在 `系统` 二级即可；且二级层级能天然降低误触发概率。

**可选增强（提升可发现性，非必做）**：在 SD 卡插入且根目录存在合法 `TAPEBOOK.BIN` 时，给「系统」这一行加角标提示「新固件可用」。实现：菜单渲染时读一个 `g_ota_image_ready` 标志（由 SD 插拔逻辑置位），在 `menu_render` 的标题后追加 `*`。

## 2. 状态机（新增 APP_STATE_OTA）

在 `app_state_t` 增加 `APP_STATE_OTA`，并在 OTA 模块内维护子阶段：

```c
typedef enum {
    OTA_PHASE_CONFIRM,   // 摘要 + 二次确认（合并一屏）
    OTA_PHASE_PROGRESS,  // 写入中（进度条）
    OTA_PHASE_DONE,      // 成功，提示重启
    OTA_PHASE_ERROR,     // 失败，显示原因
} ota_phase_t;
```

`app_ota_enter()`（替换现行桩）做的事：
```c
void app_ota_enter(void) {
    menu_close();                    // 关闭菜单叠加
    g_app_state = APP_STATE_MENU;    // 先退出菜单态
    ota_sd_begin();                  // 扫描 SD：决定进入 CONFIRM / ERROR
}
```
`ota_sd_begin()` 内部：无 SD → `OTA_PHASE_ERROR("请插入含固件的 TF 卡")`；无 `TAPEBOOK.BIN` → `ERROR("未找到 TAPEBOOK.BIN")`；存在 → 读取大小/版本 → `OTA_PHASE_CONFIRM`。

## 3. 各屏布局（320×240，横屏）

复用现有 `g_msg` 居中标签渲染 CONFIRM/DONE/ERROR；PROGRESS 用一条 LVGL 进度条（与播放界面 `bar_prog` 同风格）。建议 `display.cpp` 新增一组 `g_ota` 对象（默认隐藏），由 `ui_show_ota_*` 切换显示并隐藏 `g_player`/`g_msg`。

### 3.1 CONFIRM（摘要 + 确认，合并一屏）
```
┌──────────────────────────────────┐
│            固件升级              │  标题·青色居中
│                                  │
│  当前版本   v1.1.0               │
│  新版本     v1.2.0               │
│  镜像       TAPEBOOK.BIN         │
│  大小       1088 KB              │
│  电量       ████░ 78%  正常      │  ← 低电量变红并附警告
│                                  │
│  PLAY 确认升级   STOP 取消       │  底部提示
└──────────────────────────────────┘
```
电量低时底部改为 `电量低，请先充电后再升级`，并屏蔽 PLAY 确认（见 §5）。

### 3.2 PROGRESS（写入中）
```
│            固件升级              │
│                                  │
│  正在写入固件…                   │
│  [##########--------]  52%       │  LVGL 进度条 + 百分比
│  请勿断电 · 勿拔 TF 卡           │  橙色警示
└──────────────────────────────────┘
```
> 写入在主循环内同步进行（~1~3s），每写完一块调用 `display_show_ota_progress(percent)`；LVGL 刷新由独立 `lvgl_task`（5ms）完成，故进度条仍可动。循环内酌情 `esp_task_wdt_reset()` 防 10s 看门狗触发。

### 3.3 DONE（成功）
```
│          升级成功 ✔            │
│  新固件已写入，重启后生效       │
│  按任意键重启设备               │
└──────────────────────────────────┘
```

### 3.4 ERROR（失败）
```
│          升级失败 ✘            │
│  原因：未找到 TAPEBOOK.BIN      │
│  STOP 返回菜单   PLAY 重试      │
└──────────────────────────────────┘
```

## 4. 按键映射（OTA 态专用路由）

在 `handle_button_events`（`main.cpp:341` 菜单路由之后）增加：
```c
if (g_app_state == APP_STATE_OTA) { ota_sd_handle_button(events, n); return; }
```

| 阶段 / 按键 | PLAY_PAUSE | STOP | PREV/NEXT | REW/FF |
|-------------|-----------|------|-----------|--------|
| CONFIRM | 确认升级（→PROGRESS） | 取消（→回菜单/系统） | 忽略 | 忽略 |
| PROGRESS | 忽略（锁死） | 忽略（锁死） | 忽略 | 忽略 |
| DONE | 重启 `esp_restart()` | 重启（任意键均可） | 重启 | 重启 |
| ERROR | 重试（`ota_sd_begin`） | 返回菜单 | 忽略 | 忽略 |

设计要点：
- **PROGRESS 全键锁死**：升级中严禁任何打断，避免半截固件 + 误触。
- **DONE 任意键重启**：确保新分区真正生效（必须重启才能 `set_boot_partition` 生效）。
- 进入 OTA 前若正在播放，`app_ota_enter` 应先 `audio_player_stop()` 并置 `g_ota_in_progress` 互斥锁，独占 SD 卡（安全评估 §2.5）。

## 5. 入口前置校验（与安全评估对齐）

- **SD 在位**：进 CONFIRM 前由 `ota_sd_begin` 检查；无卡直接 ERROR。
- **镜像存在**：仅认根目录 `TAPEBOOK.BIN`（固定名，防误刷随机文件）。
- **版本防降级**：`新版本 < 当前版本` 且非强制 → CONFIRM 屏提示「该版本低于当前，是否仍升级？」或直接在 ERROR 拒绝（按评估 §2.4 实现）。
- **电量保护**：`power_mgmt_get_state()` 为 LOW/CRITICAL 时，CONFIRM 屏屏蔽 PLAY 确认，提示先充电。
- **SHA256 校验**：PROGRESS 前的 `esp_ota_end()` 内部已校验；此处不必重复，但可在 CONFIRM 额外显示「校验：待写入后验证」。

## 6. 与现有架构的衔接改动落点

| 文件 | 改动 |
|------|------|
| `main/main.cpp` | `app_state_t` 加 `APP_STATE_OTA`；`handle_button_events` 加 OTA 路由；`update_display` 加 OTA 分支；`app_ota_enter` 改为 `menu_close()+ota_sd_begin()` |
| `main/display.cpp` | 新增 `g_ota` 对象组（标题/摘要/进度条/百分比/提示，默认隐藏）+ `display_show_ota_confirm/progress/done/error` |
| `main/display.h` | 声明上述 4 个 API |
| `main/ota_sd.c/.h`（新增） | `ota_sd_begin` / `ota_sd_handle_button` / `ota_sd_render` + 写循环（见评估报告 §4） |
| `main/menu.cpp` | `app_ota_enter` 实现改为调 OTA 模块（现行桩删除） |

## 7. 结论

入口采用「**系统 → 固件升级 → 确认屏 → 进度屏 → 结果屏**」的四段式向导，复用现有 LVGL 单屏与菜单路由习惯，按键语义与播放/浏览一致（PLAY=确认/进入，STOP=取消/返回），升级中全键锁死，结果屏任意键重启。改动集中在新增 `APP_STATE_OTA` 态机 + 一组 `g_ota` 显示对象，对现有播放/菜单逻辑零侵入。

是否需要我按此设计实现（替换 `app_ota_enter` 桩为真实 SD-OTA 向导）？
