#!/bin/sh
# run-vbox.sh — 用 VirtualBox 启动 e1LibreOS ISO（macOS 上已验证可用的路径）
# 用法: ./run-vbox.sh          启动带 GUI 窗口的虚拟机（无盘 Live）
#       ./run-vbox.sh disk     挂 4GB 虚拟硬盘（setup-e1os 安装测试）
#       ./run-vbox.sh clean    注销并删除测试 VM
# 环境变量: E1OS_ISO 指定其他 ISO（如 build/e1LibreOS-1.0-workstation-x86_64.iso）
set -eu
cd "$(dirname "$0")"

ISO=${E1OS_ISO:-$PWD/build/e1LibreOS-1.0-server-x86_64.iso}
[ -f "$ISO" ] || ISO="$PWD/build/e1LibreOS-1.0-x86_64.iso"   # 兼容旧命名
VM=e1LibreOS
VB=VBoxManage
HD="$PWD/.vbox/e1os-test.vdi"

command -v $VB >/dev/null 2>&1 || { echo "错误: 未安装 VirtualBox"; exit 1; }
[ -f "$ISO" ] || { echo "错误: 未找到 $ISO，先运行 ./build.sh"; exit 1; }

if [ "${1:-}" = "clean" ]; then
    $VB controlvm "$VM" poweroff 2>/dev/null || true
    $VB unregistervm "$VM" --delete 2>/dev/null || true
    echo "已清理 VM: $VM"
    exit 0
fi

WITH_DISK=0
[ "${1:-}" = "disk" ] && WITH_DISK=1

mkdir -p "$PWD/.vbox"
: > "$PWD/.vbox/serial.log"   # file 模式为追加写，重测前清空

# 已注册则直接启动，否则创建
if ! $VB list vms | grep -q "\"$VM\""; then
    $VB createvm --name "$VM" --basefolder "$PWD/.vbox" --register
    $VB modifyvm "$VM" --ostype Linux26_64 --memory 512 \
        --audio none --usb off --graphicscontroller vboxvga --boot1 dvd \
        --nic1 nat --nictype1 82540EM --cableconnected1 on \
        --uart1 0x3F8 4 --uart-mode1 file "$PWD/.vbox/serial.log"
    $VB storagectl "$VM" --name IDE --add ide --controller PIIX4
    if [ "$WITH_DISK" = 1 ]; then
        # VirtioSCSI 磁盘（Live 已含 virtio_scsi/scsi_mod/sd_mod 模块）
        $VB storagectl "$VM" --name SCSI --add virtio-scsi
        $VB createmedium disk --filename "$HD" --size 4096 --format VDI
        $VB storageattach "$VM" --storagectl SCSI --port 0 --device 0 \
            --type hdd --medium "$HD"
    fi
elif [ "$WITH_DISK" = 1 ]; then
    $VB storagectl "$VM" --name SCSI --add virtio-scsi 2>/dev/null || true
    if [ ! -f "$HD" ]; then
        # 磁盘文件被删（重置测试环境）→ 先 detach 并注销悬空引用，再重建
        $VB storageattach "$VM" --storagectl SCSI --port 0 --device 0 \
            --type hdd --medium none 2>/dev/null || true
        $VB closemedium disk "$HD" 2>/dev/null || true
        $VB createmedium disk --filename "$HD" --size 4096 --format VDI
    fi
    $VB storageattach "$VM" --storagectl SCSI --port 0 --device 0 \
        --type hdd --medium "$HD"
fi

# 每次启动都刷新 IDE 光驱为当前 ISO（重建/变体切换后保证引导最新镜像）
$VB storageattach "$VM" --storagectl IDE --port 1 --device 0 \
    --type dvddrive --medium "$ISO"

# 通过 LaunchServices 启动（避免沙箱限制 Hypervisor.framework）；
# E1OS_HEADLESS=1 时用 headless 启动（适合自动化截图验证）
if [ "${E1OS_HEADLESS:-}" = "1" ]; then
    $VB startvm "$VM" --type headless
else
    open -na /Applications/VirtualBox.app/Contents/Resources/VirtualBoxVM.app \
        --args --comment "$VM" --startvm "$($VB list vms | grep "\"$VM\"" | sed 's/.*{\(.*\)}/\1/')"
fi
echo "VM 已启动。串口日志: .vbox/serial.log"
