# R049 统一设置菜单（计划）

> 目标：把分散在各个"长按/组合键"里的二级功能收拢到一个**统一菜单**，用户只需记住**一个入口手势（长按 STOP）**，菜单内所有功能用**同一套操作规则**。
> 状态：R049a 实施中（通用菜单框架 + 播放模式/定时关机 两个 TOGGLE 先跑通）
> 作者：CodeBuddy　日期：2026-08-12

---

## 一、为什么做（问题）

当前按键手势已经"爆炸"：

- PLAY 长按 = 切播放模式
- STOP 长按 = 进浏览
- STOP 长按（浏览态）= 加书签
- STOP + REW 组合 = 跳曲首
- FF / RW 长按 = 变速
- VOL± 长按 = 连续音量

用户根本记不住。统一菜单把"二级功能"全部摘进菜单树，第一层只保留听书直控（播放/停止/切歌/快进退/音量），第二层全用统一规则。

## 二、统一交互规则（全局唯一）

| 按键 | 菜单内作用 |
|---|---|
| PREV / NEXT | 上下移动选项 |
| VOL± | 改当前项的值（MI_TOGGLE）|
| PLAY | 进入子菜单 / 触发动作（MI_ACTION）/ TOGGLE 步进 +1 |
| STOP（短按）| 返回上一级；根级 = 退出菜单 |

**入口手势**：长按 STOP（播放/暂停/停止态）→ 打开统一菜单（取代原"长按 STOP 进浏览"）。

## 三、菜单树（方案①，已确认）

```
菜单 (长按 STOP)
├─ 浏览文件      MI_ACTION  → 进入文件浏览 (复用现有 browse)
├─ 播放
│   ├─ 播放模式   MI_TOGGLE  options=顺序/列表循环/单曲循环   (原 PLAY 长按 → 迁入)
│   └─ 定时关机   MI_TOGGLE  options=关/15/30/60/90 分钟       (引擎已就绪, 此补 UI)
├─ 书签          MI_ACTION  → 进入浏览后 STOP 长按加书签 (R049d 升级为列表管理)
└─ 系统 (R049c 起)
    ├─ A-B 复读   MI_ACTION  → 标记态 (R049b)
    ├─ 固件升级   MI_ACTION  → 确认弹窗 → 进度 → 重启 (R049c)
    ├─ USB 存储   MI_ACTION  → 提示插线挂载 (R049c)
    ├─ 按键提示音 MI_TOGGLE
    ├─ 语音播报   MI_TOGGLE
    ├─ EQ         MI_TOGGLE
    └─ 关于       MI_ACTION
```

## 四、通用菜单模型（代码骨架）

```c
typedef enum { MI_SUBMENU, MI_TOGGLE, MI_ACTION } menu_item_kind_t;
typedef struct menu_item menu_item_t;
struct menu_item {
    const char *label;
    menu_item_kind_t kind;
    const char **options;   // MI_TOGGLE: 选项字符串数组
    int  option_count;
    int  (*get_idx)(void);  // MI_TOGGLE: 当前索引
    void (*set_idx)(int i); // MI_TOGGLE: 设值 (持久化在此做)
    const menu_item_t *children; // MI_SUBMENU
    int  child_count;
    void (*on_enter)(void); // MI_ACTION: 触发
};
```

导航用栈 `menu_level_t s_stack[]`（items/count/sel/title），深度 ≤ 8。
宿主回调（main.cpp 提供，C++ 链接，非 static）：
- `app_menu_exit()`：关闭菜单并恢复进菜单前的 app 状态
- `app_enter_browse()`：关闭菜单并进入文件浏览
- `app_get_play_mode() / app_set_play_mode(int)`：播放模式读写

## 五、实施分期

| 分期 | 内容 | 状态 |
|---|---|---|
| R049a | 通用菜单框架 + 显示 + 树骨架；先跑通 MI_TOGGLE（播放模式、定时关机）| **进行中** |
| R049b | A-B 区间复读标记态（audio_player + 状态机）| 待做 |
| R049c | OTA / USB 升级确认流（先桩后实）| 待做 |
| R049d | 书签列表管理、提示音/语音/EQ/蓝牙 toggle 桩 | 待做 |
| R049e | 清理旧分散手势（PLAY 长按切模式等）+ 更新文档 | 待做 |

## 六、被取代/保留的分散手势

| 原手势 | 去向 |
|---|---|
| STOP 长按 = 进浏览 | → 改为"长按 STOP 进菜单"，浏览成为菜单项（R049a）|
| PLAY 长按 = 切播放模式 | → 菜单`播放/播放模式` VOL± 切（R049e 移除旧路径，R049a 暂保留双入口）|
| STOP + REW 跳曲首 | 保留（磁带语义直觉，属直控快捷键）|
| STOP 长按（浏览态）加书签 | 保留（浏览内行为，R049d 升级为菜单书签管理）|

## 七、落点文件

- 新增 `main/menu.{h,cpp}`：通用菜单模型 + 整棵树 + 导航状态机
- `main/display.{h,cpp}`：新增 `display_show_menu()`（复用 `g_msg` 居中文本渲染）
- `main/main.cpp`：新增 `APP_STATE_MENU`；STOP 长按入口；菜单路由；宿主回调；`update_display` 跳过菜单态
- `main/CMakeLists.txt`：加入 `menu.cpp`
- `main/power_mgmt.cpp`：不改（定时关机引擎已就绪）
- `main/audio_player.cpp`：R049b 才动（A-B 循环）
