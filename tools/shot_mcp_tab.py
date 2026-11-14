# -*- coding: utf-8 -*-
"""Click the MCP tab on Xterm's side card and screenshot the page."""
import ctypes
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


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "mcp_page.png"
    env = dict(os.environ)
    env["PATH"] = ENV + ";" + env.get("PATH", "")
    proc = subprocess.Popen([EXE], env=env, cwd=os.path.dirname(EXE))
    time.sleep(4.0)
    if proc.poll() is not None:
        print("PROCESS_EXITED code=", proc.returncode)
        return 1

    hwnd = ctypes.windll.user32.FindWindowW(None, "Xterm — 串口终端")
    if not hwnd:
        print("window not found")
        return 1
    u.ShowWindow(hwnd, 9)  # SW_RESTORE
    # Alt 键 trick：绕过 Windows 前台锁定限制（同 grab_ui.py）
    u.keybd_event(0xA4, 0, 0, 0)
    u.SetForegroundWindow(hwnd)
    u.keybd_event(0xA4, 0, 2, 0)
    # MoveWindow 到 (0,0)，尺寸不变（0,0,0,0 + SWP_NOSIZE）
    u.SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x0001 | 0x0004)  # SWP_NOSIZE|SWP_NOZORDER
    time.sleep(0.8)
    rect = ctypes.wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(rect))
    print("rect:", rect.left, rect.top, rect.right, rect.bottom)
    # 卡片贴窗口右缘，tab 行在卡片顶部：实测 MCP tab 约在右缘内 178px、顶部下 126px
    # （tab 间距 2→10 后 MCP tab 右移 2×8=16px，2026-11-14 同步；标题栏高度若再调整，
    #   此偏移需同步：偏移 = 标题栏高 + 卡片顶边距 + tab 半高，tab 位移 = 前置间隔数×间距增量）
    click(rect.right - 178, rect.top + 126)
    time.sleep(0.8)
    ImageGrab.grab().save(out)
    print("saved", out)
    return 0


if __name__ == "__main__":
    import ctypes.wintypes  # noqa: F401  RECT 需要
    sys.exit(main())
