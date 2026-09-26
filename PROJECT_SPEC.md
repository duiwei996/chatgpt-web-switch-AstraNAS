# AstraNAS 工程规格与仓库策略

本文档记录当前工程级约束；用户功能、操作方式与最新版本变化以 `README.md` 和 `CHANGELOG.md` 为准。公开提交历史从 `v1.0.0` 初始根提交开始，本文只描述当前工程，不依赖历史版本快照作为当前行为规范。

## 项目定位

- 产品：AstraNAS
- 平台：Nintendo Switch Homebrew
- 主程序：`switch/AstraNAS/AstraNAS.nro`
- 网络诊断：`switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro`
- 当前发布线：v1.0.x
- 兼容基线：Atmosphère 1.9.5 + HOS 20.5.0
- 许可证：GPL-3.0-or-later

AstraNAS 以 NAS / 本机文件管理为主，支持 SMB2/3、WebDAV(S)、SD/USB 文件浏览、网络诊断、下载与设备环境授权范围内的安装流程。项目不提供 Title Key、sigpatch、授权绕过或受保护内容。

## 权威文档

- `README.md`：当前用户功能、配置、操作和构建入口
- `BUILDING.md`：开发环境与构建方法
- `CHANGELOG.md`：版本变化
- `THIRD_PARTY_NOTES.md`：第三方来源、固定 commit 与许可证
- `SECURITY.md`：安全问题报告和敏感信息处理
- `CONTRIBUTING.md`：公开仓库贡献约定

当历史设计描述与上述当前文档冲突时，以当前源码、测试和这些权威文档为准。

## 源码与依赖策略

默认分支直接保存完整 AstraNAS 项目源码。CI 编译 checkout 本身；禁止把主源码替换为 base64 分块、delta 包或构建时重建源码的方案。

`libs/libsmb2` 使用显式 Git submodule 并固定 commit。其他第三方代码、参考实现和资源必须保留来源、许可证和必要版权声明，并在 `THIRD_PARTY_NOTES.md` 中登记。

固定外部资源必须校验 commit、Git blob 或 SHA-256；不能用同名但未验证的下载替代。

## 安全与隐私边界

- 真实 `config.ini`、`.env`、私钥和日志不进入仓库。
- 示例配置只能使用占位账号/密码和文档网段地址。
- 日志上传必须由用户明确触发；公开 issue/PR 中的日志必须先脱敏。
- 文件删除/移动必须受当前存储或远程根边界约束，并拒绝根目录、父目录逃逸和 symlink 越界。
- 安装器不提取 Title Key，不分发或安装 sigpatch，不绕过平台授权。

## GitHub Actions 策略

正式构建 workflow 默认仅手动触发，避免普通 push/PR 自动消耗 runner。

GitHub-hosted workflow 使用只读仓库权限并固定关键 Actions commit。self-hosted workflow 只允许仓库所有者在受信任上下文中手动执行；公开 PR 不得自动调度 self-hosted runner。

临时验证 workflow 应放在独立分支，完成后移除，不作为长期 `main` 内容。

## 版本规则

版本号使用 `MAJOR.MINOR.PATCH`。

以下改动必须单步提升应用版本，并同步 CMake、运行时常量、README、`config.example.ini` 和正式构建 workflow：

- 运行时行为或 UI 行为变化；
- 兼容性修复或新增功能；
- 安装/网络/存储逻辑变化；
- 会改变正式 NRO 内容或发布语义的构建变化。

以下不改变发布二进制的仓库治理改动可以保持当前应用版本：

- 文档、贡献指南、安全策略；
- `.gitignore` 等本地敏感文件防护；
- 不改变构建产物的 CI 权限/触发安全加固；
- Issue/PR 模板等协作元数据。

## 交付门禁

发布或影响 NRO 的改动至少应满足：

- 版本一致性检查通过；
- 适用的 host regression tests 通过；
- devkitA64 真实 Switch 构建成功；
- `AstraNAS.nro` 与 `AstraNAS-NetDiag.nro` 均存在且非空；
- NACP 语言和 hbmenu 布局检查通过；
- 最终产物记录 SHA-256。

纯仓库治理改动应验证最终 diff、权限/触发条件以及没有误改业务源码。
