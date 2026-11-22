#!/bin/sh
# tools/build-glibc-image.sh — glibc + systemd 分支构建（server / workstation）
#
# 与 build.sh 的 Alpine(musl) 路径完全独立：
#   server      glibc + systemd，无图形
#   workstation glibc + systemd + e1wm（静态 musl 二进制，随镜像内置）
#
# 根文件系统来源：Arch Linux bootstrap（glibc + systemd 官方最小基础）
# 根 = initramfs（整树 cpio + gzip）；/init 先挂载 proc/sys/dev/run，
# 再 exec systemd 作为 PID 1（不依赖 mkinitcpio/udev 找块设备）。
set -eu

VARIANT=${1:?用法: build-glibc-image.sh server|workstation}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build
DL=$ROOT/downloads
VER=2.0

case "$VARIANT" in
    server)      VN=Server ;;
    workstation) VN=Workstation ;;
    *) echo "错误: 仅支持 server / workstation" >&2; exit 2 ;;
esac

msg() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[31m错误:\033[0m %s\n' "$*" >&2; exit 1; }

WORK=$BUILD/glibc-$VARIANT
ROOTFS=$WORK/rootfs
BOOTSTRAP=$DL/arch-bootstrap.tar.zst
URL=https://mirrors.aliyun.com/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.zst

command -v zstd >/dev/null 2>&1 || die "需要 zstd（sudo port install zstd，或从 https://github.com/facebook/zstd 源码构建）"

# ---------------------------------------------------------------- bootstrap
if [ ! -s "$BOOTSTRAP" ]; then
    msg "下载 Arch Linux bootstrap（glibc + systemd 基础，约 122 MB）"
    curl -L --retry 3 -o "$BOOTSTRAP.part" "$URL" || die "下载失败: $URL"
    mv "$BOOTSTRAP.part" "$BOOTSTRAP"
fi

msg "解包 bootstrap -> $WORK/rootfs"
# bootstrap 内含大量只读目录/文件（0444/0555），macOS 上删除需先恢复属主写权限
chmod -R u+rwX "$WORK" 2>/dev/null || true
rm -rf "$WORK"
mkdir -p "$ROOTFS"
# macOS 大小写不敏感卷会让 terminfo 里的同名大小写硬链接报错，该目录随后整体删除
zstd -dc "$BOOTSTRAP" | tar -xf - -C "$ROOTFS" --strip-components=1 2>/dev/null || true
chmod -R u+rwX "$ROOTFS" 2>/dev/null || true
[ -x "$ROOTFS/usr/lib/systemd/systemd" ] || die "bootstrap 解包不完整（缺少 systemd）"

# ---------------------------------------------------------------- 裁剪
msg "裁剪 rootfs（文档/本地化/开发文件/ICU/hwdb）"
rm -rf "$ROOTFS/usr/share/locale" "$ROOTFS/usr/share/man" \
       "$ROOTFS/usr/share/doc" "$ROOTFS/usr/share/i18n" \
       "$ROOTFS/usr/share/gir-1.0" "$ROOTFS/usr/share/hwdata" \
       "$ROOTFS/usr/share/file" "$ROOTFS/usr/share/terminfo" \
       "$ROOTFS/usr/share/zoneinfo" \
       "$ROOTFS/usr/lib/libc.a" "$ROOTFS/usr/lib/gconv" \
       "$ROOTFS/usr/lib/libicu"* "$ROOTFS/usr/lib/udev/hwdb.bin" \
       "$ROOTFS/usr/bin/xgettext" "$ROOTFS/var/lib/pacman/sync"
rm -rf "$ROOTFS/var/cache"/* 2>/dev/null || true

# ---------------------------------------------------------------- 品牌/启动配置
msg "写入 e1LibreOS 品牌与引导配置"
cat > "$ROOTFS/usr/lib/os-release" <<EOF
NAME="e1LibreOS"
VERSION="$VER (Libre) $VN"
ID=e1libreos
VERSION_ID="$VER"
VERSION_CODENAME="libre"
PRETTY_NAME="e1LibreOS $VER (Libre) $VN"
ANSI_COLOR="0;34"
HOME_URL="https://e1libreos.example.org"
SUPPORT_URL="https://e1libreos.example.org/support"
BUG_REPORT_URL="https://e1libreos.example.org/bugzilla"
VARIANT=$VN
VARIANT_ID=$VARIANT
EOF
rm -f "$ROOTFS/etc/os-release"
ln -s ../usr/lib/os-release "$ROOTFS/etc/os-release"
printf 'e1libreos\n' > "$ROOTFS/etc/hostname"
# bootstrap 里的 machine-id / fstab 是只读文件，先删再建空文件
rm -f "$ROOTFS/etc/machine-id" "$ROOTFS/etc/fstab"
: > "$ROOTFS/etc/machine-id"
: > "$ROOTFS/etc/fstab"
rm -f "$ROOTFS/etc/locale.conf"
printf 'LANG=C.UTF-8\n' > "$ROOTFS/etc/locale.conf"

# initramfs 作为根：systemd 需要 /dev 由 devtmpfs 预挂载
cat > "$ROOTFS/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mount -t tmpfs tmpfs /run 2>/dev/null
exec /usr/lib/systemd/systemd
EOF
chmod 0755 "$ROOTFS/init"

# ---------------------------------------------------------------- 内核模块
# initramfs 直接作为根：网卡（e1000/virtio_net）、文件系统等驱动必须随镜像带上，
# 否则 e1pkg 与 SSH 无法联网。
MODDIR=$(ls -d "$BUILD"/stage/lib/modules/* 2>/dev/null | head -1 || true)
[ -n "$MODDIR" ] || die "缺少内核模块目录（先构建 desktop 分支以解出内核模块）"
KVER=$(basename "$MODDIR")
mkdir -p "$ROOTFS/usr/lib/modules"
cp -R "$MODDIR" "$ROOTFS/usr/lib/modules/$KVER"

# ---------------------------------------------------------------- 免登录
# 与 minimal/desktop/mobile 的 Live 语义一致：启动即给 root shell（串口 getty）
mkdir -p "$ROOTFS/etc/systemd/system/serial-getty@ttyS0.service.d"
cat > "$ROOTFS/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/usr/bin/agetty --autologin root --keep-baud 115200,38400,9600 %I $TERM
EOF

# server：集成 e1pkg（与 minimal/desktop/mobile 同一份源配置，公网 IPv6 源排第一）
if [ "$VARIANT" = server ]; then
    msg "服务器：集成 e1pkg 软件包管理器"
    cat "$ROOT/rootfs/usr/bin/e1pkg" > "$ROOTFS/usr/bin/e1pkg"
    chmod 0755 "$ROOTFS/usr/bin/e1pkg"
    cat "$ROOT/rootfs/etc/e1pkg.conf" > "$ROOTFS/etc/e1pkg.conf"

    # 网络：systemd-networkd + DHCP，否则 e1pkg 装完无法联网拉包
    mkdir -p "$ROOTFS/etc/systemd/network"
    cat > "$ROOTFS/etc/systemd/network/20-e1-dhcp.network" <<'EOF'
[Match]
Name=en* eth*

[Network]
DHCP=yes
EOF
    mkdir -p "$ROOTFS/etc/systemd/system/multi-user.target.wants"
    ln -sf /usr/lib/systemd/system/systemd-networkd.service \
        "$ROOTFS/etc/systemd/system/multi-user.target.wants/systemd-networkd.service"
    ln -sf /usr/lib/systemd/system/systemd-resolved.service \
        "$ROOTFS/etc/systemd/system/multi-user.target.wants/systemd-resolved.service"
    rm -f "$ROOTFS/etc/resolv.conf"
    ln -sf /run/systemd/resolve/stub-resolv.conf "$ROOTFS/etc/resolv.conf"

    # 该 Arch bootstrap 的 kmod 在 initramfs 内解析纯文本 modules.dep 不可靠
    # （desktop/mobile 分支同样问题，已用静态 busybox 解决）。
    # 注意不能把 modprobe 放进 systemd-networkd 的 ExecStartPre：该单元
    # 有 CapabilityBoundingSet 限制，busybox modprobe 会 chdir 失败。
    # 因此用独立 oneshot 服务，在 networkd 启动前加载常见网卡驱动。
    cat "$ROOT/downloads/busybox-x86_64" > "$ROOTFS/usr/bin/busybox-e1"
    chmod 0755 "$ROOTFS/usr/bin/busybox-e1"
    cat > "$ROOTFS/etc/systemd/system/e1-net-modules.service" <<'EOF'
[Unit]
Description=e1LibreOS network kernel modules
DefaultDependencies=no
After=systemd-udevd.service
Before=systemd-networkd.service network-pre.target
Conflicts=shutdown.target

[Service]
Type=oneshot
ExecStart=-/usr/bin/busybox-e1 modprobe af_packet
ExecStart=-/usr/bin/busybox-e1 modprobe e1000
ExecStart=-/usr/bin/busybox-e1 modprobe e1000e
ExecStart=-/usr/bin/busybox-e1 modprobe virtio_net
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
    ln -sf ../e1-net-modules.service \
        "$ROOTFS/etc/systemd/system/multi-user.target.wants/e1-net-modules.service"
fi

# server：tty1 同样自动登录（workstation 的 tty1 留给 e1wm）
if [ "$VARIANT" = server ]; then
    mkdir -p "$ROOTFS/etc/systemd/system/getty@tty1.service.d"
    cat > "$ROOTFS/etc/systemd/system/getty@tty1.service.d/autologin.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/usr/bin/agetty --autologin root --noclear %I $TERM
EOF
fi

# ---------------------------------------------------------------- workstation
if [ "$VARIANT" = workstation ]; then
    msg "工作站：内置 e1wm 图形界面"
    E1WM=$BUILD/cc-cache/x86_64/usr/bin/e1wm
    [ -f "$E1WM" ] || die "缺少 $E1WM（先构建 desktop 分支以生成 e1wm）"
    install -m 0755 "$E1WM" "$ROOTFS/usr/bin/e1wm"
    # 静态 busybox 提供 modprobe：initramfs 里 kmod 解析纯文本 modules.dep 不可靠，
    # busybox 版本已在 desktop/mobile 分支验证可用。
    # 注意：macOS 的 install(1)/cp 走 fcopyfile，磁盘紧张时会超时，这里用 cat 直写。
    cat "$ROOT/downloads/busybox-x86_64" > "$ROOTFS/usr/bin/busybox-e1"
    chmod 0755 "$ROOTFS/usr/bin/busybox-e1"

    mkdir -p "$ROOTFS/etc/systemd/system/multi-user.target.wants"
    cat > "$ROOTFS/etc/systemd/system/e1wm.service" <<EOF
[Unit]
Description=e1wm framebuffer desktop
Conflicts=getty@tty1.service
After=systemd-modules-load.service

[Service]
ExecStartPre=-/usr/bin/busybox-e1 modprobe simpledrm
ExecStart=/usr/bin/e1wm
Restart=always
RestartSec=2
# e1wm 通过 stdin 读键盘并 ioctl(0, KDSETMODE) 切图形模式，必须绑定 tty1
StandardInput=tty
TTYPath=/dev/tty1
StandardOutput=journal

[Install]
WantedBy=multi-user.target
EOF
    ln -sf ../e1wm.service "$ROOTFS/etc/systemd/system/multi-user.target.wants/e1wm.service"
    # tty1 让给图形界面，避免 getty 抢控制台
    ln -sf /dev/null "$ROOTFS/etc/systemd/system/getty@tty1.service"
fi

# ---------------------------------------------------------------- initramfs/内核
msg "生成 initramfs（整树 newc cpio + gzip，systemd 直接作为 PID 1）"
python3 "$ROOT/tools/mkinitramfs.py" "$ROOTFS" "$WORK/initramfs-e1.cpio"
gzip -9 -cf "$WORK/initramfs-e1.cpio" > "$WORK/initramfs-e1.gz"
rm -f "$WORK/initramfs-e1.cpio"
cp "$DL/vmlinuz-virt-x86_64" "$WORK/vmlinuz-virt-$VARIANT"

# ---------------------------------------------------------------- ISO
# mkisofs（cdrtools）或 xorrisofs（xorriso）参数兼容，任一存在即可
MKISOFS=""
# 查找顺序：仓库内源码构建(host-tools) → MacPorts(/opt/local) → PATH
for m in "$ROOT/build/host-tools/bin/mkisofs" /opt/local/bin/mkisofs; do
    if [ -z "$MKISOFS" ] && [ -x "$m" ]; then
        MKISOFS=$m
    fi
done
for c in mkisofs xorrisofs; do
    if [ -z "$MKISOFS" ] && command -v "$c" >/dev/null 2>&1; then
        MKISOFS=$(command -v "$c")
    fi
done
[ -n "$MKISOFS" ] || die "缺少 mkisofs（运行 sh tools/build-host-tools.sh，或 sudo port install cdrtools）"
[ -f "$DL/isolinux.bin" ] || die "缺少 downloads/isolinux.bin"

msg "生成可启动 ISO（BIOS isolinux + UEFI El Torito 双引导）"
rm -rf "$WORK/isostage"
mkdir -p "$WORK/isostage/isolinux" "$WORK/isostage/boot"
cp "$DL/isolinux.bin" "$DL/ldlinux.c32" "$WORK/isostage/isolinux/"
cp "$WORK/vmlinuz-virt-$VARIANT"  "$WORK/isostage/boot/vmlinuz-virt"
cp "$WORK/initramfs-e1.gz"        "$WORK/isostage/boot/initramfs-e1.gz"

if [ "$VARIANT" = workstation ]; then
    KAPPEND="vga=0x342 console=tty0 console=ttyS0,115200"
else
    KAPPEND="console=tty0 console=ttyS0,115200"
fi
cat > "$WORK/isostage/isolinux/isolinux.cfg" <<EOF
DEFAULT linux
PROMPT 0
TIMEOUT 20

LABEL linux
  MENU LABEL ^e1LibreOS $VER (Libre) $VN — Live (glibc + systemd)
  KERNEL /boot/vmlinuz-virt
  APPEND initrd=/boot/initramfs-e1.gz $KAPPEND
EOF

# ---- UEFI 双引导：super-floppy FAT16 El Torito EFI 引导镜像 ----
# downloads/grub/grubx64.efi 是 GRUB 2.12 all-modules 生成的 standalone EFI，
# 内嵌 search/menuentry/linux/initrd 启动配置，search 光盘卷上的 /boot/vmlinuz-virt。
# super-floppy 布局：FAT 文件系统从扇区 0 开始，MBR 只有一个起始 LBA=0 的
# 覆盖分区。固件将 El Torito EFI 项映射为 RAM 盘后直接挂载，FAT 必须位于 LBA0。
# 查找顺序与 build.sh 一致：E1OS_GRUB_BIN → grubx64-alpine.efi → grubx64.efi，
# 否则只有 grubx64-alpine.efi 的构建机会静默掉回"仅 BIOS"。
EFI_ARGS=""
GRUB_BIN="${E1OS_GRUB_BIN:-$DL/grub/grubx64-alpine.efi}"
[ -f "$GRUB_BIN" ] || GRUB_BIN="$DL/grub/grubx64.efi"
if [ -f "$GRUB_BIN" ]; then
    msg "生成 UEFI El Torito 引导镜像 (super-floppy FAT16 + $(basename "$GRUB_BIN"))"
    EFI_STAGE="$WORK/efistage"
    rm -rf "$EFI_STAGE"; mkdir -p "$EFI_STAGE/EFI/BOOT" "$EFI_STAGE/boot/grub"
    cp "$GRUB_BIN" "$EFI_STAGE/EFI/BOOT/BOOTX64.EFI"
    cat > "$EFI_STAGE/EFI/BOOT/GRUB.CFG" <<EOF
set timeout=2
set default=0
search --no-floppy --file /boot/vmlinuz-virt --set=root || search --no-floppy --file /EFI/BOOT/BOOTX64.EFI --set=root

menuentry "e1LibreOS $VER (Libre) $VN — Live" {
    linux /boot/vmlinuz-virt $KAPPEND
    initrd /boot/initramfs-e1.gz
}
EOF
    cp "$EFI_STAGE/EFI/BOOT/GRUB.CFG" "$EFI_STAGE/boot/grub/GRUB.CFG"
    mkdir -p "$WORK/isostage/boot"
    # 16MiB：簇数 8167 ≥ 4085，才是合规 FAT16（4MiB 卷只有 ~2000 簇，
    # 固件按簇数判定为 FAT12，簇链解析失败导致 Bds 报 Not Found）
    python3 "$ROOT/tools/mkfat32.py" "$EFI_STAGE" "$WORK/isostage/boot/efi.img" \
        --size-mb 16 --fstype 16 --hidden 0 --label E1OS_EFI
    # 补写 super-floppy MBR 分区项：active + 类型0x06(FAT16) + 起始0 + 全镜像
    python3 - "$WORK/isostage/boot/efi.img" <<'PY'
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
    # 另在 ISO9660 树放一份引导程序与配置：支持直接解析光盘文件系统
    # 的固件，同时覆盖 grub 内嵌 prefix=/boot/grub 的查找路径
    mkdir -p "$WORK/isostage/EFI/BOOT" "$WORK/isostage/boot/grub"
    cp "$GRUB_BIN" "$WORK/isostage/EFI/BOOT/BOOTX64.EFI"
    cp "$EFI_STAGE/EFI/BOOT/GRUB.CFG" "$WORK/isostage/EFI/BOOT/GRUB.CFG"
    cp "$EFI_STAGE/EFI/BOOT/GRUB.CFG" "$WORK/isostage/EFI/BOOT/grub.cfg"
    cp "$EFI_STAGE/EFI/BOOT/GRUB.CFG" "$WORK/isostage/boot/grub/grub.cfg"
    rm -rf "$EFI_STAGE"
    EFI_ARGS="-eltorito-alt-boot -eltorito-platform 0xEF -eltorito-boot boot/efi.img -no-emul-boot"
else
    msg "缺少 $GRUB_BIN，ISO 将仅支持 BIOS"
fi

"$MKISOFS" -quiet -o "$BUILD/e1LibreOS-$VER-$VARIANT-x86_64.iso" \
    -b isolinux/isolinux.bin -c isolinux/boot.cat \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    $EFI_ARGS \
    -J -R -V "e1LibreOS_${VER}_${VN}" "$WORK/isostage"
python3 "$ROOT/tools/isohybrid.py" "$BUILD/e1LibreOS-$VER-$VARIANT-x86_64.iso" \
    "$DL/isohdpfx.bin" || true
rm -rf "$WORK/isostage"

msg "构建完成：$BUILD/e1LibreOS-$VER-$VARIANT-x86_64.iso"
ls -lh "$BUILD/e1LibreOS-$VER-$VARIANT-x86_64.iso"
