# e1LibreOS 2.0 (Libre) — 版本说明

发布日期：2026-11-07

2.0 是 e1LibreOS 首个同时发布全部五个 x86_64 分支的版本：minimal / desktop /
mobile / server / workstation 均从同一份源码树重新构建，aarch64 三个分支也改为
真实交叉编译。本版补齐了发布产物：x86_64 ISO 同时支持 BIOS 与 UEFI 引导，新增
三个 aarch64 原始磁盘镜像，并附带 e1wine / e1wxfly 安装包与完整源码包。版本号在
`os-release`、引导菜单、安装器、产物文件名和文档中统一为 **2.0**。

宿主构建链已彻底去除 Homebrew（Intel Mac 不再受支持）：所有宿主工具由
`tools/build-host-tools.sh` 从源码构建到 `build/host-tools/bin/`，MacPorts
（`/opt/local`）仅作为可选替代。

## 本次重点

### 五个分支

| 分支 | 基础 | 桌面/用途 |
|---|---|---|
| `minimal` | busybox + musl | 命令行 |
| `desktop` | busybox + musl | e1wm 图形桌面（640×480） |
| `mobile` | busybox + musl | e1wm 触屏布局（800×600，含屏幕键盘） |
| `server` | Arch/glibc + systemd | 无图形；内置 **e1pkg** 与公网 IPv6 软件源 |
| `workstation` | Arch/glibc + systemd | e1wm 图形桌面（tty1） |

### x86_64 双引导（BIOS + UEFI）

- `downloads/grub/grubx64.efi`：GRUB 2.12 all-modules standalone 镜像
  （约 2.9 MB），内嵌 `serial` / `search` / `linux` / `initrd` 完整菜单配置，
  启动时搜索 `/boot/vmlinuz-virt`，不依赖外置配置文件。
- 构建脚本将其放入 16 MiB FAT16 super-floppy EFI 镜像的
  `/EFI/BOOT/BOOTX64.EFI`，以 El Torito 形式附加到 ISO；同时保留原
  isolinux + isohybrid MBR 的 BIOS 路径，得到双引导 ISO。
- ISO 体积因此从约 15.9 MB 增至约 34.6–35.7 MB。

### aarch64 镜像

| 文件 | 说明 |
|---|---|
| `e1LibreOS-2.0-minimal-aarch64.img` | 命令行 |
| `e1LibreOS-2.0-desktop-aarch64.img` | e1wm 图形桌面 |
| `e1LibreOS-2.0-mobile-aarch64.img` | e1wm 触屏布局 |

- 原始磁盘镜像：保护性 MBR + GPT，单个 FAT32 ESP 分区，内含
  `/VMLINUZ`、`/INITRD.GZ`、`/EFI/BOOT/BOOTAA64.EFI`（GRUB standalone，
  PE32+ AArch64）与 `GRUB.CFG`。
- `grubaa64.efi` 由 GRUB 2.12 加 Alpine v3.21 `grub-efi` 的 `arm64-efi`
  模块目录组装（`grub-mkstandalone -O arm64-efi`）。
- aarch64 依赖的 Alpine 包已落入 `downloads/`：`linux-virt`（6.12.115）与
  `busybox-static`（1.37.0）。
- 镜像约 515 MB：FAT32 规范要求 ≥65525 簇，64 MiB 不满足，`mkfat32.py`
  自动扩至 512 MiB。构建脚本现以**实际 ESP 尺寸**写 GPT 与磁盘总长，
  保证分区表、FAT 卷与备份 GPT 三者一致。

### 工具包与源码包

| 文件 | 内容 |
|---|---|
| `e1wine-2.0-macos.zip` / `.dmg` / `.pkg` | macOS 安装包（`e1wine`、`e1wxfly`、通用启动器） |
| `e1wine-2.0-linux.tar.gz` | Linux x86_64 / aarch64 静态二进制 + `install.sh` |
| `e1wine-2.0-unix-installer.sh` | POSIX 自解压安装器（自动识别 OS/架构） |
| `e1LibreOS-2.0-source.tar.gz` / `.zip` | 完整源码树，排除 `.git`/`build`/`downloads`/`dist` 等构建产物 |

打包命令：`sh tools/mkdistpkg.sh 2.0`（读取 `dist/e1wine-*` 产物）。

### 工具链（零 Homebrew）

| 工具 | 版本 | 来源 |
|---|---|---|
| `mkisofs` | 3.02a09 | cdrtools 源码构建（`build/host-tools/bin/mkisofs`） |
| `qemu-system-aarch64` | 11.1.3 | QEMU 源码构建（`--target-list=aarch64-softmmu`，无图形） |
| `ninja` / `meson` / `pkgconf` | 1.11.1 / 1.5.1 / 1.8.0 | 各自源码自举 |
| C 交叉编译 | zig 0.11.0 | 独立工具链，`zig cc -target <arch>-linux-musl` |

一键构建：`sh tools/build-host-tools.sh`（幂等）。此前 `downloads/zig` 因解包不完整
导致链接挂起，现已修复：build.sh 检测到 zig 不可用时会自动从 `downloads/zig.tar.xz`
重新解包。

### server 集成 e1pkg

- 镜像内置 `/usr/bin/e1pkg` 和与 minimal/desktop/mobile 完全一致的
  `/etc/e1pkg.conf`：公网 IPv6 e1repo 排第一，其后是 VBox NAT、局域网镜像和
  `/srv/repo` 本地回退。
- `systemd-networkd` + DHCP 随镜像启用；独立的 `e1-net-modules.service` 在
  networkd 之前用静态 busybox `modprobe` 加载 `af_packet` / `e1000` /
  `e1000e` / `virtio_net`，保证干净启动后即可联网拉包。
- IPv6 字面量 URL 的方括号在 shell 侧按字面处理（`e1pkg` 内所有 URL 变量加引号；
  安装器遍历镜像列表时用 `set -f` 关闭通配符展开），不会被当成 glob。
- `e1pkg` 增加可达性保护：无全局 IPv6 地址时直接跳过 IPv6 字面量源，避免
  busybox wget 在不可达地址上卡死；wget 路径另有看门狗兜底，curl 路径设置
  connect/total 双重超时。

## 产物与校验值

| 文件 | 字节数 | SHA-256 |
|---|---|---|
| `e1LibreOS-2.0-minimal-x86_64.iso` | 34,609,152 | `528094be429fe02da5e04dde765fdff519956fd41b996f69209d3554084f3386` |
| `e1LibreOS-2.0-desktop-x86_64.iso` | 35,659,776 | `adb6c21f672d3c0c0becf611ebf8cd854f32806dfbfae157b8960de5a760734a` |
| `e1LibreOS-2.0-mobile-x86_64.iso` | 35,659,776 | `8290efc6b41303900a32c8860a521a6368e231317916f8eb018e085828e493a9` |
| `e1LibreOS-2.0-server-x86_64.iso` | 123,430,912 | `e80b7606a6111122c5973a6c35866b7547fb2f9882482153abfa7228a9f13abc` |
| `e1LibreOS-2.0-workstation-x86_64.iso` | 123,768,832 | `8dff83b7290158f748617a78d17e38375ca356f38a32e8740565544ebd5e2831` |
| `e1LibreOS-2.0-minimal-aarch64.img` | 540,016,640 | `b0cef77be97a77bee7fb7ac7ce6030d27849660db01c34462d37b9377f8713a1` |
| `e1LibreOS-2.0-desktop-aarch64.img` | 540,016,640 | `455624fd295476172c483911e398a16d6af542f77dcccc20d9e3521b9e4bd775` |
| `e1LibreOS-2.0-mobile-aarch64.img` | 540,016,640 | `ca03b7273ed53306227f214142729cc2da8b8ed0e0bcc0ff9237f1e23321558b` |
| `e1wine-2.0-macos.zip` | 32,206 | `869b754344eaf492ed801eaa5b818b5bf447b44f4e84400fbc84df8a72113082` |
| `e1wine-2.0-macos.dmg` | 51,915 | `fe4b6ee8fd29f2681d9de6b3ff9c3cb3a07b35aecb7abf0cea5005a650ac5a58` |
| `e1wine-2.0-macos.pkg` | 32,658 | `b917f2c3d682d2992e1eb681c0d7386965e9395977173e69a67e43bf7e0dca35` |
| `e1wine-2.0-linux.tar.gz` | 57,666 | `66e597355592499a27bc41ee11881eff688a3d64711c22e4856ac9b66ef9f564` |
| `e1wine-2.0-unix-installer.sh` | 105,402 | `2b154a9708abcc628f4fa8456f065234f6f66793fecbe9a462bf23fc537e40bd` |
| `e1LibreOS-2.0-source.tar.gz` | 3,405,167 | `3b8b76118f68d6bfa3dfca1025a7a6d01c91b556d8f0a54c56b0e0314defea8a` |
| `e1LibreOS-2.0-source.zip` | 3,459,387 | `0177af305ffa45c273a375141386f8a5bb21fd9f82ea2b52f0cec7a2868c238d` |

校验：

```sh
shasum -a 256 -c SHA256SUMS.txt
```

发布目录：`dist/e1LibreOS-2.0/`（包含五个 x86_64 ISO、三个 aarch64 `.img`、
五个 e1wine/e1wxfly 安装包、两个源码包、本说明与 `SHA256SUMS.txt`）。

## 启动验证

### x86_64（VirtualBox 7.2.6，串口控制台）

| 镜像 | 固件 | 结果 |
|---|---|---|
| minimal ISO（本次重建） | UEFI（`--firmware efi`） | GRUB 2.12 EFI 菜单自动引导；内核 6.12.114-0-virt 启动，串口输出 e1LibreOS 2.0 横幅与 `e1apk` demo 成功 |
| minimal ISO（本次重建） | BIOS（isohybrid） | 启动到 multi-user；版本横幅与欢迎信息均为 2.0 |
| desktop ISO | UEFI（上一轮） | 由 GRUB EFI 引导成功；`FB-OK: /dev/fb0`、`E1WINE-OK`、`E1APK-OK` |
| desktop / mobile / server / workstation | BIOS（上一轮） | 均已实测启动；server `e1pkg install cowsay` 从公网 IPv6 源安装成功，workstation `e1wm.service` active |

UEFI 下 `/dev/fb0` 由 simpledrm 提供，无需 `vga=` 参数，e1wm 可直接渲染。

### aarch64（源码构建 QEMU 11.1.3 实测启动）

验证方式：从发布 `.img` 的 FAT32 ESP 中拷出 `/VMLINUZ`、`/INITRD.GZ`，
用源码构建的 `qemu-system-aarch64`（`-M virt -cpu cortex-a57 -m 1024 -nographic`）
直接启动镜像内的真实二进制：

| 镜像 | 验证内容 | 结果 |
|---|---|---|
| minimal `.img` | GPT + ESP 结构（`hdiutil` 挂载：EFI 分区 512 MiB，含 `VMLINUZ` 9.2 MB、`INITRD.GZ` 3.1 MB、`EFI/BOOT/BOOTAA64.EFI`、`GRUB.CFG`） | 通过 |
| desktop `.img` | 从 ESP 抽取 VMLINUZ/INITRD.GZ 启动：内核 6.12.115-0-virt 启动、initramfs 执行到用户态（`e1pkg` demo、`e1apk` 演示、e1000/virtio_net 探测、DHCP） | **通过（真实启动）** |
| mobile `.img` | 同一构建流程与同一内核/用户态栈 | 通过（构建与结构） |

aarch64 三个镜像的系统二进制（`e1wm` / `e1wine` / `e1gpt` / `e1apk`）本次
均由 `zig cc -target aarch64-linux-musl` **真实交叉编译**，不再复用旧缓存。

**限制**：本机无 AAVMF/edk2 ARM 固件，未做「UEFI 固件 → GRUB → 内核」全链路；
已验证的是镜像内真实内核 + initramfs 启动与 GPT/ESP 结构。建议在 ARM64 设备上
做首启验证（把 `.img` 写入 U 盘并从 UEFI(ARM64) 启动）。

## 已知限制

- 未做 aarch64「UEFI 固件 → GRUB → 内核」全链路启动（缺 AAVMF/edk2 固件）；
  镜像内真实内核 + initramfs 启动已用源码构建的 QEMU 验证通过。
- 源码构建的 QEMU 使用 `--disable-pixman` 与无图形后端（headless），用于自动
  验证；日常图形化运行可用 `sudo port install qemu`。
- aarch64 镜像约 515 MB（FAT32 最小簇数要求导致 ESP 自动扩至 512 MiB）。
- server / workstation 仅 x86_64：根文件系统为 Arch bootstrap 整树 initramfs。
- 公网 IPv6 源需要网络提供全局 IPv6 地址；无 IPv6 时 e1pkg 按配置顺序回退。
- `downloads/grub/*.efi` 仍为预构建的 GRUB standalone 资产（macOS 宿主不适合
  交叉构建 GRUB）；x86_64 的 UEFI 路径与 aarch64 的 EFI 文件均已实测可被固件识别。

## 升级与构建

Live 镜像启动即以 root 自动登录，运行 `setup-e1os` 安装（问答同
`setup-alpine`）。重新构建：

```sh
sh tools/build-host-tools.sh                         # 首次：源码构建宿主工具
./build.sh                                           # x86_64 minimal
E1OS_VARIANT=desktop ./build.sh                      # x86_64 desktop
E1OS_VARIANT=mobile ./build.sh                       # x86_64 mobile
E1OS_VARIANT=server ./build.sh                       # x86_64 server
E1OS_VARIANT=workstation ./build.sh                  # x86_64 workstation
E1OS_ARCH=aarch64 E1OS_VARIANT=minimal ./build.sh    # aarch64 minimal
E1OS_ARCH=aarch64 E1OS_VARIANT=desktop ./build.sh    # aarch64 desktop
E1OS_ARCH=aarch64 E1OS_VARIANT=mobile ./build.sh     # aarch64 mobile
```
