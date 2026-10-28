# Shuttle

[English](README.md) · 简体中文

[![release](https://img.shields.io/github/v/release/turinglambdaai/shuttle)](https://github.com/turinglambdaai/shuttle/releases/latest) ![platform](https://img.shields.io/badge/platform-Windows-lightgrey) ![built with](https://img.shields.io/badge/built%20with-Rivet-9333ea) [![CI](https://github.com/turinglambdaai/shuttle/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/shuttle/actions/workflows/ci.yml)

两台公司 Windows 电脑之间的文件「直传」小工具。文件从 A 机拖进去，B 机自动收到并弹通知——看起来是直接互传，实际走的是共享盘上的一个信箱目录。

解决的具体问题：加密电脑上的 EFS 加密 zip 直接发送，另一台无法解密；先解密、再经共享盘中转拷贝则是好的。Shuttle 把这个手工流程自动化了。

## 使用方式

1. **发送**：把文件拖进 Shuttle 窗口（或点拖放区选择文件）。文件被放入共享盘信箱目录，状态显示「已送达信箱」。
2. **接收**：另一台电脑的 Shuttle 每约 1.5 秒查看一次信箱，发现新文件就自动拉取到 `Downloads\Shuttle`，随后弹 Windows 通知，并清掉信箱里的残留。
3. 关闭窗口 = 缩到托盘，接收保持在线；托盘右键「退出」才是真正退出。设置里可勾选开机自启。

## 两台电脑各需一次的配置

1. 在第一台电脑上构建或取得 Shuttle 程序目录（见下文构建），整个目录拷到第二台电脑任意位置（`raco rivet package` 产物亦可）。
2. 两台电脑各自启动一次 Shuttle，打开右上角⚙设置：
   - **中转目录**：两台必须指向**同一路径**（默认 `Z:\19 汽车电子事业部\研发部\04_Software\LiDAR\.shuttle`，可换到任意有写入权的共享位置）；
   - **下载目录**：本机接收文件的落盘位置（默认 `Downloads\Shuttle`）。
3. 需要随 Windows 自启就在设置里勾选（写 HKCU Run 键，无需管理员）。

两台电脑用同一域账号即可，工具按「机器名」区分收发，互不干扰。

## 分发（GitHub Releases，参考 Taskly 家族约定）

发布完全由 CI 完成——**本机无需 MSVC 工具链**（CI runner 自带）：

1. 把版本号写进 `rivet.rktd`（同时重新 `raco rivet schema --output rivet-schema.json` 无需变动则跳过），CHANGELOG.md 加 `## [X.Y.Z]` 段，提交推送。
2. 打 annotated tag 并推送，CI 自动完成其余一切：

   ```bash
   git tag -a v0.1.0 -m "Shuttle 0.1.0" && git push origin v0.1.0
   ```

3. Release 产物（家族命名约定，与 Taskly 一致）：
   - `shuttle-X.Y.Z-windows-x64.msi` / `shuttle-X.Y.Z-windows-x64.zip`（便携版），各带 `.sha256` 边车，汇总 `SHA256SUMS`；
   - 无版本号稳定别名 `shuttle-windows-x64.msi` / `shuttle-windows-x64.zip`，直链固定为 `releases/latest/download/<别名>`；
   - SBOM 与第三方声明留在 CI artifact，不上 release（避免边车与正文脱节）。
4. 两台电脑各装一次 MSI（或解压便携版）即完成部署；更新时下载新 MSI 重装即可（未接应用内自动更新，属有意裁剪）。

工作流：`.github/workflows/ci.yml`（push 即跑 acceptance：schema 契约门 + WinUI 全量原生构建）、`.github/workflows/release.yml`（tag 触发）。rivet 以 pin 引用（`RIVET_PIN` 单一真源）。`scripts/check-release-version.sh` 是发布预检：tag 必须带注解且与 `rivet.rktd`、生成客户端内嵌版本三方一致。

## 从源码构建

前置：

- **Racket 9.x CS**（含 `raco`）
- **rivet 包**：`raco pkg install --auto rivet`
- **MSVC 工具链**（WinUI 3 宿主需要，一次性）：

  ```bat
  winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  ```

  （安装会弹一次 UAC 确认；装完重开终端，`raco rivet doctor` 应显示 toolchain complete。）

构建并运行：

```bash
raco rivet dev      # 构建 + 启动
raco rivet package  # 生成可分发包
```

## 测试

```bash
raco test app/backend.rkt                                   # 纯单元测试
racket -e '(require (submod "app/backend.rkt" test)) (test-main)'   # 真实共享盘全链路自测
```

全链路自测使用独立的 `.shuttle-selftest` 信箱目录（伪装对端机器名发送 → 本机接收校验 → 远端清理），不触碰真实邮件，结束后自动清理。

## 设计要点

- **信箱协议**：每次传输在信箱目录建一个子文件夹（`yyyyMMdd-HHmmss-<id>`），写入顺序为 `manifest.json`（先写，崩溃也可归属）→ `payload.part`（复制完改名 `payload`）→ `shuttle.done`（最后写）。接收端只认带 `.done` 的文件夹，复制后校验大小，确认落盘才删除远端——共享盘删除不进回收站，绝不误删。
- **权限自愈**：部门共享盘的公共目录对普通域账号常是「能建不能删」。Shuttle 每次建完自己的传输子文件夹，立即 `icacls <子文件夹> /grant "<域账号>:(OI)(CI)F"`（**不加 /t**，(OI)(CI) 继承由文件服务器自动传播整棵子树），创建者即所有者，整棵子树从此可自由管理。删除失败则重试并通知用户手动处理，不影响已收到的文件。
- **只清理自己的**：启动/每小时清扫仅处理清单里发送者是本机且超龄的文件夹（崩溃残留 1 天、已完成发送 7 天），绝不碰他人内容。
- **防重放**：已处理过的传输 id 记入 `consumed.jsonl`，不会重复下载。
- **目录健康**：共享盘断连时状态栏红点提示「中转目录不可达」，恢复后自动变绿。

技术栈：Racket 后端（全部业务逻辑）+ WinUI 3 原生宿主（展示与系统集成），经 [rivet](https://github.com/turinglambdaai/rivet) 的 typed RPC 通信。

## 目录结构

```
app/backend.rkt        Racket 引擎 + RVT1 契约（信箱、轮询、icacls、配置、历史）
windows/               WinUI 3 宿主（XAML + C++/WinRT：拖放、列表、托盘、设置）
shared/i18n/           界面文案单一源（zh/en）
assets/app.ico         应用图标
docs/acceptance/       验收记录与权限证据
```
