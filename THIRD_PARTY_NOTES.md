# AstraNAS 第三方依赖与参考说明

## 直接依赖

### libsmb2

用途：SMB2 / SMB3 网络访问。

构建版本：CI 固定 commit。

### libcurl

用途：HTTP、HTTPS、WebDAV、FTP 等网络协议访问。

### libusbhsfs

- 上游：`DarkMatterCore/libusbhsfs`
- 固定构建 commit：`3b897ed6a79c910aa3f13693f17b80afb802b3b5`
- 构建类型：`BUILD_TYPE=GPL`
- 用途：挂载 USB Mass Storage 分区并通过标准文件 API 浏览和读取安装包。
- GPL 构建同时链接 devkitPro 的 `switch-ntfs-3g` 与 `switch-lwext4`，覆盖 FAT/exFAT、NTFS 和 EXT2/3/4。

### json-c

仅保留给旧 manifest / 主机回归代码；v1.0.0 文件管理器主执行文件不依赖远端游戏数据库。

## 工程参考

### AtmoXL-Titel-Installer

- 上游：`dezem/AtmoXL-Titel-Installer`
- 固定参考 commit：`8516930078991664243e07fc0d03b31e6e781eb2`
- 上游许可证：GPL-3.0
- 用途：参考成熟 Switch 安装器的文件选择、队列、进度、错误处理、包容器与安装后缓存生命周期设计。
- v1.2.7 直接复用上游 `romfs/audio/fertig.wav` 作为安装完成提示音；AstraNAS 使用 libnx `audout` 播放该原始 WAV，不修改音效内容。

AstraNAS 的本地 Title provider 包含固定 AtmoXL 安装核心，并保留上游许可证与版权声明。PFS0/HFS0 前置检查、安装设置、进度/取消适配和缓存生命周期为 AstraNAS 集成代码。

### NSZ

- 上游规范：`nicoboss/nsz`
- 上游许可证：MIT
- 用途：实现 NSZ/XCZ 外层兼容、NCZ section 解析，以及连续和 `NCZBLOCK` 分块 Zstandard 解压。
- 集成不包含密钥文件，也不改变 NCZ/NCA 原有技术保护信息。

### Awoo Installer

- 上游：`Huntereb/Awoo-Installer`
- 上游许可证：GPL-3.0
- 用途：核对 NSP/NSZ/XCI/XCZ 安装任务、内容进度、取消和错误传播方式。
- AstraNAS 不接入其 sigpatch 下载/安装功能。

### Goldleaf

- 上游：`XorTroll/Goldleaf`
- 上游许可证：GPL-3.0
- 用途：核对当前 libnx 下的 NSP 内容写入、应用记录合并、Ticket 查询/管理和安装后内容状态。
- AstraNAS 还参考其 libusbhsfs 驱动器枚举、任意目录浏览和目录内批量安装交互；没有接入 Title Key 展示、Ticket/已安装 Title 管理、NSP 导出或 Quark PC USB 浏览功能。

## 许可证说明

第三方组件继续保留各自许可证与版权声明。

AstraNAS 自有源码许可证：

```text
GPL-3.0-or-later
```

详细法律文本以项目内许可证文件为准。


## Dependency layout
`libs/libsmb2` is tracked as a Git submodule at commit `0b2aa4310c7a0510c666fe7da0eebce0f7e93824`.
