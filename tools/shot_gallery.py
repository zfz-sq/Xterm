# -*- coding: utf-8 -*-
"""Generate README gallery screenshots from the packaged Xterm.exe.

Launches install/bin/Xterm.exe, drives it (hotkeys + title-bar menu clicks),
and saves window crops into docs/screenshots/. User config.json is backed up
and restored afterwards; theme is forced per-shot via the config file.
"""
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time

from PIL import ImageGrab

REPO = r"D:\Qt\QtProject\Xterm"
EXE = os.path.join(REPO, r"build\ssh-release\install\bin\Xterm.exe")
OUT = os.path.join(REPO, r"docs\screenshots")
CONFIG = os.path.expandvars(r"%LOCALAPPDATA%\Xterm\Xterm\config.json")

u = ctypes.windll.user32
VK = {"ENTER": 0x0D, "ESC": 0x1B, "DOWN": 0x28, "F9": 0x78, "F10": 0x79}


def dpi_aware():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # per-monitor
    except Exception:
        pass


def key(vk):
    u.keybd_event(vk, 0, 0, 0)
    u.keybd_event(vk, 0, 2, 0)  # KEYEVENTF_KEYUP


def click(x, y):
    u.SetCursorPos(x, y)
    time.sleep(0.05)
    u.mouse_event(2, 0, 0, 0, 0)  # LEFTDOWN
    u.mouse_event(4, 0, 0, 0, 0)  # LEFTUP


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


def foreground(hwnd):
    u.ShowWindow(hwnd, 9)  # SW_RESTORE
    u.keybd_event(0xA4, 0, 0, 0)  # ALT down (workaround for foreground lock)
    u.SetForegroundWindow(hwnd)
    u.keybd_event(0xA4, 0, 2, 0)
    time.sleep(0.6)


def rect_of(hwnd):
    r = ctypes.wintypes_rect()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    return r


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


ctypes.wintypes_rect = RECT


def shot(hwnd, name):
    r = rect_of(hwnd)
    img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom))
    img.save(os.path.join(OUT, name))
    print("saved", name, img.size)


def launch(theme, backup):
    cfg = dict(backup) if backup else {}
    cfg["theme"] = theme
    os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
    with open(CONFIG, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    time.sleep(3.5)
    wins = [w for w in windows_of_pid(proc.pid) if w[1]]
    if not wins:
        proc.kill()
        raise SystemExit(f"no titled window for theme={theme}")
    return proc, wins[0][0]


def main():
    dpi_aware()
    os.makedirs(OUT, exist_ok=True)
    backup = None
    had_config = os.path.exists(CONFIG)
    if had_config:
        with open(CONFIG, encoding="utf-8") as f:
            backup = json.load(f)
    try:
        # ---- dark theme session ----
        proc, hwnd = launch("dark", backup)
        foreground(hwnd)
        shot(hwnd, "main-dark.png")          # serial page, dark
        key(VK["F9"]); time.sleep(0.8)       # switch to SSH page
        shot(hwnd, "ssh-dark.png")
        key(VK["F9"]); time.sleep(0.4)       # back to serial

        # font dialog: title-bar "设置" menu, 3rd item = 字体
        r = rect_of(hwnd)
        click(r.left + 60, r.top + 15); time.sleep(0.6)
        for _ in range(3):
            key(VK["DOWN"]); time.sleep(0.12)
        key(VK["ENTER"]); time.sleep(1.0)
        dlg = [w for w in windows_of_pid(proc.pid) if "字体" in w[1]]
        if dlg:
            shot(dlg[0][0], "font-dialog.png")
            key(VK["ESC"]); time.sleep(0.4)
        else:
            key(VK["ESC"]); print("font dialog NOT found (menu click missed?)")

        # about dialog: "帮助" menu, 3rd item = 关于 Xterm
        r = rect_of(hwnd)
        dlg = []
        for mx in (148, 156, 140):
            click(r.left + mx, r.top + 15); time.sleep(0.6)
            for _ in range(3):
                key(VK["DOWN"]); time.sleep(0.12)
            key(VK["ENTER"]); time.sleep(1.0)
            dlg = [w for w in windows_of_pid(proc.pid) if "关于 Xterm" in w[1]]
            if dlg:
                break
            key(VK["ESC"]); time.sleep(0.3)  # close stray menu, try next offset
        if dlg:
            shot(dlg[0][0], "about.png")
            key(VK["ESC"]); time.sleep(0.4)
        else:
            print("about dialog NOT found (all menu offsets missed)")

        # F10 SFTP requires a live SSH session; try, skip silently if refused
        key(VK["F10"]); time.sleep(1.0)
        others = [w for w in windows_of_pid(proc.pid)
                  if w[1] and "Xterm —" not in w[1]]
        if others:
            shot(others[0][0], "sftp.png")
            key(VK["ESC"]); time.sleep(0.3)
        else:
            print("sftp skipped (no ssh session)")
        proc.kill()

        # ---- light theme session ----
        proc, hwnd = launch("light", backup)
        foreground(hwnd)
        shot(hwnd, "main-light.png")
        proc.kill()
    finally:
        if had_config and backup is not None:
            with open(CONFIG, "w", encoding="utf-8") as f:
                json.dump(backup, f, ensure_ascii=False, indent=2)
        elif not had_config and os.path.exists(CONFIG):
            os.remove(CONFIG)
    print("done ->", OUT)


if __name__ == "__main__":
    sys.exit(main())
