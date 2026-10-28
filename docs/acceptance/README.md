# Shuttle 验收记录

日期：2026-10-27 · 环境：Windows 11（INSZOA2400559），域账号 INNOLIGHT\ren.ji，Racket 9.3 CS，rivet v0.12.0（最新发布版）

## 已验证 ✅

### 1. Racket 引擎（app/backend.rkt）

| 项 | 结果 |
|---|---|
| `raco make app/backend.rkt` 编译 | 通过 |
| `raco test app/backend.rkt` 纯单元测试（文件名净化/同名去重/清单往返） | 3/3 通过 |
| 真实共享盘全链路自测（`test-main`，独立 `.shuttle-selftest` 信箱） | 通过 |

全链路自测内容：伪装对端机器名（SELFTEST-PEER）发送中文文件名文件 → 信箱子文件夹 `manifest.json`/`payload`/`shuttle.done` 结构齐全 → **icacls 自授权生效（payload 继承 `ren.ji:(I)(F)` 完全控制）** → 本机轮询发现、复制、大小校验、下载落盘 → 远端子文件夹被清理 → 历史写入 `history.jsonl`。共享盘无测试残留。

### 2. 共享盘权限模型（docs/acceptance/acl-evidence.txt）

- LiDAR 目录对 Domain Users 继承 **(M) 修改权**（含删除）——默认中转目录天然可管理；印证「权限问题不是每个目录都有」。
- `icacls (OI)(CI)F`（未加 /t）授权后：子文件夹出现显式 `(F)`，随后创建的文件自动继承 `(I)(F)`——自愈机制对「RX,W 能建不能删」的公共目录同样成立（分析见 `acl-evidence.md`）。

### 3. Rivet 契约与代码生成

- `raco rivet schema --output rivet-schema.json` 基线刷新；`raco rivet schema check --json` → `compatible: true`。
- `raco rivet build` 推进到 MSVC 步骤前全部完成：后端契约分析、**C++ typed API 真实再生成**（`windows/GeneratedBackend.hpp` 由构建器覆盖，方法名/形状与本仓库手写基线一致）、图标资源预处理通过。
- `raco rivet i18n`：31 键 zh/en 双语表生成 `windows/GeneratedI18n.hpp`。

### 4. WinUI 3 宿主源码

`windows/` 完整交付：自定义标题栏 + Mica 背景、拖放区（拖入高亮/点击选择）、传输列表（绑定 `TransferItem` 可观察运行时类，增量刷新进度）、设置对话框（中转/下载目录 + HKCU 自启）、托盘常驻（rivet `TrayController`：右键菜单、气泡通知、Explorer 重启自愈）、错误 InfoBar、信箱健康红绿点。

## 受阻 ⏸（一次 UAC 之遥）

**WinUI 3 宿主编译与 GUI 截屏**：本机缺 MSVC Build Tools。`winget install Microsoft.VisualStudio.2022.BuildTools`（C++ 工作负载）三次尝试均因 UAC 无人确认而终止（exit 1602，无人值守会话无法完成提权确认）。

补齐方式（管理员组用户点一次「是」即可）：

```bat
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

随后在本仓库：`raco rivet build && raco rivet dev` → GUI 启动后截屏补充到本目录。

## 已知边界 / 遗留

- 发送方不知道对端机器名（信箱是匿名的），界面以「本机 <机器名>」标注自身；收到的文件在历史中显示「来自 <对端机器名>」。
- 暂不支持文件夹传输（拖入文件夹会明确报错）；不支持传输取消（LAN 场景风险可接受）。
- `raco rivet i18n` 会无条件重写 macOS 表文件路径（本产品已移除该平台，文件已 gitignore）——可考虑给 rivet 提 issue：i18n 写表前应检查平台目录是否存在。
- 首次运行会在 `%APPDATA%\Shuttle` 写 config.json/consumed.jsonl/history.jsonl。

## GUI 视觉验收（2026-10-28 补充）

`gui-main.png`：v0.1.0 发布包在本机的真机运行截图——自绘标题栏（Shuttle · 本机 INSZOA2400559）、拖放区、空态文案均正确渲染；后台的接收轮询在启动后自动于共享盘建立信箱目录（`%APPDATA%\Shuttle\config.json` 同步落盘）。发布包自包含，运行不需要任何工具链。

待下一版迭代：窗口默认尺寸已改为紧凑的 460×660（DPI 缩放，bf77844）；设置齿轮按钮在截图中不可见，下次真机运行需确认。

## 本机构建工具链结论（2026-10-28，供后续机器参考）

1. `winget install Microsoft.VisualStudio.2022.BuildTools`（VCTools + includeRecommended）只覆盖纯 C++；**WinUI 3 C++ 的 XAML 预编译管线还依赖 UWP C++ 组件**（提供 `VC\v170\Application Type\Windows Store\` 目标文件）。
2. 缺组件时的报错链：`MSB8036`（TargetPlatformVersion 未赋值）→ 清掉 ApplicationType 后变 `MSB8020`（v143 工具集找不到）→ 即使再绕过，XAML 编译目标（生成 `App.xaml.g.h` 与命名元素访问器）静默不执行，C++ 编译必然失败。**清 ApplicationType 是死路，必须装组件。**
3. 补装（一次 UAC，管理员 PowerShell）：

   ```powershell
   Start-Process 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe' -Verb RunAs -ArgumentList 'modify','--installPath','C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools','--quiet','--norestart','--add','Microsoft.VisualStudio.Workload.UniversalBuildTools','--add','Microsoft.VisualStudio.ComponentGroup.UWP.VC.BuildTools','--includeRecommended'
   ```

4. 陷阱备忘：这台机器上 Taskly 与 Shuttle 同为 rivet 应用、宿主进程同名 `RivetHost.exe`——按进程名杀进程会误伤，必须按窗口标题区分。
