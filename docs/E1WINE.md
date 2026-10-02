# e1wine / e1wxfly —— EXE 兼容与应用包装

e1LibreOS 包含两个互补的工具：

- **e1wine**：轻量 Win32 PE 兼容层（运行 / 分析 PE32+ 文件），位于
  [src/e1wine/e1wine.c](../src/e1wine/e1wine.c)
- **e1wxfly**：应用包装工具（类似 WineBotter / Wineskin），位于
  [src/e1wxfly/e1wxfly](../src/e1wxfly/e1wxfly)，纯 Python 3 零依赖

## e1wine：PE 兼容层

设计理念同 Wine（Wine Is Not an Emulator）：不模拟 CPU，把 PE32+ 映像直接
装入进程地址空间，用 `__attribute__((ms_abi))` 桥接 Windows x64 ABI 与宿主
System V ABI，并提供 kernel32 等 Win32 API 桩。

```sh
e1wine program.exe [args...]   # 直接运行 Windows PE32+ 程序
e1wine --info program.exe      # 查看 PE 头 / 节 / 导入表
e1wine --list program.exe      # 仅列出导入的 DLL
e1wine --version
```

构建全平台二进制：

```sh
./tools/build-e1wine.sh all dist
```

| 产物 | 说明 |
|---|---|
| `dist/e1wine-linux-x86_64` | x86_64-linux-musl 全静态，无 glibc 依赖 |
| `dist/e1wine-linux-aarch64` | aarch64-linux-musl 全静态 |
| `dist/e1wine-macos-universal` | x86_64 + arm64 双架构 Mach-O |
| `dist/e1wine-unix-<sys>-<arch>` | 宿主 cc 原生编译的 Unix 版 |

限制：仅 x86_64 PE32+（不支持 32 位 PE / .NET）；仅静态导入表；无 SEH、
TLS 回调与窗口子系统。复杂 Windows 程序仍建议安装完整 Wine（`apk add wine`）。

## e1wxfly：应用包装

三种包装模式：

### 1. EXE → 自包含 Linux 程序（自动加权）

```sh
e1wxfly elf hello.exe -o hello
# 产物是单个可执行 shell 脚本（0755），内含 base64(e1wine) + 原始 EXE
./hello arg1 arg2
```

运行时脚本把 e1wine 与 EXE 解到 `~/.cache/e1wxfly/`（按 CRC32 去重，
自动 `chmod +x`），然后 `exec e1wine <exe>` 并透传全部参数。目标机只需
有 `python3`（e1LibreOS 自带），无需预装 e1wine 或任何运行库。
打包时从同目录 / `dist/` 找 `e1wine-linux-x86_64`，也可用
`E1WINE_ELF_LAUNCHER` 指定。

### 2. EXE → macOS .app

```sh
E1WINE_MAC_LAUNCHER=./e1wine-mac-launcher-universal \
  e1wxfly app hello.exe -n Hello -i AppIcon.icns
open Hello.app
```

生成：

```
Hello.app/Contents/MacOS/e1wine-launcher   # x86_64+arm64 universal
Hello.app/Contents/Resources/hello.exe     # 原始 EXE
Hello.app/Contents/Info.plist
Hello.app/Contents/Resources/AppIcon.icns  # 可选
```

启动器运行时委托系统 Wine 执行 EXE；未安装 Wine 时弹窗给出指引。
Apple Silicon 需要 Rosetta 2 + Wine（Wine Staging / CrossOver 等）。

### 3. X11 / Wayland 应用包装

```sh
e1wxfly wrap-x11 /usr/bin/firefox -n firefox -o firefox-portable
./firefox-portable
```

把第三方 X11/Wayland 应用（Firefox、xterm、GIMP 等）与解包启动器合成单文件，
首次运行自动解包到 `~/.cache/e1wxfly/` 并加权执行。在 e1wm 桌面的
“X11 应用”入口可查看已安装的 X11 程序（`apk add firefox` 等安装）。

## 说明

- 载荷完整性由 trailer 中的 CRC32 保证；e1wxfly 的 trailer magic 为
  `E1WXFLY1`（旧版 e1wine convert 为 `E1WINEP1`，已废弃）。
- aarch64 Linux 运行 x86_64 EXE 仍需 box64 / qemu-user 级别的用户态模拟。
