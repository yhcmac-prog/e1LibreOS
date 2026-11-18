#!/bin/sh
# run.sh — 用 QEMU 启动 e1LibreOS
# 用法: ./run.sh [kernel|iso]
cd "$(dirname "$0")" || exit 1

MODE="${1:-kernel}"

# QEMU 查找顺序：MacPorts(/opt/local) → 仓库内源码构建 → PATH
QEMU=""
for c in /opt/local/bin/qemu-system-x86_64 build/host-tools/bin/qemu-system-x86_64; do
    if [ -x "$c" ]; then QEMU=$c; break; fi
done
if [ -z "$QEMU" ]; then
    QEMU=$(command -v qemu-system-x86_64 2>/dev/null || true)
fi
[ -n "$QEMU" ] || {
    echo "错误: 未找到 qemu-system-x86_64"
    echo "      可用 sudo port install qemu，或从 https://download.qemu.org 源码构建"
    exit 1; }

case "$MODE" in
    kernel)
        [ -f build/vmlinuz-virt ]   || { echo "错误: 先运行 ./build.sh"; exit 1; }
        [ -f build/initramfs-e1.gz ] || { echo "错误: 先运行 ./build.sh"; exit 1; }
        exec "$QEMU" \
            -m 512 -no-reboot \
            -kernel build/vmlinuz-virt \
            -initrd build/initramfs-e1.gz \
            -append "console=ttyS0,115200" \
            -nographic
        ;;
    iso)
        ISO=build/e1LibreOS-2.0-desktop-x86_64.iso
        [ -f "$ISO" ] || ISO=build/e1LibreOS-2.0-minimal-x86_64.iso
        [ -f "$ISO" ] || { echo "错误: 未找到 ISO，先运行 ./build.sh"; exit 1; }
        exec "$QEMU" \
            -m 512 -no-reboot \
            -cdrom "$ISO" -boot d \
            -nographic
        ;;
    *)
        echo "用法: ./run.sh [kernel|iso]"; exit 1 ;;
esac
