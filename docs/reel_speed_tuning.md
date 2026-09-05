# 磁带卷轴旋转速度调参说明

## 现状（R109 后续修复）
- 旋转改为**预渲染 24 帧静态位图**切换（`main/reel_img.h` 的 `reel_frame_dsc[24]`，每 15° 一帧，透明背景）。
- 动画回调 `reel_anim_cb`（`main/display.cpp`）用**浮点累积器** `s_reel_acc` 控制推进速度，每 tick（33ms）累积 `speed` 帧，整数部分进帧。
- 实测速度：PLAYING 当前 `speed = 0.25f` ≈ 7.5 帧/秒 ≈ 0.31 圈/秒。用户反馈仍偏快，需自行下调。

## 怎么改速度
文件：`main/display.cpp` → `reel_anim_cb`（约 line 850）

```c
case PLAYER_STATE_PLAYING:      speed =  0.25f; break;   // ← 改这个数值
case PLAYER_STATE_FAST_FORWARD: speed =  3.0f * (1 + s_reel_gear); break;
case PLAYER_STATE_REWIND:       speed = -3.0f * (1 + s_reel_gear); break;
```

- 数值越小越慢：`0.15f` ≈ 0.19 圈/秒；`0.10f` ≈ 0.125 圈/秒。
- 想更顺滑可改 `tools/gen_reel.py` 的 `FRAMES = 24` 调大（如 36/48），重新生成 `reel_img.h`：
  ```
  cd D:\zhutao\audio_player && python tools/gen_reel.py
  ```
- 想改整体节奏还可调 `reel_anim_cb` 的 timer 周期（`lv_timer_create(reel_anim_cb, 33, NULL)`，约 line 319），单位 ms。

## 改完编译烧录
```
build.bat build flash
```

## 关键变量速查
| 变量 | 位置 | 作用 |
|------|------|------|
| `s_reel_acc` | display.cpp 静态 | 帧累积器（保证低速平滑不跳变） |
| `s_reel_frame` | display.cpp 静态 | 当前帧索引 0..23 |
| `REEL_FRAME_COUNT` | reel_img.h (#define) | 帧数 = 24 |
| `FRAMES` | gen_reel.py | 生成帧数，需与上面一致 |
