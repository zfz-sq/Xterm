# -*- coding: utf-8 -*-
"""Attach to the RUNNING Xterm (light theme) and capture light-theme shots.

PrintWindow-based (immune to overlap); drives F9 and title-bar menus.
The instance and its SSH session are left untouched.
"""
import ctypes
import subprocess
import time

from PIL import Image

OUT_DIR = r"D:\Qt\QtProject\Xterm\docs\screenshots"

u = ctypes.windll.user32
VK = {"ENTER": 0x0D, "ESC": 0x1B, "DOWN": 0x28, "F9": 0x78}
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


def click(x, y):
    u.SetCursorPos(x, y)
    time.sleep(0.05)
    u.mouse_event(2, 0, 0, 0, 0)
    u.mouse_event(4, 0, 0, 0, 0)


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


def open_menu_dialog(pid, hwnd, btn_x, item_downs, title_part):
    """Click a title-bar menu button, navigate with DOWN, ENTER; retry on foreground races."""
    for attempt in range(3):
        foreground(hwnd)  # re-raise every attempt: mouse clicks hit whatever is on top
        r = RECT()
        u.GetWindowRect(hwnd, ctypes.byref(r))
        before = set(w[0] for w in windows_of_pid(pid))
        click(r.left + btn_x, r.top + 15)
        time.sleep(0.6)
        now = windows_of_pid(pid)
        popup = any(h not in before for h, _ in now)  # QMenu popup appeared?
        if not popup:
            print(f"  attempt {attempt+1}: menu did not open, retrying")
            continue
        for _ in range(item_downs):
            key(VK["DOWN"])
            time.sleep(0.12)
        key(VK["ENTER"])
        time.sleep(1.0)
        dlg = [w for w in windows_of_pid(pid) if title_part in w[1]]
        if dlg:
            return dlg[0][0]
        key(VK["ESC"])
        time.sleep(0.3)
        print(f"  attempt {attempt+1}: dialog not found after menu nav, retrying")
    return None


def main():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        pass
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-Process Xterm -ErrorAction Stop).Id"],
        capture_output=True, text=True)
    pid = int(out.stdout.strip())
    main_w = [w for w in windows_of_pid(pid) if w[1].startswith("Xterm")]
    if not main_w:
        raise SystemExit("running Xterm main window not found")
    hwnd = main_w[0][0]
    foreground(hwnd)

    capture_window(hwnd).save(OUT_DIR + r"\main.png")
    print("saved main.png")

    key(VK["F9"]); time.sleep(0.8)                 # SSH parameter page
    capture_window(hwnd).save(OUT_DIR + r"\ssh.png")
    print("saved ssh.png")
    key(VK["F9"]); time.sleep(0.4)                 # back

    for name, btn_x, downs, title in (
            ("font-dialog.png", 60, 3, "字体"),
            ("about.png", 148, 3, "关于 Xterm")):
        hwnd_d = open_menu_dialog(pid, hwnd, btn_x, downs, title)
        if hwnd_d:
            time.sleep(0.3)
            capture_window(hwnd_d).save(OUT_DIR + "\\" + name)
            print("saved", name)
            key(VK["ESC"]); time.sleep(0.4)
        else:
            key(VK["ESC"])
            print(name, "NOT found (menu click missed)")
    print("done")


if __name__ == "__main__":
    raise SystemExit(main())
