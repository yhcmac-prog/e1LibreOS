# e1LibreOS

**简体中文** | [English](README.md)

e1LibreOS 2.0 (Libre) —— 参照 Fedora 理念构建的最小可启动 Linux 发行版。
内核与用户态全部来自自由软件，系统镜像可在 macOS / Linux 上纯脚本零 root 交叉构建。

## 特性一览

- **小而完整**：Alpine `linux-virt` 6.12 内核 + busybox 静态用户态，Live 环境常驻内存（initramfs）
- **五个构建分支**（`E1OS_VARIANT`），当前全部可直接构建
  - `minimal`：纯命令行，最小体积（busybox + musl）
  - `desktop`：自研 framebuffer 窗口管理器 **e1wm**（无 X11/Wayland，直写 `/dev/fb0`）
  - `mobile`：800×600 触屏适配 + 虚拟键盘，复用 e1wm
  - `server`：glibc + systemd 基础系统，无图形（Arch Linux bootstrap 根文件系统，systemd 作 PID 1），内置 **e1pkg** 与统一的 `/etc/e1pkg.conf`（公网 IPv6 源优先）
  - `workstation`：glibc + systemd，内置 **e1wm** 图形界面，由 `e1wm.service` 在 tty1 启动
- **双架构**：x86_64（isohybrid BIOS + UEFI El Torito 双引导 ISO）、aarch64（UEFI 原始 `.img`，GPT + FAT32 ESP）
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
| `e1LibreOS-2.0-minimal-x86_64.iso` | x86_64 | BIOS + UEFI | 命令行 |
| `e1LibreOS-2.0-desktop-x86_64.iso` | x86_64 | BIOS + UEFI | e1wm 图形桌面 |
| `e1LibreOS-2.0-mobile-x86_64.iso` | x86_64 | BIOS + UEFI | 触屏版 |
| `e1LibreOS-2.0-server-x86_64.iso` | x86_64 | BIOS + UEFI | glibc + systemd + e1pkg，无图形 |
| `e1LibreOS-2.0-workstation-x86_64.iso` | x86_64 | BIOS + UEFI | glibc + systemd + e1wm |
| `e1LibreOS-2.0-minimal-aarch64.img` | aarch64 | UEFI | 命令行 |
| `e1LibreOS-2.0-desktop-aarch64.img` | aarch64 | UEFI | e1wm 图形桌面 |
| `e1LibreOS-2.0-mobile-aarch64.img` | aarch64 | UEFI | 触屏版 |

同时附带工具包与源码包：

| 文件 | 内容 |
|---|---|
| `e1wine-2.0-macos.zip` / `.dmg` / `.pkg` | macOS 版 e1wine + e1wxfly 安装包 |
| `e1wine-2.0-linux.tar.gz` | Linux x86_64 / aarch64 静态二进制 + `install.sh` |
| `e1wine-2.0-unix-installer.sh` | POSIX 自解压安装器 |
| `e1LibreOS-2.0-source.tar.gz` / `.zip` | 完整源码树（不含构建产物） |

两条 glibc + systemd 分支目前仅 x86_64：根文件系统是 Arch Linux bootstrap 整树
直接打进 initramfs，systemd 作为 PID 1 运行（`/init` 先挂载 proc/sys/dev/run，
再 exec `/usr/lib/systemd/systemd`）。两者都在串口控制台（ttyS0）自动以 root 登录；`server` 的 tty1 同样自动登录，
`workstation` 则把 tty1 交给 e1wm（tty1 的 getty 被 mask，避免与图形界面抢占）。

### 虚拟机（VirtualBox）

```sh
./run-vbox.sh            # 创建并启动 x86_64 BIOS 虚拟机
```

或手动新建 VM：Other Linux 6.x、≥512MB 内存、≥4GB 磁盘、挂入 ISO。
Live 环境 root 自动登录（无密码）；安装后 root 默认密码见安装器问答。

### 真机 / U 盘

```sh
# x86_64：ISO 已做 isohybrid，可直接 dd
sudo dd if=build/e1LibreOS-2.0-desktop-x86_64.iso of=/dev/sdX bs=4M conv=fsync
# aarch64：原始磁盘镜像
sudo dd if=build/e1LibreOS-2.0-desktop-aarch64.img of=/dev/sdX bs=4M conv=fsync
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
./build.sh                                      # x86_64 minimal ISO
E1OS_VARIANT=desktop ./build.sh                 # x86_64 desktop ISO
E1OS_VARIANT=mobile ./build.sh                  # x86_64 mobile ISO
E1OS_VARIANT=server ./build.sh                  # x86_64 glibc + systemd server ISO
E1OS_VARIANT=workstation ./build.sh             # x86_64 glibc + systemd + e1wm ISO
E1OS_ARCH=aarch64 E1OS_VARIANT=desktop ./build.sh   # aarch64 .img
```

## 文档

- [docs/BUILDING.md](docs/BUILDING.md)：构建指南（macOS/Linux、交叉工具链、排错）
- [docs/INSTALL.md](docs/INSTALL.md)：五种安装模式与 `setup-e1os` 问答说明
- [docs/E1WINE.md](docs/E1WINE.md)：e1wine 跨平台 EXE 转换工具
- [docs/e1repo.md](docs/e1repo.md)：e1repo 依赖仓库结构与自建方法
- [docs/RELEASE-2.0.md](docs/RELEASE-2.0.md)：2.0 版本说明与发布清单
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
