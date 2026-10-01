# 构建指南

## 1. 宿主环境要求

| 依赖 | 用途 | macOS 安装 | Linux 安装 |
|---|---|---|---|
| python3 ≥ 3.9 | cpio/FAT 镜像生成 | 系统自带 | `apk add python3` / `apt install python3` |
| curl | 拉取上游文件 | 系统自带 | 包管理器安装 |
| gzip / tar | 解包压缩 | 系统自带 | 系统自带 |
| mkisofs | x86_64 ISO 打包 | `brew install cdrtools` | `apt install genisoimage` |

自研 C 组件（e1wm / e1gpt / e1apk / e1wine）全部使用构建时自动下载的
**zig 0.11.0 独立工具链**交叉编译为 musl 静态二进制，宿主机无需 gcc/musl 工具链。

> 在不能写 `/usr/local` 的受限环境（如沙箱），可把 Homebrew cdrtools bottle
> 解压到仓库内 `brew/` 目录，build.sh 会自动识别 `brew/cdrtools/*/bin/mkisofs`。

## 2. 构建矩阵

```sh
# x86_64（输出 ISO，BIOS + UEFI 双引导）
./build.sh                                  # server
E1OS_VARIANT=workstation ./build.sh         # workstation（e1wm 图形）
E1OS_VARIANT=mobile ./build.sh              # mobile（800x600 触屏）

# aarch64（输出原始 .img，GPT + ESP + grubaa64.efi）
E1OS_ARCH=aarch64 ./build.sh
E1OS_ARCH=aarch64 E1OS_VARIANT=workstation ./build.sh
E1OS_ARCH=aarch64 E1OS_VARIANT=mobile ./build.sh
```

产物位于 `build/`：

```
e1LibreOS-1.0-<variant>-x86_64.iso     # 约 32 MB（含 16 MB EFI 引导镜像）
e1LibreOS-1.0-<variant>-aarch64.img    # 67 MB，直接 dd 到磁盘
initramfs-e1.gz                         # Live initramfs
vmlinuz-virt-<arch>                     # 内核
```

## 3. 构建过程做了什么

1. 从 Alpine v3.21 仓库下载 `linux-virt` apk，提取内核与精简模块集
   （virtio 磁盘/网卡、ext4、FAT、workstation 另含 simpledrm/USB HID 栈）
2. x86_64 获取 busybox.net 静态 busybox；aarch64 使用 Alpine `busybox-static`
3. zig 交叉编译自研组件并放入 rootfs
4. 生成 e1pkg 样例包仓库 `rootfs/srv/repo/repo.db`
5. [tools/mkinitramfs.py](../tools/mkinitramfs.py) 生成 newc 格式 cpio（macOS 自带
   BSD cpio 不支持 newc，所以用纯 Python）并 gzip
6. x86_64：isolinux BIOS 引导 + [tools/mkfat32.py](../tools/mkfat32.py) 生成
   16 MiB FAT16 super-floppy EFI 引导镜像（grubx64-alpine.efi），mkisofs 合成
   双引导 ISO，最后 [tools/isohybrid.py](../tools/isohybrid.py) 写 isohybrid MBR
7. aarch64：[tools/build-arm-image.sh](../tools/build-arm-image.sh) 生成
   GPT + FAT32 ESP 磁盘镜像

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

## 6. 常见问题

- **“所有 Alpine 镜像均不可达”**：镜像探测可能瞬时超时，重跑 `./build.sh` 即可。
- **`linux-virt.apk 下载不完整`**：删除 `downloads/linux-virt-*.apk` 后重跑。
- **未找到 mkisofs，跳过 ISO**：安装 cdrtools（见第 1 节）或把二进制放入 `brew/`。
- **FAT EFI 镜像无法引导**：EFI El Torito 镜像必须 ≥ 16 MiB
  （4 MiB 卷簇数不足 4085，固件会按 FAT12 解析而损坏），build.sh 默认值勿改小。
