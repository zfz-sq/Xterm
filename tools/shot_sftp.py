# -*- coding: utf-8 -*-
"""Attach to the running Xterm, press F10, screenshot the SFTP window, close it.

The main process and its SSH session are left untouched (SFTP window gets
WM_CLOSE only). Run while Xterm is connected to an SSH target.
"""
import ctypes
import os
import time

from PIL import Image, ImageGrab

OUT = r"D:\Qt\QtProject\Xterm\docs\screenshots\sftp.png"

u = ctypes.windll.user32
WM_CLOSE = 0x0010


def windows_of_pid(pid):
    result = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        wpid = ctypes.c_uint()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid:
            buf = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(hwnd, buf, 256)
            if u.IsWindowVisible(hwnd):
                result.append((hwnd, buf.value))
        return True

    u.EnumWindows(cb, None)
    return result


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]


def capture_window(hwnd):
    """PrintWindow-based capture: works even when the window is obscured."""
    r = RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    gdi = ctypes.windll.gdi32
    hdc = u.GetWindowDC(hwnd)
    mem = gdi.CreateCompatibleDC(hdc)
    bmp = gdi.CreateCompatibleBitmap(hdc, w, h)
    gdi.SelectObject(mem, bmp)
    u.PrintWindow(hwnd, mem, 2)  # PW_RENDERFULLCONTENT
    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth, bmi.biHeight, bmi.biPlanes = w, -h, 1
    bmi.biBitCount = 32
    data = ctypes.create_string_buffer(w * h * 4)
    gdi.GetDIBits(mem, bmp, 0, h, data, ctypes.byref(bmi), 0)
    gdi.DeleteObject(bmp)
    gdi.DeleteDC(mem)
    u.ReleaseDC(hwnd, hdc)
    return Image.frombuffer("RGBA", (w, h), data, "raw", "BGRA", 0, 1)


def main():
    import subprocess
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        pass

    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-Process Xterm -ErrorAction Stop).Id"],
        capture_output=True, text=True)
    pid = int(out.stdout.strip())
    wins = windows_of_pid(pid)
    main_w = [w for w in wins if w[1].startswith("Xterm")]
    print("pid", pid, "windows:", wins)
    if not main_w:
        raise SystemExit("main window not found")
    hwnd = main_w[0][0]

    u.ShowWindow(hwnd, 9)
    u.keybd_event(0xA4, 0, 0, 0)
    u.SetForegroundWindow(hwnd)
    u.keybd_event(0xA4, 0, 2, 0)
    time.sleep(0.6)

    u.keybd_event(0x79, 0, 0, 0)  # F10
    u.keybd_event(0x79, 0, 2, 0)
    time.sleep(3.0)  # first open builds the SFTP channel + listings

    wins = windows_of_pid(pid)
    sftp = [w for w in wins if "文件管理" in w[1]]
    if not sftp:
        print("SFTP window not found; is SSH connected? windows:", wins)
        return 1

    hwnd_s, title = sftp[0]
    time.sleep(0.5)
    img = capture_window(hwnd_s)
    img.save(OUT)
    print("saved", OUT, img.size, "title:", title)

    u.PostMessageW(hwnd_s, WM_CLOSE, 0, 0)  # close SFTP only
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
