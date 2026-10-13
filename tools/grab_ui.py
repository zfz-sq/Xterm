# -*- coding: utf-8 -*-
"""Launch Xterm.exe, locate its window by PID, screenshot it."""
import ctypes
import os
import subprocess
import sys
import time

from PIL import ImageGrab

EXE = r"D:\Qt\QtProject\Xterm\build\ci-build\Xterm.exe"
ENV = r"D:\Qt6\6.8.3\llvm-mingw_64\bin;D:\Qt6\Tools\llvm-mingw1706_64\bin"

u = ctypes.windll.user32


def windows_of_pid(pid):
    result = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        wpid = ctypes.c_uint()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid:
            buf = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(hwnd, buf, 256)
            result.append((hwnd, buf.value, bool(u.IsWindowVisible(hwnd))))
        return True

    u.EnumWindows(cb, None)
    return result


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "xterm_ui.png"
    env = dict(os.environ)
    env["PATH"] = ENV + ";" + env.get("PATH", "")
    proc = subprocess.Popen([EXE], env=env, cwd=os.path.dirname(EXE))
    time.sleep(3.5)

    print("poll=", proc.poll())
    wins = windows_of_pid(proc.pid)
    print("windows:", wins)
    if proc.poll() is not None:
        print("PROCESS_EXITED code=", proc.returncode)
        return 1

    visible = [w for w in wins if w[2]]
    if not visible:
        print("NO_VISIBLE_WINDOW")
        return 1

    hwnd, title, _ = visible[0]
    print("focusing:", title)
    u.ShowWindow(hwnd, 9)  # SW_RESTORE
    u.keybd_event(0xA4, 0, 0, 0)
    u.SetForegroundWindow(hwnd)
    u.keybd_event(0xA4, 0, 2, 0)
    time.sleep(0.8)
    ImageGrab.grab().save(out)
    print("saved", out)
    proc.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
