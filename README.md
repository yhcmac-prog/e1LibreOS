# e1LibreOS

[简体中文](README.zh-CN.md) | **English**

e1LibreOS 1.0 (Libre) is a minimal bootable Linux distribution built around the
Fedora philosophy of free software. Both the kernel and user space come entirely
from upstream free-software components, and the system images can be
cross-built on macOS or Linux using plain scripts — no root access required.

## Highlights

- **Small yet complete**: Alpine `linux-virt` 6.12 kernel + static busybox user
  space; the Live system runs entirely from memory (initramfs)
- **Three variants**
  - `server`: command line only, smallest footprint
  - `workstation`: the in-house framebuffer window manager **e1wm** (no
    X11/Wayland — it renders directly to `/dev/fb0`)
  - `mobile`: 800×600 touchscreen layout with an on-screen keyboard, sharing e1wm
- **Two architectures**: x86_64 (dual BIOS + UEFI bootable ISO) and
  aarch64 (UEFI raw `.img`)
- **In-house ecosystem**
  - [e1pkg](rootfs/usr/bin/e1pkg): dnf-style package manager (post-order
    dependency resolution with reverse-dependency protection)
  - [e1wine](src/e1wine/e1wine.c): lightweight Win32 PE compatibility layer
    (runs and inspects PE files)
  - [e1wxfly](src/e1wxfly/e1wxfly): application wrapper (EXE → ELF / `.app`,
    plus X11/Wayland app packaging, à la WineBotter / Wineskin)
  - [e1gpt](src/e1gpt): dependency-free GPT partitioning tool
  - [e1apk](src/e1apk): APK packaging tool
- **21 built-in graphical apps**: Finder, System Settings, Terminal, the
  e1Browser (HTTP/HTTPS), Calculator, Calendar, Clock, Notes, Activity Monitor,
  Disk Utility, Screenshot, Installer, an X11 apps launcher, and more
- **[setup-e1os](rootfs/usr/sbin/setup-e1os) installer**: its Q&A flow mirrors
  Alpine's `setup-alpine` one-to-one and supports five install modes (see
  [docs/INSTALL.md](docs/INSTALL.md))

## Quick Start

### Download prebuilt images

Get them from [GitHub Releases](../../releases):

| File | Architecture | Firmware | Notes |
|---|---|---|---|
| `e1LibreOS-1.0-server-x86_64.iso` | x86_64 | BIOS+UEFI | Command line |
| `e1LibreOS-1.0-workstation-x86_64.iso` | x86_64 | BIOS+UEFI | Graphical desktop |
| `e1LibreOS-1.0-mobile-x86_64.iso` | x86_64 | BIOS+UEFI | Touchscreen variant |
| `e1LibreOS-1.0-{server,workstation,mobile}-aarch64.img` | aarch64 | UEFI | `dd` to disk / SD card |

### Virtual machine (VirtualBox)

```sh
./run-vbox.sh            # create and start an x86_64 BIOS VM
```

Or create a VM manually: type "Other Linux 6.x", at least 512 MB RAM and 4 GB
disk, attach the ISO. The Live environment logs in as root automatically with no
password; after installation the root password is whatever you set in the
installer prompts.

### Bare metal / USB stick

```sh
# x86_64: the ISO is isohybrid and can be dd'd directly
sudo dd if=build/e1LibreOS-1.0-workstation-x86_64.iso of=/dev/sdX bs=4M conv=fsync
# aarch64: raw disk image
sudo dd if=build/e1LibreOS-1.0-workstation-aarch64.img of=/dev/sdX bs=4M conv=fsync
```

### Installing to disk

After booting the Live system:

```sh
setup-e1os                       # interactive (setup-alpine-compatible Q&A)
setup-e1os --auto /dev/sda       # non-interactive; answers via environment vars
```

## Building from source

Requirements: macOS or Linux with `python3`, `curl`, `gzip`, and `tar`. The
x86_64 cross toolchain (zig) is fetched automatically on first build. See
[docs/BUILDING.md](docs/BUILDING.md) for details.

```sh
./build.sh                                              # x86_64 server ISO
E1OS_VARIANT=workstation ./build.sh                     # x86_64 workstation ISO
E1OS_VARIANT=mobile ./build.sh                          # x86_64 mobile ISO
E1OS_ARCH=aarch64 E1OS_VARIANT=workstation ./build.sh   # aarch64 .img
```

## Documentation

- [docs/BUILDING.md](docs/BUILDING.md): build guide (macOS/Linux, cross
  toolchain, troubleshooting)
- [docs/INSTALL.md](docs/INSTALL.md): the five install modes and the
  `setup-e1os` question flow
- [docs/E1WINE.md](docs/E1WINE.md): e1wine and e1wxfly cross-platform tools
- [docs/e1repo.md](docs/e1repo.md): e1repo dependency repository layout and
  how to self-host one
- [CONTRIBUTING.md](CONTRIBUTING.md): build, coding conventions, and pull
  request workflow

## Source layout

```
build.sh                 one-shot build script (variant/architecture matrix)
rootfs/                  initramfs skeleton (init, etc, in-house binaries)
packages/src/            sample e1pkg package sources (hello / cowsay / e1fetch)
src/e1wine/              e1wine PE compatibility layer (C)
src/e1wxfly/             e1wxfly app wrapper (Python)
src/e1gpt/               GPT partitioning tool (C)
src/e1apk/               APK packaging tool (C)
src/fbui/                e1wm framebuffer window manager + 21 apps (C)
tools/                   mkinitramfs / mkfat32 / isohybrid / build helpers
sign_github.py           GitHub cookie-login helper script
```

## Free software

e1LibreOS only aggregates free software; each upstream component keeps its
original license (GPL-2.0-only and others). All original code in this
repository is released under the MIT license.
