# 构建指南

## 1. 宿主环境要求

| 依赖 | 用途 | 获取方式 |
|---|---|---|
| python3 ≥ 3.9 | cpio/FAT 镜像生成 | 系统自带 |
| curl | 拉取上游文件 | 系统自带 |
| gzip / tar / xz | 解包压缩 | 系统自带 |
| zstd | 解包 Arch bootstrap（glibc 分支） | `sudo port install zstd`（或源码构建） |
| mkisofs | x86_64 ISO 打包 | `sh tools/build-host-tools.sh`（源码构建 cdrtools）或 `sudo port install cdrtools` |

> Homebrew 已停止对 Intel Mac 支持，本项目**不再依赖 brew**。
> 宿主工具统一由 `tools/build-host-tools.sh` 从源码构建到 `build/host-tools/bin/`，
> 可用 MacPorts 替代的依赖优先用 MacPorts（`/opt/local`）。

### 1.1 源码构建宿主工具

```sh
sh tools/build-host-tools.sh
```

脚本幂等，按需构建并输出到 `build/host-tools/bin/`：

| 工具 | 版本 | 来源 |
|---|---|---|
| `c++` | 包装 | 系统 clang + SDK 内 libc++ 头（CommandLineTools 头不完整） |
| `ninja` | 1.11.1 | `configure.py --bootstrap` |
| `meson` | 1.5.1 | PyPI sdist（纯 Python） |
| `pkgconf` / `pkg-config` | 1.8.0 | 源码构建 |
| `mkisofs` | 3.02a09 | cdrtools 源码（MacPorts 已装则直接链接） |
| `qemu-system-aarch64` | 11.1.3 | QEMU 源码，`--target-list=aarch64-softmmu`，无图形/pixman |

QEMU 构建依赖 glib（`/usr/local` 或 MacPorts）与 libfdt（脚本自动克隆 dtc 子项目）。
`mkisofs`、`qemu-system-aarch64` 也支持用 MacPorts 安装（`sudo port install cdrtools qemu`）。

## 2. 构建分支与矩阵

共 5 个分支，当前全部可直接构建：

| 分支 `E1OS_VARIANT` | 用户态 / 图形 | 镜像内容差异 |
|---|---|---|
| `minimal`（默认） | busybox + musl，纯文本控制台 | 无 e1wm 启动、无 DRM/USB-HID 模块、无 `vga=` 参数、无 `/etc/e1mode` |
| `desktop` | busybox + musl，自研 e1wm | 含 simpledrm/DRM + USB 输入模块，`vga=0x342`（640×480×32），tty1 直接拉起 e1wm |
| `mobile` | busybox + musl，e1wm 移动布局 | 与 desktop 同栈，另加 `vga=0x343`（800×600×32）与 `/etc/e1mode=mobile`（大字号 + 虚拟键盘） |
| `server` | Arch bootstrap（glibc）+ systemd，无图形 | 整树 initramfs 作根，`/init` exec systemd 作 PID 1；tty1/ttyS0 getty；内置 e1pkg + `/etc/e1pkg.conf`（公网 IPv6 e1repo 优先） |
| `workstation` | 同 server + e1wm | 内置 e1wm 与 DRM 模块，`e1wm.service` 绑定 tty1 启动，mask tty1 getty |

```sh
# x86_64（输出 ISO；downloads/grub/grubx64.efi 存在时为 BIOS + UEFI，否则仅 BIOS）
./build.sh                                  # minimal
E1OS_VARIANT=desktop ./build.sh             # desktop（e1wm 图形）
E1OS_VARIANT=mobile ./build.sh              # mobile（800x600 触屏）

# aarch64（输出原始 .img，GPT + ESP + grubaa64.efi）
E1OS_ARCH=aarch64 ./build.sh
E1OS_ARCH=aarch64 E1OS_VARIANT=desktop ./build.sh
E1OS_ARCH=aarch64 E1OS_VARIANT=mobile ./build.sh

# server（glibc + systemd）与 workstation（glibc + systemd + e1wm）
E1OS_VARIANT=server ./build.sh
E1OS_VARIANT=workstation ./build.sh
```

产物位于 `build/`：

```
e1LibreOS-2.0-<variant>-x86_64.iso     # 约 35 MB（minimal/desktop/mobile，含 UEFI）；约 118 MB（server/workstation）
e1LibreOS-2.0-<variant>-aarch64.img    # 约 515 MB（GPT + 512 MiB FAT32 ESP），直接 dd 到磁盘
initramfs-e1.gz                         # Live initramfs
vmlinuz-virt-<arch>                     # 内核
```

`<variant>` 为五个分支名之一。`server` / `workstation` 的 glibc 分支体积大是因为
Arch bootstrap 整棵树（裁剪后约 300 MB）都在 initramfs 里，运行时全量驻留内存，
虚拟机建议 ≥ 2 GB RAM。

## 3. 构建过程做了什么

1. 从 Alpine v3.21 仓库下载 `linux-virt` apk，提取内核与精简模块集
   （virtio 磁盘/网卡、ext4、FAT；desktop / mobile 另含 simpledrm/USB HID 栈）
2. x86_64 获取 busybox.net 静态 busybox；aarch64 使用 Alpine `busybox-static`
3. zig 交叉编译自研组件并放入 rootfs
4. 生成 e1pkg 样例包仓库 `rootfs/srv/repo/repo.db`
5. [tools/mkinitramfs.py](../tools/mkinitramfs.py) 生成 newc 格式 cpio（macOS 自带
   BSD cpio 不支持 newc，所以用纯 Python）并 gzip
6. x86_64：isolinux BIOS 引导；`downloads/grub/grubx64.efi` 存在时，再用
   [tools/mkfat32.py](../tools/mkfat32.py) 生成 16 MiB FAT16 super-floppy
   EFI 引导镜像（内含 `/EFI/BOOT/BOOTX64.EFI`），合成 BIOS + UEFI 双引导
   ISO（缺少 grub 时仅 BIOS）；最后 [tools/isohybrid.py](../tools/isohybrid.py)
   写 isohybrid MBR
7. aarch64：[tools/build-arm-image.sh](../tools/build-arm-image.sh) 生成
   GPT + FAT32 ESP 磁盘镜像（`/VMLINUZ`、`/INITRD.GZ`、
   `/EFI/BOOT/BOOTAA64.EFI`、外置 `GRUB.CFG` 兼容副本）。FAT32 规范要求
   ≥65525 簇，64 MiB 会不满足，`mkfat32.py` 自动扩到 512 MiB；脚本以**实际
   ESP 尺寸**写 GPT 并计算磁盘总长（否则分区表与 FAT 卷不符、备份 GPT 被覆盖）

### 3.1 glibc + systemd 分支（server / workstation）

这两条分支走独立脚本 [tools/build-glibc-image.sh](../tools/build-glibc-image.sh)，
`build.sh` 检测到 `E1OS_VARIANT=server|workstation` 时直接委托给它：

1. 下载 Arch Linux bootstrap（`archlinux-bootstrap-x86_64.tar.zst`，约 122 MB，
   阿里云镜像），解包到 `build/glibc-<variant>/rootfs`
2. 裁剪体积：删除 locale / man / doc / i18n / terminfo / zoneinfo / ICU /
   `libc.a` / gconv / hwdb.bin 等，584 MB → 约 300 MB
3. 写入 e1LibreOS 品牌：`usr/lib/os-release`（带 `VARIANT_ID`）、`etc/hostname`、
   空 `machine-id` / `fstab`、`locale.conf`
4. 写 `/init` 包装脚本：挂载 proc/sys/devtmpfs/run 后 `exec /usr/lib/systemd/systemd`
5. 与其它分支共用 Alpine `linux-virt` 内核（内核与用户态 libc 无关）
6. 用同一个 [tools/mkinitramfs.py](../tools/mkinitramfs.py) 把整棵树打成 newc
   cpio 并 gzip（**initramfs 即根文件系统**，不使用 mkinitcpio / squashfs），
   再由 mkisofs + isolinux 生成 ISO

`server` 额外：复制 `e1pkg` 与仓库统一的 `/etc/e1pkg.conf`，启用
`systemd-networkd` + DHCP，并由 `e1-net-modules.service` 在 networkd 之前用
静态 busybox `modprobe` 加载 e1000/e1000e/virtio_net/af_packet，保证
`e1pkg` 能联网拉包（该 bootstrap 的 kmod 在 initramfs 内解析文本
`modules.dep` 不可靠，故不使用系统 kmod）。

`workstation` 额外：复制 e1wm 静态二进制与 DRM 内核模块，写 `e1wm.service`
（`ExecStartPre=modprobe simpledrm`、绑定 tty1 作标准输入、随镜像启动），
并 mask `getty@tty1.service`。此分支仅 x86_64。

两条分支都通过 systemd drop-in 让串口 getty 以 `--autologin root` 启动
（Live 免登录，与 minimal/desktop/mobile 语义一致）；`server` 的 tty1 同样自动登录。

### 3.2 引导资产 downloads/grub/

| 文件 | 用途 | 生成方式 |
|---|---|---|
| `grubx64.efi` | x86_64 ISO 的 UEFI El Torito 引导 | `grub-mkstandalone -O x86_64-efi`（GRUB 2.12，内嵌 `serial`/`search`/`linux`/`initrd` 菜单配置） |
| `grubaa64.efi` | aarch64 `.img` 的 ESP 引导 | `grub-mkstandalone -O arm64-efi -d <Alpine grub arm64-efi 模块目录>`（PE32+ AArch64） |

两者均为 standalone 镜像并**内嵌完整 menuentry**，不依赖外置配置文件：
x86_64 搜索 `/boot/vmlinuz-virt`，aarch64 搜索 `/VMLINUZ`。外置
`GRUB.CFG` 仍随镜像提供，供其它 grub 二进制（如发行版自带 grub）使用。
`E1OS_GRUB_BIN` 可覆盖 x86_64 所用文件。

## 4. 调试钩子（环境变量）

| 变量 | 作用 |
|---|---|
| `E1OS_GRUB_BIN=/path/grubx64.efi` | 替换 EFI 引导用 grub 二进制 |
| `E1OS_EFI_IMG=/path/efi.img` | 直接使用外部 EFI 镜像（A/B 调试） |
| `E1OS_HYBRID=0` | 不写 isohybrid MBR |
| `E1OS_AUTOFLAG=` | 覆盖内核启动参数 |

## 5. e1wine 全平台二进制

```sh
./tools/build-e1wine.sh all dist
```

输出 `dist/`：

```
e1wine-linux-x86_64            # x86_64-linux-musl 静态
e1wine-linux-aarch64           # aarch64-linux-musl 静态
e1wine-macos-universal         # x86_64 + arm64 Mach-O
e1wine-mac-launcher-universal  # .app 内嵌启动器
e1wine-unix-<sys>-<arch>       # 宿主 cc 原生编译版
```

macOS 上构建时会先用 clang 生成 universal 启动器并嵌入
`src/e1wine/mac_launcher_blob.h`（该文件已提交，非 macOS 宿主直接复用）。
详见 [E1WINE.md](E1WINE.md)。

打包为各平台安装包（读取上面的 `dist/` 产物）：

```sh
sh tools/mkdistpkg.sh 2.0      # 版本号即产物名后缀
```

输出 `build/distpkg/`：`e1wine-2.0-macos.zip/.dmg/.pkg`、
`e1wine-2.0-linux.tar.gz`、`e1wine-2.0-unix-installer.sh` 与 `SHA256SUMS.txt`。

## 6. 常见问题

- **“所有 Alpine 镜像均不可达”**：镜像探测可能瞬时超时，重跑 `./build.sh` 即可。
- **`linux-virt.apk 下载不完整`**：删除 `downloads/linux-virt-*.apk` 后重跑。
- **未找到 mkisofs，跳过 ISO**：运行 `sh tools/build-host-tools.sh`（源码构建 cdrtools），
  或 `sudo port install cdrtools`。
- **zig 报 `lib/libc/musl/...` 源文件找不到**：`downloads/zig` 解包不完整，删除该目录后
  build.sh 会用 `downloads/zig.tar.xz` 自动重新解包。
- **FAT EFI 镜像无法引导**：EFI El Torito 镜像必须 ≥ 16 MiB
  （4 MiB 卷簇数不足 4085，固件会按 FAT12 解析而损坏），build.sh 默认值勿改小。

## 7. 发布打包

全部镜像、工具包与源码包齐备后：

```sh
# 1) 五个 x86_64 ISO + 三个 aarch64 .img（全部真实交叉编译，不用 E1OS_NO_CC）
for v in minimal desktop mobile server workstation; do
    E1OS_VARIANT=$v ./build.sh
done
for v in minimal desktop mobile; do
    E1OS_ARCH=aarch64 E1OS_VARIANT=$v ./build.sh
done

# 2) e1wine / e1wxfly 安装包
sh tools/mkdistpkg.sh 2.0

# 3) 源码包（排除构建产物与依赖下载）
mkdir -p /tmp/srcpkg/e1LibreOS-2.0
rsync -a --exclude='.git' --exclude='.vbox' --exclude='build' \
      --exclude='downloads' --exclude='dist' \
      --exclude='.DS_Store' --exclude='__pycache__' \
      ./ /tmp/srcpkg/e1LibreOS-2.0/
tar -C /tmp/srcpkg -czf build/distpkg/e1LibreOS-2.0-source.tar.gz e1LibreOS-2.0
zip -qry build/distpkg/e1LibreOS-2.0-source.zip /tmp/srcpkg/e1LibreOS-2.0  # 或先 cd 到 /tmp/srcpkg

# 4) 汇总到 dist/e1LibreOS-2.0 并生成统一校验值
mkdir -p dist/e1LibreOS-2.0
cp build/*-2.0-*-x86_64.iso build/*-2.0-*-aarch64.img \
   build/distpkg/e1wine-2.0-* build/distpkg/e1LibreOS-2.0-source.* \
   docs/RELEASE-2.0.md dist/e1LibreOS-2.0/
cd dist/e1LibreOS-2.0
shasum -a 256 * > SHA256SUMS.txt
shasum -a 256 -c SHA256SUMS.txt       # 末尾会出现几行 SHA256SUMS.txt 自身缺失，忽略即可
```

产物命名固定为 `e1LibreOS-<版本>-<variant>-<arch>.iso`（aarch64 为 `.img`），
版本号在 `build.sh` 顶部的 `VER` 与 `tools/build-glibc-image.sh` 的 `VER`
两处维护，需保持一致。
