# AstraNAS v1.0.7

AstraNAS 是面向 Nintendo Switch 的 NAS / 远程文件管理 Homebrew，可直接浏览 SMB / WebDAV、下载文件、安装受设备环境授权的内容、管理本机 SD/USB 文件，并提供独立的 `AstraNAS-NetDiag` 网络诊断程序。

主程序要求通过 hbmenu **完整应用模式**运行；LibraryApplet/Applet 模式会提示后退出。当前兼容基线为 **Atmosphère 1.9.5 + HOS 20.5.0**。

## v1.0.7 分类回归门禁修正

- 修正 v1.0.6 host regression 仍在旧 NSP/XCI 实现文件中查找 `not_applicable_compressed` 的问题；该状态现由共享 `source_entry_audit.hpp` 统一维护。
- 正式功能代码不变：CNMT Content-ID mismatch 的结构化分类、友好错误与安全门槛沿用 v1.0.6。
- 保留 source-entry audit 单元测试和完整 Switch 构建，确保共享审计模型与设备端实现同时通过。

## v1.0.6 CNMT 源包不一致友好分类

- 将 CNMT source audit 从纯字符串升级为结构化结果；只有 `content_id_match=no` 且目标/文件表 bounds 正常、无 entry overlap 时，才判定为“安装包 CNMT 内容与 Content ID 不一致”。
- 命中后错误会直接给出文件名、期望 Content ID、实际完整 entry SHA-256，并提示重新获取、重新复制或重新打包安装包，不再让用户从底层 `Invalid NCA header` 自行推断。
- 如果 bounds/overlap 审计异常、压缩 CNMT 不适用 Content-ID 直接比较或审计失败，则仍保留原始 header/source audit 路径，避免把 parser/布局问题错误归类为坏包。
- 新增 host 分类回归，验证只有证据链完整的 mismatch 才触发坏包结论；NCA 校验和安装成功条件保持不变。

## v1.0.5 长诊断链无截断与审计结论前置

- 修复 `THROW_FORMAT` 固定 640 字节 payload 导致嵌套安装异常被静默截断的问题；格式化异常改为按实际长度动态分配，完整保留 header probe 与 source audit。
- CNMT 源审计把 `content_id_match`、target/table bounds、overlap 和完整 entry SHA-256 前置，确保最关键结论优先进入日志。
- 新增 host 回归测试，构造超过 1 KiB 的异常 payload 并验证尾部 sentinel 仍存在，防止未来再次退化为固定缓冲截断。
- 安装策略与 NCA 安全校验保持不变，本版只修复诊断可观测性。

## v1.0.4 CNMT 源包完整性审计

- 不放宽任何 NCA 安装校验；当 CNMT header 已确认无效时，才额外读取该 metadata entry 做源包审计。
- 对未压缩 `.cnmt.nca` 计算整个 entry 的 SHA-256，并用前 16 字节直接对比文件名中的 NCA Content ID，日志输出 `entry_sha256` 与 `content_id_match=yes/no`。
- 同时输出 PFS0/HFS0 的 `data_base`、entry relative/absolute offset、entry end、package size，并验证目标 entry 是否越界、整个文件表是否越界或重叠。
- 对 `.cnmt.ncz` 仍记录压缩 entry 的 SHA-256 指纹，但明确标记 `content_id_match=not_applicable_compressed`，避免将压缩文件哈希错误地与解压后 NCA Content ID 比较。
- 审计只在 header 失败路径触发，因此正常安装不会增加整 entry 的额外读取。

## v1.0.3 小型 CNMT / NCA Header 兼容修复

- NCA header 现在先检查原始 `NCA3`，否则再走 AES-XTS 解密；可区分 plaintext 与 encrypted header，避免把已明文化的 header 再解密成随机数据。
- `NcaWriter` 不再把 `0x4000` 当作所有 NCA 的固定最小前缀：先收够真实 crypto header `0xC00` 解析声明大小，小于 `0x4000` 的合法 CNMT（例如 `0xE00`）按真实大小写入；NCZ 仍保留规范的 `0x4000` 前缀。
- plaintext header 在写入 NCM 前会重新加密；CNMT 强制 Content-ID 哈希继续作为最终完整性校验。
- header 失败日志新增 `raw_magic`、`decrypted_magic`、`header_mode` 与 `key_self_test`；header-key 自检会从设备已注册 NCA 读取真实 header 验证当前 SPL 派生 key。

## v1.0.2 CNMT 复用与源读取诊断修复

- 修正 v1.0.1 的 CNMT 自愈顺序：已有 CNMT 现在先在 ContentStorage 中校验大小并直接挂载读取；健康内容完全不读取 NAS 包，只有异常内容才进入 verified replacement 修复。
- 包内 NCA/CNMT header 改用统一探针；头异常时会在同一绝对偏移重读一次，并记录文件名、PFS0/HFS0 绝对偏移、entry 大小、解密 magic、声明大小、两次读取是否一致及两份原始头 SHA-256。
- NCA header key 派生现在检查 SPL Crypto 的全部 Result，不再把密钥派生失败误报成 `Invalid NCA header`。
- NSP/XCI 的随机头/票据读取纳入安装性能统计，早期失败日志不再错误显示“源读取=0”。

## v1.0.1 安装完整性修复

- 修复 SD/NAND 中已有同 Content ID、同大小但已损坏的 CNMT 被直接复用，导致安装在 `OpenFileSystemWithId` 阶段立即失败的问题。
- NSP/NSZ 与 XCI/XCZ 在准备阶段会先复用可正常挂载的 CNMT；已有 CNMT 无法读取或大小异常时，会从当前安装包使用 replacement placeholder 自动修复后再重试。
- CNMT 新写入和自动修复始终强制执行 Content-ID/SHA-256 校验；大体积内容 NCA 仍按 `verify_nca_content_hash` 配置决定是否做完整哈希，避免默认安装性能回退。
- 强制替换路径现在允许修复“已注册但大小错误”的旧 NCA，不再在 replacement 写入前被旧 size 检查阻断。

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

两个正式 GitHub Actions workflow 默认都使用 `workflow_dispatch` 手动触发。为完成本次已授权的 v1.0.7 主分支错误分类交付，GitHub-hosted workflow 保留一个只匹配“v1.0.6 提交 → v1.0.7”这一次迁移的 push 门禁；该提交完成后，后续 main push 不会自动运行。self-hosted workflow 始终仅手动触发。

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
