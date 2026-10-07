#!/usr/bin/env python3
# make_release.py — 用 cookie 登录态经 GitHub 网页创建 Release 并上传资产
# （user_session cookie 无 REST API 权限，故走网页表单，同 v1.0 流程）
#
# 用法: python3 tools/make_release.py <tag> <asset_dir>
import sys, os, time, glob
from playwright.sync_api import sync_playwright

USER_SESSION = "wYT8SNyOiTplFr_IKpuXq1WMTRI89CW-QB_IouaLENFQwvHB"
REPO = "yhcmac-prog/e1LibreOS"

NOTES = """## What's new in v1.1

### e1wxfly — standalone application wrapper (WineBotter/Wineskin-style)
Split out of e1wine into its own zero-dependency Python 3 tool:
- `e1wxfly elf <exe>` — self-contained Linux program: embeds the e1wine
  runtime (base64) plus the EXE; unpacks to `~/.cache/e1wxfly/`, auto
  `chmod +x`, and execs. Target machine only needs python3.
- `e1wxfly app <exe>` — macOS `.app` bundle (universal x86_64+arm64 launcher)
- `e1wxfly wrap-x11 <binary>` — self-extracting wrapper for third-party
  X11/Wayland applications (Firefox, xterm, GIMP, ...)

### e1wm desktop
- e1Browser now supports **HTTPS** (via `openssl s_client`)
- New **X11 Apps** launcher for third-party X11/Wayland applications
- Removed four placeholder/demo apps (Contacts, Weather, Stocks, Dictionary)
  — 21 real built-in applications remain

### Toolchain / docs
- e1wine slimmed down: the `convert` subcommand moved entirely to e1wxfly
- English is now the primary `README.md`; Chinese moved to `README.zh-CN.md`
- `E1OS_NO_CC=1 ./build.sh` rebuilds images reusing previously compiled
  binaries when a cross compiler is unavailable

## Download (15 assets)

**Images** — x86_64 dual BIOS+UEFI hybrid ISOs, aarch64 UEFI raw images:

| File | Arch | Variant |
|---|---|---|
| e1LibreOS-1.0-{server,workstation,mobile}-x86_64.iso | x86_64 | CLI / GUI / touch |
| e1LibreOS-1.0-{server,workstation,mobile}-aarch64.img | aarch64 | CLI / GUI / touch |

**Tools** — e1wine for Linux (x86_64/aarch64, static musl), macOS universal,
the macOS .app launcher, a generic Unix build, the `e1wxfly` wrapper itself,
and a sample `hello.exe`.

**Source** — e1LibreOS-1.1-source.tar.gz (signed tag v1.1).

SHA-256 checksums are in `SHA256SUMS.txt`.

Login in the Live image is automatic as root with no password; run
`setup-e1os` to install (Q&A identical to Alpine setup-alpine).
"""

def main():
    tag, asset_dir = sys.argv[1], sys.argv[2]
    assets = sorted(
        p for p in glob.glob(os.path.join(asset_dir, "*"))
        if os.path.isfile(p)
    )
    assert assets, "no assets"

    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=False)
        ctx = browser.new_context(accept_downloads=True)
        ctx.add_cookies([{
            "name": "user_session", "value": USER_SESSION,
            "domain": ".github.com", "path": "/",
        }])
        page = ctx.new_page()
        page.set_default_timeout(120000)

        def _on_reqfail(req):
            print(f"   REQFAIL {req.method} {req.url[:120]} "
                  f"{req.failure}", flush=True)
        page.on("requestfailed", _on_reqfail)

        def _on_resp(resp):
            u = resp.url
            if resp.request.method in ("POST", "PUT", "PATCH", "DELETE") \
                    or resp.status >= 400:
                if any(k in u for k in ("release", "upload", "asset",
                                        "s3.amazonaws", "githubusercontent")):
                    print(f"   RESP {resp.request.method} {resp.status} "
                          f"{u[:150]}", flush=True)
        page.on("response", _on_resp)

        url = f"https://github.com/{REPO}/releases/new?tag={tag}"
        print("-> opening", url)
        page.goto(url, wait_until="domcontentloaded")

        # 登录态校验：被重定向到登录页则 cookie 失效
        if "/login" in page.url:
            sys.exit("cookie 已失效，请更新 user_session")

        # 标题（GitHub release 表单加载较慢，显式等待）
        title_sel = "input#release_name, input[name='release[name]']"
        page.wait_for_selector(title_sel, timeout=60000)
        page.fill(title_sel, f"e1LibreOS v1.1")

        # 正文
        page.fill("textarea#release_body, textarea[name='release[body]']", NOTES)

        # 上传资产（input[type=file] 常为隐藏元素，直接 set_input_files）
        inputs = page.locator("input[type=file]")
        print(f"-> found {inputs.count()} file inputs on form", flush=True)
        for i in range(inputs.count()):
            try:
                html = inputs.nth(i).evaluate("e => e.outerHTML.slice(0, 200)")
                print(f"   input[{i}]: {html}", flush=True)
            except Exception as ex:
                print(f"   input[{i}] inspect failed: {ex}", flush=True)
        # 优先 multiple 且可见性最宽松的附件输入；否则退回首例
        multi = page.locator("input[type=file][multiple]")
        file_input = multi.first if multi.count() else inputs.first
        # GitHub 的 React 组件不响应对隐藏 input 的编程式 files 赋值，
        # 必须真实点击 dropzone 标签并拦截文件选择对话框
        try:
            with page.expect_file_chooser(timeout=15000) as fc_info:
                page.locator(
                    "text=Attach binaries by dropping them here"
                ).first.click(timeout=10000)
            fc_info.value.set_files(assets)
        except Exception as ex:
            print(f"   label click failed: {ex}; force-click input",
                  flush=True)
            with page.expect_file_chooser(timeout=15000) as fc_info:
                file_input.click(force=True)
            fc_info.value.set_files(assets)
        print(f"   input.files.length = "
              f"{file_input.evaluate('e => e.files.length')}", flush=True)
        print(f"-> {len(assets)} assets queued, waiting for uploads...",
              flush=True)
        page.screenshot(path="/tmp/rel-queued.png",
                        animations="disabled", timeout=15000)

        # 上传组件状态：从 #releases-upload 向上找包含资产行文本的容器，
        # 读其 innerText（含 "Uploading… 45%" / 错误文案）
        dump_js = """() => {
          const inp = document.querySelector('#releases-upload');
          let box = inp ? inp.parentElement : null;
          for (let i = 0; box && i < 6; i++) {
            if ((box.innerText || '').length > 40) break;
            box = box.parentElement;
          }
          return box ? box.innerText.replace(/\\n{2,}/g, '\\n').slice(-1200)
                     : '(no upload box)';
        }"""
        names = [os.path.basename(p) for p in assets]
        deadline = time.time() + 35 * 60
        pending = list(names)
        stalled_for = 0
        ticks = 0
        while time.time() < deadline:
            box_text = file_input.evaluate(dump_js)
            pending = [n for n in pending if (n not in box_text)]
            uploading = ("uploading" in box_text.lower()
                         or "%" in box_text)
            err_phrases = ("something went wrong", "we were unable",
                           "failed to upload", "upload failed",
                           "saving draft failed",
                           "network error", "yowza",
                           "can’t process", "can't process")
            low = box_text.lower()
            errs = [p for p in err_phrases if p in low]
            print(f"--- poll {ticks}: pending={len(pending)} "
                  f"uploading={uploading} errs={errs}", flush=True)
            print(box_text[-600:], flush=True)
            ticks += 1
            if ticks % 6 == 1:
                try:
                    page.screenshot(path="/tmp/rel-progress.png",
                                    animations="disabled", timeout=15000)
                except Exception as ex:
                    print(f"   screenshot skipped: {ex}", flush=True)
            if errs:
                try:
                    page.screenshot(path="/tmp/rel-error.png",
                                    animations="disabled", timeout=15000)
                except Exception:
                    pass
                sys.exit("upload widget reports: " + "; ".join(errs))
            if not pending and not uploading:
                stalled_for += 1
                if stalled_for >= 2:
                    break
            else:
                stalled_for = 0
            time.sleep(5)
        if pending:
            page.screenshot(path="/tmp/rel-timeout.png")
            sys.exit(f"upload timeout, missing: {pending}")
        print("-> all assets uploaded", flush=True)

        # 发布
        page.click("button:has-text('Publish release')")
        page.wait_for_url("**/releases/tag/**", timeout=180000)
        page.wait_for_timeout(3000)
        final = page.url
        print("RELEASE_URL=", final)
        if "/releases/tag/" not in final:
            sys.exit(f"unexpected final url: {final}")
        browser.close()

if __name__ == "__main__":
    main()
