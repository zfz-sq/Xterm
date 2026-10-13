# -*- coding: utf-8 -*-
"""SSH 真机 GUI 验证：启动 Debug 版 Xterm，切到 SSH 模式、点连接、截图看终端。

用法:
    python tools/gui_ssh_check.py probe     # 只启动 + 截图，用于确定按钮坐标
    python tools/gui_ssh_check.py run       # 完整跑一次并截图留证

会临时把 config.json 的 mode 改成 ssh 并备份，跑完还原。
"""
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_ssh import OUT_DIR, click, client_rect, foreground, press_enter, shot, \
    type_text, windows_of_pid

EXE = r"D:\Qt\QtProject\Xterm\build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Debug\Xterm.exe"
CONFIG = os.path.join(os.environ["LOCALAPPDATA"], "Xterm", "Xterm", "config.json")
BACKUP = CONFIG + ".guibak"

# 客户区坐标（probe 截图按边框偏移 8,31 换算而来）
CLIENT = {
    "host": (132, 29),
    "connect": (765, 29),
    "term": (442, 369),
}


def prepare_config():
    existed = os.path.isfile(CONFIG)
    if existed:
        shutil.copy2(CONFIG, BACKUP)
        with open(CONFIG, "r", encoding="utf-8") as f:
            root = json.load(f)
        root["mode"] = "ssh"          # 直接开在 SSH 页，省一次模式切换
        with open(CONFIG, "w", encoding="utf-8") as f:
            json.dump(root, f, ensure_ascii=False)
    return existed


def restore_config(existed):
    if existed:
        shutil.copy2(BACKUP, CONFIG)
        os.remove(BACKUP)
    print("config restored")


def main():
    step = sys.argv[1] if len(sys.argv) > 1 else "probe"
    os.makedirs(OUT_DIR, exist_ok=True)
    existed = prepare_config()
    proc = subprocess.Popen([EXE])

    hwnd = None
    deadline = time.time() + 30
    while time.time() < deadline:
        time.sleep(1.0)
        wins = windows_of_pid(proc.pid)
        if wins:
            hwnd = wins[0][0]
            break
    if not hwnd:
        print("NO_WINDOW")
        restore_config(existed)
        return 1

    foreground(hwnd)
    time.sleep(1.0)
    print("client:", client_rect(hwnd).right, client_rect(hwnd).bottom)
    try:
        shot(hwnd, "gui-00-initial.png")
        if step == "run":
            # 1) 光标点到主机框，确保模式/主机已就位；2) 点「连接」
            click(hwnd, *CLIENT["host"])
            shot(hwnd, "gui-01-before-connect.png")
            click(hwnd, *CLIENT["connect"])
            time.sleep(3.0)
            shot(hwnd, "gui-02-after-3s.png")     # 应看到 shell 提示符
            time.sleep(3.0)
            shot(hwnd, "gui-03-after-6s.png")     # 与上一张应当几乎一致(不刷屏)
            # 3) 在终端里敲一条命令验证可输入、可提交、有输出
            click(hwnd, CLIENT["term"][0], CLIENT["term"][1])
            type_text("echo GUI_MARKER_777")
            shot(hwnd, "gui-04-typed.png")         # 应有回显但未提交
            press_enter()
            time.sleep(2.0)
            shot(hwnd, "gui-05-entered.png")       # 应出现 GUI_MARKER_777 输出
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        restore_config(existed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
