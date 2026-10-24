#!/usr/bin/env python3
# mkfat32-fixed.py — 零依赖最小 FAT16/FAT32 镜像生成器（8.3 文件名，文件连续存放）
# 专为 e1LibreOS EFI 启动镜像设计：
#   FAT32（aarch64 ESP）: python3 mkfat32-fixed.py <src> <out> --size-mb 64
#   FAT16（x86 ISO El Torito EFI 引导镜像，体积小）:
#                          python3 mkfat32-fixed.py <src> <out> --size-mb 4 --fstype 16
# 约束：目录层级/文件名全部符合 8.3（EFI/BOOT/BOOTX64.EFI）。
#
# [FIX] 相对原版 mkfat32.py 的四处修复（2026-10-10 QEMU aarch64 实测验证）：
#  1) FAT32 卷自动扩容到 ≥65525 簇（EDK II 拒绝挂载的根因）：
#     原版第 73-74 行允许 <65525 簇的"小 FAT32"（--size-mb 64 → 仅约 16348 簇），
#     但 EDK II 固件 FAT 驱动据此拒绝挂载整个 ESP——实测 UEFI 报 BootXXXX Not
#     Found、mtools 报 "Bad media types ff/f8"，导致全部 aarch64 镜像无法引导。
#     本版在 FAT32 卷不足 65525 簇时自动倍增 --size-mb 直至合规。
#  2) 子目录 '..' 指向根目录时 cluster 字段写 0：
#     原版写父目录簇（FAT32 根簇=2），fsck.fat 报 "Invalid '..' entry in the
#     second slot"。FAT 约定"指向根目录时簇号写 0"（mkfs.fat 亦如此）；FAT16
#     根目录 cluster=0 天然正确，仅 FAT32 受影响。
#  3) FAT32 根目录补卷标目录项（attr=0x08）：
#     使根目录非空、可被 fsck 识别为合法父目录，并消除 "no volume label in
#     root directory" 告警，与 BPB 卷标一致。根目录不写 . 和 ..（fsck 不接受）。
#  4) FAT[0] 低字节必须 = media 描述符 0xF8（GRUB 拒绝挂载的根因）：
#     FAT 规范要求 FAT[0] 低 8 位等于 BPB 的 media 字节 0xF8。GRUB 的 FAT 驱动
#     校验不一致即拒绝挂载该卷，导致读不到 configfile 而停在 grub>。原版 FAT32
#     用 0x0FFFFFFF（低字节 0xFF）；FAT16 用 0xFFF8 恰好正确。
#  其余逻辑（FAT16 固定根目录区、文件连续存放、启动扇区/FSInfo、双 FAT）保持原样。
import os, sys, struct, math, random, argparse

EOC32 = 0x0FFFFFFF
FAT32_MIN_CLUSTERS = 65525  # FAT32 规范最小簇数（EDK II 以此为判定门槛）

def sfn(name):
    """文件名 -> 11 字节短目录项名（大写 8+3）。"""
    up = name.upper()
    if "." in up:
        base, _, ext = up.rpartition(".")
    else:
        base, ext = up, ""
    if not (1 <= len(base) <= 8 and len(ext) <= 3):
        sys.exit(f"mkfat: 文件名不符合 8.3: {name}")
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")

class Node:
    def __init__(self, name, parent, cluster=0):
        self.name = name          # 目录名；None 表示根
        self.parent = parent
        self.cluster = cluster
        self.entries = []         # 子目录 Node
        self.files = []           # (sfn11, attr, clus, size)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--size-mb", type=int, required=True)
    ap.add_argument("--hidden", type=int, default=2048)
    ap.add_argument("--fstype", type=int, choices=(16, 32), default=32)
    ap.add_argument("--label", default="E1OS_EFI")
    a = ap.parse_args()

    BPS = 512
    FAT32 = (a.fstype == 32)
    SPC = 8 if FAT32 else 4
    NFATS = 2
    total = a.size_mb * 2048
    req_mb = a.size_mb

    # FAT16：根目录是保留区之后的固定区域（不占簇）
    ROOTENTS = 0 if FAT32 else 512
    root_sec = (ROOTENTS * 32 + BPS - 1) // BPS

    # ---- 迭代求 FAT 扇区数 ----
    if FAT32:
        # [FIX 1] 自动扩容：FAT32 卷必须 ≥65525 簇，否则 EDK II 固件拒绝挂载 ESP
        nres, fat_sec, data_start, nclusters = 32, 1, 0, 0
        while True:
            nres, fat_sec = 32, 1
            for _ in range(24):
                data_sec = total - nres - NFATS * fat_sec
                need = math.ceil((data_sec // SPC) * 4 / BPS)
                if need <= fat_sec:
                    break
                fat_sec = need
            data_start = nres + NFATS * fat_sec
            nclusters = (total - data_start) // SPC
            if nclusters >= FAT32_MIN_CLUSTERS:
                break
            old_mb, req_mb = req_mb, req_mb * 2
            total = req_mb * 2048
            if req_mb > 8192:
                sys.exit("mkfat: FAT32 扩容超限")
        if req_mb != a.size_mb:
            print(f"mkfat: FAT32 卷 {a.size_mb} MiB 仅 {nclusters} 簇 < 规范最小 {FAT32_MIN_CLUSTERS}"
                  f"（EDK II 固件拒绝挂载），自动扩至 {req_mb} MiB（{nclusters} 簇）")
    else:
        nres = 1
        fat_sec = 1
        for _ in range(12):
            data_sec = total - nres - NFATS * fat_sec - root_sec
            need = math.ceil((data_sec // SPC) * 2 / BPS)
            if need <= fat_sec:
                break
            fat_sec = need
        root_lba = nres + NFATS * fat_sec
        data_start = root_lba + root_sec
        nclusters = (total - data_start) // SPC
        if not FAT32 and nclusters >= 65525:
            sys.exit("mkfat: FAT16 卷过大，请用 --fstype 32")

    def c2s(c): return data_start + (c - 2) * SPC

    buf = bytearray(total * BPS)
    def put(sec, off, data):
        buf[sec*BPS+off:sec*BPS+off+len(data)] = data

    # ---- 收集目录树（只接受 8.3）----
    root = Node(None, None, 0 if not FAT32 else 2)
    file_list = []
    for dp, dns, fns in os.walk(a.src):
        for fn in fns:
            apath = os.path.join(dp, fn)
            rel = os.path.relpath(apath, a.src).replace(os.sep, "/").upper()
            parts = rel.split("/")
            for p in parts:
                sfn(p)
            file_list.append((apath, parts))

    # ---- FAT ----
    fat = bytearray(fat_sec * BPS)
    def fat_set(n, v):
        if FAT32:
            struct.pack_into("<I", fat, n*4, v)
        else:
            struct.pack_into("<H", fat, n*2, v)
    eoc = EOC32 if FAT32 else 0xFFF8
    # [FIX 4] FAT[0] 低字节必须 = media 描述符 0xF8（FAT 规范；GRUB 的 FAT 驱动
    #         校验 FAT[0].media 与 BPB 是否一致，不一致则拒绝挂载该卷，导致
    #         GRUB 读不到 configfile 而停在 grub>。原版 FAT32 用 0x0FFFFFFF，
    #         低字节成 0xFF；FAT16 用 0xFFF8 恰好正确）
    fat_set(0, 0x0FFFFFF8 if FAT32 else 0xFFF8)
    fat_set(1, 0x0FFFFFFF if FAT32 else 0xFFFF)
    if FAT32:
        fat_set(2, eoc)
    nxt = 3 if FAT32 else 2    # FAT32 根目录占簇 2；FAT16 根目录在固定区，簇从 2 起
    def alloc(nclus):
        nonlocal nxt
        start = nxt
        for i in range(nclus):
            fat_set(nxt + i, eoc if i == nclus-1 else nxt + i + 1)
        nxt += nclus
        return start

    # 子目录（自顶向下）分配簇
    for _, parts in sorted(file_list, key=lambda x: (len(x[1]), x[1])):
        cur = root
        for dname in parts[:-1]:
            hit = None
            for e in cur.entries:
                if isinstance(e, Node) and e.name == dname:
                    hit = e; break
            if hit is None:
                hit = Node(dname, cur, alloc(1))
                cur.entries.append(hit)
            cur = hit

    # 文件分配 + 写数据
    for absf, parts in sorted(file_list, key=lambda x: x[1]):
        cur = root
        for dname in parts[:-1]:
            cur = next(e for e in cur.entries if isinstance(e, Node) and e.name == dname)
        with open(absf, "rb") as f:
            data = f.read()
        nclus = max(1, math.ceil(len(data) / (SPC * BPS)))
        start = alloc(nclus)
        if nxt > nclusters + 2:
            sys.exit("mkfat: 空间不足，请增大 --size-mb")
        sec = c2s(start)
        buf[sec*BPS:sec*BPS+len(data)] = data
        cur.files.append((sfn(parts[-1]), 0x20, start, len(data)))

    def dir_entry(name11, attr, clus, size):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        # FAT 目录项：20-21=首簇高字（仅 FAT32），26-27=首簇低字，28-31=大小
        struct.pack_into("<H", e, 20, (clus >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 26, clus & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    label11 = a.label.ljust(11)[:11].encode("ascii")

    def serialize(node):
        ents = []
        if node.name is not None:
            # 子目录：. 指向自身，.. 指向父目录
            ents.append(dir_entry(b"." + b" "*10, 0x10, node.cluster, 0))
            # [FIX 2] 父为根目录时 '..' 的簇字段写 0（FAT 约定，对齐 mkfs.fat）：
            #         原版 FAT32 写成根簇 2，fsck.fat 报 "Invalid '..' entry in the
            #         second slot"（FAT16 根 cluster=0 天然正确，故仅 FAT32 受影响）
            pc = 0 if (node.parent is not None and node.parent.name is None) \
                  else (node.parent.cluster if node.parent else 0)
            ents.append(dir_entry(b".." + b" "*9, 0x10, pc, 0))
        elif FAT32:
            # [FIX 3] FAT32 根目录：写卷标条目（attr=0x08，与 BPB 卷标一致），
            #         使根目录非空、可被 fsck 识别为合法父目录。经对比 mkfs.fat
            #         合规卷：根目录只有卷标条目、不写 . 和 ..（fsck 对根目录
            #         的 . / .. 报 "Bad short file name"，而对子目录 . / .. 接受）。
            ents.append(dir_entry(label11, 0x08, 0, 0))
        # FAT16 根目录（node.name is None 且 not FAT32）：保持原版空根目录
        for child in node.entries:
            ents.append(dir_entry(sfn(child.name), 0x10, child.cluster, 0))
        for nm, attr, clus, size in node.files:
            ents.append(dir_entry(nm, attr, clus, size))
        blob = b"".join(ents)
        if FAT32:
            if len(blob) > SPC*BPS:
                sys.exit("mkfat: 目录项超过单簇")
            put(c2s(node.cluster), 0, blob)
        else:
            if node.name is None:
                # FAT16：根目录写入固定区域
                if len(blob) > root_sec * BPS:
                    sys.exit("mkfat: FAT16 根目录项超过保留区域")
                put(root_lba, 0, blob)
            else:
                if len(blob) > SPC*BPS:
                    sys.exit("mkfat: 目录项超过单簇")
                put(c2s(node.cluster), 0, blob)

    def walk(n):
        serialize(n)
        for e in n.entries:
            walk(e)
    walk(root)

    # ---- 启动扇区 ----
    bs = bytearray(512)
    bs[0:3] = b"\xEB\x58\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", bs, 11, BPS)
    bs[13] = SPC
    volid = random.randint(0, 0xFFFFFFFF)
    label = a.label.ljust(11)[:11].encode("ascii")
    if FAT32:
        struct.pack_into("<H", bs, 14, nres)
        bs[16] = NFATS
        struct.pack_into("<H", bs, 17, 0)
        struct.pack_into("<H", bs, 19, 0)
        bs[21] = 0xF8
        struct.pack_into("<H", bs, 22, 0)
        struct.pack_into("<H", bs, 24, 32)
        struct.pack_into("<H", bs, 26, 64)
        struct.pack_into("<I", bs, 28, a.hidden)
        struct.pack_into("<I", bs, 32, total)
        struct.pack_into("<I", bs, 36, fat_sec)
        struct.pack_into("<H", bs, 40, 0)
        struct.pack_into("<H", bs, 42, 0)
        struct.pack_into("<I", bs, 44, 2)
        struct.pack_into("<H", bs, 48, 1)
        struct.pack_into("<H", bs, 50, 6)
        bs[64] = 0x80
        bs[66] = 0x29
        struct.pack_into("<I", bs, 67, volid)
        bs[71:82] = label
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"
        put(0, 0, bytes(bs))
        put(6, 0, bytes(bs))           # 备份引导扇区
        # FSInfo（保留区扇区 1）
        fsi = bytearray(512)
        struct.pack_into("<I", fsi, 0, 0x41615252)
        struct.pack_into("<I", fsi, 484, 0x61417272)
        struct.pack_into("<I", fsi, 488, 0xFFFFFFFF)
        struct.pack_into("<I", fsi, 492, 0xFFFFFFFF)
        struct.pack_into("<I", fsi, 508, 0xAA550000)
        put(1, 0, bytes(fsi))
    else:
        struct.pack_into("<H", bs, 14, nres)
        bs[16] = NFATS
        struct.pack_into("<H", bs, 17, ROOTENTS)
        if total < 65535:
            struct.pack_into("<H", bs, 19, total)
            struct.pack_into("<I", bs, 32, 0)
        else:
            struct.pack_into("<H", bs, 19, 0)
            struct.pack_into("<I", bs, 32, total)
        bs[21] = 0xF8
        struct.pack_into("<H", bs, 22, fat_sec)
        struct.pack_into("<H", bs, 24, 32)
        struct.pack_into("<H", bs, 26, 64)
        struct.pack_into("<I", bs, 28, a.hidden)
        bs[36] = 0x80
        bs[38] = 0
        bs[39] = 0x29
        struct.pack_into("<I", bs, 40, volid)
        bs[54:65] = label
        bs[65:73] = b"FAT16   "
        bs[510:512] = b"\x55\xAA"
        put(0, 0, bytes(bs))

    # FAT ×2
    put(nres, 0, bytes(fat))
    put(nres + fat_sec, 0, bytes(fat))

    with open(a.out, "wb") as f:
        f.write(bytes(buf))
    print(f"mkfat: {a.out} FAT{a.fstype} {req_mb} MiB, {nxt-2} 簇已用, {nclusters} 簇可用")

if __name__ == "__main__":
    main()
