# AstraNAS 更新记录

## v1.0.8

- 本机下载恢复 NAS 原文件名，不再默认追加 16 位对象哈希；远端 object key 继续仅保存在 `.astranas-meta`。
- 文件名清洗新增完整 UTF-8 校验；合法中文保持原样，非法序列替换为 `_`，超长名称按 UTF-8 字符边界裁剪并保留扩展名，避免中文被截成非法路径后在本机列表中消失。
- 本机已有同名但无法由 sidecar 证明为同一远端对象时不再静默删除覆盖，而是明确提示用户先处理冲突文件。
- 新增下载文件名 host regression，并同步 1.0.8 版本与一次性 main 构建门禁。

## v1.0.7

- 修正 v1.0.6 host regression 的旧路径假设：`not_applicable_compressed` 已迁移到共享 `source_entry_audit.hpp`，测试门禁同步指向新的唯一实现位置。
- 不改动 CNMT 源包不一致分类逻辑、安装安全校验或用户错误文本；重新执行完整 host regression、Switch build 和 Artifact 验收。
- 版本同步为 1.0.7，并为本次 main 修复提交设置一次性 GitHub-hosted 构建门禁。

## v1.0.6

- CNMT source audit 改为结构化结果，明确区分 Content-ID match/mismatch/not-applicable/unavailable 与 bounds/overlap 状态。
- 只有未压缩 CNMT 完整 entry 的 Content ID 不匹配、目标和整个容器表均未越界且不存在 entry overlap 时，才输出“安装包 CNMT 内容与 Content ID 不一致”的友好错误。
- 友好错误直接携带文件名、期望 Content ID、实际 SHA-256 和完整 source audit，并提示重新获取、复制或打包安装包。
- 其他 header、布局、压缩或审计异常继续保留原始诊断，不误分类；安装校验和成功条件不变。
- 新增 source-entry audit host regression，并同步 1.0.6 版本与一次性 main 构建门禁。

## v1.0.5

- 修复 `THROW_FORMAT` 640 字节固定 payload 在多层异常包装时静默截断 source audit 尾部的问题，改为动态长度格式化。
- CNMT 审计日志将 Content-ID 比较、bounds/overlap 和完整 entry SHA-256 移到最前，避免关键判定被后续偏移详情挤出。
- 新增长异常 host regression，验证超过 1 KiB 的格式化错误仍完整保留尾部 sentinel。
- 不改变安装成功条件、NCA header 校验或 Content-ID 校验；本版只提升诊断完整性。
- 版本同步为 1.0.5，并为本次授权 main 提交设置一次性 GitHub-hosted 构建门禁。

## v1.0.4

- CNMT header 无效时增加完整源 entry 审计，不改变正常安装与安全校验策略。
- 未压缩 CNMT 会计算整个 entry SHA-256，并将 digest 前 16 字节与 NCA Content ID 比较，直接输出 `content_id_match=yes/no`。
- NSP/PFS0 与 XCI/HFS0 同步输出 data base、relative/absolute offset、entry end、package size、目标越界、文件表越界与重叠状态。
- 压缩 CNMT 仅记录压缩 entry 指纹并标记 Content-ID 比较不适用，避免错误结论。
- 审计读取继续进入安装性能统计，便于区分 header probe 与完整 CNMT 验证成本。
- 版本同步为 1.0.4，并为本次授权 main 提交设置一次性 GitHub-hosted 构建门禁。

## v1.0.3

- NCA header 探针支持 plaintext/encrypted 自动识别，并在错误日志中同时记录 raw/decrypted magic、header mode、同偏移重读结果与 header-key 自检状态。
- 新增基于设备已注册 NCA 的 SPL header-key 自检；明文 header 需要重新加密时，自检明确失败会阻止写入。
- 重构 NcaWriter 前缀状态机：先在 0xC00 解析声明大小，再把前缀目标设为 min(0x4000, nca_size)，从而支持 0xE00 等合法小型 CNMT，同时保持 NCZ 的 0x4000 前缀语义。
- plaintext header 写入 NCM 前恢复为标准加密 header；CNMT Content-ID/SHA-256 强校验继续覆盖最终写入路径。
- 版本同步为 1.0.3，并为本次授权 main 提交设置一次性 GitHub-hosted 构建门禁。

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