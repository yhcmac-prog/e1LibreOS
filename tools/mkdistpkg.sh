#!/bin/sh
# mkdistpkg.sh — 把 dist/ 中的 e1wine / e1wxfly 打成各平台安装包
# 产物（build/distpkg/）：
#   e1wine-<V>-macos.zip            macOS 拖机即用 + Install/Uninstall 命令
#   e1wine-<V>-macos.dmg            macOS 磁盘镜像（同 zip 内容）
#   e1wine-<V>-macos.pkg            macOS 安装器（安装到 /usr/local/bin）
#   e1wine-<V>-linux.tar.gz         Linux 双架构静态二进制 + install.sh
#   e1wine-<V>-unix-installer.sh    Unix/BSD 自解压安装器（POSIX sh）
#   SHA256SUMS.txt
# 用法: ./tools/mkdistpkg.sh [版本号]   # 默认 1.1
set -eu

V="${1:-1.1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
OUT="$ROOT/build/distpkg"
STAGE="$OUT/stage"

[ -f "$DIST/e1wine-macos-universal" ] || { echo "缺少 dist/e1wine-macos-universal，先跑 tools/build-e1wine.sh"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$OUT" "$STAGE"

cat > "$STAGE/README.txt" <<EOF
e1wine $V — lightweight Win32 PE compatibility layer
e1wxfly $V — application wrapper (EXE -> ELF / .app / X11)

Install (macOS):   double-click "Install.command", or
                   sudo cp e1wine e1wxfly e1wine-mac-launcher-universal /usr/local/bin/
Install (Linux):   ./install.sh                # -> /usr/local/bin
                   PREFIX=\$HOME/.local ./install.sh
Unix installer:    sudo sh e1wine-$V-unix-installer.sh   (auto-detect OS/arch)

Usage:
  e1wine program.exe            run a Win32 PE32+ program
  e1wxfly elf hello.exe         self-contained Linux wrapper
  e1wxfly app  hello.exe        macOS .app bundle
  e1wxfly wrap-x11 firefox      wrap a third-party X11/Wayland app

License: MIT (original code); see the project repository for details.
https://github.com/yhcmac-prog/e1LibreOS
EOF

# ---------------------------------------------------------------- macOS zip/dmg
MAC="$STAGE/e1wine-$V-macos"
mkdir -p "$MAC"
cp "$DIST/e1wine-macos-universal"           "$MAC/e1wine"
cp "$DIST/e1wine-mac-launcher-universal"    "$MAC/"
cp "$DIST/e1wxfly"                          "$MAC/"
chmod 0755 "$MAC/e1wine" "$MAC/e1wine-mac-launcher-universal" "$MAC/e1wxfly"
cp "$STAGE/README.txt" "$MAC/README.txt"

cat > "$MAC/Install.command" <<'EOF'
#!/bin/bash
# e1wine 安装器 — 复制到 /usr/local/bin（需要管理员授权）
set -e
cd "$(dirname "$0")"
sudo mkdir -p /usr/local/bin
sudo install -m 0755 e1wine /usr/local/bin/e1wine
sudo install -m 0755 e1wxfly /usr/local/bin/e1wxfly
sudo install -m 0755 e1wine-mac-launcher-universal /usr/local/bin/e1wine-mac-launcher-universal
echo
echo "已安装: e1wine / e1wxfly -> /usr/local/bin"
echo "试一试: e1wine --version"
read -n 1 -s -r -p "按任意键关闭..."
EOF
cat > "$MAC/Uninstall.command" <<'EOF'
#!/bin/bash
set -e
sudo rm -f /usr/local/bin/e1wine /usr/local/bin/e1wxfly /usr/local/bin/e1wine-mac-launcher-universal
echo "已卸载 e1wine / e1wxfly"
read -n 1 -s -r -p "按任意键关闭..."
EOF
chmod 0755 "$MAC/Install.command" "$MAC/Uninstall.command"

(cd "$STAGE" && zip -qr "$OUT/e1wine-$V-macos.zip" "e1wine-$V-macos")
hdiutil create -volname "e1wine $V" -srcfolder "$MAC" -format UDZO \
    -ov -quiet "$OUT/e1wine-$V-macos.dmg"

# ---------------------------------------------------------------- macOS pkg
PKGROOT="$STAGE/pkg-root/usr/local/bin"
mkdir -p "$PKGROOT"
cp "$DIST/e1wine-macos-universal"        "$PKGROOT/e1wine"
cp "$DIST/e1wine-mac-launcher-universal" "$PKGROOT/"
cp "$DIST/e1wxfly"                       "$PKGROOT/"
chmod 0755 "$PKGROOT"/*
mkdir -p "$STAGE/pkg-scripts"
cat > "$STAGE/pkg-scripts/postinstall" <<'EOF'
#!/bin/bash
echo "e1wine / e1wxfly installed to /usr/local/bin"
exit 0
EOF
chmod 0755 "$STAGE/pkg-scripts/postinstall"
pkgbuild --root "$STAGE/pkg-root" --scripts "$STAGE/pkg-scripts" \
    --identifier com.e1libreos.e1wine --version "$V" \
    --quiet "$OUT/e1wine-$V-macos.pkg"

# ---------------------------------------------------------------- Linux tar.gz
LNX="$STAGE/e1wine-$V-linux"
mkdir -p "$LNX"
cp "$DIST/e1wine-linux-x86_64"  "$LNX/"
cp "$DIST/e1wine-linux-aarch64" "$LNX/"
cp "$DIST/e1wxfly"              "$LNX/"
chmod 0755 "$LNX"/*
cp "$STAGE/README.txt" "$LNX/README.txt"
cat > "$LNX/install.sh" <<'EOF'
#!/bin/sh
# e1wine Linux 安装器 — 按 uname -m 选择静态二进制
set -eu
PREFIX="${PREFIX:-/usr/local}"
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)          BIN=e1wine-linux-x86_64 ;;
    aarch64|arm64)   BIN=e1wine-linux-aarch64 ;;
    *) echo "不支持的架构: $ARCH（请直接用宿主 cc 编译 src/e1wine/e1wine.c）" >&2; exit 1 ;;
esac
mkdir -p "$PREFIX/bin"
cp "$BIN" "$PREFIX/bin/e1wine"
cp e1wxfly "$PREFIX/bin/e1wxfly"
chmod 0755 "$PREFIX/bin/e1wine" "$PREFIX/bin/e1wxfly"
echo "已安装 ($BIN -> $PREFIX/bin/e1wine, e1wxfly)"
EOF
chmod 0755 "$LNX/install.sh"
tar -C "$STAGE" -czf "$OUT/e1wine-$V-linux.tar.gz" "e1wine-$V-linux"

# ------------------------------------------------------- Unix/BSD self-installer
UNIX="$STAGE/e1wine-$V-unix-payload"
mkdir -p "$UNIX"
cp "$DIST/e1wine-macos-universal"  "$UNIX/"
cp "$DIST/e1wine-linux-x86_64"     "$UNIX/"
cp "$DIST/e1wine-linux-aarch64"    "$UNIX/"
cp "$DIST/e1wxfly"                 "$UNIX/"
chmod 0755 "$UNIX"/*
cp "$STAGE/README.txt" "$UNIX/README.txt"
tar -C "$UNIX" -czf "$STAGE/unix-payload.tgz" .
cat > "$OUT/e1wine-$V-unix-installer.sh" <<EOF
#!/bin/sh
# e1wine $V Unix/BSD 自解压安装器 — POSIX sh，自动识别 OS/架构
# 用法: sudo sh $0 [PREFIX]      默认 PREFIX=/usr/local
#       sh $0 --uninstall
set -eu
EOF
cat >> "$OUT/e1wine-$V-unix-installer.sh" <<'EOF'
ME="$0"
case "$ME" in /*) ;; *) ME="$(pwd)/$ME" ;; esac
if [ "${1:-}" = "--uninstall" ]; then
    P="${2:-/usr/local}"
    rm -f "$P/bin/e1wine" "$P/bin/e1wxfly"
    echo "已卸载 e1wine / e1wxfly ($P/bin)"
    exit 0
fi
PREFIX="${1:-/usr/local}"

b64d() {
    base64 -d 2>/dev/null || base64 -D 2>/dev/null || openssl base64 -d -A
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
SKIP=$(awk '/^__E1WINE_PAYLOAD__$/{print NR+1; exit}' "$ME")
tail -n +"$SKIP" "$ME" | b64d > "$TMP/payload.tgz"
tar -C "$TMP" -xzf "$TMP/payload.tgz"

OS=$(uname -s); ARCH=$(uname -m)
case "$OS" in
    Darwin) BIN=e1wine-macos-universal ;;
    Linux)
        case "$ARCH" in
            x86_64)        BIN=e1wine-linux-x86_64 ;;
            aarch64|arm64) BIN=e1wine-linux-aarch64 ;;
            *) echo "不支持的架构: $ARCH" >&2; exit 1 ;;
        esac ;;
    *)
        echo "警告: $OS 未受官方支持，尝试 Linux 静态二进制（FreeBSD 需启用 Linuxulator）"
        BIN=e1wine-linux-x86_64 ;;
esac

if [ ! -w "$PREFIX" ] || [ ! -w "$PREFIX/bin" ] 2>/dev/null; then
    mkdir -p "$PREFIX/bin" 2>/dev/null || true
fi
if ! cp "$TMP/$BIN" "$PREFIX/bin/e1wine" 2>/dev/null; then
    echo "无权限写 $PREFIX/bin，请改用:  sudo sh $ME $PREFIX" >&2
    exit 1
fi
cp "$TMP/e1wxfly" "$PREFIX/bin/e1wxfly"
chmod 0755 "$PREFIX/bin/e1wine" "$PREFIX/bin/e1wxfly"
echo "已安装 ($OS/$ARCH: $BIN -> $PREFIX/bin/{e1wine,e1wxfly})"
echo "试一试: $PREFIX/bin/e1wine --version"
exit 0
__E1WINE_PAYLOAD__
EOF
base64 < "$STAGE/unix-payload.tgz" >> "$OUT/e1wine-$V-unix-installer.sh"
chmod 0755 "$OUT/e1wine-$V-unix-installer.sh"

# ---------------------------------------------------------------- checksums
rm -rf "$STAGE"
(cd "$OUT" && shasum -a 256 \
    "e1wine-$V-macos.zip" "e1wine-$V-macos.dmg" "e1wine-$V-macos.pkg" \
    "e1wine-$V-linux.tar.gz" "e1wine-$V-unix-installer.sh" > SHA256SUMS.txt)

ls -la "$OUT"
