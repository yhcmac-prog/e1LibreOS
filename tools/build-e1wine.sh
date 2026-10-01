#!/bin/sh
# build-e1wine.sh — 为不同宿主平台构建 e1wine（e1LibreOS 的 Wine 变种）
#
# 用法:
#   tools/build-e1wine.sh linux <输出路径>          # x86_64-linux-musl 静态（e1LibreOS 内置）
#   tools/build-e1wine.sh linux-arm64 <输出路径>    # aarch64-linux-musl 静态（交叉，PE 运行见说明）
#   tools/build-e1wine.sh macos <输出路径>          # 当前 mac 原生架构
#   tools/build-e1wine.sh macos-universal <路径>    # x86_64+arm64 双架构 Mach-O
#   tools/build-e1wine.sh unix <输出路径>           # 通用 Unix：按当前 cc/平台原生编译
#   tools/build-e1wine.sh all [dist目录]           # 一次性构建全部可构建目标
#
# convert 功能（EXE→自解压 ELF / EXE→.app）需要 macOS 双架构启动器数据块：
# 在 macOS 上本脚本会先用 clang 编译 src/e1wine/mac-launcher.c 为 universal
# binary，并生成 src/e1wine/mac_launcher_blob.h 供各平台 e1wine 内嵌。
set -eu
cd "$(dirname "$0")/.."

TARGET=${1:-}
OUT=${2:-dist}
SRC=src/e1wine/e1wine.c
LAUNCHER_SRC=src/e1wine/mac-launcher.c
BLOB_H=src/e1wine/mac_launcher_blob.h
ZIG=./downloads/zig/zig
mkdir -p build dist

# ---------- 生成 macOS universal 启动器 blob ----------
ensure_blob() {
    if [ "$(uname -s)" != "Darwin" ]; then
        return 0   # 非 macOS 宿主：沿用仓库中已提交的 blob（无则 --app 报错提示）
    fi
    if ! command -v clang >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
        echo "警告: 无 clang/cc，跳过 macOS 启动器 blob 生成" >&2
        return 0
    fi
    CCX=clang; command -v clang >/dev/null 2>&1 || CCX=cc
    # 源码比 blob 新（或 blob 不存在）才重建
    if [ ! -f "$BLOB_H" ] || [ "$LAUNCHER_SRC" -nt "$BLOB_H" ]; then
        echo "==> 构建 macOS universal 启动器 (x86_64 + arm64)"
        "$CCX" -arch x86_64 -arch arm64 -O2 -mmacosx-version-min=10.13 \
            -o build/mac-launcher-universal "$LAUNCHER_SRC"
        echo "==> 生成 $BLOB_H"
        if command -v python3 >/dev/null 2>&1; then
            python3 - "$PWD/build/mac-launcher-universal" "$BLOB_H" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
with open(dst, 'w') as f:
    f.write('/* 自动生成：mac-launcher universal Mach-O 数据块，请勿手改 */\n')
    f.write('static const unsigned char e1_mac_launcher_blob[] = {\n')
    for i in range(0, len(data), 12):
        f.write('    ' + ','.join('0x%02x' % b for b in data[i:i+12]) + ',\n')
    f.write('};\nstatic const unsigned int e1_mac_launcher_blob_len = %d;\n' % len(data))
PY
        else
            # 无 python3 时用 od 兜底
            {
                echo '/* 自动生成：mac-launcher universal Mach-O 数据块 */'
                echo 'static const unsigned char e1_mac_launcher_blob[] = {'
                od -An -tx1 -v build/mac-launcher-universal | tr -s ' ' | \
                    sed 's/ $//;s/^ /0x/;s/ /,0x/g;s/.*/    &,/'
                echo '};'
                echo "static const unsigned int e1_mac_launcher_blob_len = $(wc -c < build/mac-launcher-universal | tr -d ' ');"
            } > "$BLOB_H"
        fi
    fi
}

# 编译 e1wine 的公共额外参数（在 ensure_blob 之后赋值，见脚本末尾）
BLOB_FLAGS=""

build_one() {
    t="$1"; o="$2"
    case "$t" in
    linux)
        [ -x "$ZIG" ] || { echo "错误: 缺少 $ZIG" >&2; exit 1; }
        # shellcheck disable=SC2086
        "$ZIG" cc -target x86_64-linux-musl -Os -s -static \
            -DE1WINE_PLATFORM='"linux-x86_64"' $BLOB_FLAGS -o "$o" "$SRC"
        ;;
    linux-arm64)
        [ -x "$ZIG" ] || { echo "错误: 缺少 $ZIG" >&2; exit 1; }
        # 注意：aarch64 上无法直接运行 x86_64 PE；该目标主要提供 convert/信息工具，
        # PE 运行需 box64/qemu 用户态模拟（见 E1WINE.md）。
        # shellcheck disable=SC2086
        "$ZIG" cc -target aarch64-linux-musl -Os -s -static \
            -DE1WINE_PLATFORM='"linux-aarch64"' $BLOB_FLAGS -o "$o" "$SRC"
        ;;
    macos)
        cc=${CC:-cc}
        # shellcheck disable=SC2086
        "$cc" -O2 -Wall -DE1WINE_PLATFORM='"macos-x86_64"' $BLOB_FLAGS \
            -o "$o" "$SRC"
        ;;
    macos-universal)
        cc=${CC:-clang}
        command -v "$cc" >/dev/null 2>&1 || cc=cc
        # shellcheck disable=SC2086
        "$cc" -O2 -arch x86_64 -arch arm64 -mmacosx-version-min=10.13 \
            -DE1WINE_PLATFORM='"macos-universal"' $BLOB_FLAGS -o "$o" "$SRC"
        ;;
    unix)
        cc=${CC:-cc}
        plat="unix-$(uname -s)-$(uname -m)"
        # shellcheck disable=SC2086
        "$cc" -O2 -DE1WINE_PLATFORM="\"$plat\"" $BLOB_FLAGS -o "$o" "$SRC"
        ;;
    *)
        echo "错误: 未知目标 '$t'" >&2
        exit 1
        ;;
    esac
    chmod +x "$o"
    echo "e1wine ($t) -> $o"
}

if [ -z "$TARGET" ]; then
    cat >&2 <<EOF
用法: $0 <target> <输出路径>
target: linux | linux-arm64 | macos | macos-universal | unix | all
        $0 all [dist目录]   # 构建全部可构建目标到 dist/
EOF
    exit 1
fi

ensure_blob

# 此刻 blob 已就绪，组装公共编译参数
if [ -f "$BLOB_H" ]; then
    BLOB_FLAGS="-DE1WINE_HAVE_MAC_LAUNCHER -Isrc/e1wine"
fi

if [ "$TARGET" = "all" ]; then
    D=${2:-dist}
    mkdir -p "$D"
    build_one linux           "$D/e1wine-linux-x86_64"
    build_one linux-arm64     "$D/e1wine-linux-aarch64"
    if [ "$(uname -s)" = "Darwin" ]; then
        build_one macos-universal "$D/e1wine-macos-universal"
    fi
    build_one unix            "$D/e1wine-unix-$(uname -s)-$(uname -m)"
    if [ -f build/mac-launcher-universal ]; then
        cp build/mac-launcher-universal "$D/e1wine-mac-launcher-universal"
    fi
    echo "全部产物已输出到 $D/"
    exit 0
fi

[ -n "$OUT" ] || { echo "错误: 缺少输出路径" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
build_one "$TARGET" "$OUT"
