#!/usr/bin/env python3
"""mkinitramfs.py — 将 stage 目录打包为 newc 格式 cpio 归档（Linux initramfs）。

用法: python3 tools/mkinitramfs.py <stage_dir> <output.cpio>

特性：
- 纯 Python 实现 newc (070701) 格式，跨平台（macOS 的 BSD cpio 不支持 newc）
- 确定性输出：固定 uid/gid=0、mtime=0、按序遍历
- 支持 目录 / 普通文件 / 符号链接，保留权限位
"""
import os
import stat
import sys

MAGIC = b"070701"
TRAILER = b"TRAILER!!!"
BLOCK = 4

S_IFDIR = 0o040000
S_IFREG = 0o100000
S_IFLNK = 0o120000
S_IFCHR = 0o020000

# initramfs 不会自动挂载 devtmpfs，必须在归档里预置关键设备节点
# （内核引导初期 /init 与其输出都依赖 /dev/console、/dev/null）
DEVICE_NODES = {
    "dev/console": (5, 1, 0o600),
    "dev/null":    (1, 3, 0o666),
    "dev/tty":     (5, 0, 0o666),
    "dev/tty1":    (4, 1, 0o620),
    "dev/ttyS0":   (4, 64, 0o620),
    "dev/zero":    (1, 5, 0o666),
    "dev/random":  (1, 8, 0o666),
    "dev/urandom": (1, 9, 0o666),
}


def pad_to(data: bytes, block: int = BLOCK) -> bytes:
    r = len(data) % block
    return data if r == 0 else data + b"\x00" * (block - r)


def header(ino: int, mode: int, size: int, namesize: int,
           rdevmajor: int = 0, rdevminor: int = 0) -> bytes:
    fields = [
        ino, mode, 0, 0,          # ino, mode, uid, gid
        1, 0,                     # nlink, mtime
        size, 0, 0,               # filesize, devmajor, devminor
        rdevmajor, rdevminor,     # rdevmajor, rdevminor
        namesize, 0,              # namesize, check
    ]
    return MAGIC + b"".join(b"%08X" % (f & 0xFFFFFFFF) for f in fields)


def dev_node(ino: int, name: str, major: int, minor: int, mode: int) -> bytes:
    name_b = name.encode() + b"\x00"
    out = header(ino, S_IFCHR | mode, 0, len(name_b), major, minor) + name_b
    return pad_to(out)


def entry(ino: int, name: str, st: os.stat_result, root: str) -> bytes:
    full = os.path.join(root, name)
    if stat.S_ISDIR(st.st_mode):
        mode, size, data = S_IFDIR, 0, b""
    elif stat.S_ISLNK(st.st_mode):
        mode, data = S_IFLNK, os.readlink(full).encode()
        size = len(data)
    elif stat.S_ISREG(st.st_mode):
        mode = S_IFREG | (st.st_mode & 0o7777)
        with open(full, "rb") as f:
            data = f.read()
        size = len(data)
    else:
        return b""  # 跳过设备文件等（devtmpfs 启动时会自动生成）

    name_b = name.encode() + b"\x00"
    out = header(ino, mode, size, len(name_b)) + name_b
    out = pad_to(out) + pad_to(data)
    return out


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out_path = sys.argv[1], sys.argv[2]
    if not os.path.isdir(root):
        sys.exit(f"错误: {root} 不是目录")

    out = bytearray()
    ino = 300000  # 避开常见保留 inode 区间

    # 确保根目录条目在最前
    out += entry(ino, ".", os.lstat(root), root)

    # 设备节点紧随其后（若 stage 里已有同名文件则跳过）
    for name, (major, minor, mode) in sorted(DEVICE_NODES.items()):
        if not os.path.lexists(os.path.join(root, name)):
            ino += 1
            out += dev_node(ino, name, major, minor, mode)

    seen = {"."}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for d in dirnames:
            rel = os.path.relpath(os.path.join(dirpath, d), root)
            seen.add(rel)
            out += entry(ino, rel, os.lstat(os.path.join(dirpath, d)), root)
        for fn in sorted(filenames):
            rel = os.path.relpath(os.path.join(dirpath, fn), root)
            seen.add(rel)
            out += entry(ino, rel, os.lstat(os.path.join(dirpath, fn)), root)

    # 兜底：加入未被 walk 到的符号链接目标（如顶层 symlink 目录）
    for dirpath, dirnames, filenames in os.walk(root):
        for n in dirnames + filenames:
            rel = os.path.relpath(os.path.join(dirpath, n), root)
            if rel not in seen:
                seen.add(rel)
                st = os.lstat(os.path.join(dirpath, n))
                if stat.S_ISLNK(st.st_mode):
                    out += entry(ino, rel, st, root)

    out += header(ino + 1, 0, 0, len(TRAILER) + 1) + TRAILER + b"\x00"
    out = pad_to(bytes(out), 512)  # 按惯例尾部补齐到 512 字节块

    with open(out_path, "wb") as f:
        f.write(out)
    print(f"mkinitramfs: {out_path}  ({len(out)} bytes)")


if __name__ == "__main__":
    main()
