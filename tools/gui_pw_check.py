# -*- coding: utf-8 -*-
"""复现「密码提示上敲键盘没反应」这类投诉，并区分真缺陷与脚本假故障。

三段观测：
  1) 点「连接」→ 用 ssh.exe 子进程数作为客观判据（比看像素可靠）；
  2) 阶段 A 不点终端直接投按键 —— 检验产品在 opened 时有没有把焦点交给终端；
  3) 阶段 B 点进终端再投一个错误密码 —— 键盘通路正常的话终端上必然出现
     「Permission denied」+ 新的一行 password 提示。

跑完会打印 Xterm 的 stderr 字节数：修复标准句柄继承之后应当是 0，
非 0 说明 ssh.exe 的诊断信息又绕过伪终端漏到父进程去了。

用法: python tools/gui_pw_check.py
"""
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_ssh import OUT_DIR, SCAN_RETURN, VK_RETURN, ensure_foreground, \
    pid_of, place_window, post_click, post_key, post_text, shot, \
    top_hwnd_at, windows_of_pid

# 与 gui_ssh_check.py 同一套标定坐标（图像坐标减边框偏移 8,31）
CLIENT = {
    "host": (132, 29),
    "connect": (765, 29),
    "term": (442, 369),
}

EXE = r"D:\Qt\QtProject\Xterm\build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Debug\Xterm.exe"
CONFIG = os.path.join(os.environ["LOCALAPPDATA"], "Xterm", "Xterm", "config.json")
BACKUP = CONFIG + ".pwcheck.bak"
ERRLOG = os.path.join(OUT_DIR, "pwcheck-stderr.log")


def read_password():
    with open(CONFIG, "r", encoding="utf-8") as f:
        root = json.load(f)
    name = root.get("lastProfile")
    for p in root.get("profiles", []):
        if p.get("name") == name:
            return p.get("ssh", {}).get("password", ""), name
    return "", name


def ssh_running():
    """客观判据：有没有 ssh.exe 子进程被拉起来（比看像素可靠）。"""
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "@(Get-Process ssh -ErrorAction SilentlyContinue).Count"],
        capture_output=True, text=True).stdout.strip()
    return int(out or 0)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    shutil.copy2(CONFIG, BACKUP)
    pw, prof = read_password()
    print("profile=%s password_len=%d" % (prof, len(pw)))

    errf = open(ERRLOG, "wb")
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
        return 1

    place_window(hwnd, 0, 0, 1056, 719)
    ensure_foreground(hwnd, proc.pid)
    print("foreground_ok after setup = %s" % ensure_foreground(hwnd, proc.pid))
    time.sleep(1.0)

    def gclick(name, x, y):
        """客户区坐标直投鼠标消息，绕开 SetCursorPos 的 DPI 坐标系问题。"""
        top = top_hwnd_at(hwnd, x, y)
        if top is None or pid_of(top) != proc.pid:
            print("  [warn] %s 处被其他窗口遮挡（PostMessage 不受影响）" % name)
        post_click(hwnd, x, y)

    try:
        print("ssh.exe before click = %d" % ssh_running())
        post_click(hwnd, CLIENT["connect"][0], CLIENT["connect"][1])
        time.sleep(1.0)
        print("ssh.exe after click  = %d" % ssh_running())
        shot(hwnd, "pw-00-clicked.png")
        print("shot right after connect click")
        for sec in (2, 4, 6):
            time.sleep(2.0)
            shot(hwnd, "pw-%02ds.png" % sec)
            print("shot at %ds" % sec)

        # 阶段 A：连接后不点终端，直接投按键 —— 检验产品是否把焦点放进终端
        print("[A] post keys WITHOUT clicking terminal")
        post_text(hwnd, "id")
        time.sleep(1.0)
        shot(hwnd, "pw-07-focus-type.png")
        post_key(hwnd, VK_RETURN, SCAN_RETURN)
        time.sleep(1.5)
        shot(hwnd, "pw-08-focus-enter.png")

        # 阶段 B：先点终端拿控件焦点，再投一个故意错的密码
        # 键盘通路正常的话 ssh.exe 必然回「Permission denied」并重新出提示行
        print("[B] click terminal, then post a wrong password")
        gclick("term", *CLIENT["term"])
        post_text(hwnd, "xterm_wrong_pw_probe")
        time.sleep(0.5)
        shot(hwnd, "pw-09-typed-pw.png")
        post_key(hwnd, VK_RETURN, SCAN_RETURN)
        time.sleep(4.0)
        shot(hwnd, "pw-10-after-enter.png")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        errf.close()
        shutil.copy2(BACKUP, CONFIG)
        os.remove(BACKUP)
    with open(ERRLOG, "rb") as f:
        err = f.read().decode("utf-8", "replace")
    print("stderr bytes=%d" % len(err))
    if err.strip():
        print(err[:2000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
