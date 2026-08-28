# 全局状态机 (APP_STATE)

> 由 `main.cpp` 的 `app_state_t` 枚举定义（`main.cpp:87` 起），主循环按当前状态路由按键、刷新显示、调度 SD/音频/OTA。
> 本文档根据代码实测调用链整理，供后续维护参考。如发现与代码不符，以代码为准。

## 状态枚举（10 个顶层状态）

| 状态 | 含义 |
|---|---|
| `APP_STATE_IDLE` | 无 TF 卡 / 待机（进入 light sleep 前） |
| `APP_STATE_STOPPED` | 已加载播放列表，停止播放，待播放 |
| `APP_STATE_PLAYING` | 正常播放 |
| `APP_STATE_PAUSED` | 暂停（保留断点，音频管道暂停） |
| `APP_STATE_FAST_FORWARD` | 快进（变速播放，倍速 > 1） |
| `APP_STATE_REWIND` | 快退（变速播放，倍速 < 0） |
| `APP_STATE_BROWSING` | 浏览/选曲界面 |
| `APP_STATE_MENU` | 设置菜单（独立于播放的向导） |
| `APP_STATE_BT_SPEAKER` | 蓝牙音箱 (A2DP Sink) 模式 |
| `APP_STATE_OTA` | TF 卡固件升级向导（独立于菜单） |

> 注：`FAST_FORWARD` / `REWIND` 是播放态下的变速子态，由 `audio_player_set_speed()` 切换倍速实现，不丢失播放上下文。

## 关键状态转移

```
                 TF 卡插入 (sd_scan_and_load)
   IDLE ──────────────────────────────────────► STOPPED
     ▲                                            │
     │ light sleep (main.cpp:1267)                │ PLAY_PAUSE 短按
     │                                            ▼
     │                                       PLAYING ◄──────────────┐
     │                                            │                  │
     │                              PLAY_PAUSE 短按│ PLAY_PAUSE 短按  │
     │                                            ▼                  │
     │                                         PAUSED ──────────────┘
     │                                            │
     │                         FF/RW 按键        │  退出 FF/RW
     │                           ▼  ▲             │
     │                    FAST_FORWARD / REWIND ─┘
     │
     │  TF 卡拔出 / 自动关机 (auto_off) / 低电量
     └──────────────────────────────────────────► STOPPED
                                                  (最终 light sleep → IDLE)

   STOPPED/PLAYING/PAUSED ── MODE 键 ──► BT_SPEAKER
   STOPPED/PLAYING/PAUSED ── MENU 键 ──► MENU ── 退出 ──► 原状态
   任意播放态 ── OTA 菜单项 ──► OTA ── app_ota_exit() ──► 进入前状态
```

## 转移触发点（实测）

- **IDLE → STOPPED**：TF 卡插入后 `sd_scan_and_load()` 成功，`main.cpp` 置 `APP_STATE_STOPPED`。
- **STOPPED → PLAYING**：`BTN_ID_PLAY_PAUSE` 短按且在 `STOPPED/IDLE` 态 → `play_current_track()`。
- **PLAYING ↔ PAUSED**：`BTN_ID_PLAY_PAUSE` 短按在 `PLAYING`/`PAUSED` 间切换（`main.cpp:565-574`）。
- **PLAYING/PAUSED → FAST_FORWARD/REWIND**：快进/快退按键（`main.cpp:586-589`），再次按原速返回。
- **→ BT_SPEAKER**：`MODE` 键（`main.cpp:576-579`）。
- **→ MENU**：`MENU` 键（`main.cpp:581-584`）。
- **→ OTA**：菜单选中升级项 → `app_enter_ota()`（`main.cpp:369-374`），置 `g_ota_in_progress=true`。
- **OTA → 原状态**：`app_ota_exit()`（`main.cpp:376-393`）清 `g_ota_in_progress` 并恢复 `g_state_before_menu`。

## 与断点保存的关系

- `save_current_position()`（`main.cpp:164`）在内部调用 `settings_flush()`，**并非**"只保存不落盘"。
- 主循环每 `AUTO_SAVE_INTERVAL_US` 对 `PLAYING/PAUSED/FAST_FORWARD/REWIND` 态自动保存（`main.cpp:1184-1189`）。
- 暂停 (`audio_player_pause`, `audio_player.cpp:338`) 本身不立即落盘，依赖上述周期自动保存；意外掉电窗口 ≤ `AUTO_SAVE_INTERVAL`。

## OTA 子状态 (ota_sd.cpp)

独立于 `APP_STATE` 枚举，由 `ota_phase_t` 描述向导内部阶段：

| 阶段 | 含义 |
|---|---|
| `OTA_PHASE_CONFIRM` | 摘要 + 二次确认（低电量锁死） |
| `OTA_PHASE_PROGRESS` | 写入中（全键锁死，喂 WDT） |
| `OTA_PHASE_DONE` | 成功，任意键重启 |
| `OTA_PHASE_ERROR` | 失败（可重试/返回） |

> 健壮性：确认/错误态若 120s 无操作，`ota_sd_tick()` 自动调用 `app_ota_exit()`，保证 `g_ota_in_progress` 必然清零（review #24 修复）。
