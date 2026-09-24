# IDF 必需补丁（项目构建前置条件）

本项目的音频播放（ESP-ADF 5.x MP3 解码器）依赖一个 **ESP-IDF 官方未内置、需手动打的补丁**。
任何要编译/烧录本项目的机器，都必须先完成本节步骤，否则链接/运行期会报
`Not found right xTaskCreateRestrictedPinnedToCore` 并导致音频播放失败。

---

## 背景：为什么需要这个补丁

- `xTaskCreateRestrictedPinnedToCore()` 是 **老版 FreeRTOS 的受限任务创建 API**（可指定静态栈 + MPU region + 钉核）。
- ESP-IDF **v5.4+ 移除了该 API**（ESP 官方认为不应作为公开 API）。
- 但 **ESP-ADF 5.x 的 MP3 解码器任务仍调用它**，且检测到符号缺失时会主动打印：
  ```
  Not found right xTaskCreateRestrictedPinnedToCore.
  Please enter IDF-PATH with "cd $IDF_PATH" and apply the IDF patch with
  "git apply $ADF_PATH/idf_patches/idf_-128_freertos.patch" first
  ```
  （报错里的 `idf_-128_freertos.patch` 是 ADF 脚本的硬编码字符串，**真实文件名见下**）
- 因此必须给 IDF 源码补回该函数，ADF 才能创建解码器任务、正常播放。

---

## 必需补丁清单（仅 1 个）

| 项 | 值 |
|---|---|
| 适用 IDF 版本 | **v5.5.3**（本项目锁定版本） |
| 补丁文件 | `D:\esp\esp-adf\idf_patches\idf_v5.5_freertos.patch` |
| 改动位置 | `D:\esp\v5.5.3\esp-idf`（**IDF 安装目录，非本项目仓库**） |
| 改动内容 | 给 FreeRTOS 加 3 处：`idf_additions.h` 声明、`freertos_tasks_c_additions.h` 实现、`linker_common.lf` 导出符号 |
| 是否入本项目 git | **否**（属于环境维护，git status 不可见） |

> ⚠️ 其他版本 patch（`idf_v5.4_freertos.patch` 等）**不适用于 v5.5.3**，请勿混用。
> `idf_patches/` 目录里还有 esp32p4 / v3.3 / http_client 等补丁，本项目均不需要。

---

## 应用步骤（新机器 / 重装 IDF 后必做）

```bat
REM 1. 进入 IDF 源码根目录
cd /d D:\esp\v5.5.3\esp-idf

REM 2. 先 dry-run 确认能干净应用
git apply --check D:\esp\esp-adf\idf_patches\idf_v5.5_freertos.patch
REM 输出应为空（无报错即表示可应用）

REM 3. 正式打补丁
git apply D:\esp\esp-adf\idf_patches\idf_v5.5_freertos.patch

REM 4. 验证已生效（应搜到 3 处匹配）
git diff --stat components/freertos
```

打完补丁后，照常 `build.bat build` 即可（idf.py 会自动重编 freertos 并链接新符号）。

---

## 副作用与维护注意事项

1. **升级 IDF 会被覆盖**：重装/升级 `D:\esp\v5.5.3\esp-idf` 后此补丁消失，必须重新打。
2. **换机器同理**：任何新开发机编译本项目前都要打。
3. **撤销补丁**（如需）：`cd D:\esp\v5.5.3\esp-idf && git apply -R D:\esp\esp-adf\idf_patches\idf_v5.5_freertos.patch`
4. **低风险提示**：补丁仅新增一个静态分配任务函数，不改动任何现有逻辑，风险极低。

---

## 关联记录

- 根因与排查过程见 `FLASH_TROUBLESHOOTING.md` 的 R052/R053 章节。
- 本补丁对应开发任务 **R053-adf-patch**（分支 `fix-r053-audio`）。
