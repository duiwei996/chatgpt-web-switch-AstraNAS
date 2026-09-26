# AstraNAS 构建与验收

本文档描述当前 `main` 正常源码树的构建方式。CI 直接编译当前 checkout，不会从压缩包、base64、overlay 或 delta 重建项目源码。

## 兼容基线

- Atmosphère: `1.9.5`
- HOS: `20.5.0`
- devkitPro image: `devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282`
- libsmb2 submodule: `0b2aa4310c7a0510c666fe7da0eebce0f7e93824`

## 获取源码

```bash
git clone --recurse-submodules <repository>
cd chatgpt-web-switch-AstraNAS
```

已有 checkout 如果缺少 submodule：

```bash
git submodule update --init --recursive
```

## Switch 依赖

AstraNAS 固定使用 devkitPro 的 Switch portlibs；正式 CI 从版本化 DownloadBridge 工具链包恢复这些依赖：

```text
switch-curl
switch-libjson-c
switch-ntfs-3g
switch-lwext4
```

Docker 兜底构建与 DownloadBridge 工具链生成流程会从固定的 `libs/libsmb2` submodule 构建/安装 Switch 版本：

```bash
make -C libs/libsmb2 -f Makefile.platform switch_install
```

USB HDD 使用固定版本 libusbhsfs GPL 构建：

```bash
git clone https://github.com/DarkMatterCore/libusbhsfs.git /tmp/astranas-libusbhsfs
git -C /tmp/astranas-libusbhsfs checkout 3b897ed6a79c910aa3f13693f17b80afb802b3b5
make -C /tmp/astranas-libusbhsfs BUILD_TYPE=GPL install
```

`scripts/build_in_devkitpro_docker.sh` 和 DownloadBridge 工具链生成流程会在临时目录获取并核对这个 commit，不使用浮动分支内容。主 CI 则直接使用已经包含该固定版本的工具链 Release。

不要把 libsmb2 改成每次 CI 动态 clone 的浮动依赖；父仓库 gitlink 就是当前可复现版本。

## 可复用 DownloadBridge 工具链

默认可复用工具链保存在公开 DownloadBridge 的版本化 GitHub Release：

```text
repo: duiwei996/chatgpt-web-github-actions-DownloadBridge
tag:  astranas-devkitpro-v1
asset: devkitpro-switch-astranas.tar.gz
sha256: af4ec72ba7c20b7aedc79f5d63bffa842e2665e740a878cc78c13d34051791b9
```

本机或临时 Linux 环境可以直接运行：

```bash
./scripts/build_with_bridge_toolchain.sh
```

脚本会优先复用已存在的 `/opt/devkitpro`；否则复用 `$HOME/.cache/astranas/` 中已下载的压缩包，缓存缺失时才从固定 Release 下载，并在解压前强制校验 SHA-256。

GitHub Actions 还会用 `actions/cache` 保存同一压缩包，因此同一工具链 revision 的后续构建通常不需要再次下载。工具链、libsmb2 或 libusbhsfs 版本变化时，应在 DownloadBridge 中发布新的 revision/tag，而不是覆盖旧 revision。

## 构建

```bash
./scripts/build_switch.sh
```

成功后应生成：

```text
dist/switch/AstraNAS/AstraNAS.nro
dist/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro
```

`AstraNAS-NetDiag.nro` 是独立的纯网络/RAM 诊断工具，不链接安装器/NCM。

最小 NRO 结构门禁：产物非空，并在文件头区域包含 `NRO0`。

## Host 回归检查

仓库提供：

```bash
./scripts/run_host_tests.sh
```

它用于检查路径边界、本地文件操作、复制/替换和相关 host 回归。Host 测试不能代替 devkitA64 的真实 Switch 编译。

## GitHub Actions

正式 workflow：

```text
.github/workflows/build-nro.yml
```

流程为：

```text
checkout 当前源码 + submodule
-> 检查正常源码布局
-> 恢复 actions/cache 中的工具链压缩包；未命中时从 DownloadBridge Release 下载并校验
-> 解压固定 devkitPro/portlibs 环境
-> 编译 AstraNAS.nro + AstraNAS-NetDiag.nro
-> 打包对应源码
-> 生成 SHA256SUMS.txt / BUILD-MANIFEST.txt
-> 校验 NRO0 / ZIP / SHA-256
-> 上传 Artifact
```

最终发布 Artifact 应包含：

```text
AstraNAS.nro
AstraNAS-NetDiag.nro
AstraNAS-source.zip
config.example.ini
BUILD-MANIFEST.txt
SHA256SUMS.txt
```

## 源码包要求

对应源码 ZIP 必须包含真实可维护源码和固定依赖源码，且不得出现下列历史重建结构：

```text
build-kit/
overlay/
v030-delta/
v030-user-backend/
*.b64
```

同时不应把 `build/`、`dist/`、`release/`、对象文件或旧 NRO 当成源码提交。

## 验收边界

`BUILD PASSED` 只表示目标 commit 在真实 devkitA64/libnx 构建链中成功生成本次要求的两个 NRO。

`ARTIFACT VERIFIED` 还要求下载 GitHub Actions Artifact 后独立复算 SHA-256，并确认 NRO、源码 ZIP 和 manifest 对应同一个 commit。

`DEVICE TESTED` 只有在实体 Nintendo Switch 上完成实际测试后才能标记为 yes。
