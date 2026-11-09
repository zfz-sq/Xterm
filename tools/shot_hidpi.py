# -*- coding: utf-8 -*-
"""2x HiDPI screenshots: launch the packaged Xterm with QT_SCALE_FACTOR=2.

Qt then renders every glyph/widget at double pixel density (true 2x, NOT
post-upscaling), so GitHub can show crisp images on HiDPI screens.
Flow: main (serial) -> F9 SSH params -> F5 connect (password auto-loaded
from Windows Credential Manager) -> F10 SFTP dual-pane -> close & restore config.
"""
import ctypes
import os
import shutil
import subprocess
import time

from PIL import Image

EXE = r"D:\Qt\QtProject\Xterm\build\ssh-release\install\bin\Xterm.exe"
CFG = os.path.join(os.environ["LOCALAPPDATA"], "Xterm", "Xterm", "config.json")
OUT_DIR = r"D:\Qt\QtProject\Xterm\docs\screenshots"

u = ctypes.windll.user32
VK = {"F5": 0x74, "F9": 0x78, "F10": 0x79}
WM_CLOSE = 0x0010


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


def key(vk):
    u.keybd_event(vk, 0, 0, 0)
    u.keybd_event(vk, 0, 2, 0)


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


def capture_window(hwnd):
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


def foreground(hwnd):
    u.ShowWindow(hwnd, 9)
    u.keybd_event(0xA4, 0, 0, 0)
    u.SetForegroundWindow(hwnd)
    u.keybd_event(0xA4, 0, 2, 0)
    time.sleep(0.6)


def title_of(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(hwnd, buf, 256)
    return buf.value


def ensure_fit(hwnd, max_w=1880, max_h=1000):
    """Shrink/move the window fully inside the screen before PrintWindow.

    PrintWindow copies the DWM texture, and regions that stick out of the
    physical screen may never be composited there (black bands) — the 2x
    window is 2080x1360 on a 1920x1080 display. Fitting inside keeps the
    full texture live; rendering stays 2x (logical size shrinks instead).
    """
    r = RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    scale = min(1.0, max_w / w, max_h / h)
    nw, nh = int(w * scale), int(h * scale)
    if (nw, nh) == (w, h) and 0 <= r.left <= 30 and 0 <= r.top <= 30:
        return
    u.SetWindowPos(hwnd, 0, 10, 10, nw, nh, 0x4)  # SWP_NOZORDER
    time.sleep(1.5)


def find_window(pid, prefix):
    for h, t in windows_of_pid(pid):
        if t.startswith(prefix):
            return h
    return None


def main():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        pass
    if subprocess.run(["powershell", "-NoProfile", "-Command",
                       "(Get-Process Xterm -ErrorAction SilentlyContinue) -ne $null"],
                      capture_output=True, text=True).stdout.strip() == "True":
        raise SystemExit("another Xterm is running — close it first")

    backup = CFG + ".hidpi-bak"
    shutil.copy2(CFG, backup)
    env = dict(os.environ)
    env["QT_SCALE_FACTOR"] = "2"
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env)
    try:
        hwnd = None
        for _ in range(30):
            time.sleep(0.5)
            hwnd = find_window(proc.pid, "Xterm")
            if hwnd:
                break
        if not hwnd:
            raise SystemExit("main window not found")
        time.sleep(2.0)  # let the UI settle

        foreground(hwnd)
        if "串口" not in title_of(hwnd):     # start on the serial page
            key(VK["F9"])
            time.sleep(1.0)
        ensure_fit(hwnd)
        foreground(hwnd)
        capture_window(hwnd).save(OUT_DIR + r"\main.png")
        print("saved main.png")

        key(VK["F9"]); time.sleep(1.0)       # SSH parameter page
        capture_window(hwnd).save(OUT_DIR + r"\ssh.png")
        print("saved ssh.png")

        # F5 connect -> wait -> F10 SFTP; retry a couple of times
        key(VK["F5"])
        sftp = None
        for attempt in range(3):
            time.sleep(5.0 if attempt == 0 else 4.0)
            foreground(hwnd)
            key(VK["F10"]); time.sleep(2.0)
            sftp = find_window(proc.pid, "文件管理")
            if sftp:
                break
        if sftp:
            ensure_fit(sftp)
            foreground(sftp)
            capture_window(sftp).save(OUT_DIR + r"\sftp.png")
            print("saved sftp.png")
            u.PostMessageW(sftp, WM_CLOSE, 0, 0)
            time.sleep(0.8)
        else:
            print("sftp.png NOT captured (connect failed?) — keeping existing file")

        u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        for _ in range(20):
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        if proc.poll() is None:
            proc.kill()
    finally:
        shutil.copy2(backup, CFG)
        os.remove(backup)
    print("done")


if __name__ == "__main__":
    raise SystemExit(main())
