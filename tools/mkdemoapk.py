#!/usr/bin/env python3
"""mkdemoapk.py — 生成演示 APK（无需 aapt/sdk）

手工构造二进制 AXML 格式的 AndroidManifest.xml，配合假 classes.dex
（仅 dex 魔数头）与 assets 资源，用 ZIP store 方式打包成合法结构的
APK，供 e1apk info/list/extract 演示与测试。

用法: python3 tools/mkdemoapk.py <output.apk>
"""
import struct
import sys
import zipfile


def u16(v):
    return struct.pack("<H", v & 0xFFFF)


def u32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


# ---------------------------------------------------------------- AXML 构造
NS_URI = "http://schemas.android.com/apk/res/android"

STRINGS = [
    "manifest", "application", "activity", "intent-filter", "action",
    "category", "uses-sdk", "uses-permission",
    NS_URI,                              # 8  命名空间 URI
    "package", "versionCode", "versionName", "minSdkVersion",
    "targetSdkVersion", "name", "label",  # 9..15 属性名
    "com.e1os.demo", "1.0", "Hello e1", ".MainActivity",  # 16..19 值
    "android.intent.action.MAIN",         # 20
    "android.intent.category.LAUNCHER",   # 21
    "android.permission.INTERNET",        # 22
    "android",                            # 23 命名空间前缀
]

I_MANIFEST, I_APP, I_ACTIVITY, I_FILTER, I_ACTION, I_CATEGORY, I_SDK, I_PERM = range(8)
I_URI = 8
I_PKG, I_VERCODE, I_VERNAME, I_MINSDK, I_TGTSKD, I_NAME, I_LABEL = range(9, 16)
V_PKG, V_VER, V_LABEL, V_ACT = 16, 17, 18, 19
V_MAIN, V_LAUNCHER, V_INTERNET = 20, 21, 22
I_ANDROID = 23


def build_string_pool():
    """UTF-16LE string pool（flags=0），返回块字节串"""
    data = b""
    offsets = []
    for s in STRINGS:
        offsets.append(len(data))
        data += u16(len(s)) + s.encode("utf-16-le") + u16(0)
    while len(data) % 4:
        data += b"\x00"
    header_size = 0x1C
    strings_start = header_size + 4 * len(STRINGS)
    size = strings_start + len(data)
    chunk = u16(0x0001) + u16(header_size) + u32(size)
    chunk += u32(len(STRINGS)) + u32(0) + u32(0)      # count, styleCount, flags
    chunk += u32(strings_start) + u32(0)              # stringsStart, stylesStart
    chunk += b"".join(u32(o) for o in offsets)
    chunk += data
    return chunk


def attr(ns, name, raw, dtype, data):
    """一个 20 字节属性项"""
    return u32(ns) + u32(name) + u32(raw) + u16(8) + bytes([0, dtype]) + u32(data)


def attr_str(ns, name, sidx):
    """字符串属性：rawValue 与 typed value 都指向字符串池索引"""
    return attr(ns, name, sidx, 0x03, sidx)


def attr_int(ns, name, value):
    return attr(ns, name, 0xFFFFFFFF, 0x10, value)


def start_element(name_idx, attrs):
    """START_ELEMENT 块：node(16) + attrExt(20) + attrs×20"""
    size = 16 + 20 + 20 * len(attrs)
    chunk = u16(0x0102) + u16(0x0010) + u32(size) + u32(1) + u32(0xFFFFFFFF)
    chunk += u32(0xFFFFFFFF) + u32(name_idx)          # ns=-1, 元素名
    chunk += u16(0x14) + u16(0x14) + u16(len(attrs)) + u16(0) + u16(0) + u16(0)
    chunk += b"".join(attrs)
    return chunk


def end_element(name_idx):
    chunk = u16(0x0103) + u16(0x0010) + u32(24) + u32(1) + u32(0xFFFFFFFF)
    chunk += u32(0xFFFFFFFF) + u32(name_idx)
    return chunk


def start_ns():
    chunk = u16(0x0100) + u16(0x0010) + u32(24) + u32(1) + u32(0xFFFFFFFF)
    chunk += u32(I_ANDROID) + u32(I_URI)
    return chunk


def end_ns():
    chunk = u16(0x0101) + u16(0x0010) + u32(24) + u32(1) + u32(0xFFFFFFFF)
    chunk += u32(I_ANDROID) + u32(I_URI)
    return chunk


def build_axml():
    chunks = b""
    chunks += build_string_pool()
    chunks += start_ns()

    # <manifest package=.. versionCode=1 versionName="1.0">
    chunks += start_element(I_MANIFEST, [
        attr_str(0xFFFFFFFF, I_PKG, V_PKG),
        attr_int(I_URI, I_VERCODE, 1),
        attr_str(I_URI, I_VERNAME, V_VER),
    ])
    # <uses-sdk minSdkVersion=21 targetSdkVersion=28>
    chunks += start_element(I_SDK, [
        attr_int(I_URI, I_MINSDK, 21),
        attr_int(I_URI, I_TGTSKD, 28),
    ])
    chunks += end_element(I_SDK)
    # <uses-permission android:name="android.permission.INTERNET">
    chunks += start_element(I_PERM, [attr_str(I_URI, I_NAME, V_INTERNET)])
    chunks += end_element(I_PERM)
    # <application android:label="Hello e1">
    chunks += start_element(I_APP, [attr_str(I_URI, I_LABEL, V_LABEL)])
    # <activity android:name=".MainActivity"> + MAIN/LAUNCHER
    chunks += start_element(I_ACTIVITY, [attr_str(I_URI, I_NAME, V_ACT)])
    chunks += start_element(I_FILTER, [])
    chunks += start_element(I_ACTION, [attr_str(I_URI, I_NAME, V_MAIN)])
    chunks += end_element(I_ACTION)
    chunks += start_element(I_CATEGORY, [attr_str(I_URI, I_NAME, V_LAUNCHER)])
    chunks += end_element(I_CATEGORY)
    chunks += end_element(I_FILTER)
    chunks += end_element(I_ACTIVITY)
    chunks += end_element(I_APP)
    chunks += end_element(I_MANIFEST)
    chunks += end_ns()

    total = 8 + len(chunks)
    return u16(0x0003) + u16(0x0008) + u32(total) + chunks


# ---------------------------------------------------------------- 打包
def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]

    manifest = build_axml()
    # 最小 classes.dex：仅魔数 + 版本号（e1apk info 识别用）
    dex = b"dex\n035\x00" + b"\x00" * 64
    assets = b"Hello from e1apk demo asset!\nThis file was extracted from the APK.\n"
    meta = (b"Manifest-Version: 1.0\n"
            b"Created-By: 1.0 (e1LibreOS mkdemoapk)\n\n")

    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        # 固定时间戳保证确定性输出
        zi = lambda name, data: zipfile.ZipInfo(name, (2026, 9, 12, 0, 0, 0))
        for name, data in [
            ("AndroidManifest.xml", manifest),
            ("classes.dex", dex),
            ("assets/hello.txt", assets),
            ("META-INF/MANIFEST.MF", meta),
        ]:
            info = zi(name, data)
            info.external_attr = 0o644 << 16
            z.writestr(info, data, zipfile.ZIP_STORED)

    print(f"demo APK -> {out}")


if __name__ == "__main__":
    main()
