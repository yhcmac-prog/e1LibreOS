#!/usr/bin/env python3
# vbox-touch.py — 用 VirtualBox XPCOM API 向运行中的 VM 注入绝对触摸/手势事件
# 前置：VM 已配置 --mouse usbtablet（USB 手写板走 EV_ABS，e1wm mobile 按触摸处理）
# 用法:
#   python3 tools/vbox-touch.py <VM名> tap <x> <y>
#   python3 tools/vbox-touch.py <VM名> swipe <x1> <y1> <x2> <y2> [步数]
# 坐标系：虚拟机屏幕像素（mobile 800x600），内部归一化到 0..0xffff
import sys
import time

VBOX = "/Applications/VirtualBox.app/Contents/MacOS"
sys.path.insert(0, VBOX)
sys.path.insert(0, VBOX + "/sdk/installer/python/vboxapi/src")
sys.path.insert(0, VBOX + "/sdk/bindings/xpcom/python")
import vboxapi  # noqa: E402

vmname = sys.argv[1]
action = sys.argv[2]
args = [int(a) for a in sys.argv[3:]]

# 取当前虚拟机分辨率（mobile 默认 800x600）
mgr = vboxapi.VirtualBoxManager()
vbox = mgr.getVirtualBox()
mach = vbox.findMachine(vmname)
try:
    W = mach.displayData.screenWidth(0) or 800
    H = mach.displayData.screenHeight(0) or 600
except Exception:
    W, H = 800, 600

sess = mgr.getSessionObject()
mach.lockMachine(sess, 1)  # LockType_Shared
try:
    mouse = sess.console.mouse

    def put(x, y, btn):
        ax = int(x * 0xffff / max(W - 1, 1))
        ay = int(y * 0xffff / max(H - 1, 1))
        try:
            mouse.putMouseEventAbsolute(ax, ay, 0, 0, btn, 0)
        except TypeError:
            mouse.putMouseEventAbsolute(ax, ay, 0, 0, btn)

    if action == "tap":
        x, y = args
        put(x, y, 1); time.sleep(0.05)
        put(x, y, 0)
    elif action == "swipe":
        x1, y1, x2, y2 = args[:4]
        steps = args[4] if len(args) > 4 else 12
        put(x1, y1, 1); time.sleep(0.15)      # 触下并短暂停留
        for i in range(1, steps + 1):
            put(x1 + (x2 - x1) * i // steps,
                y1 + (y2 - y1) * i // steps, 1)
            time.sleep(0.03)
        time.sleep(0.05)
        put(x2, y2, 0)
    print("OK %s@%dx%d" % (action, W, H))
finally:
    sess.unlockMachine()
