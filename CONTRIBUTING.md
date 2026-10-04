# Contributing to e1LibreOS

Thanks for your interest in e1LibreOS! This document explains how to build,
test, and submit changes. Small, focused patches are always welcome.

## Getting set up

You only need macOS or Linux with `python3` (≥ 3.9), `curl`, `gzip`, `tar`,
and `mkisofs` (x86_64 ISOs only). The zig 0.11.0 cross toolchain used for the
in-house C components is downloaded automatically on the first build — no root
access and no system compiler required.

See [docs/BUILDING.md](docs/BUILDING.md) for the full guide.

```sh
# build all three variants for both architectures
E1OS_VARIANT=server      ./build.sh
E1OS_VARIANT=workstation ./build.sh
E1OS_VARIANT=mobile      ./build.sh
E1OS_ARCH=aarch64 E1OS_VARIANT=workstation ./build.sh
```

If a cross compiler is unavailable in your environment
(`zig cc` hangs or is blocked), cached binaries can be reused:

```sh
E1OS_NO_CC=1 ./build.sh
```

## Repository map

| Path | What lives there | Language / conventions |
|---|---|---|
| `build.sh` | one-shot image build (variant × arch matrix) | POSIX shell, busybox-ash compatible |
| `rootfs/` | initramfs skeleton: `init`, `etc/`, in-house binaries and scripts | busybox ash; no bashisms |
| `packages/src/` | sample e1pkg packages (`hello`, `cowsay`, `e1fetch`) | `meta` + `payload/` tree |
| `src/fbui/` | e1wm framebuffer window manager and built-in apps | C, musl-static via zig |
| `src/e1wine/` | PE compatibility layer | C, musl-static / native |
| `src/e1wxfly/` | application wrapper tool | Python 3, standard library only |
| `src/e1gpt/`, `src/e1apk/` | GPT and packaging tools | C, musl-static via zig |
| `tools/` | `mkinitramfs.py`, `mkfat32.py`, `isohybrid.py`, helpers | Python 3, standard library only |

## Coding conventions

- **C components** must stay dependency-free and compile with the bundled zig
  toolchain into static musl binaries. No glibc-only or dynamic-linking
  assumptions.
- **Python tools** target Python 3.9+ and use the standard library only — they
  run both on the build host and inside the Live image.
- **Shell scripts** run under busybox ash in the initramfs: use `#!/bin/sh`,
  avoid arrays, `[[ ]]`, process substitution and other bashisms.
- Prefer minimal edits over rewrites; keep diffs focused on one change.
- User-facing strings inside the image may be Chinese or English; code comments
  in existing files are mostly Chinese — match the surrounding file.

## Adding an e1pkg package

1. Create `packages/src/<name>/meta`:

   ```ini
   name=<name>
   version=1.0
   summary=One-line description
   depends=libfoo,libbar
   ```

2. Place files exactly as they should appear in the root filesystem under
   `packages/src/<name>/payload/` (e.g. `payload/usr/bin/<name>`).
3. Rebuild; the package is picked up by the generated `repo.db`. Dependencies
   are resolved post-order, and e1pkg refuses removals that would break
   reverse dependencies — keep `depends` accurate.

## Adding an e1wm built-in app

Apps are enumerated in [src/fbui/e1wm.c](src/fbui/e1wm.c). When adding one,
keep all of these in sync: the `APP_*` enum (which drives `APP_COUNT`),
the `S_APP_*` label strings, the `app_icon_col[]` color table, and the icon
glyph / key / draw branches in `draw_app`. Prefer wiring real functionality
over placeholder demos.

## Testing your change

Before opening a pull request:

1. Build every variant affected by your change (server / workstation / mobile).
2. Boot the result:
   - `./run-vbox.sh` — x86_64 VirtualBox VM (BIOS)
   - `./run.sh` — local QEMU/VirtualBox helper, if available
3. For installer changes, run `setup-e1os` in the Live image and verify the
   boot of the installed disk.
4. For e1pkg changes, test install/remove/update plus a dependency-removal
   attempt that must be rejected.
5. For e1wm/e1wine/e1wxfly changes, exercise the feature on the image itself.

## Commit messages

The project uses Conventional Commits style, in English or Chinese:

```
feat: add <thing>
fix: <bug and its trigger>
refactor: <scope> cleanup
docs: <what changed>
chore: <maintenance>
```

Explain the **why** in the body when the change is non-obvious.

## Pull request process

1. Fork the repository and create a topic branch from `main`.
2. Keep PRs small and single-purpose; state what you tested.
3. Make sure CI/build sanity checks pass and no build artifacts
   (`build/`, `dist/`, `*.iso`, `*.img`, binaries under `rootfs/usr/bin/`)
   are accidentally committed.
4. Never commit credentials, cookies, or local toolchain caches.
5. A maintainer may request changes — please rebase rather than merge main into
   your branch when updating.

## Reporting issues

Include in an issue: host OS, the exact command you ran, architecture
(x86_64 / aarch64) and variant (server / workstation / mobile), plus the full
error output or a serial/VirtualBox log. Boot and installer bugs are easiest to
reproduce with the image attached or linked.

## Licensing

All original code in this repository is MIT-licensed; by contributing you agree
to license your contribution under the same terms. Aggregated upstream
components (kernel, busybox, etc.) keep their original licenses.
