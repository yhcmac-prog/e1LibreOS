#!/bin/sh
# build-host-tools.sh — 源码构建 e1LibreOS 需要的宿主工具（不依赖 Homebrew）
#
# 产物统一放在 build/host-tools/bin/：
#   c++        宿主 C++ 包装（CommandLineTools 的 libc++ 头不完整，改用 SDK 头）
#   ninja      Ninja 1.11.1（configure.py --bootstrap）
#   meson      Meson 1.5.1（PyPI sdist，纯 Python）
#   pkgconf    pkgconf 1.8.0 + pkg-config 软链
#   mkisofs    cdrtools 3.02a09（MacPorts 已安装时直接链接 /opt/local/bin/mkisofs）
#   qemu-system-aarch64  QEMU 11.1.3（源码构建，仅 aarch64-softmmu）
#
# 用法: sh tools/build-host-tools.sh
# 幂等：已存在可用的可执行文件就跳过对应构建。
set -eu
cd "$(dirname "$0")/.."

REPO=$(pwd)
BIN="$REPO/build/host-tools/bin"
SRC="$REPO/build/src"
DL="$REPO/downloads"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

msg() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[31m错误:\033[0m %s\n' "$*" >&2; exit 1; }
have() { [ -x "$BIN/$1" ]; }

mkdir -p "$BIN" "$SRC" "$DL"

fetch() { # <url> <out>
    [ -s "$2" ] && return 0
    msg "下载 $1"
    curl -fL --retry 3 --retry-all-errors -o "$2" "$1" || die "下载失败: $1"
}

# ---------------------------------------------------------------- c++ 包装
SDK=$(xcrun --show-sdk-path 2>/dev/null || echo /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk)
if ! have c++; then
    msg "生成宿主 C++ 包装（SDK libc++ 头）"
    cat > "$BIN/c++" <<EOF
#!/bin/sh
exec /usr/bin/c++ -nostdinc++ -isystem "$SDK/usr/include/c++/v1" "\$@"
EOF
    chmod +x "$BIN/c++"
fi
"$BIN/c++" -x c++ -E /dev/null >/dev/null 2>&1 || die "C++ 包装不可用（检查 SDK: $SDK）"

# ---------------------------------------------------------------- ninja
if ! have ninja; then
    if [ ! -d "$SRC/ninja-1.11.1" ]; then
        fetch https://codeload.github.com/ninja-build/ninja/tar.gz/refs/tags/v1.11.1 \
              "$SRC/ninja-1.11.1.tar.gz"
        tar xzf "$SRC/ninja-1.11.1.tar.gz" -C "$SRC"
    fi
    msg "构建 ninja 1.11.1"
    ( cd "$SRC/ninja-1.11.1" && rm -rf build && \
      CXX="$BIN/c++" python3 configure.py --bootstrap >/dev/null && \
      cp ninja "$BIN/ninja" ) || die "ninja 构建失败"
fi

# ---------------------------------------------------------------- meson
if ! have meson; then
    MDIR=$(ls -d "$SRC"/meson-* 2>/dev/null | head -1 || true)
    if [ -z "$MDIR" ]; then
        fetch https://files.pythonhosted.org/packages/source/m/meson/meson-1.5.1.tar.gz \
              "$SRC/meson-1.5.1.tar.gz"
        tar xzf "$SRC/meson-1.5.1.tar.gz" -C "$SRC"
        MDIR="$SRC/meson-1.5.1"
    fi
    msg "生成 meson 包装（$(basename "$MDIR")）"
    cat > "$BIN/meson" <<EOF
#!/bin/sh
exec python3 "$MDIR/meson.py" "\$@"
EOF
    chmod +x "$BIN/meson"
fi

# ---------------------------------------------------------------- pkgconf
if ! have pkgconf; then
    if [ ! -d "$SRC/pkgconf-1.8.0" ]; then
        fetch https://distfiles.ariadne.space/pkgconf/pkgconf-1.8.0.tar.gz \
              "$SRC/pkgconf-1.8.0.tar.gz"
        tar xzf "$SRC/pkgconf-1.8.0.tar.gz" -C "$SRC"
    fi
    msg "构建 pkgconf 1.8.0"
    ( cd "$SRC/pkgconf-1.8.0" && \
      ./configure --prefix="$REPO/build/host-tools" >/dev/null && \
      make -j"$JOBS" >/dev/null && make install >/dev/null ) || die "pkgconf 构建失败"
fi
ln -sf pkgconf "$BIN/pkg-config"

# ---------------------------------------------------------------- mkisofs
if ! have mkisofs; then
    if [ -x /opt/local/bin/mkisofs ]; then
        msg "使用 MacPorts cdrtools: /opt/local/bin/mkisofs"
        ln -sf /opt/local/bin/mkisofs "$BIN/mkisofs"
    else
        if [ ! -d "$SRC/cdrtools-3.02" ]; then
            fetch https://downloads.sourceforge.net/project/cdrtools/alpha/cdrtools-3.02a09.tar.bz2 \
                  "$DL/cdrtools-3.02a09.tar.bz2"
            tar xjf "$DL/cdrtools-3.02a09.tar.bz2" -C "$SRC"
        fi
        msg "构建 mkisofs（cdrtools 3.02a09）"
        # cdrtools 是 K&R 风格 C；现代 clang 默认把 implicit-int 当错误
        ( cd "$SRC/cdrtools-3.02" && \
          make -j"$JOBS" CC=clang \
               CFLAGS="-std=gnu89 -O2 -Wno-error=implicit-int \
                       -Wno-error=implicit-function-declaration \
                       -Wno-error=int-conversion \
                       -Wno-error=incompatible-function-pointer-types" >/dev/null ) \
          || die "cdrtools 构建失败"
        cp "$SRC/cdrtools-3.02/mkisofs/OBJ/"*/mkisofs "$BIN/mkisofs"
    fi
fi

# ---------------------------------------------------------------- QEMU（源码构建）
if ! have qemu-system-aarch64; then
    QDIR="$SRC/qemu/qemu-11.1.3"
    if [ ! -d "$QDIR" ]; then
        QXZ="$DL/qemu-11.1.3.tar.xz"
        [ -s "$QXZ" ] || [ -s "$DL/qemu-11.1.3.tar.tar" ] || \
            fetch https://download.qemu.org/qemu-11.1.3.tar.xz "$QXZ"
        [ -s "$QXZ" ] || QXZ="$DL/qemu-11.1.3.tar.tar"
        msg "解包 QEMU 11.1.3"
        mkdir -p "$SRC/qemu"
        xz -dc "$QXZ" | tar xf - -C "$SRC/qemu"
    fi
    # libfdt（QEMU 的 wrap 指向 gitlab.com；不通时用 GitHub 镜像并固定提交）
    if [ ! -d "$QDIR/subprojects/dtc" ]; then
        msg "获取 dtc/libfdt 子项目"
        git clone --depth 1 https://github.com/dgibson/dtc.git "$QDIR/subprojects/dtc" \
            || die "dtc 克隆失败"
        ( cd "$QDIR/subprojects/dtc" && \
          git fetch --depth 1 origin b6910bec11614980a21e46fbccc35934b671bd81 && \
          git checkout b6910bec11614980a21e46fbccc35934b671bd81 ) >/dev/null 2>&1 || true
    fi
    msg "配置 QEMU（target-list=aarch64-softmmu，无图形/pixman）"
    ( cd "$QDIR" && \
      PATH="$BIN:$PATH" \
      PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/opt/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
      ./configure --target-list=aarch64-softmmu --prefix="$REPO/build/host-tools" \
                  --disable-werror --disable-docs --disable-tools \
                  --disable-gtk --disable-sdl --disable-vnc --disable-curses \
                  --disable-spice --disable-pixman --disable-slirp \
                  --disable-capstone --enable-fdt=internal >/dev/null ) \
        || die "QEMU configure 失败（若缺 glib：sudo port install glib2）"
    msg "编译 QEMU（-j$JOBS）"
    ( cd "$QDIR" && PATH="$BIN:$PATH" ninja -C build -j"$JOBS" >/dev/null && \
      PATH="$BIN:$PATH" ninja -C build install >/dev/null ) || die "QEMU 编译失败"
fi

# ---------------------------------------------------------------- 版本清单
msg "宿主工具就绪："
"$BIN/ninja" --version       | sed 's/^/    ninja      /'
"$BIN/meson" --version       | sed 's/^/    meson      /'
"$BIN/pkgconf" --version     | sed 's/^/    pkgconf    /'
"$BIN/mkisofs" -version 2>&1 | head -1 | sed 's/^/    /'
"$BIN/qemu-system-aarch64" -version 2>&1 | head -1 | sed 's/^/    /'
