# Contributing to AstraNAS

感谢你参与 AstraNAS。

## 开始之前

- 先阅读 `README.md`、`BUILDING.md`、`THIRD_PARTY_NOTES.md` 和 `SECURITY.md`。
- Issue / PR 不要包含真实 NAS 账号、密码、访问令牌、私有地址或未经脱敏的日志。
- 不提交 `config.ini`、`.env`、私钥、构建产物或本机日志。
- 不提交 Title Key、sigpatch、受保护内容或用于绕过平台授权的材料。

## 开发流程

1. 从最新 `main` 创建分支。
2. 保持改动聚焦，避免在修复任务里顺带做大范围重构。
3. 对明确回归增加最小可复现测试或 host regression test。
4. 运行适用的 host checks；Switch 构建改动还应使用 devkitA64 做真实 NRO 构建。
5. PR 中说明：问题、修改范围、验证方法、已知限制。

## 构建与测试

主机回归：

```bash
./scripts/run_host_tests.sh
```

Switch 构建：

```bash
export DEVKITPRO=/opt/devkitpro
./scripts/build_switch.sh
```

也可按 `BUILDING.md` 使用仓库固定的 DownloadBridge 工具链。

## 版本规则

会改变运行时行为、兼容性、构建输出或正式发布内容的改动，应按 Semantic Versioning 单步提升版本，并同步 CMake、运行时常量、README、示例配置和正式构建 workflow。

纯文档、贡献规范、安全策略、忽略规则等不改变发布二进制的仓库治理改动，可以不提升应用版本。

## Pull Request 原则

- 不强推覆盖 `main`。
- 不把临时 CI / 调试 workflow 长期留在 `main`，除非它是项目正式流程的一部分。
- GitHub Actions 依赖尽量固定到 commit SHA。
- self-hosted runner 只用于受信任的维护者手动任务；不要给公开 PR 添加 self-hosted 自动触发。

## 第三方代码

保留原作者版权与许可证声明。新增或更新第三方代码/资源时，在 `THIRD_PARTY_NOTES.md` 记录来源、版本/commit、许可证和用途。
