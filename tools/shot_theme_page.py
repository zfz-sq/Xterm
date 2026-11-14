# -*- coding: utf-8 -*-
"""Click the theme tab on Xterm's side card, optionally scroll the list, screenshot."""
import ctypes
import ctypes.wintypes
import os
import subprocess
import sys
import time

from PIL import ImageGrab

EXE = r"D:\Qt\QtProject\Xterm\build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Debug\Xterm.exe"
ENV = r"D:\Qt6\6.8.3\llvm-mingw_64\bin"

u = ctypes.windll.user32


def click(x, y):
    u.SetCursorPos(int(x), int(y))
    time.sleep(0.1)
    u.mouse_event(2, 0, 0, 0, 0)  # LEFTDOWN
    u.mouse_event(4, 0, 0, 0, 0)  # LEFTUP


def wheel(x, y, delta):
    u.SetCursorPos(int(x), int(y))
    time.sleep(0.1)
    u.mouse_event(0x0800, 0, 0, int(delta), 0)  # WHEEL


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "theme_page.png"
    scrolls = int(sys.argv[2]) if len(sys.argv) > 2 else 0  # 向下滚动的滚轮格数
    env = dict(os.environ)
    env["PATH"] = ENV + ";" + env.get("PATH", "")
    proc = subprocess.Popen([EXE], env=env, cwd=os.path.dirname(EXE))
    time.sleep(4.0)
    if proc.poll() is not None:
        print("PROCESS_EXITED code=", proc.returncode)
        return 1

    hwnd = u.FindWindowW(None, "Xterm — 串口终端")
    if not hwnd:
        print("window not found")
        return 1
    u.ShowWindow(hwnd, 9)
    u.keybd_event(0xA4, 0, 0, 0)
    u.SetForegroundWindow(hwnd)
    u.keybd_event(0xA4, 0, 2, 0)
    u.SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x0001 | 0x0004)
    time.sleep(0.8)
    rect = ctypes.wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(rect))
    print("rect:", rect.left, rect.top, rect.right, rect.bottom)
    # 主题 tab = MCP tab 左移一个 tab 位（tab 中心距 34px：按钮 24 + 间距 10）
    click(rect.right - 212, rect.top + 126)
    time.sleep(0.6)
    if scrolls:
        for _ in range(scrolls):
            wheel(rect.right - 100, rect.top + 350, -120)  # 一格 -120，向下滚
            time.sleep(0.35)
    time.sleep(0.5)
    ImageGrab.grab().save(out)
    print("saved", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
