#!/usr/bin/env python3
"""gen_pinyin.py — 从 Unicode Unihan kMandarin + kHanyuPinlu 生成拼音字典头文件

解析 Unihan_Readings.txt:
  - kMandarin：拼音（带声调），去声调后按音节分组
  - kHanyuPinlu：频率数据，格式 "pinyin(freq)"，用于按频率排序

每音节按频率降序排列（高频字在前），无频率数据的按码位升序兜底，
取前 MAX_PER 字生成 src/fbui/pinyin_dict.h。

用法:
    python3 tools/gen_pinyin.py   # 需 /tmp/Unihan_data/Unihan_Readings.txt

拼音格式：无声调小写，ü→v（用户输入 v 代替 ü）
"""
import os
import re
import sys
import unicodedata

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "src", "fbui", "pinyin_dict.h")
UNIHAN = "/tmp/Unihan_data/Unihan_Readings.txt"
MAX_PER = 30  # 每音节最多保留字数

# 声调→基元映射
TONE_MAP = str.maketrans({
    "ā": "a", "á": "a", "ǎ": "a", "à": "a",
    "ē": "e", "é": "e", "ě": "e", "è": "e",
    "ī": "i", "í": "i", "ǐ": "i", "ì": "i",
    "ō": "o", "ó": "o", "ǒ": "o", "ò": "o",
    "ū": "u", "ú": "u", "ǔ": "u", "ù": "u",
    "ǖ": "v", "ǘ": "v", "ǚ": "v", "ǜ": "v", "ü": "v",
    "ń": "n", "ň": "n", "ǹ": "n",
    "ḿ": "m",
})


def strip_tone(s: str) -> str:
    """去声调标记，ü→v"""
    s = unicodedata.normalize("NFC", s)
    s = s.translate(TONE_MAP)
    return s.strip().lower()


def load_data():
    """读 kMandarin + kHanyuPinlu，返回 {py: [(cp, freq), ...]}"""
    if not os.path.isfile(UNIHAN):
        sys.exit("找不到 " + UNIHAN + "，请先下载 Unihan.zip 解压到 /tmp/Unihan_data/")
    mandarin = {}   # cp -> py
    freq_map = {}    # cp -> max frequency
    with open(UNIHAN, encoding="utf-8") as f:
        for line in f:
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            cp_str = parts[0].strip()
            field = parts[1].strip()
            value = parts[2].strip()
            if not cp_str.startswith("U+"):
                continue
            cp = int(cp_str[2:], 16)
            if cp < 0x4E00 or cp > 0x9FFF:
                continue
            if field == "kMandarin":
                py = strip_tone(value)
                if py and len(py) <= 6 and " " not in py:
                    mandarin[cp] = py
            elif field == "kHanyuPinlu":
                # 格式: hǎo(6060) hāo(142) hào(115)
                for m in re.finditer(r"(\S+?)\((\d+)\)", value):
                    freq = int(m.group(2))
                    if cp in freq_map:
                        if freq > freq_map[cp]:
                            freq_map[cp] = freq
                    else:
                        freq_map[cp] = freq
    # 按拼音分组
    d = {}
    for cp, py in mandarin.items():
        d.setdefault(py, []).append((cp, freq_map.get(cp, 0)))
    # 每音节排序：频率降序，无频率的按码位升序
    for py in d:
        d[py].sort(key=lambda x: (-x[1], x[0]))
    return d


def main():
    d = load_data()
    keys = sorted(d.keys())
    chars_all = set()

    lines = []
    lines.append("/* pinyin_dict.h — 自动生成（tools/gen_pinyin.py），勿手改\n"
                 " *\n"
                 " * 拼音→汉字映射表，来源 Unicode Unihan kMandarin（去声调）。\n"
                 " * 按 kHanyuPinlu 频率降序排列，每音节最多 "
                 + str(MAX_PER) + " 字。\n"
                 " * ü→v（用户输入 v 代替 ü），无声调。\n"
                 " */\n"
                 "#ifndef PINYIN_DICT_H\n"
                 "#define PINYIN_DICT_H\n\n"
                 "struct ime_entry { const char *py; const char *chars; };\n\n"
                 "static const struct ime_entry ime_dict[] = {\n")
    for py in keys:
        cps = [cp for cp, _ in d[py][:MAX_PER]]
        chars = "".join(chr(cp) for cp in cps)
        chars_all.update(cps)
        lines.append('    { "' + py + '", "' + chars + '" },\n')
    lines.append("};\n")
    lines.append("\nstatic const int ime_dict_count = "
                 "sizeof(ime_dict) / sizeof(ime_dict[0]);\n")
    lines.append("\n#endif /* PINYIN_DICT_H */\n")

    with open(OUT, "w", encoding="utf-8") as f:
        f.writelines(lines)
    print("pinyin_dict.h: " + str(len(keys)) + " 音节, "
          + str(len(chars_all)) + " 字 -> " + OUT)


if __name__ == "__main__":
    main()
