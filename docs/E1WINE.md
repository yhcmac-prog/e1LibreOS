# e1wine —— 跨平台 EXE 转换工具

e1wine 是 e1LibreOS 的 Wine 变种工具。`convert` 子命令把现有的 Windows EXE
**包装**为目标平台可直接分发的产物（运行时包装，不做机器码静态重编译）：

- **Linux ELF**：生成单文件自解压程序，**自动加权 0755**，目标机上直接 `./` 运行
- **macOS .app**：生成标准应用包（双架构启动器 + EXE 载荷 + Info.plist）
- **Unix**：用宿主编译器原生构建的通用版（FreeBSD/其它 Unix 可用）

## 构建

```sh
./tools/build-e1wine.sh all dist
```

| 产物 | 说明 |
|---|---|
| `dist/e1wine-linux-x86_64` | x86_64-linux-musl 全静态，无 glibc 依赖 |
| `dist/e1wine-linux-aarch64` | aarch64-linux-musl 全静态 |
| `dist/e1wine-macos-universal` | x86_64 + arm64 双架构 Mach-O |
| `dist/e1wine-mac-launcher-universal` | .app 内嵌的 universal 启动器 |
| `dist/e1wine-unix-<sys>-<arch>` | 宿主 cc 原生编译的 Unix 版 |

单目标构建：`./tools/build-e1wine.sh linux dist/e1wine`（或 `linux-arm64` /
`macos` / `macos-universal` / `unix`）。

## 用法

```text
e1wine convert --elf  [ -n 名称 ] [ -o 输出文件 ] <input.exe>
e1wine convert --app  [ -n 名称 ] [ -i icon.icns ] [ -o Name.app ] <input.exe>
e1wine --version
e1wine --info <file>      查看自解压包内嵌的 EXE 信息
e1wine --list <file>      列出内嵌载荷
```

### 1. EXE → Linux 自解压 ELF（自动加权）

```sh
./e1wine-linux-x86_64 convert --elf hello.exe -o hello
# 产物布局: [e1wine ELF][原始 EXE][trailer "E1WINEP1"]
# 自动 chmod 0755，拷到任意 Linux x86_64 机器即可：
./hello
```

运行时 e1wine 从自身尾部读出 EXE（CRC32 校验），解包到
`~/.cache/e1wine/` 并自动 `chmod +x` 后加载执行。全静态 musl 二进制，
目标机无需安装任何运行库。

### 2. EXE → macOS .app

```sh
./e1wine-macos-universal convert --app hello.exe -n Hello -i AppIcon.icns
open Hello.app
```

生成：

```
Hello.app/Contents/MacOS/e1wine-launcher   # x86_64+arm64 universal
Hello.app/Contents/Resources/hello.exe     # 原始 EXE
Hello.app/Contents/Info.plist
Hello.app/Contents/Resources/AppIcon.icns  # 可选
```

启动器在运行时委托系统 Wine 执行 EXE；未安装 Wine 时会弹窗给出安装指引。
Apple Silicon 需要 Rosetta 2 + Wine（如 Wine Staging / CrossOver）。

### 3. Unix 版

```sh
./tools/build-e1wine.sh unix /usr/local/bin/e1wine   # 用当前平台 cc 原生编译
e1wine convert --elf app.exe -o app && ./app
```

`--info` / `--list` 可读取任意自解压包中内嵌 EXE 的名称、大小与 CRC。

## 说明与限制

- e1wine 是**包装器/加载器**，不包含 PE→ELF 指令翻译；EXE 的实际执行依赖
  目标机 Wine（macOS/Linux）。
- Linux x86_64 上推荐安装 Wine：`apk add wine`（或系统对应包）。
- aarch64 Linux 运行 x86_64 EXE 需要 box64 / qemu-user 级别的用户态模拟。
- 载荷完整性由 trailer 中的 CRC32 保证；trailer magic 为 `E1WINEP1`。
