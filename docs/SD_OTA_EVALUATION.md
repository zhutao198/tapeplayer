# TF 卡固件升级（SD-OTA）可行性与安全评估

> 对应需求：R049c「先桩后实」——把 `app_ota_enter` 的信息屏桩替换为「从 TF 卡读取固件镜像刷写」的真实升级。
> 评估日期：2026-08-12 ｜ 目标硬件：ESP32-S3-WROOM-1 N16R8（16MB Flash / 8MB PSRAM）

## 0. 结论

**可行性：高，可直接落地。** 所有底层前提已经具备，无需改硬件、无需改分区表、无需联网：

| 前提 | 现状 | 是否满足 |
|------|------|----------|
| TF 卡接口 | SPI2 已挂 FATFS 到 `/sdcard`，`SD_CD_IO=IO38` 在位检测可用 | ✅ |
| OTA 分区表 | `partitions_ota.csv` = factory+otadata+ota_0+ota_1，sdkconfig 已指向 | ✅ |
| 当前固件已在 OTA 表上运行 | `build/` 含 `partition-table.bin`、`ota_data_initial.bin` | ✅ |
| 固件体积 < 槽位 | `audiobook_player.bin` = 1.06MB < 2MB，余量 ~0.94MB | ✅ |
| OTA API | ESP-IDF v5.x `esp_ota_ops.h` / `esp_partition.h` 可用 | ✅ |

预计新增约 **250~350 行 C**，工作量集中在：挂载校验 → 读取镜像 → `esp_ota_*` 写入 → 回滚标记。风险可控。

---

## 1. 推荐升级流程

```
用户进「系统 → 固件升级」
  → 扫描 /sdcard/TAPEBOOK.BIN（仅根目录，固定文件名）
  → 显示摘要：文件名 / 大小 / 版本 / 来源
  → 用户确认（「确认升级？按 PLAY，取消按 STOP」）
  → 校验（见 §3.4）→ 进度条「写入中，请勿断电」
  → esp_ota_end 校验 → esp_ota_set_boot_partition
  → 提示「升级成功，请重启」→ 用户确认重启 esp_restart()
```

伪代码（核心写循环）：

```c
const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL); // 返回 ota_0 或 ota_1，绝不返回 factory
FILE *f = fopen("/sdcard/TAPEBOOK.BIN", "rb");
fseek(f,0,SEEK_END); long sz = ftell(f); fseek(f,0,SEEK_SET);
if (sz > dst->size) { abort("镜像超过槽位"); }           // 防越界
esp_ota_handle_t h;
esp_ota_begin(dst, sz, &h);
uint8_t buf[4096];
while ((n = fread(buf,1,sizeof buf,f)) > 0) {
    if (sd_removed()) { esp_ota_abort(h); abort("TF卡被拔出"); } // 防中途拔卡
    esp_ota_write(h, buf, n);
}
fclose(f);
esp_ota_end(h);                 // ★ 内部校验 SHA256 + 镜像适用于 ESP32-S3，失败即返回错误
esp_ota_set_boot_partition(dst); // ★ 仅在校验通过后，才把新槽标记为启动
esp_restart();
```

---

## 2. 安全保障（分级，按必做/推荐）

### 2.1 防变砖（必做，A/B 架构天然提供）
- **factory 分区永不被 OTA 覆盖**：`esp_ota_get_next_update_partition()` 只返回 `ota_0`/`ota_1`，绝不返回 factory。即使两次 OTA 都失败，bootloader 仍回退到 factory → 设备永不彻底变砖。
- **先写全、后切换**：只有 `esp_ota_end()` 成功（镜像完整且适用于本芯片）后才调用 `esp_ota_set_boot_partition()`。中途断电 → otadata 仍指向旧有效槽 → 重启回到旧固件。

### 2.2 写入完整性（必做，IDF 内置）
- `esp_ota_end()` 会执行 `esp_image_verify`：校验**镜像头魔数 0xE9、芯片 ID=ESP32-S3、尾部 SHA256**。截断/损坏/非本芯片的镜像在此被拒绝，不会标记为可启动。
- 写循环中对每一次 `fread` 检测 SD 错误或 `SD_CD` 失活 → 立即 `esp_ota_abort()` 并中止，绝不标记启动分区。

### 2.3 启动回滚（必做，防 boot loop —— 最关键一环）
- 开启 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`。新 OTA 镜像首次启动被标记为 **PENDING**。
- 应用启动成功（音频栈 + SD 挂载 + 关键外设初始化完成）后调用 `esp_ota_mark_app_valid_cancel_rollback()` 确认有效。
- 若新固件能烧进、能启动但初始化阶段崩溃，则不会被标记有效 → 下次上电自动回退到上一槽。**否则会出现「能写能启动但反复死机」的假死砖。**
- 当前固件尚未调用该函数，属于本次实现的必加项（在 `main.cpp` 初始化尾部、加一个短延时/健康判定后调用）。

### 2.4 镜像鉴权与防降级（推荐，实现「安全」的硬指标）
仅靠 §2.2 只能防「损坏」，不能防「恶意/旧版」：
- **配套清单文件**：TF 卡同时放 `TAPEBOOK.BIN` + `TAPEBOOK.SHA256`（文本，构建时由 `espsecure.py digest` 生成）。刷写前比对文件实际 SHA256，不符即拒。
- **版本防降级**：固件内嵌语义版本（如 `APP_VERSION` "1.1.0"）。刷写前读取当前运行版本，若新镜像版本 **低于** 当前且非「强制」标记，则拒绝（防降级攻击重新引入已知 bug）。版本号可写进镜像自定义段或在清单里附带。
- **（可选）签名校验**：放一张 ed25519 公钥到 NVS/flash，对 `TAPEBOOK.SHA256` 做签名 `TAPEBOOK.SIG` 校验，杜绝任意人制作升级包。消费设备建议做，但会耦合构建流程（需保管私钥）。**开发期可先不做，先用 SHA+版本+回滚三件套。**

### 2.5 操作安全（UX，必做）
- **显式二次确认**：绝不自动升级；摘要展示版本/大小，PLAY 确认、STOP 取消。
- **进度可视化 + 禁断电容告**：写入中显示百分比与「请勿断电」；若电池电量低于阈值，先提示「请连接充电器」再允许。
- **互斥锁**：升级期间置全局 `g_ota_in_progress`，禁止音乐播放/浏览同时访问 SD（SD 卡为单挂载点，必须独占），避免读竞争导致镜像损坏。
- **仅在停止态进入**：升级入口在菜单系统子项，天然处于非播放态；进入时先 `audio_player_stop()`。

---

## 3. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| 写入中 TF 卡被拔出 | 镜像残缺 | `SD_CD` 实时检测 + `fread` 错误处理 → `esp_ota_abort`，不切启动分区 |
| 写入中意外断电 | 新槽半成品 | otadata 未切 → 重启回旧固件（§2.1） |
| 新固件能启动但初始化崩溃 | 假死砖/重启循环 | 启动回滚 `mark_app_valid`（§2.3） |
| 误拷错文件 / 旧版本 | 降级/不兼容 | SHA256 清单 + 版本防降级（§2.4） |
| 镜像被篡改/恶意 | 安全风险 | 清单签名（§2.4 可选） |
| SD 与播放并发访问 | 数据竞争 | `g_ota_in_progress` 互斥锁（§2.5） |
| 固件未来膨胀超 2MB | 写失败 | 监控体积；必要时改 `partitions_ota.csv` 把槽扩到 3MB（16MB Flash 充裕） |
| 首次从 factory 升级后 factory 残留 | 可被回退（双刃） | 视为安全网，保留即可；如需防回退旧版可额外标记 |

---

## 4. 实现落点（建议）

1. **新增 `main/ota_sd.c` / `main/ota_sd.h`**：封装 `sd_ota_check()`（扫描+校验）、`sd_ota_perform()`（写循环）、`sd_ota_reboot()`。
2. **改写 `menu.cpp:132` `app_ota_enter`**：改为升级向导（扫描→摘要→确认→进度→结果），复用 `display_show_info` 与新增的进度接口。
3. **`main.cpp` 初始化尾部**：加 `esp_ota_mark_app_valid_cancel_rollback()`（条件：`g_ota_in_progress` 为假且外设健康）。
4. **`CMakeLists.txt`**：组件增加依赖 `esp_partition`、`fatfs`、`sdmmc`、`esp_ota_ops`、`esp_app_format`（多数已间接引入，仅确认）。
5. **`sdkconfig.defaults`**：加 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`。
6. **构建脚本**：在 `build.bat` 产物之外，新增一步把 `audiobook_player.bin` 复制为 `TAPEBOOK.BIN` 并生成 `TAPEBOOK.SHA256`，方便出包。

---

## 5. 是否需要「安全启动 / Flash 加密」

- **当前不启用** Secure Boot / Flash Encryption，故 `esp_image_verify` 只校验镜像自带 SHA256（防损坏），不校验签名（防篡改）。
- 对消费级有声书播放器，**建议先落地「SHA256 清单 + 版本防降级 + 启动回滚」三件套**即可达到工程安全；完整 Secure Boot V2 会强耦合构建/密钥管理、拖慢迭代，待量产前再评估开启。

---

## 6. 建议下一步

最小安全版本范围已清晰：**§2.1 + §2.2 + §2.3 + §2.5 + §2.4 的 SHA256 清单/版本防降级** 即可交付一个「不会变砖、不会降级、不会中途损坏」的 SD 升级。

是否需要我现在按上述方案实现（替换桩为真实 SD-OTA）？若实现，签名校验（§2.4 可选项）是否一并做？
