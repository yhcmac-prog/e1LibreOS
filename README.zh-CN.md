# e1LibreOS

**简体中文** | [English](README.md)

e1LibreOS 1.0 (Libre) —— 参照 Fedora 理念构建的最小可启动 Linux 发行版。
内核与用户态全部来自自由软件，系统镜像可在 macOS / Linux 上纯脚本零 root 交叉构建。

## 特性一览

- **小而完整**：Alpine `linux-virt` 6.12 内核 + busybox 静态用户态，Live 环境常驻内存（initramfs）
- **三种变体**
  - `server`：纯命令行，最小体积
  - `workstation`：自研 framebuffer 窗口管理器 **e1wm**（无 X11/Wayland，直写 `/dev/fb0`）
  - `mobile`：800×600 触屏适配 + 虚拟键盘，复用 e1wm
- **双架构**：x86_64（BIOS + UEFI 双引导 ISO）、aarch64（UEFI 原始 `.img`）
- **自研生态**
  - [e1pkg](rootfs/usr/bin/e1pkg)：dnf 风格包管理器（后序遍历依赖解析 + 反向依赖保护）
  - [e1wine](src/e1wine/e1wine.c)：轻量 Win32 PE 兼容层（运行/分析 PE 文件）
  - [e1wxfly](src/e1wxfly/e1wxfly)：应用包装工具（EXE→ELF/.app、X11/Wayland 应用封装）
  - [e1gpt](src/e1gpt)：零依赖 GPT 分区工具
  - [e1apk](src/e1apk)：APK 打包工具
- **21 个内置图形应用**：访达 Finder、系统设置、终端、e1 浏览器（HTTP/HTTPS）、计算器、日历、
  时钟、备忘录、活动监视器、磁盘工具、截屏、安装器、X11 应用支持等
- **安装器 [setup-e1os](rootfs/usr/sbin/setup-e1os)**：问答流程与 Alpine `setup-alpine` 逐项对齐，
  支持五种安装模式（见 [docs/INSTALL.md](docs/INSTALL.md)）

## 快速开始

### 下载预构建镜像

到 [GitHub Releases](../../releases) 下载：

| 文件 | 架构 | 固件 | 说明 |
|---|---|---|---|
| `e1LibreOS-1.0-server-x86_64.iso` | x86_64 | BIOS+UEFI | 命令行 |
| `e1LibreOS-1.0-workstation-x86_64.iso` | x86_64 | BIOS+UEFI | 图形桌面 |
| `e1LibreOS-1.0-mobile-x86_64.iso` | x86_64 | BIOS+UEFI | 触屏版 |
| `e1LibreOS-1.0-{server,workstation,mobile}-aarch64.img` | aarch64 | UEFI | `dd` 到磁盘/SD 卡 |

### 虚拟机（VirtualBox）

```sh
./run-vbox.sh            # 创建并启动 x86_64 BIOS 虚拟机
```

或手动新建 VM：Other Linux 6.x、≥512MB 内存、≥4GB 磁盘、挂入 ISO。
Live 环境 root 自动登录（无密码）；安装后 root 默认密码见安装器问答。

### 真机 / U 盘

```sh
# x86_64：ISO 已做 isohybrid，可直接 dd
sudo dd if=build/e1LibreOS-1.0-workstation-x86_64.iso of=/dev/sdX bs=4M conv=fsync
# aarch64：原始磁盘镜像
sudo dd if=build/e1LibreOS-1.0-workstation-aarch64.img of=/dev/sdX bs=4M conv=fsync
```

### 安装到硬盘

Live 登录后运行：

```sh
setup-e1os               # 交互式（问答同 setup-alpine）
setup-e1os --auto /dev/sda   # 非交互，环境变量预置答案
```

## 从源码构建

需要 macOS 或 Linux + `python3 / curl / gzip / tar`（x86_64 交叉编译自带 zig 工具链，
首次构建自动下载）。详见 [docs/BUILDING.md](docs/BUILDING.md)。

```sh
./build.sh                                      # x86_64 server ISO
E1OS_VARIANT=workstation ./build.sh             # x86_64 workstation ISO
E1OS_VARIANT=mobile ./build.sh                  # x86_64 mobile ISO
E1OS_ARCH=aarch64 E1OS_VARIANT=workstation ./build.sh   # aarch64 .img
```

## 文档

- [docs/BUILDING.md](docs/BUILDING.md)：构建指南（macOS/Linux、交叉工具链、排错）
- [docs/INSTALL.md](docs/INSTALL.md)：五种安装模式与 `setup-e1os` 问答说明
- [docs/E1WINE.md](docs/E1WINE.md)：e1wine 跨平台 EXE 转换工具
- [docs/e1repo.md](docs/e1repo.md)：e1repo 依赖仓库结构与自建方法
- [CONTRIBUTING.md](CONTRIBUTING.md)：贡献指南（构建、编码规范、提交流程）

## 源码结构

```
build.sh                 一键构建脚本（变体/架构矩阵）
rootfs/                  initramfs 根文件系统骨架（init、etc、自研二进制）
packages/src/            e1pkg 样例包源（hello / cowsay / e1fetch）
src/e1wine/              e1wine PE 兼容层（C）
src/e1wxfly/             e1wxfly 应用包装工具（Python）
src/e1gpt/               GPT 分区工具（C）
src/e1apk/               APK 打包工具（C）
src/fbui/                e1wm framebuffer 窗口管理器 + 21 应用（C）
tools/                   mkinitramfs / mkfat32 / isohybrid / 构建脚本
sign_github.py           GitHub cookie 登录辅助脚本
```

## 自由软件

e1LibreOS 仅聚合自由软件；各上游组件遵循其原有许可证（GPL-2.0-only 等），
自研代码采用 MIT 许可证。
