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

## v0.1.2 标题栏重叠修复验证（2026-10-29，UIAutomation 坐标实测）

用户在真机发现设置齿轮与系统最小化/最大化/关闭按钮重叠（ExtendsContentIntoTitleBar 后系统按钮浮在右上角，齿轮钉在 CaptionBar 最右缘）。

修复后 UIA 实测（150% DPI）：

```
Minimize rect=(738,266)-(807,314)   Maximize (807,266)-(876,314)   Close (876,266)-(945,314)
Settings rect=(668,267)-(725,315)
```

齿轮右缘 725 < 最小化左缘 738（间距 = 设计的 8 DIP padding），垂直 1px 内对齐系统按钮带（32 DIP 顶部带 + Top 对齐）。两处根因：① RightInset 是物理像素，直接喂 DIP Thickness 在 >100% DPI 下二次缩放（首版 1.5 倍过宽）；② 齿轮原来垂直居中于 48 DIP 行，改为 Height=32 + Top 对齐。截图 `gui-v012.png`。

## v0.1.3 设置入口移出标题栏（2026-10-29）

用户反馈 v0.1.2「齿轮贴着系统按钮」整体设计别扭——非惯例模式。重构为紧凑工具惯例：标题栏只承载身份（Shuttle · 本机名），系统按钮独占右上角；设置齿轮移至底部状态行右端（左端 = 信箱健康点 + 文案）。0.1.1/0.1.2 的 RightInset/DPI 换算机制整体删除（不再需要）。

UIA 实测（150% DPI）：Settings 按钮 rect=(737,1065)-(795,1107)，位于窗口底部；系统按钮 (624..831,152..200) 在顶部——两者相距 900+ px，不可能重叠。截图 `gui-v013.png`（标题栏区域）；底部状态行因 PrintWindow 对 Mica+DirectComposition 的合成截断未入图，元素存在性以 UIA 为准。

## v0.1.5 两个用户真机 bug（2026-10-29）

1. **send-file RPC 结果类型错误**（用户拖文件报 "expected Transfer received Unsupported"）：RPC 体返回了内部 entry hasheq，与声明的 Transfer 记录不符，wire 层校验拒绝——而且发生在传输已入队之后（文件其实已发送，UI 却显示失败）。教训与 0.1.4 同源：**RPC 层（wire 校验）此前从未被测试触达**。修复 = RPC 体抽成 `begin-send!` 返回 `(entry->record entry)`；回归测试用 `record-ref` 断言记录类型（hasheq 会在 record-ref 处 raise）。`init!` 加 `#:force?` 让各自测可重指配置。
2. **每次发送弹黑色终端窗**：icacls 自愈是控制台子进程，从无控制台的 GUI 宿主派生时 Windows 为其新开可见终端（Win11 表现为 Windows Terminal 闪窗）。修复 = `conhost --hostless` 包裹（headless 控制台），不支持该 flag 的旧 conhost 探测一次后回退直调（闪烁但可用）。

## v0.1.6 启动崩溃修复（2026-10-29，真机复现→修复→真机验证闭环）

用户报告：跑过一次传输后，第二次启动必报 `hash-ref: no value found for key: 'stamp`。根因：账本行（file/ts/msg/ok）被 load-history! 原样灌进传输表，而表条目按 UI schema（filename/stamp/message）消费——list-transfers 排序读 'stamp 无默认即炸。0.1.x 全系列共有，首次传输后必触发。

验证闭环：① 往 %APPDATA%\Shuttle\history.jsonl 植入真实 schema 账本行 → 重启 v0.1.5 → UIA 读到 ErrorBar 同款报错（复现）；② 修复后 v0.1.6 真机重启 → ErrorBar 干净、传输列表两行历史完整渲染（`↓ b.bin 失败 9B · 10-29 18:01` / `↑ a.txt 已送达信箱 5B · 10-29 18:00`，方向/文件名/状态/时间戳映射全对）。

教训（三连击同族）：schema 有两套（账本 vs 条目），写读两端从没被同一个测试走过；「绕过组件的单测不算覆盖」之外再加一条——**用真实产物文件（这里的 history.jsonl）做测试数据**，而不是测试里现造的理想数据。

## v0.1.7 发送挂死 + 黑窗修复（2026-10-29）

用户真机：拖文件永远停在「排队中」，弹出标题为 RivetHost.exe 的黑色终端窗。根因：v0.1.5 引入的 `conhost --headless` 包装在 Win11 默认终端委托（Windows Terminal）下不生效——GUI 宿主派生 conhost 时终端被委托给 WT，黑窗照弹（标题显示宿主进程名），且发送线程在该路径上被挂死。隔离复现显示 probe 本身能退出但输出 VT 序列泄漏、cmd 语法错误——该技巧在委托环境下根本不可靠。

修复：**彻底移除子进程方案**，授权改为进程内 advapi32 FFI（GetNamedSecurityInfoW → SetEntriesInAclW → SetNamedSecurityInfoW，EXPLICIT_ACCESS 手工构造，TRUSTEE_IS_NAME 域账号）。无子进程 = 无窗口 = 无挂点。ffi/unsafe 经 `raco ctool --mods` 全程序编译打进 core.zo（FFI 内建于 CS 运行时，无需 stage 额外 collection）；FFI 不可用时回退直调 icacls（闪窗但可用，v0.1.4 实证不挂）。

验证：真实共享盘全链路自测通过——FFI 授权后 payload 继承 (I)(F) 断言过、远端清理成功。教训：**GUI 宿主调用控制台子进程的「隐藏」技巧（conhost --headless、-WindowStyle）在 Win11 默认终端委托时代都不可靠；进程内 API（FFI）才是正解**。UI 自动化点击 WinUI 3 的 Tapped（mouse_event 合成单击/双击）不可靠，按钮类交互验收应给控件加 AutomationProperties 或用 InvokePattern。

## v0.1.8 发送挂死终局修复（2026-10-29）

用户真机：v0.1.7 黑窗消失但仍卡「排队中」。逐步计时复现 + 残留信箱文件夹（空壳：正好死在授权步）+ 新增的分步诊断日志（send-debug.log）三线合围，定性 v0.1.7 FFI 方案的致命缺陷：**Racket CS 的 FFI 调用在「原线程」执行，而 WinUI 宿主的消息循环占据原线程不让路——后台线程的任何 FFI 调用永久等待**。REPL（原线程空闲）5ms 成功、GUI/selftest（后台线程）必挂，完全吻合。且 kill-thread 无法强杀卡在 FFI 里的线程，超时保护自身也挂。

修复：① 授权改 **wscript 隐藏执行 icacls**（wscript 是 GUI 子系统宿主，从 GUI 进程 spawn 不分配控制台；生成 .js 以 window style 0 隐藏运行并等待退出码）——纯子进程，不经原线程；② 所有外部调用加 **5-10 秒硬超时**（call-with-hard-timeout：worker 线程 + sync/timeout）；③ **新增 send-debug.log 分步诊断日志**（每步一行，用户机器上的挂死会留下精确停止点——本轮靠它一步定位 apply 参数错误）；④ **UI 轮询**：后台线程的事件到不了 UI（事件发射器在 RPC 线程），列表改为 1.5s 轮询权威表全量调和——「排队中」不动、历史清空不刷新、收到文件不显示三个症状一并根治。

验证：真实共享盘全链路自测通过（诊断日志记录 grant ok → copy → send complete 全链，(I)(F) 继承断言过、清理成功、零残留）；CI + Release 全绿。教训：**跨进程/运行时边界的能力（FFI/子进程/事件）必须先在目标线程环境实证，REPL 通过不等于宿主通过；给用户交付前先交付可观测性**。
