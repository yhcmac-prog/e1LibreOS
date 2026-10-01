#!/usr/bin/env python3
# vbox-mouse.py — 用 VirtualBox XPCOM API 向运行中的 VM 注入相对鼠标事件
# 用法: python3 tools/vbox-mouse.py <VM名> <dx> <dy> [click]
#   click: left=左键单击  right=右键单击
# 光标起始位置在屏幕中心 (W/2, H/2)，相对移动即可定位任意菜单项
import sys

VBOX = "/Applications/VirtualBox.app/Contents/MacOS"
sys.path.insert(0, VBOX)
sys.path.insert(0, VBOX + "/sdk/installer/python/vboxapi/src")
sys.path.insert(0, VBOX + "/sdk/bindings/xpcom/python")
import vboxapi  # noqa: E402

vm, dx, dy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
click = sys.argv[4] if len(sys.argv) > 4 else ""

mgr = vboxapi.VirtualBoxManager()
vbox = mgr.getVirtualBox()
mach = vbox.findMachine(vm)
sess = mgr.getSessionObject()
mach.lockMachine(sess, 1)  # 1 = LockType_Shared
try:
    mouse = sess.console.mouse
    if click.startswith("drag"):
        # drag <dx> <dy> [步数]：左键按住做相对移动后松开（e1wm 把 REL×2）
        steps = int(sys.argv[5]) if len(sys.argv) > 5 else 10
        mouse.putMouseEvent(0, 0, 0, 0, 1)          # press
        import time
        for i in range(1, steps + 1):
            mouse.putMouseEvent(dx // steps, dy // steps, 0, 0, 1)
            time.sleep(0.03)
        mouse.putMouseEvent(0, 0, 0, 0, 0)          # release
    elif dx or dy:
        mouse.putMouseEvent(dx, dy, 0, 0, 0)
    if click == "left":
        mouse.putMouseEvent(0, 0, 0, 0, 1)   # press
        mouse.putMouseEvent(0, 0, 0, 0, 0)   # release
    elif click == "right":
        mouse.putMouseEvent(0, 0, 0, 1, 2)   # press
        mouse.putMouseEvent(0, 0, 0, 1, 0)   # release
    elif click.startswith("wheel"):
        dz = int(click.split(":")[1]) if ":" in click else -3
        for _ in range(abs(dz)):
            mouse.putMouseEvent(0, 0, 1 if dz > 0 else -1, 0, 0)
    print("OK")
finally:
    sess.unlockMachine()
