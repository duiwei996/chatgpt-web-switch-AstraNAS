# AstraNAS v1.0.0

AstraNAS 是面向 Nintendo Switch 的 NAS / 远程文件管理 Homebrew，可直接浏览 SMB / WebDAV、下载文件、安装受设备环境授权的内容、管理本机 SD/USB 文件，并提供独立的 `AstraNAS-NetDiag` 网络诊断程序。

主程序要求通过 hbmenu **完整应用模式**运行；LibraryApplet/Applet 模式会提示后退出。当前兼容基线为 **Atmosphère 1.9.5 + HOS 20.5.0**。

## v1.0.0 首个公开版本

这是 AstraNAS 的首个公开版本，提供下方列出的 NAS/本机文件管理、安装、批量队列与网络诊断功能。公开源代码、应用内版本、示例配置和构建元数据统一使用 `1.0.0`。

## 主要功能

- `NAS 文件`：浏览 SMB / WebDAV；下载、单个安装、X 批量勾选、批量顺序管理、测速、删除。
- `本机文件`：浏览 SD 与已挂载 USB；单个安装、上传到 NAS、X 批量勾选、批量顺序管理、移动、删除。
- `安装缓存`：仅在关闭网络直装时显示，用于缓存式安装；NRO 可按需要内部使用暂存路径。
- `已安装`：读取已安装内容，二次确认后卸载应用内容，不请求删除存档。
- `设置`：维护 SMB / WebDAV 独立连接资料、安装选项、网络诊断和手动安装日志上传。
- `AstraNAS-NetDiag`：纯 RAM 网络基线、链路信息、手动高级 SO_RCVBUF 扫描、手动测速日志上传。

支持 `.nro`、`.nsp/.nsz/.xci/.xcz`，以及 libnx concatenation file、`00/01/...`、`.ns0/.ns1`、`.xc0/.xc1`、`.nsp.00/.xci.00`、`*.part00` 等已实现的分卷形式。远程分卷不满足网络直装条件时应使用缓存模式。

## 常用操作

NAS 与本机文件页面统一：

```text
A       进入目录；文件打开操作菜单
B       返回上级；移动模式下取消移动
X       勾选 / 取消当前可安装文件
Y       打开当前页面操作
L / R   切换页面
+       正常退出
```

批量顺序页：

```text
↑ / ↓   选择项目
← / →   调整当前项目实际安装顺序
X       移除当前项目
Y       清空全部选择
A       按当前 #1 → #2 → #3… 顺序开始安装
B       返回继续选择
```

本机移动：`A -> 文件操作 -> 移动到其他目录`，浏览到目标目录后按 `Y -> 移动到当前目录`；按 B 可取消。

## 日志隐私策略

安装/测速完成后不会自动连接 NAS，也不会因为尚未配置上传目录而弹出目录选择器。

```text
安装日志：sdmc:/switch/AstraNAS/install-latest.log
测速日志：sdmc:/switch/AstraNAS/netdiag-latest.log
```

上传必须由用户手动触发：

```text
主程序：设置 -> 安装日志 -> 上传最近安装日志
NetDiag：网络配置 -> 上传最近测速日志
```

首次手动上传且没有已保存目录时，才会按需打开 NAS 目录选择器；取消或上传失败均保留本地日志。

## 配置

推荐直接在 Switch 的设置界面修改。配置 schema 仍为 3：

```ini
config_version=3
protocol=smb

smb_server=192.168.1.10
smb_port=0
smb_share=
smb_root=/
smb_username=your_smb_user
smb_password=your_smb_password
smb_remote_dir=

webdav_server=192.168.1.10
webdav_port=0
webdav_root=/
webdav_username=your_webdav_user
webdav_password=your_webdav_password
webdav_remote_dir=
webdav_profile_tls=true
webdav_profile_tls_verify=true

local_root=sdmc:/
local_dir=sdmc:/
cache_dir=sdmc:/switch/AstraNAS/cache
download_retries=2
network_direct_install=true
verify_sha256=false
verify_nca_content_hash=false
install_target=sd
delete_source_after_install=true
ignore_required_firmware=false
validate_nca=true
```

`protocol` 只决定当前启用的协议；SMB/WebDAV 资料分别保存。`smb_port=0` 默认 445；`webdav_port=0` 按 HTTP/HTTPS 使用 80/443。

`network_direct_install=true` 时远程安装优先走直装，且 UI 隐藏安装缓存页。WebDAV 直装要求可固定对象的强 ETag 与正确的 Range/Content-Range 行为；SMB 要求稳定文件身份与偏移读取。NRO 例外：为了安装 Homebrew 文件，内部仍可使用暂存流程。

## 普通下载高速路径

满足对象身份稳定条件时：

```text
SMB/WebDAV direct read
        ↓
连续流 / 有序流水线
        ↓
16 × 8 MiB RAM ring（128 MiB）
        ↓
4 MiB 文件写片段
        ↓
.astranas-part -> fsync -> rename
```

保留 `.astranas-part`、`.astranas-meta`、断点续传、远端 identity 校验、失败重试、可选 SHA-256 和最终原子发布。进度页同时显示网络纯读、本机写入、等待落盘与 RAM 占用。

网络直装继续使用深度更大的：

```text
SMB/WebDAV -> 128 × 8 MiB（1 GiB）read-ahead -> 安装核心 -> NCM
```

## 网络诊断

主程序设置页的网络诊断和独立 NetDiag 均只把测试数据读入 RAM，不写 SD、不执行安装。

NetDiag“一键基础诊断”默认执行四项基础测试。需要研究 socket receive buffer 时，在 `网络配置 -> 高级缓冲扫描` 手动运行 `256 KiB / 512 KiB / 1 MiB / 2 MiB / 4 MiB` 档位。Switch API 无法可靠提供 PHY Rate、MCS、20/40/80 MHz 信道带宽和重传率时，报告不会从吞吐反推这些值。

## 放入 SD 卡

推荐布局：

```text
sdmc:/switch/AstraNAS/AstraNAS.nro
sdmc:/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro
```

主程序配置在 `sdmc:/switch/AstraNAS/config.ini`，NetDiag 读取同一份连接配置。

## 构建

仓库内两个正式 GitHub Actions workflow 都是 **仅 `workflow_dispatch` 手动触发**，push/PR 不会自动消耗 runner。

本地 devkitPro：

```bash
export DEVKITPRO=/opt/devkitpro
./scripts/build_switch.sh
```

或使用固定 DownloadBridge 工具链：

```bash
./scripts/build_with_bridge_toolchain.sh
```

预期输出：

```text
dist/switch/AstraNAS/AstraNAS.nro
dist/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro
```

## 安全边界

本机删除/移动限制在当前存储根以内，拒绝删除根、父目录逃逸和 symlink 越界。NAS 删除限制在当前配置远程范围内，并拒绝远程根及 SMB 共享根。

网络直装不会自动删除 NAS 原文件。缓存/本机源只有安装成功后才按设置清理；失败或取消时保留。普通文件下载使用临时文件与最终 rename，避免半写入结果直接覆盖目标。

安装器不提取 Title Key、不分发或安装 sigpatch，也不绕过平台授权；受保护内容仍依赖设备本身具备有效授权和兼容环境。
