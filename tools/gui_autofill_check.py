# -*- coding: utf-8 -*-
"""代填通路回归：写入一个故意错误的密码，连接后完全不碰键盘。

如果自动代填仍然生效，终端里会自动出现 3 次「Permission denied」+ 提示行，
第 4 次提示时代填额度用尽，应改为给出「请在终端里手动输入」的说明。
全程零键盘输入，所以这些拒绝只可能来自代填路径 —— 正好把代填通路钉死。

用法: python tools/gui_autofill_check.py
"""
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_ssh import OUT_DIR, foreground, place_window, post_click, shot, \
    windows_of_pid

EXE = r"D:\Qt\QtProject\Xterm\build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Debug\Xterm.exe"
CONFIG = os.path.join(os.environ["LOCALAPPDATA"], "Xterm", "Xterm", "config.json")
BACKUP = CONFIG + ".autofill.bak"
CLIENT = {"connect": (765, 29)}
WRONG_PW = "zz_autofill_probe"


def set_wrong_password():
    with open(CONFIG, "r", encoding="utf-8") as f:
        root = json.load(f)
    for p in root.get("profiles", []):
        if p.get("name") == root.get("lastProfile"):
            p.setdefault("ssh", {})["password"] = WRONG_PW
    with open(CONFIG, "w", encoding="utf-8") as f:
        json.dump(root, f, ensure_ascii=False)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.isfile(CONFIG):
        print("NO_CONFIG：先在界面里连接过一台主机（写入配置文件）")
        return 1
    shutil.copy2(CONFIG, BACKUP)
    set_wrong_password()

    errf = open(os.path.join(OUT_DIR, "autofill-stderr.log"), "wb")
    proc = subprocess.Popen([EXE], stderr=errf)
    hwnd = None
    deadline = time.time() + 30
    while time.time() < deadline:
        time.sleep(0.5)
        wins = windows_of_pid(proc.pid)
        if wins:
            hwnd = wins[0][0]
            break
    if not hwnd:
        print("NO_WINDOW")
        shutil.copy2(BACKUP, CONFIG)
        os.remove(BACKUP)
        return 1

    place_window(hwnd, 0, 0, 1056, 719)
    foreground(hwnd)
    try:
        post_click(hwnd, CLIENT["connect"][0], CLIENT["connect"][1])
        # 只等不碰键盘：任何 "Permission denied" 都只能来自代填
        for sec in (5, 10, 16):
            time.sleep(5.0 if sec == 5 else 5.0 if sec == 10 else 6.0)
            shot(hwnd, "af-%02ds.png" % sec)
            print("shot at %ds" % sec)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        errf.close()
        shutil.copy2(BACKUP, CONFIG)
        os.remove(BACKUP)
    with open(os.path.join(OUT_DIR, "autofill-stderr.log"), "rb") as f:
        leaked = f.read().decode("utf-8", "replace")
    print("stderr leaked bytes=%d%s" % (len(leaked), "  <-- 不该有内容" if leaked else ""))
    print("config restored")
    return 0


if __name__ == "__main__":
    sys.exit(main())
