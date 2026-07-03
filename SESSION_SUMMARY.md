# SESSION_SUMMARY.md — TapeBook 关键决策与经验

> **最后更新**：2026-07-03（4 个 R 节点完成：baseline / R001 / R002 / R003）

---

## 1. 会话主线（按时间）

| 时间 | 主题 | 结果 |
|---|---|---|
| 2026-07-03 上午 | 阅读 README + DEVELOP_STATUS + HARDWARE_MIGRATION | 摸清项目状态：V1.0 MVP 8/10，3 个 P0 阻塞 |
| 2026-07-03 中午 | 用户确认方向：先解 P0 阻塞 + 初始化 git | 创建 5 个任务 |
| 2026-07-03 中午 | 用户补充 GitHub 仓库地址 | 调整计划：本地 → GitHub `zhutao198/tapeplayer` |
| 2026-07-03 下午 | git init + .gitignore + 3 类文件 + 首次 commit | ✅ baseline commit `938abbe` |
| 2026-07-03 下午 | R001 启用 ESP-ADF（追认 sdkconfig 已配） | ✅ commit `c0c67e4` |
| 2026-07-03 下午 | R002 启用 u8g2（删 334M 手动源码 + 备份 + idf component 路径） | ✅ commit `d773f05` |
| 2026-07-03 下午-晚上 | R003 build 验证（多次失败 + 修 4 个子模块 + 暴露 ADF 5.5 引用方式变化） | ⚠️ commit `333e44e`（build 未通过） |

---

## 2. 关键决策

### 决策 D001：本地项目上传为 GitHub 新仓库
- **背景**：本地 `D:\zhutao\audio_player` 无 .git；用户指向 `https://github.com/zhutao198/tapeplayer`
- **决定**：本地是主源，GitHub 为新仓库
- **实施**：git init → 首次 commit (baseline) → R001-R003 → （待 push）

### 决策 D002：3 类核心文件立即建
- **背景**：全局 CLAUDE.md 9.2 强制要求；项目根原本缺失
- **决定**：在首次 commit 前建好 CONTEXT/SESSION_SUMMARY/开发日志
- **理由**：避免 R 节点机制空转

### 决策 D003：.gitignore 屏蔽 build 日志 + 第三方源码
- **背景**：根目录有大量 `*.log` / `*.err` / `*.out`（编译产物）+ 334M `components/u8g2/`（手动 clone）
- **决定**：全部忽略；`sdkconfig` 忽略但 `sdkconfig.defaults` 保留
- **理由**：仓库只留源码 + 文档 + 模板

### 决策 D004：R002 删 334M components/u8g2 + 走 idf component
- **背景**：手动 clone 的 334M 源码难维护
- **决定**：备份后删 + 改用 `idf.py add-dependency lfengineering/u8g2_esp32`
- **结果**：R002 commit 后发现 `lfengineering/u8g2_esp32` 在 registry **不存在** → R003 暂禁用

### 决策 D005：R003 暂禁用 u8g2 + 保留 ADF 修复
- **背景**：R002 用了错的 u8g2 component 名 → build fail；同时发现 R001 漏 ADF REQUIRES → 4 文件修复
- **决定**：u8g2 暂禁用，ADF 修复 + 子模块 fix 走 R003 commit（即使 build 仍 fail）
- **理由**：保留进度，避免代码丢失

### 决策 D006：ESP-IDF 子模块 4 个修复不入仓
- **背景**：micro-ecc / lib_esp32 / lib_esp32c2 / lib_esp32c3_family 都坏（HEAD bad object）
- **决定**：直接修复 ESP-IDF 仓库子模块（环境维护），不入 audio_player 仓
- **记录**：在开发日志 R003 节点留 T-003 教训 + 修复命令模式

### 决策 D007：评审文档归档不建 R 节点
- **背景**：用户要求评审 `docs/HARDWARE_PIN_WIRING.md`
- **决定**：输出 `docs/HARDWARE_PIN_WIRING_REVIEW.md`（V1.0），**不**建 R 节点（评审是调研/审计，不是代码修改）
- **理由**：按规范 8.1"触发时机"严格性，评审归档不属于 R 节点触发范围；用普通 commit 保留 git history

---

## 3. 关键成就

- ✅ **仓库基线建立**（baseline commit + tag，172 文件入仓）
- ✅ **3 类核心文件齐备**（CONTEXT.md / SESSION_SUMMARY.md / 开发日志.md）
- ✅ **R001 完成**（ESP-ADF 启用，已在 sdkconfig 配 + 注释追认）
- ✅ **R002 完成**（u8g2 改用 idf component 路径 + 删 334M 手动源码 + 备份到 D 盘）
- ✅ **R003 完成**（commit `333e44e` 含 5 文件修复 + sdkconfig 清理；build 验证未通过但发现关键阻塞点）
- ✅ **ESP-IDF 4 子模块修复**（micro-ecc / lib_esp32 / lib_esp32c2 / lib_esp32c3_family）

---

## 4. 经验教训

### L001：WebFetch / WebSearch 网络受限
- **现象**：WebFetch github.com / components.espressif.com / WebSearch 多个域失败
- **应对**：依赖本地目录 + git ls-remote 探测 + 询问用户澄清意图
- **未来**：涉及网络查询时，**直接 `git ls-remote`** 或让用户提供信息

### L002：项目名"audio_player"（本地）/ "tapeplayer"（GitHub）/ "TapeBook"（README）三者不同
- **现象**：路径 / 仓库名 / 文档项目名 3 个不同
- **建议**：所有引用统一用"TapeBook"或"tapeplayer"

### L003：tar 备份 + rm -rf 必须分两步 + 验证（教训 T-001）
- **现象**：第一次 `tar czf D:/u8g2_backup.tar.gz components/u8g2/` 失败（Git Bash 把 D:/ 当 host），但 `rm -rf` 仍执行
- **正确用法**：
  ```bash
  # 错误：tar czf D:/backup.tar.gz src/    ← D:/ 被 Git Bash 当 host
  # 正确：tar czf /d/backup.tar.gz src/    ← POSIX 路径 /d/ = D:\
  # 验证：tar tzf /d/backup.tar.gz | head -3
  # 再删：rm -rf src/   （验证成功后再删）
  ```
- **教训**：删前必须先验证 tar 成功；两命令分开跑，不要 `&&` 链式

### L004：ESP-IDF build 在 Git Bash 下跑不通（教训 T-002）
- **现象**：./build.bat 不被 bash 解释；cmd /c 开新窗口；cmd //c 路径转换失败
- **正确用法**：
  ```bash
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; & 'D:\esp\v5.5.3\esp-idf\export.ps1' | Out-Null; \$env:ADF_PATH='D:\esp\esp-adf'; idf.py build"
  ```
- **关键点**：
  1. 必须在 **PowerShell 内部** `Remove-Item Env:MSYSTEM`（bash 子 shell `unset` 不传给 PowerShell）
  2. `export.ps1` 通过 `&` dot-source 调，保留环境变量
  3. 不要用 cmd /c "build.bat" 跑 build.bat（开新窗口）

### L005：ESP-IDF 子模块 fix 模式（教训 T-003）
- **现象**：ESP-IDF 5.5.3 install 时多个子模块只 clone 元数据没 fetch objects
- **修复 3 步**：
  1. `cd <submodule_path>`
  2. `git fetch origin`
  3. `git reset --hard <ESP-IDF 锁定的 commit>`（从 `.gitmodules` sbom-hash 或 `HEAD` 文件读）
- **建议**：建立 `tools/fix_idf_submodules.sh` 一键修复

### L006：ADF 5.5 组件引用方式变化（教训 T-004）
- **现象**：`main/CMakeLists.txt REQUIRES audio_pipeline` 被弃用
- **正确**：`main/idf_component.yml dependencies: espressif/audio_pipeline: "*"`
- **教训**：遇到 "The component X could not be found" + 提示"moved to IDF component manager" 时，**改用 idf_component.yml 引用**

### L007：idf_component.yml YAML 空 dict 也报错
- **现象**：`dependencies:` 后只有 `# 注释` → "Input should be a valid dictionary"
- **解决**：整个 `dependencies:` 块注释掉（包括 key）

### L008：删 334M 前必须先备份 + 验证
- 见 L003（备份失败的教训）
- **未来**：所有"先备份后删"操作必须分两步：① tar + `tar tzf` 验证 ② 再 rm

---

## 5. 性能指标

暂无（首次建档，未跑 benchmark）。

---

## 6. 未来方向

### R004 计划（下次会话）
1. **用户提供正确 u8g2 idf component 名**（用 WebSearch 找候选）
2. 恢复 `main/idf_component.yml dependencies:` 块（用正确名）
3. 恢复 `sdkconfig.defaults CONFIG_USE_U8G2=y`
4. **`main/CMakeLists.txt` 移除 R003 加的 ADF REQUIRES**（改用 idf_component.yml 引用）
5. `main/idf_component.yml` 加 ADF 依赖：`espressif/audio_pipeline` 等（待验证名）
6. 重新 build 验证（继续修残留子模块）
7. 推到 GitHub

### 短期
- V1.0 MVP 补完（10/10）：文件夹浏览、OLED 屏底部对齐
- V1.1 体验增强：定时关机、按键音、屏幕保护

### 中期
- 量产前：OTA 接收代码（HTTP/HTTPS）
- V1.2 进阶：书签、语音、电量、设置菜单

### 长期
- V2.0 远期：蓝牙 A2DP、EQ、速度微调

---

## 7. 持久化资源

| 资源 | 路径 / 链接 |
|---|---|
| GitHub 仓库 | https://github.com/zhutao198/tapeplayer（待 push）|
| 本地仓库 | D:\zhutao\audio_player |
| ESP-IDF v5.5.3 | D:\esp\v5.5.3\esp-idf（**4 子模块已修复**）|
| ESP-ADF v2.7 | D:\esp\esp-adf |
| u8g2 备份 | D:\u8g2_backup_20260703.tar.gz（282 MB）|
| ESP-IDF v5.3 离线安装器 | https://dl.espressif.com/dl/esp-idf/ |
| ESP-ADF v2.7 | https://github.com/espressif/esp-adf |
| WROOM-1 datasheet | hardware/esp32-s3-wroom-1_wroom-1u_datasheet_cn.pdf |
| WROOM-2 datasheet | hardware/esp32-s3-wroom-2_datasheet_cn.pdf |

---

## 8. R 节点 Git 状态

```
333e44e R003: build 验证未通过；修 R001 ADF REQUIRES + 修 R002 u8g2 暂禁用 + 修 sdkconfig 格式
d773f05 R002: 启用 u8g2 OLED 显示（改用 idf component 替换手动源码）
c0c67e4 R001: 启用 ESP-ADF（解锁音频播放真实路径）
938abbe baseline: 仓库基线 + 3 类核心文件 + .gitignore
```

**4 个 R 节点** 全部 committed + tagged（annotated）。

---

**作者**：Claude（4 R 节点完成）  
**更新规则**：每次 R 节点 commit 后同步更新
