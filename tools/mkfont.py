#!/usr/bin/env python3
"""mkfont.py — 生成 e1wm 的 CJK 字库头文件

从 GNU Unifont .hex 提取 src/fbui/e1wm.c 中实际用到的全部非 ASCII 字符
的 16x16 字形，生成 src/fbui/font16_cjk.h（1bpp 位图，每字 32 字节，
行优先、每行 2 字节、字节内 MSB 为最左像素）。

用法:
    python3 tools/mkfont.py            # 生成/更新 font16_cjk.h
Unifont hex 缓存在 downloads/unifont.hex（约 20MB），已存在则不再下载。

新增界面文案后重新运行本脚本即可扩字。
"""
import gzip
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
E1WM = os.path.join(ROOT, "src", "fbui", "e1wm.c")
OUT = os.path.join(ROOT, "src", "fbui", "font16_cjk.h")
HEXFILE = os.path.join(ROOT, "downloads", "unifont.hex")

URLS = [
    "https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz",
    "https://unifoundry.com/pub/unifont/unifont-15.1.05/font-builds/unifont-15.1.05.hex.gz",
    "https://unifoundry.com/pub/unifont/unifont-14.0.06/font-builds/unifont-14.0.06.hex.gz",
]


def strip_comments(src: str) -> str:
    """去掉 // 与 /* */ 注释（字符串字面量内的 // 不动），只保留代码+文案。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':                       # 字符串字面量原样保留
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i]); out.append(src[i + 1]); i += 2
                    continue
                out.append(src[i])
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "'":                       # 字符字面量原样保留
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i]); out.append(src[i + 1]); i += 2
                    continue
                out.append(src[i])
                if src[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def needed_codepoints() -> list:
    chars = set()
    for path in [E1WM, os.path.join(ROOT, "src", "fbui", "pinyin_dict.h")]:
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            src = f.read()
        code = strip_comments(src)
        chars.update(ch for ch in code if ord(ch) > 0x7F)
        # 源码中的非 ASCII 文案常以 "\xNN" 十六进制转义书写（如法语的
        # "\xc3\xa9" = é、中文整句转义），需解码 UTF-8 后才能收集码点，
        # 否则对应字形不会进入字库，运行时渲染为空白。
        for run in re.finditer(r'(?:\\x[0-9a-fA-F]{2})+', code):
            raw = bytes(int(x, 16)
                        for x in re.findall(r'\\x([0-9a-fA-F]{2})', run.group(0)))
            try:
                chars.update(raw.decode('utf-8'))
            except UnicodeDecodeError:
                pass
    return sorted(ord(ch) for ch in chars)


def ensure_hexfile() -> None:
    if os.path.isfile(HEXFILE) and os.path.getsize(HEXFILE) > 1_000_000:
        return
    os.makedirs(os.path.dirname(HEXFILE), exist_ok=True)
    last_err = None
    for url in URLS:
        print(f"下载 {url}")
        gz = HEXFILE + ".gz"
        r = subprocess.run(["curl", "-fL", "--retry", "2", "--max-time", "180",
                            "-o", gz, url])
        if r.returncode == 0 and os.path.getsize(gz) > 100_000:
            with open(gz, "rb") as fi, open(HEXFILE, "wb") as fo:
                fo.write(gzip.decompress(fi.read()))
            os.remove(gz)
            return
        last_err = f"curl exit {r.returncode}"
    sys.exit(f"Unifont hex 下载失败: {last_err}")


def load_glyphs() -> dict:
    glyphs = {}
    with open(HEXFILE, encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cp_str, _, bits = line.partition(":")
            try:
                cp = int(cp_str, 16)
            except ValueError:
                continue
            glyphs[cp] = bytes.fromhex(bits)
    return glyphs


def main() -> None:
    cps = needed_codepoints()
    if not cps:
        sys.exit("e1wm.c 中未发现非 ASCII 字符")
    ensure_hexfile()
    glyphs = load_glyphs()

    rows, missing = [], []
    for cp in cps:
        g = glyphs.get(cp)
        if g is None:
            missing.append(cp)
            continue
        if len(g) == 16:               # 半宽字形 → 居中扩展为全宽
            g = bytes(16) + g + bytes(0)
        elif len(g) != 32:
            missing.append(cp)
            continue
        rows.append((cp, g))
    if missing:
        names = ", ".join(f"U+{cp:04X}" for cp in missing)
        sys.exit(f"Unifont 缺少字形: {names}")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("/* font16_cjk.h — 自动生成，勿手改（tools/mkfont.py）\n"
                " *\n"
                " * e1wm 用 16x16 CJK 字形，来源 GNU Unifont（OFL-1.1 / GPL-2.0+ 双许可），\n"
                " * 仅包含 src/fbui/e1wm.c 中实际用到的字符，1bpp，每字 32 字节：\n"
                " * 行优先 16 行 x 2 字节，字节内 MSB 为最左像素。\n"
                " */\n"
                "#ifndef FONT16_CJK_H\n"
                "#define FONT16_CJK_H\n\n"
                f"#define CJK_COUNT {len(rows)}\n\n"
                "static const unsigned short cjk_index[CJK_COUNT] = {\n")
        for cp, _ in rows:
            f.write(f"    0x{cp:04X},\n")
        f.write("};\n\nstatic const unsigned char cjk_bitmap[CJK_COUNT][32] = {\n")
        for cp, g in rows:
            body = ", ".join(f"0x{b:02X}" for b in g)
            f.write(f"    /* U+{cp:04X} */\n    {{{body}}},\n")
        f.write("};\n\n#endif /* FONT16_CJK_H */\n")

    names = " ".join(chr(cp) for cp, _ in rows)
    print(f"font16_cjk.h: {len(rows)} 字形 -> {OUT}")
    print(f"收录字符: {names}")


if __name__ == "__main__":
    main()
