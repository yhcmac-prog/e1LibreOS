#!/usr/bin/env python3
"""isohybrid.py — isohybrid 的 Python 实现。

将 El Torito 可引导 ISO 补写 MBR 分区表，使其 dd 到 U 盘后也能 BIOS 引导。
与 syslinux isohybrid 做法一致：
1. 把 isohdpfx.bin（MBR 引导代码）写入 ISO 前 432 字节
2. 解析 El Torito 引导目录，取得 isolinux.bin 的加载扇区 (load RBA)
3. 写入分区表项: 可引导 / 类型 0x17(隐藏) / 起始 LBA = load RBA / 大小 = 其余扇区
4. 写入 0x55AA 引导签名

用法: python3 tools/isohybrid.py <image.iso> <isohdpfx.bin>
"""
import struct
import sys

SECTOR = 512
ISO_SECTOR = 2048


def lba_to_chs(lba: int) -> bytes:
    """粗略 CHS 换算（255 磁头 / 63 扇区，柱面上限 1023）。"""
    cyl = lba // (255 * 63)
    head = (lba // 63) % 255
    sect = lba % 63 + 1
    if cyl > 1023:
        cyl = 1023
        head, sect = 254, 63
    return bytes((head, ((cyl >> 8) << 6) | sect, cyl & 0xFF))


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    iso_path, mbr_path = sys.argv[1], sys.argv[2]

    with open(mbr_path, "rb") as f:
        mbr_code = f.read()
    if len(mbr_code) < 432:
        sys.exit("错误: isohdpfx.bin 应至少 432 字节")

    with open(iso_path, "r+b") as f:
        # 1) El Torito Boot Record VD 在扇区 17；其偏移 71..74 为引导目录 LBA
        f.seek(17 * ISO_SECTOR + 71)
        cat_lba = struct.unpack("<I", f.read(4))[0]
        if not cat_lba:
            sys.exit("错误: 未找到 El Torito 引导目录指针（该 ISO 可能不可引导）")

        # 2) 引导目录: 校验项(32B) + 默认入口项(32B)；入口项第 0 字节 0x88=可引导，
        #    偏移 8..11 为 isolinux.bin 的 load RBA（引导镜像起始扇区）
        f.seek(cat_lba * ISO_SECTOR + 32)
        entry = f.read(32)
        if len(entry) < 32 or entry[0] != 0x88:
            sys.exit("错误: 未找到可引导的 El Torito 默认入口项")
        load_rba = struct.unpack("<I", entry[8:12])[0]

        f.seek(0, 2)
        total = f.tell() // SECTOR
        if load_rba >= total:
            sys.exit("错误: load RBA 超出镜像范围")

        # 3) 写入 MBR 引导代码
        f.seek(0)
        f.write(mbr_code[:432])

        # 4) 写入第一个分区表项
        f.seek(446)
        part = bytearray(16)
        part[0] = 0x80                       # 可引导
        part[1:4] = lba_to_chs(load_rba)     # 起始 CHS
        part[4] = 0x17                       # 类型: 隐藏 IFS/HPFS（isohybrid 惯例）
        part[5:8] = lba_to_chs(total - 1)    # 结束 CHS
        part[8:12] = struct.pack("<I", load_rba)
        part[12:16] = struct.pack("<I", total - load_rba)
        f.write(part)

        # 5) 引导签名
        f.seek(510)
        f.write(b"\x55\xaa")

    print(f"isohybrid: MBR 已写入（分区起始扇区 {load_rba}，共 {total - load_rba} 个 512 字节扇区）")


if __name__ == "__main__":
    main()
