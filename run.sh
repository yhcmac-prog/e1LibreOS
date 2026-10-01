#!/bin/sh
# run.sh — 用 QEMU 启动 e1LibreOS
# 用法: ./run.sh [kernel|iso]
cd "$(dirname "$0")" || exit 1

MODE="${1:-kernel}"

case "$MODE" in
    kernel)
        [ -f build/vmlinuz-virt ]   || { echo "错误: 先运行 ./build.sh"; exit 1; }
        [ -f build/initramfs-e1.gz ] || { echo "错误: 先运行 ./build.sh"; exit 1; }
        command -v qemu-system-x86_64 >/dev/null 2>&1 || {
            echo "错误: 未安装 qemu，请运行: brew install qemu"; exit 1; }
        exec qemu-system-x86_64 \
            -m 512 -no-reboot \
            -kernel build/vmlinuz-virt \
            -initrd build/initramfs-e1.gz \
            -append "console=ttyS0,115200" \
            -nographic
        ;;
    iso)
        ISO=build/e1LibreOS-1.0-x86_64.iso
        [ -f "$ISO" ] || { echo "错误: 未找到 $ISO，先运行 ./build.sh"; exit 1; }
        command -v qemu-system-x86_64 >/dev/null 2>&1 || {
            echo "错误: 未安装 qemu，请运行: brew install qemu"; exit 1; }
        exec qemu-system-x86_64 \
            -m 512 -no-reboot \
            -cdrom "$ISO" -boot d \
            -nographic
        ;;
    *)
        echo "用法: ./run.sh [kernel|iso]"; exit 1 ;;
esac
