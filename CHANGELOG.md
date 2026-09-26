# AstraNAS 更新记录

## v1.0.2

- 修正 v1.0.1 CNMT 自愈顺序：已注册且大小正确、可挂载的 CNMT 直接复用，不再先读取包内 NCA header。
- 只有既有 CNMT 大小异常或挂载失败时才从当前包执行强哈希 replacement 修复；新 CNMT 仍强制 Content-ID/CNMT SHA-256。
- 新增统一 NCA header 探针：无效 header 会同偏移二次读取，并输出容器类型、文件名、绝对偏移、entry 大小、解密 magic/声明大小、重读一致性和 SHA-256 指纹。
- SPL Crypto header key 派生全部检查返回码，密钥服务失败会直接报告真实错误，不再伪装成 NCA header 损坏。
- NSP/XCI 随机读取纳入“源读取”性能统计，早期 header/CNMT 失败也能看到真实读耗时和吞吐。
- 版本同步为 1.0.2，并为本次授权 main 提交设置一次性 GitHub-hosted 构建门禁。

## v1.0.1

- 修复安装准备阶段盲信已有 CNMT 的问题：同 Content ID 的已注册 CNMT 如果大小异常、无法挂载或完整性损坏，会从当前包自动使用 replacement placeholder 修复并重试。
- CNMT 新写入与自动修复强制执行 Content-ID/SHA-256 校验，避免把损坏的控制元数据重新注册进 NCM。
- 修复强制替换仍会被“已有 NCA 大小不匹配”预检查阻断的问题。
- NSP/NSZ 与 XCI/XCZ 共用同一 CNMT 自愈路径；保留事务回滚、NAS 源文件与既有大体积 NCA 哈希性能设置。
- 正式构建版本同步为 1.0.1，并为本次已授权 main 交付保留一次性 GitHub-hosted push 构建门禁；正式 Artifact 恢复源码 ZIP、manifest 与完整 SHA256 清单。

## v1.0.0

首个公开版本，包含当前完整应用能力：

- 浏览 SMB / WebDAV 和 SD/USB 文件，支持下载、上传、移动、删除及安装管理。
- 支持单项与顺序批量安装、网络直装和缓存安装，并在安装前校验队列顺序及依赖信息。
- 提供 AstraNAS-NetDiag 独立网络诊断程序、测速、手动日志上传和本地优先日志策略。
- 应用版本、示例配置、NRO 元数据及 GitHub Actions 构建信息统一标记为 `1.0.0`。