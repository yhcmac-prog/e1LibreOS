#!/bin/sh
# build-arm-image.sh — 生成 aarch64 UEFI 原始磁盘镜像（GPT + FAT32 ESP）
#
# 用法: build-arm-image.sh <vmlinuz> <initrd.gz> <grubaa64.efi> <variant> <out.img>
#
# 布局: 保护性 MBR + GPT；分区1 ESP(2048 扇区对齐)，内容：
#   /VMLINUZ            linux-virt aarch64 内核（含 EFI stub）
#   /INITRD.GZ          e1LibreOS initramfs
#   /EFI/BOOT/BOOTAA64.EFI  grubaa64.efi（removable media 路径）
#   /EFI/BOOT/GRUB.CFG  （以及 /grub、/boot/grub 下的兼容副本）
# 说明: grubaa64.efi 由 GRUB 2.12 + Alpine arm64-efi 模块生成的 standalone
#       PE32+ AArch64 镜像，内嵌启动配置；另放外置配置兼容其它 grub 二进制。
set -eu
cd "$(dirname "$0")/.."

VMLINUZ=$1; INITRD=$2; GRUBEF=$3; VARIANT=$4; OUT=$5
[ -f "$VMLINUZ" ] && [ -f "$INITRD" ] && [ -f "$GRUBEF" ] || { echo "参数缺失" >&2; exit 1; }

BUILD=$(dirname "$OUT")
STAGE="$BUILD/armstage"
ESP_FAT="$BUILD/esp.fat32"
E1GPT_HOST="$BUILD/e1gpt-host"
rm -rf "$STAGE"; mkdir -p "$STAGE/EFI/BOOT" "$STAGE/grub" "$STAGE/boot/grub"

cp "$VMLINUZ" "$STAGE/VMLINUZ"
cp "$INITRD"  "$STAGE/INITRD.GZ"
cp "$GRUBEF"  "$STAGE/EFI/BOOT/BOOTAA64.EFI"
VN=$(printf '%s' "$VARIANT" | cut -c1 | tr 'a-z' 'A-Z')$(printf '%s' "$VARIANT" | cut -c2-)
cat > "$STAGE/EFI/BOOT/GRUB.CFG" <<EOF
set timeout=0
set default=0
search --no-floppy --file /VMLINUZ --set=root
menuentry "e1LibreOS 2.0 (Libre) $VN aarch64" {
    linux /VMLINUZ console=tty0 console=ttyAMA0,115200
    initrd /INITRD.GZ
}
EOF
# 兼容 grub 内嵌 prefix=/grub 或 /boot/grub 的二进制
for cfg in "$STAGE/grub/GRUB.CFG" "$STAGE/grub/grub.cfg" \
           "$STAGE/boot/grub/GRUB.CFG" "$STAGE/boot/grub/grub.cfg"; do
    cp "$STAGE/EFI/BOOT/GRUB.CFG" "$cfg"
done

# ESP 容量（内容 + 6MiB 余量，FAT32 最小约 33MiB，取 64 起步）
need_kb=$(du -sk "$STAGE" | awk '{print int($1/1024)+6}')
ESP_MB=$need_kb; [ "$ESP_MB" -lt 64 ] && ESP_MB=64
echo "==> ESP 内容约 $((ESP_MB-6)) MiB，FAT32 分区请求 ${ESP_MB} MiB"
python3 tools/mkfat32.py "$STAGE" "$ESP_FAT" --size-mb "$ESP_MB" --hidden 2048
# mkfat32 为满足 FAT32 ≥65525 簇会自动扩容（64 → 512 MiB）；GPT 分区与磁盘
# 尺寸必须按实际生成的 ESP 计算，否则分区表声明 64 MiB 而 FAT 卷实为 512 MiB，
# 且备份 GPT 会被 ESP 数据覆盖（固件可能拒绝挂载/找不到分区）
ESP_SEC=$(( $(wc -c < "$ESP_FAT") / 512 ))
ESP_MB=$((ESP_SEC / 2048))
echo "==> FAT32 ESP 实际 ${ESP_MB} MiB"

# 宿主原生编译 e1gpt（macOS/Linux 均可运行于普通文件镜像）
cc -O2 -o "$E1GPT_HOST" src/e1gpt/e1gpt.c 2>/dev/null \
  || clang -O2 -o "$E1GPT_HOST" src/e1gpt/e1gpt.c

# 磁盘 = 2048(对齐) + ESP + 2MiB（备份 GPT/余量）
TOTAL_SEC=$((2048 + ESP_SEC + 4096))
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=512 count=0 seek="$TOTAL_SEC" 2>/dev/null
"$E1GPT_HOST" "$OUT" "esp:$ESP_MB"
dd if="$ESP_FAT" of="$OUT" bs=512 seek=2048 conv=notrunc 2>/dev/null

rm -rf "$STAGE" "$ESP_FAT" "$E1GPT_HOST"
echo "==> $OUT ($((TOTAL_SEC/2048)) MiB) 生成完成"
echo "    烧录: dd if=$OUT of=/dev/你的U盘 ; UEFI(ARM64) 机器从 U 盘启动"
