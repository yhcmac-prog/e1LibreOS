#!/bin/sh
# build.sh — e1LibreOS 构建脚本
# 产物: build/initramfs-e1.gz + build/vmlinuz-virt + 可选 ISO
set -eu
cd "$(dirname "$0")"

DL=downloads
BUILD=build
SRC=packages/src

msg() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[31m错误:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 变体
# E1OS_VARIANT=server（默认，命令行）/ workstation（图形界面，自研 e1wm）
#             / mobile（移动端：800x600 触屏 + 虚拟键盘，复用 e1wm）
VARIANT=${E1OS_VARIANT:-server}
case "$VARIANT" in
    server|workstation|mobile) ;;
    *) die "E1OS_VARIANT 仅支持 server / workstation / mobile" ;;
esac
VN=$(printf '%s' "$VARIANT" | cut -c1 | tr 'a-z' 'A-Z')$(printf '%s' "$VARIANT" | cut -c2-)
# 是否需要 e1wm 图形界面（workstation / mobile 共用同一图形栈）
NEED_GFX=0
[ "$VARIANT" = workstation ] && NEED_GFX=1
[ "$VARIANT" = mobile ] && NEED_GFX=1

# ---------------------------------------------------------------- 目标架构
# E1OS_ARCH=x86_64（默认，ISO+isolinux/MBR）/ aarch64（GPT+ESP EFI 原始镜像）
ARCH=${E1OS_ARCH:-x86_64}
case "$ARCH" in
    x86_64|aarch64) ;;
    *) die "E1OS_ARCH 仅支持 x86_64 / aarch64" ;;
esac
ZTARGET="$ARCH-linux-musl"

for c in python3 curl gzip tar; do
    command -v "$c" >/dev/null 2>&1 || die "缺少依赖: $c"
done

# ---------------------------------------------------------------- 下载
fetch() { # <url> <out>
    [ -s "$2" ] && return 0
    msg "下载 $1"
    curl -fL --retry 3 --retry-all-errors -C - --progress-bar -o "$2" "$1" \
        || die "下载失败: $1"
}

mkdir -p "$DL" "$BUILD"

# E1OS_NO_CC=1：跳过全部 C 交叉编译，沿用 build/cc-cache/$ARCH 中已编译好的二进制
# 用于交叉编译器缺失/卡死的环境（缓存由此前一次正常构建自动填充）。
NO_CC="${E1OS_NO_CC:-}"
CCCACHE="$BUILD/cc-cache/$ARCH"
if [ -n "$NO_CC" ]; then
    mkdir -p "$CCCACHE/usr/bin" "$CCCACHE/usr/sbin" "$CCCACHE/usr/share/e1wine"
    for f in usr/bin/e1wm usr/bin/e1wine usr/sbin/e1gpt usr/bin/e1apk; do
        [ -f "rootfs/$f" ] && [ ! -f "$CCCACHE/$f" ] && cp "rootfs/$f" "$CCCACHE/$f"
    done
fi
# keep_cc <相对路径>：NO_CC 模式从缓存恢复二进制，成功返回 0
keep_cc() {
    [ -n "$NO_CC" ] || return 1
    if [ -f "$CCCACHE/$1" ]; then
        mkdir -p "$(dirname "rootfs/$1")"
        cp "$CCCACHE/$1" "rootfs/$1"; chmod +x "rootfs/$1"
        msg "E1OS_NO_CC: 沿用已编译的 $1"
        return 0
    fi
    msg "E1OS_NO_CC: 缓存中无 $1，该组件本次缺失"
    return 1
}

# 内核与驱动模块统一来自 Alpine linux-virt 包（版本自洽，模块 vermagic 一致）
# 镜像优先级：阿里云（对 wget/curl UA 均友好）-> 官方 dl-cdn
for mirror in https://mirrors.aliyun.com/alpine https://dl-cdn.alpinelinux.org/alpine; do
    if curl -fsL --max-time 15 -o /dev/null "$mirror/v3.21/main/$ARCH/"; then
        ALPINE_REPO="$mirror/v3.21/main/$ARCH"
        break
    fi
done
[ -n "${ALPINE_REPO:-}" ] || die "所有 Alpine 镜像均不可达"
KVAPK="$DL/linux-virt-$ARCH.apk"
if [ -s "$KVAPK" ] && ! gzip -t "$KVAPK" 2>/dev/null; then
    rm -f "$KVAPK"   # 上次中断可能留下截断文件
fi
if [ ! -s "$KVAPK" ]; then
    msg "解析 linux-virt ($ARCH) 包版本"
    apkname=$(curl -fsL "$ALPINE_REPO/" | grep -oE 'linux-virt-[0-9][^"]*\.apk' | sort -u | tail -n 1) \
        || die "无法访问 Alpine 源"
    fetch "$ALPINE_REPO/$apkname" "$KVAPK"
    gzip -t "$KVAPK" || die "linux-virt.apk 下载不完整，请重跑 ./build.sh"
fi
# busybox：x86_64 用 busybox.net 静态 1.35.0；aarch64 用 Alpine busybox-static 包
if [ "$ARCH" = x86_64 ]; then
    fetch https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox "$DL/busybox-$ARCH"
else
    BBSAPK="$DL/busybox-static-$ARCH.apk"
    if [ ! -s "$BBSAPK" ]; then
        bbname=$(curl -fsL "$ALPINE_REPO/" | grep -oE 'busybox-static-[0-9][^"]*\.apk' | sort -u | tail -n 1)
        fetch "$ALPINE_REPO/$bbname" "$BBSAPK"
    fi
    rm -rf "$DL/bbsrc"; mkdir -p "$DL/bbsrc"
    tar -xzf "$BBSAPK" -C "$DL/bbsrc" bin/busybox.static
    cp "$DL/bbsrc/bin/busybox.static" "$DL/busybox-$ARCH"
    rm -rf "$DL/bbsrc"
fi

# syslinux 仅 x86_64 ISO 需要；下载失败不阻塞构建（仅跳过 ISO）
if [ "$ARCH" = x86_64 ] && [ ! -f "$DL/isolinux.bin" ] && [ ! -s "$DL/syslinux.apk" ]; then
    if curl -fL --retry 1 --max-time 180 --progress-bar \
        -o "$DL/syslinux.apk" \
        https://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86_64/syslinux-6.04_pre1-r16.apk; then
        :
    else
        rm -f "$DL/syslinux.apk"
        msg "syslinux 下载失败/超时，本次构建将跳过 ISO"
    fi
fi

chmod +x "$DL/busybox-$ARCH"

# 从 Alpine syslinux.apk 提取引导文件（isolinux.bin / ldlinux.c32 / isohdpfx.bin）
if [ ! -f "$DL/isolinux.bin" ] && [ -s "$DL/syslinux.apk" ]; then
    msg "提取 syslinux 引导文件"
    mkdir -p "$DL/syslinux-src"
    tar -xzf "$DL/syslinux.apk" -C "$DL/syslinux-src"
    cp "$DL/syslinux-src/usr/share/syslinux/isolinux.bin" "$DL/"
    cp "$DL/syslinux-src/usr/share/syslinux/ldlinux.c32"  "$DL/"
    cp "$DL/syslinux-src/usr/share/syslinux/isohdpfx.bin" "$DL/"
    chmod +x "$DL/isolinux.bin"
    rm -rf "$DL/syslinux-src"
fi

# ---------------------------------------------------------------- 打包软件
msg "构建样例软件包与仓库 (repo.db)"
mkdir -p rootfs/srv/repo
: > rootfs/srv/repo/repo.db
for d in "$SRC"/*/; do
    [ -f "$d/meta" ] || continue
    name=$(grep '^name='    "$d/meta" | cut -d= -f2-)
    ver=$(grep  '^version=' "$d/meta" | cut -d= -f2-)
    sum=$(grep  '^summary=' "$d/meta" | cut -d= -f2-)
    dep=$(grep  '^depends=' "$d/meta" | cut -d= -f2-)
    # 确保 payload 里的可执行脚本带 +x 权限
    find "$d/payload" -type f -exec chmod +x {} \; 2>/dev/null || true
    pkg="$name-$ver.e1.tar.gz"
    tar -czf "rootfs/srv/repo/$pkg" -C "$d" meta payload
    printf '%s|%s|%s|%s|%s\n' "$name" "$ver" "$sum" "$dep" "$pkg" >> rootfs/srv/repo/repo.db
done
[ -s rootfs/srv/repo/repo.db ] || die "未发现任何软件包源"

# ---------------------------------------------------------------- 驱动模块
# 从 linux-virt apk 提取 Live 环境所需的精简模块集（磁盘 + 网卡），
# 供 Live 环境识别 virtio 磁盘/网卡，供 setup-e1os 联网安装使用
# 注意：此步骤必须在 stage 组装之前执行，否则模块进不了 initramfs
rm -rf "$DL/lvsrc"; mkdir -p "$DL/lvsrc"
tar -xzf "$KVAPK" -C "$DL/lvsrc" boot/vmlinuz-virt 2>/dev/null
cp "$DL/lvsrc/boot/vmlinuz-virt" "$DL/vmlinuz-virt-$ARCH"
KVER=$(tar -tzf "$KVAPK" | grep -m1 -oE 'lib/modules/[^/]+/' | cut -d/ -f3)
[ -n "$KVER" ] || die "无法从 linux-virt.apk 解析内核版本"
# 内核版本变更时清理旧模块目录（避免 vermagic 不匹配的模块混入 initramfs）
for old in rootfs/lib/modules/*; do
    [ -d "$old" ] || continue
    [ "$(basename "$old")" = "$KVER" ] || rm -rf "$old"
done
msg "提取 Live 环境驱动模块 ($KVER)"
SRCLIB="$DL/lvsrc/lib/modules/$KVER"
if tar -xzf "$KVAPK" -C "$DL/lvsrc" \
    "lib/modules/$KVER/modules.dep" 2>/dev/null && [ -f "$SRCLIB/modules.dep" ]; then
    E1OS_VARIANT="$VARIANT" python3 - "$SRCLIB" "rootfs/lib/modules/$KVER" "$KVAPK" <<'PY'
import os, shutil, gzip, subprocess, sys
src, dst, apk = sys.argv[1], sys.argv[2], sys.argv[3]
# linux-virt 包内模块为压缩格式 (.ko.gz)，统一解压为 .ko
strip_gz = lambda p: p[:-3] if p.endswith(".gz") else p
entries = {}
for ln in open(os.path.join(src, "modules.dep")).read().splitlines():
    if not ln.strip():
        continue
    path, _, deps = ln.partition(":")
    path = strip_gz(path.strip())
    entries[os.path.basename(path)] = (path, [strip_gz(d.strip()) for d in deps.split()])
want = set()
def add(b):
    if b in want or b not in entries:
        return
    want.add(b)
    for d in entries[b][1]:
        add(os.path.basename(d))
# 磁盘（virtio-blk + VirtualBox VirtioSCSI 链）+ 网卡 + 文件系统（setup-e1os 用）
# ext4 挂载时经 crypto API 动态 request_module("crc32c")（不在 depends= 里，需显式提取）
# fat/vfat/nls：EFI ESP 与 diskless USB 配置介质；usb-storage：U盘安装/无盘启动
for root in ("virtio_blk.ko", "virtio_scsi.ko", "scsi_mod.ko", "sd_mod.ko",
             "usb-storage.ko", "uas.ko",
             "virtio_net.ko", "net_failover.ko", "failover.ko", "e1000.ko",
             "af_packet.ko", "virtio_console.ko", "ext2.ko", "ext4.ko",
             "fat.ko", "vfat.ko", "nls_cp437.ko", "nls_iso8859-1.ko", "nls_utf8.ko",
             "crc32c_generic.ko"):
    add(root)
# workstation / mobile：DRM simpledrm 显卡栈。无 legacy vesafb（CONFIG_FB_VESA 不存在），
# 靠 isolinux vga=0x342/0x343 进入 VESA 32bpp 图形模式，内核 CONFIG_SYSFB_SIMPLEFB
# 依据 screen_info 创建 simple-framebuffer 设备，simpledrm 绑定后出 /dev/fb0
if os.environ.get("E1OS_VARIANT") in ("workstation", "mobile"):
    for root in ("simpledrm.ko", "drm_shmem_helper.ko", "syscopyarea.ko",
                 "sysfillrect.ko", "sysimgblt.ko", "fb_sys_fops.ko",
                 "drm_kms_helper.ko", "drm.ko", "fb.ko", "i2c-core.ko",
                 "evdev.ko", "psmouse.ko",
                 "usbhid.ko", "hid.ko", "hid-generic.ko",
                 "usbcore.ko", "usb-common.ko",
                 # USB 主机控制器（VirtualBox USB tablet：piix3 用 OHCI，需 PCI 胶水层）
                 "uhci-hcd.ko", "ohci-hcd.ko", "ehci-hcd.ko",
                 "ohci-pci.ko", "ehci-pci.ko",
                 "ehci-platform.ko", "ohci-platform.ko"):
        add(root)
# 从 apk 解出 .ko.gz（源 modules.dep 里的路径即包内路径）
apk_root = src.split("lib/modules/")[0]
gz_members = ["lib/modules/%s/%s.gz" % (os.path.basename(dst), e[0]) for e in
              (entries[b] for b in sorted(want))]
subprocess.run(["tar", "-xzf", apk, "-C", apk_root] + gz_members, check=True)
out = []
for b in sorted(want):
    rel, deps = entries[b]
    d = os.path.join(dst, rel)
    os.makedirs(os.path.dirname(d), exist_ok=True)
    with gzip.open(os.path.join(src, rel + ".gz"), "rb") as fi:
        with open(d, "wb") as fo:
            shutil.copyfileobj(fi, fo)   # gunzip 模块文件
    out.append(rel + ": " + " ".join(deps))
os.makedirs(dst, exist_ok=True)
open(os.path.join(dst, "modules.dep"), "w").write("\n".join(out) + "\n")
# modules.alias：内核 request_module("crc32c") 等运行时别名 → busybox modprobe 需要
open(os.path.join(dst, "modules.alias"), "w").write(
    "alias crc32c crc32c_generic\n"
    "alias crypto-crc32c crc32c_generic\n"
)
print(f"mkmodules: {len(want)} 个模块 -> {dst}")
PY
else
    msg "linux-virt.apk 中未找到 modules.dep，跳过模块提取"
fi
rm -rf "$DL/lvsrc"

# ---------------------------------------------------------------- 自研图形界面
# 拼音字典头文件（若 Unihan 数据在 /tmp 可用则重新生成，否则沿用已提交的版本）
if [ -d /tmp/Unihan_data ] && ! [ -f src/fbui/pinyin_dict.h ]; then
    python3 tools/gen_pinyin.py || msg "gen_pinyin 失败，沿用旧字典"
fi
# e1wm 源码含新中文文案时先重生成 CJK 字库（含拼音字典的 3873 字）
if [ -f "$DL/unifont.hex" ]; then
    python3 tools/mkfont.py || msg "mkfont 失败，沿用旧字库"
fi
# e1wm: framebuffer 桌面（静态编译，零运行时依赖），workstation 版核心
# 交叉编译优先用 zig cc（自带 musl + Linux 头，单文件下载即可用）；
# 回退 musl.cc 交叉工具链
ZIG="$DL/zig/zig"
GCC_X="$DL/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc"
if [ ! -x "$ZIG" ]; then
    msg "下载 zig 交叉编译器（编译自研 e1wm 图形界面，约 45MB）"
    if curl -fL -C - --retry 2 -o "$DL/zig.tar.xz" \
        https://ziglang.org/download/0.11.0/zig-macos-x86_64-0.11.0.tar.xz; then
        mkdir -p "$DL/zig"
        tar -xJf "$DL/zig.tar.xz" -C "$DL/zig" --strip-components 1
    else
        rm -f "$DL/zig.tar.xz"
        msg "zig 下载失败，回退 musl.cc 工具链"
    fi
fi
if [ ! -x "$ZIG" ] && [ ! -x "$GCC_X" ]; then
    msg "下载 musl 交叉工具链（备选，约 90MB）"
    if curl -fL -C - --retry 2 -o "$DL/musl-cross.tgz" \
        https://musl.cc/x86_64-linux-musl-cross.tgz; then
        tar -xzf "$DL/musl-cross.tgz" -C "$DL/"
    else
        rm -f "$DL/musl-cross.tgz"
        msg "工具链下载失败，本次构建不含 e1wm"
    fi
fi
rm -f rootfs/usr/bin/e1wm
if keep_cc usr/bin/e1wm; then
    :
elif [ -x "$ZIG" ]; then
    msg "交叉编译自研图形界面 e1wm (zig cc + musl 静态, $ARCH)"
    "$ZIG" cc -target "$ZTARGET" -Os -s -static \
        -o rootfs/usr/bin/e1wm src/fbui/e1wm.c || die "e1wm 编译失败"
elif [ -x "$GCC_X" ]; then
    msg "交叉编译自研图形界面 e1wm (musl-gcc 静态)"
    "$GCC_X" -Os -s -static -o rootfs/usr/bin/e1wm src/fbui/e1wm.c \
        || die "e1wm 编译失败"
elif [ "$NEED_GFX" = 1 ]; then
    die "$VARIANT 变体需要 e1wm，但交叉编译器不可用"
else
    msg "无可用交叉编译器，本次构建不含 e1wm"
fi
if [ -f rootfs/usr/bin/e1wm ]; then
    chmod +x rootfs/usr/bin/e1wm
fi

# ---------------------------------------------------------------- e1wine（Wine 变种，Win32 PE 兼容层）
# 纯 C，无图形依赖，全变体内置：可在 e1LibreOS 上运行直接调用 kernel32 的 PE32+ EXE。
# Linux(musl 静态) 与 macOS(原生) 共用同一源码，由 tools/build-e1wine.sh 切换目标。
rm -f rootfs/usr/bin/e1wine
if keep_cc usr/bin/e1wine; then
    :
elif [ -x "$ZIG" ]; then
    msg "交叉编译 e1wine Linux 版 (zig cc + musl 静态, $ARCH)"
    EW_TARGET=linux; [ "$ARCH" = aarch64 ] && EW_TARGET=linux-arm64
    sh tools/build-e1wine.sh "$EW_TARGET" rootfs/usr/bin/e1wine || die "e1wine 编译失败"
    # 构建演示用 PE32+ EXE（直接调 kernel32，无 CRT 依赖，随镜像分发；x86_64 PE）
    mkdir -p rootfs/usr/share/e1wine
    "$ZIG" cc -target x86_64-windows-gnu -nostdlib \
        -Wl,-e,entry -Wl,--subsystem,console -lkernel32 \
        -o rootfs/usr/share/e1wine/hello.exe src/e1wine/hello-pe.c 2>/dev/null \
        || msg "演示 PE hello.exe 构建失败（不影响系统）"
    rm -f rootfs/usr/share/e1wine/hello.pdb
elif [ "$ARCH" = x86_64 ] && [ -x "$GCC_X" ]; then
    msg "交叉编译 e1wine Linux 版 (musl-gcc 静态)"
    "$GCC_X" -Os -s -static -DE1WINE_PLATFORM='"linux"' \
        -o rootfs/usr/bin/e1wine src/e1wine/e1wine.c || die "e1wine 编译失败"
else
    msg "无交叉编译器，本次构建不含 e1wine"
fi
[ -f rootfs/usr/bin/e1wine ] && chmod +x rootfs/usr/bin/e1wine

# ---------------------------------------------------------------- e1wxfly（应用包装工具，Python 脚本）
# 从 e1wine 分离出来的独立工具：把 EXE/X11 应用包装为 ELF/.app/自解压脚本
rm -f rootfs/usr/bin/e1wxfly
if [ -f src/e1wxfly/e1wxfly ]; then
    cp src/e1wxfly/e1wxfly rootfs/usr/bin/e1wxfly
    chmod +x rootfs/usr/bin/e1wxfly
    msg "安装 e1wxfly 应用包装工具"
fi

# ---------------------------------------------------------------- e1gpt（安装器 GPT 分区工具）
# setup-e1os 的 custom/sys(EFI)/data 模式用它写保护性 MBR + GPT（ESP/Linux/swap 类型 GUID）
rm -f rootfs/usr/sbin/e1gpt
if keep_cc usr/sbin/e1gpt; then
    :
elif [ -x "$ZIG" ]; then
    msg "交叉编译 e1gpt 分区工具 (zig cc + musl 静态, $ARCH)"
    "$ZIG" cc -target "$ZTARGET" -Os -s -static \
        -o rootfs/usr/sbin/e1gpt src/e1gpt/e1gpt.c || die "e1gpt 编译失败"
elif [ "$ARCH" = x86_64 ] && [ -x "$GCC_X" ]; then
    "$GCC_X" -Os -s -static -o rootfs/usr/sbin/e1gpt src/e1gpt/e1gpt.c || die "e1gpt 编译失败"
fi
[ -f rootfs/usr/sbin/e1gpt ] && chmod +x rootfs/usr/sbin/e1gpt

# ---------------------------------------------------------------- e1apk（APK 工具 + AXML 清单解析）
# 自研 ZIP 解包（免 zlib）+ 二进制 AXML 清单解析；生成演示 APK 随 ISO 分发
if keep_cc usr/bin/e1apk; then
    :
elif [ -x "$ZIG" ]; then
    msg "交叉编译 e1apk APK 工具 (zig cc + musl 静态, $ARCH)"
    "$ZIG" cc -target "$ZTARGET" -Os -s -static \
        -DE1APK_PLATFORM='"linux"' \
        -o rootfs/usr/bin/e1apk src/e1apk/e1apk.c || die "e1apk 编译失败"
    mkdir -p rootfs/usr/share/e1apk
    python3 tools/mkdemoapk.py rootfs/usr/share/e1apk/hello.apk \
        || msg "演示 APK 构建失败（不影响系统）"
fi
[ -f rootfs/usr/bin/e1apk ] && chmod +x rootfs/usr/bin/e1apk

# ---------------------------------------------------------------- 组装 rootfs
msg "组装 stage 根文件系统"
# 统一修复执行权限（init 脚本无 +x 会导致 busybox init 无法启动系统）
chmod +x rootfs/init rootfs/etc/rc.init rootfs/etc/rc.shutdown \
        rootfs/bin/autologin rootfs/usr/bin/e1pkg rootfs/usr/bin/e1demo \
        rootfs/usr/bin/e1wm-start rootfs/usr/bin/e1wxfly \
        rootfs/etc/udhcpc.script rootfs/usr/sbin/setup-e1os \
        rootfs/usr/sbin/e1os-commit rootfs/usr/sbin/e1gpt 2>/dev/null || true

rm -rf "$BUILD/stage"
cp -R rootfs "$BUILD/stage"

# 变体品牌写入 os-release（setup-e1os / e1demo 据此决定默认安装 profile）
if [ "$NEED_GFX" = 1 ]; then
    # workstation / mobile 共用 e1wm 图形界面，仅品牌字串与模式开关不同
    sed -i '' -e "s/^VERSION=.*/VERSION=\"1.0 (Libre) $VN\"/" \
              -e "s/^PRETTY_NAME=.*/PRETTY_NAME=\"e1LibreOS 1.0 (Libre) $VN\"/" \
        "$BUILD/stage/etc/os-release"
    # Live 模式 tty1 直接进入自研图形界面 e1wm（磁盘安装版由 setup-e1os 负责），
    # tty2 保留文字控制台（e1pkg 窗口的操作提示指向 Alt+F2）
    awk '{ if ($0 ~ /^tty1::respawn:/ && !done) {
               print "tty1::respawn:/usr/bin/e1wm"
               print "tty2::respawn:/sbin/getty 38400 tty2"
               done = 1
           } else print }' \
        "$BUILD/stage/etc/inittab" > "$BUILD/stage/etc/inittab.new" \
        && mv "$BUILD/stage/etc/inittab.new" "$BUILD/stage/etc/inittab"
    # mobile 专属：e1wm 启动时读 /etc/e1mode 启用移动模式（大字体 + 虚拟键盘）
    if [ "$VARIANT" = mobile ]; then
        printf 'mobile\n' > "$BUILD/stage/etc/e1mode"
    fi
fi
printf 'VARIANT=%s\nVARIANT_ID=%s\n' "$VN" "$VARIANT" >> "$BUILD/stage/etc/os-release"

# busybox + 关键符号链接（/init 的 shebang 依赖 /bin/sh）
cp "$DL/busybox-$ARCH" "$BUILD/stage/bin/busybox"
chmod 755 "$BUILD/stage/bin/busybox"
ln -sf busybox "$BUILD/stage/bin/sh"
mkdir -p "$BUILD/stage/sbin"
ln -sf /bin/busybox "$BUILD/stage/sbin/init"

# 预创建运行时目录
mkdir -p "$BUILD/stage/root" "$BUILD/stage/home" \
         "$BUILD/stage/var/lib/e1pkg/installed" \
         "$BUILD/stage/var/cache/e1pkg" \
         "$BUILD/stage/var/log" "$BUILD/stage/tmp"
chmod 1777 "$BUILD/stage/tmp" "$BUILD/stage/var/tmp" 2>/dev/null || true

# ---------------------------------------------------------------- initramfs
msg "生成 initramfs (newc cpio + gzip)"
python3 tools/mkinitramfs.py "$BUILD/stage" "$BUILD/initramfs-e1.cpio"
gzip -9 -cf "$BUILD/initramfs-e1.cpio" > "$BUILD/initramfs-e1.gz"
rm -f "$BUILD/initramfs-e1.cpio"

cp "$DL/vmlinuz-virt-$ARCH" "$BUILD/vmlinuz-virt-$ARCH"

# ---------------------------------------------------------------- 引导产物
if [ "$ARCH" = x86_64 ]; then
# x86_64：ISO（isolinux + El Torito，isohybrid 后可 dd 到 U 盘）
MKISOFS=""
command -v mkisofs >/dev/null 2>&1 && MKISOFS=mkisofs
for m in brew/cdrtools/*/bin/mkisofs; do [ -x "$m" ] && MKISOFS="$m"; done
if [ -n "$MKISOFS" ] && [ -f "$DL/isolinux.bin" ]; then
    msg "生成可启动 ISO (isolinux + El Torito)"
    rm -rf "$BUILD/isostage"
    mkdir -p "$BUILD/isostage/isolinux" "$BUILD/isostage/boot"
    cp "$DL/isolinux.bin" "$DL/ldlinux.c32" "$BUILD/isostage/isolinux/"
    cp "$BUILD/vmlinuz-virt-$ARCH"  "$BUILD/isostage/boot/vmlinuz-virt"
    cp "$BUILD/initramfs-e1.gz" "$BUILD/isostage/boot/initramfs-e1.gz"
    cat > "$BUILD/isostage/isolinux/isolinux.cfg" <<EOF
DEFAULT linux
PROMPT 0
TIMEOUT 20

LABEL linux
  MENU LABEL ^e1LibreOS 1.0 (Libre) $VN — Live
  KERNEL /boot/vmlinuz-virt
EOF
    # workstation / mobile：VESA 32bpp 图形模式（vga=0x200|mode），图形模式是
    # simpledrm/simple-framebuffer 出现的前提；server 保持文本控制台
    #   workstation: vga=0x342 (VESA 0x142 640x480x32)
    #   mobile:      vga=0x343 (VESA 0x143 800x600x32 平板触屏）
    if [ "$VARIANT" = workstation ]; then
        KAPPEND="vga=0x342 console=tty0 console=ttyS0,115200${E1OS_AUTOFLAG:-}"
        printf '  APPEND initrd=/boot/initramfs-e1.gz %s\n' "$KAPPEND" >> "$BUILD/isostage/isolinux/isolinux.cfg"
    elif [ "$VARIANT" = mobile ]; then
        KAPPEND="vga=0x343 console=tty0 console=ttyS0,115200${E1OS_AUTOFLAG:-}"
        printf '  APPEND initrd=/boot/initramfs-e1.gz %s\n' "$KAPPEND" >> "$BUILD/isostage/isolinux/isolinux.cfg"
    else
        KAPPEND="console=tty0 console=ttyS0,115200${E1OS_AUTOFLAG:-}"
        printf '  APPEND initrd=/boot/initramfs-e1.gz %s\n' "$KAPPEND" >> "$BUILD/isostage/isolinux/isolinux.cfg"
    fi

    # ---- UEFI 双引导：super-floppy FAT16 El Torito EFI 引导镜像 ----
    # Ubuntu 签名 grubx64.efi（PE32+，含 iso9660/search/linux 模块）位于
    # 镜像内 /EFI/BOOT/BOOTX64.EFI；grub.cfg 先 search 光盘卷
    # 再加载 /boot 下内核（与 BIOS 共用同一份内核）。
    # 布局与 Alpine/xorriso 一致：FAT 文件系统从扇区 0 开始，MBR 分区表
    # 只有一个起始 LBA=0 的覆盖分区（super-floppy）。固件将 El Torito
    # EFI 项映射为 RAM 盘后直接挂载其文件系统，FAT 必须位于 LBA0。
    EFI_ARGS=""
    # grub 二进制选择：Alpine 版 grubx64-alpine.efi 实测可在 VBox/OVMF 直接
    # 引导（Ubuntu 签名版 grubx64.efi 在部分 OVMF 上 StartImage 即崩溃黑屏）；
    # 可用 E1OS_GRUB_BIN 覆盖
    GRUB_BIN="${E1OS_GRUB_BIN:-$DL/grub/grubx64-alpine.efi}"
    [ -f "$GRUB_BIN" ] || GRUB_BIN="$DL/grub/grubx64.efi"
    if [ -f "$GRUB_BIN" ]; then
        msg "生成 UEFI El Torito 引导镜像 (super-floppy FAT16 + $(basename "$GRUB_BIN"))"
        EFI_STAGE="$BUILD/efistage"
        rm -rf "$EFI_STAGE"; mkdir -p "$EFI_STAGE/EFI/BOOT" "$EFI_STAGE/boot/grub"
        cp "$GRUB_BIN" "$EFI_STAGE/EFI/BOOT/BOOTX64.EFI"
        cat > "$EFI_STAGE/EFI/BOOT/GRUB.CFG" <<EOF
set timeout=2
set default=0
search --no-floppy --file /boot/vmlinuz-virt --set=root || search --no-floppy --file /EFI/BOOT/BOOTX64.EFI --set=root

menuentry "e1LibreOS 1.0 (Libre) $VN — Live" {
    linux /boot/vmlinuz-virt $KAPPEND
    initrd /boot/initramfs-e1.gz
}
EOF
        cp "$EFI_STAGE/EFI/BOOT/GRUB.CFG" "$EFI_STAGE/boot/grub/GRUB.CFG"
        mkdir -p "$BUILD/isostage/boot"
        if [ -n "${E1OS_EFI_IMG:-}" ] && [ -f "$E1OS_EFI_IMG" ]; then
            # A/B 调试：直接使用外部现成 EFI 引导镜像
            cp "$E1OS_EFI_IMG" "$BUILD/isostage/boot/efi.img"
            msg "使用外部 EFI 镜像: $E1OS_EFI_IMG"
        else
        # 16MiB：簇数 8167 ≥ 4085，才是合规 FAT16（4MiB 卷只有 ~2000 簇，
        # 固件按簇数判定为 FAT12，簇链解析失败导致 Bds 报 Not Found）
        python3 tools/mkfat32.py "$EFI_STAGE" "$BUILD/isostage/boot/efi.img" \
            --size-mb 16 --fstype 16 --hidden 0 --label E1OS_EFI
        # 补写 super-floppy MBR 分区项：active + 类型0x06(FAT16) + 起始0 + 全镜像
        python3 - "$BUILD/isostage/boot/efi.img" <<'PY'
import sys, struct
p = sys.argv[1]
with open(p, "r+b") as f:
    f.seek(0, 2); nsec = f.tell() // 512
    f.seek(446)
    pe = bytes([0x80, 0x00, 0x01, 0x00, 0x06, 0xFE, 0xFF, 0xFF])
    pe += struct.pack("<I", 0) + struct.pack("<I", nsec)
    f.write(pe)
    f.seek(510); f.write(b"\x55\xAA")
print(f"super-floppy EFI 镜像: {nsec*512//1048576} MiB, FAT16@LBA0")
PY
        fi
        # 另在 ISO9660 树放一份引导程序与配置：支持直接解析光盘文件系统
        # 的固件，同时覆盖 grub 内嵌 prefix=/boot/grub 的查找路径
        mkdir -p "$BUILD/isostage/EFI/BOOT" "$BUILD/isostage/boot/grub"
        cp "$GRUB_BIN" "$BUILD/isostage/EFI/BOOT/BOOTX64.EFI"
        cp "$EFI_STAGE/EFI/BOOT/GRUB.CFG" "$BUILD/isostage/EFI/BOOT/GRUB.CFG"
        cp "$EFI_STAGE/EFI/BOOT/GRUB.CFG" "$BUILD/isostage/EFI/BOOT/grub.cfg"
        cp "$EFI_STAGE/EFI/BOOT/GRUB.CFG" "$BUILD/isostage/boot/grub/grub.cfg"
        rm -rf "$EFI_STAGE"
        EFI_ARGS="-eltorito-alt-boot -eltorito-platform 0xEF -eltorito-boot boot/efi.img -no-emul-boot"
    else
        msg "缺少 downloads/grub/grubx64.efi，ISO 将仅支持 BIOS"
    fi

    "$MKISOFS" -quiet -o "$BUILD/e1LibreOS-1.0-$VARIANT-x86_64.iso" \
        -b isolinux/isolinux.bin -c isolinux/boot.cat \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        $EFI_ARGS \
        -J -R -V "e1LibreOS_1.0_${VN}" "$BUILD/isostage"
    # 写入 isohybrid MBR，使其可 dd 到 U 盘引导
    # E1OS_HYBRID=0 可跳过（混合 MBR 会让部分 UEFI 固件把光盘误判为分区硬盘）
    if [ "${E1OS_HYBRID:-1}" = 1 ]; then
        python3 tools/isohybrid.py "$BUILD/e1LibreOS-1.0-$VARIANT-x86_64.iso" "$DL/isohdpfx.bin" \
            && msg "已写入 isohybrid MBR（可 dd 到 U 盘）"
    fi
    rm -rf "$BUILD/isostage"
else
    msg "未找到 mkisofs，跳过 ISO（brew install cdrtools 后重新构建即可）"
fi
else
# aarch64：GPT + FAT32 ESP EFI 原始磁盘镜像（在 Intel Mac 上交叉构建，标注 UNTESTED）
if [ -f "$DL/grub/grubaa64.efi" ]; then
    msg "生成 aarch64 UEFI 磁盘镜像 (GPT + ESP + grubaa64.efi) [UNTESTED]"
    sh tools/build-arm-image.sh \
        "$BUILD/vmlinuz-virt-$ARCH" "$BUILD/initramfs-e1.gz" \
        "$DL/grub/grubaa64.efi" "$VARIANT" \
        "$BUILD/e1LibreOS-1.0-$VARIANT-aarch64.img"
else
    die "缺少 downloads/grub/grubaa64.efi，无法生成 aarch64 镜像"
fi
fi

# ---------------------------------------------------------------- 完成
msg "构建完成，产物在 $BUILD/:"
ls -lh "$BUILD" | awk 'NR>1 {printf "    %-24s %s\n", $NF, $5}'
cat <<EOF

启动方式:
  1) 直接内核启动:  ./run.sh        （需要 qemu-system-x86_64）
  2) ISO 启动:      ./run.sh iso    （若已生成 ISO）
  3) VirtualBox:    ./run-vbox.sh   （macOS 已验证）
  4) 真机/U 盘:     dd if=$BUILD/e1LibreOS-1.0-$VARIANT-x86_64.iso of=/dev/你的U盘

变体:
  ./build.sh                                  -> Server 版（命令行，x86_64 ISO）
  E1OS_VARIANT=workstation ./build.sh         -> Workstation 版（自研 e1wm 图形界面）
  E1OS_VARIANT=mobile ./build.sh              -> Mobile 版（800x600 触屏 + 虚拟键盘）
  E1OS_ARCH=aarch64 ./build.sh                -> aarch64 UEFI 镜像（.img，交叉构建，UNTESTED）
    两者可组合: E1OS_ARCH=aarch64 E1OS_VARIANT=workstation ./build.sh
  安装器 setup-e1os（问答与 setup-alpine 同款）支持 sys/data/custom/preserve/diskless

在虚拟机里登录后可试:
  e1pkg install e1fetch && e1fetch
EOF
