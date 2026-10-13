# -*- coding: utf-8 -*-
"""SSH / 串口双模式 GUI 冒烟测试：真实鼠标键盘操作 + 窗口截图。

目的是验证"切换模式只是把另一条挂到后台"这条硬要求：SSH 正在连接时切回
串口再切回来，会话必须还活着（不重连、不清屏、不丢状态）。

用法:
    python tools/smoke_ssh.py probe      # 只启动截图，用于确定坐标
    python tools/smoke_ssh.py run        # 跑完整流程

坐标都写在 CLIENT 里，是相对窗口客户区的偏移；改布局后先跑 probe 再看图校准。
测试期间会临时备份 %LOCALAPPDATA%/Xterm/Xterm/config.json，跑完还原，
不污染用户的真实配置。
"""
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time

from PIL import ImageGrab

EXE = r"D:\Qt\QtProject\Xterm\build\ci-build\Xterm.exe"
QT_BIN = r"D:\Qt6\6.8.3\llvm-mingw_64\bin;D:\Qt6\Tools\llvm-mingw1706_64\bin"
OUT_DIR = r"D:\Qt\QtProject\Xterm\build\smoke"
CONFIG = os.path.join(os.environ["LOCALAPPDATA"], "Xterm", "Xterm", "config.json")
BACKUP = CONFIG + ".smokebak"

# 客户区坐标，probe 后按截图校准
CLIENT = {
    "mode": (940, 60),
    "host": (300, 60),
    "connect": (430, 60),
}

user32 = ctypes.windll.user32
SendInput = user32.SendInput

# 64 位下 HWND 必须按 c_void_p 传，否则句柄会被截断成 32 位
_HW = ctypes.c_void_p


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


for _name, _args, _rest in (
    ("GetWindowThreadProcessId", (_HW, ctypes.POINTER(ctypes.c_uint)), None),
    ("IsWindowVisible", (_HW,), ctypes.c_bool),
    ("GetWindowRect", (_HW, ctypes.c_void_p), ctypes.c_bool),
    ("GetClientRect", (_HW, ctypes.c_void_p), ctypes.c_bool),
    ("ClientToScreen", (_HW, ctypes.c_void_p), ctypes.c_bool),
    ("SetForegroundWindow", (_HW,), ctypes.c_bool),
    ("ShowWindow", (_HW, ctypes.c_int), ctypes.c_bool),
    ("BringWindowToTop", (_HW,), ctypes.c_bool),
    ("AttachThreadInput", (ctypes.c_uint, ctypes.c_uint, ctypes.c_bool), ctypes.c_bool),
    ("GetForegroundWindow", (), _HW),
    ("SetWindowPos", (_HW, _HW, ctypes.c_int, ctypes.c_int,
                       ctypes.c_int, ctypes.c_int, ctypes.c_uint), ctypes.c_bool),
    ("WindowFromPoint", (POINT,), _HW),
    ("GetAncestor", (_HW, ctypes.c_uint), _HW),
    ("PostMessageW", (_HW, ctypes.c_uint, ctypes.c_void_p, ctypes.c_ssize_t),
     ctypes.c_bool),
    ("SwitchToThisWindow", (_HW, ctypes.c_bool), None),
    ("VkKeyScanW", (ctypes.c_wchar,), ctypes.c_short),
    ("MapVirtualKeyW", (ctypes.c_uint, ctypes.c_uint), ctypes.c_uint),
):
    _fn = getattr(user32, _name)
    _fn.argtypes = _args
    if _rest is not None:
        _fn.restype = _rest

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
KEYEVENTF_UNICODE = 0x0004
KEYEVENTF_KEYUP = 0x0002


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long),
                ("mouseData", ctypes.c_ulong), ("dwFlags", ctypes.c_ulong),
                ("time", ctypes.c_ulong), ("dwExtraInfo", ctypes.c_void_p)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("dwExtraInfo", ctypes.c_void_p)]


class INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("iu", INPUTUNION)]


def windows_of_pid(pid):
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        wpid = ctypes.c_uint()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        r = RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        if r.right - r.left < 200 or r.bottom - r.top < 200:
            return True  # 同名零尺寸辅助窗口
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        found.append((hwnd, buf.value, (r.left, r.top, r.right, r.bottom)))
        return True

    user32.EnumWindows(cb, None)
    return found


def foreground(hwnd):
    """把 hwnd 变成真正的前台窗口。

    Windows 的焦点窃取防护会拒绝后台进程调 SetForegroundWindow，除非调用方
    "刚刚有过键盘输入"。所以先注入一次 Alt 按下/抬起伪造键盘活动，再配合
    AttachThreadInput；仍不行就退回 SwitchToThisWindow（该 API 不受防护限制）。
    """
    press_key(VK_MENU)
    ftid = user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), None)
    mtid = kernel32_GetCurrentThreadId()
    user32.AttachThreadInput(mtid, ftid, True)
    user32.ShowWindow(hwnd, 5)  # SW_SHOW
    user32.BringWindowToTop(hwnd)
    ok = user32.SetForegroundWindow(hwnd)
    user32.AttachThreadInput(mtid, ftid, False)
    if not ok:
        user32.SwitchToThisWindow(hwnd, True)
        time.sleep(0.2)
        user32.SetForegroundWindow(hwnd)


def kernel32_GetCurrentThreadId():
    return ctypes.windll.kernel32.GetCurrentThreadId()


def client_rect(hwnd):
    r = RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r


def window_rect(hwnd):
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r


def click(hwnd, x, y):
    """客户区坐标点击（先转屏幕坐标，再 SendInput）。"""
    p = POINT(x, y)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    user32.SetCursorPos(p.x, p.y)
    time.sleep(0.08)
    for flags in (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP):
        inp = INPUT(type=INPUT_MOUSE,
                    iu=INPUTUNION(mi=MOUSEINPUT(0, 0, 0, flags, 0, None)))
        SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.04)


HWND_TOPMOST = -1
SWP_SHOWWINDOW = 0x0040


def place_window(hwnd, x, y, w, h):
    """把被测窗口钉到固定屏幕位置并置顶。

    截图用 ImageGrab 抓屏幕矩形，任何叠在上面的窗口都会污染画面；更糟的是
    click 走 ClientToScreen + SetCursorPos，被遮挡时鼠标事件落到遮挡者身上，
    表现为「点了连接没反应」这类假故障。
    """
    user32.SetWindowPos(hwnd, ctypes.c_void_p(HWND_TOPMOST), x, y, w, h,
                        SWP_SHOWWINDOW)
    time.sleep(0.4)


def top_hwnd_at(hwnd, x, y):
    """返回该客户区点位置上真正接受鼠标事件的顶层窗口，用于遮挡自检。"""
    p = POINT(x, y)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    hit = user32.WindowFromPoint(p)
    if not hit:
        return None
    root = user32.GetAncestor(hit, 2)  # GA_ROOTOWNER
    return root or hit


WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
MK_LBUTTON = 0x0001


def post_click(hwnd, x, y):
    """直接往窗口投 WM_LBUTTONDOWN/UP（客户区坐标）。

    click() 走 SetCursorPos + SendInput，受 DPI 虚拟化影响：Python 进程与
    Qt 进程的 DPI 感知上下文不同时，ClientToScreen 与截图坐标系会各自缩放，
    点击落到错误控件上，表现为「点了按钮没反应」的假故障。
    """
    lparam = (int(y) << 16) | (int(x) & 0xFFFF)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam)
    time.sleep(0.05)


WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CHAR = 0x0102


def _key_lparam(scan, up, repeats=1):
    """KF_* 打包：bit0-15 重复次数，bit16-23 扫描码，bit30 上下文，bit31 抬起位。"""
    v = (repeats & 0xFFFF) | ((scan & 0xFF) << 16)
    if up:
        v |= 0x40000000 | 0x80000000
    return v


def post_key(hwnd, vk, scan=0, ch=None):
    """往窗口直投一次按键（可选同时投 WM_CHAR）。

    走 SendInput 的键盘注入要求目标窗口真的是前台活动窗口，而自动化脚本
    常常抢不到前台（系统的焦点窃取防护），按键会打到别的程序上，得出
    「终端敲了没反应」的假结论。PostMessage 不受激活状态影响。
    """
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, _key_lparam(scan, False))
    if ch is not None:
        user32.PostMessageW(hwnd, WM_CHAR, ord(ch), _key_lparam(scan, False))
    time.sleep(0.02)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, _key_lparam(scan, True))
    time.sleep(0.02)


def post_text(hwnd, text):
    """按字符直投按键。

    只投 WM_KEYDOWN/UP，不额外投 WM_CHAR：Qt 会用 vk+scan 自己 ToUnicode
    还原 event->text()，与真实按键一致。再补一份 WM_CHAR 会让 QPlainTextEdit
    把它当第二次输入处理，出现重复字符。
    """
    for c in text:
        vk = user32.VkKeyScanW(c)
        if vk == -1:
            continue
        # VkKeyScanW 低字节是 VK，扫描码要另问 MapVirtualKey（Qt 用它做 ToUnicode）
        post_key(hwnd, vk & 0xFF, user32.MapVirtualKeyW(vk & 0xFF, 0))


def pid_of(hwnd):
    wpid = ctypes.c_uint()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
    return wpid.value


def foreground_hwnd():
    return user32.GetForegroundWindow()


def ensure_foreground(hwnd, pid):
    """确认 hwnd 真的是前台活动窗口，否则键盘注入会打到别的程序上。

    窗口 TOPMOST / 可见 不等于 已激活：只有激活窗口才收键盘。不校验就
    type_text，会得出「终端敲了没反应」的假结论。
    """
    fg = foreground_hwnd()
    if fg and pid_of(fg) == pid:
        return True
    foreground(hwnd)
    time.sleep(0.4)
    fg = foreground_hwnd()
    return bool(fg) and pid_of(fg) == pid


def type_text(text):
    for ch in text:
        for up in (False, True):
            flags = KEYEVENTF_UNICODE | (KEYEVENTF_KEYUP if up else 0)
            inp = INPUT(type=INPUT_KEYBOARD,
                        iu=INPUTUNION(ki=KEYBDINPUT(0, ord(ch), flags, 0, None)))
            SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.02)


VK_RETURN = 0x0D
VK_ESCAPE = 0x1B
VK_MENU = 0x12
SCAN_RETURN = 0x1C


def press_key(vk, scan=0):
    """按一次虚拟键（按下+抬起）。

    type_text 全程用 KEYEVENTF_UNICODE，'\\n' 只会变成字符事件，
    Qt 侧 keyPressEvent 的 key() 不是 Key_Return —— 回车必须走虚拟键码。
    """
    for flags in (0, KEYEVENTF_KEYUP):
        inp = INPUT(type=INPUT_KEYBOARD,
                    iu=INPUTUNION(ki=KEYBDINPUT(vk, scan, flags, 0, None)))
        SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.02)


def press_enter():
    press_key(VK_RETURN, SCAN_RETURN)


def shot(hwnd, name):
    wr = window_rect(hwnd)
    img = ImageGrab.grab(bbox=(wr.left, wr.top, wr.right, wr.bottom))
    path = os.path.join(OUT_DIR, name)
    img.save(path)
    print("saved", name, img.size)


def backup_config():
    if os.path.isfile(CONFIG):
        shutil.copy2(CONFIG, BACKUP)
        return True
    return False


def restore_config(existed):
    if existed and os.path.isfile(BACKUP):
        shutil.copy2(BACKUP, CONFIG)
        os.remove(BACKUP)
    elif not existed and os.path.isfile(CONFIG):
        os.remove(CONFIG)
    print("config restored")


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "probe"
    os.makedirs(OUT_DIR, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = QT_BIN + ";" + env.get("PATH", "")
    env["QT_LOGGING_TO_CONSOLE"] = "1"

    existed = backup_config()
    proc = subprocess.Popen([EXE], env=env, cwd=os.path.dirname(EXE))

    # 全新构建的首次启动会被 Defender 扫一遍，窗口可能要十几秒才出来
    wins = []
    deadline = time.time() + 30
    while time.time() < deadline:
        time.sleep(1.0)
        if proc.poll() is not None:
            print("PROCESS_EXITED code=", proc.returncode)
            restore_config(existed)
            return 1
        wins = windows_of_pid(proc.pid)
        if wins:
            break
    print("windows:", wins)
    if not wins:
        print("NO_WINDOW")
        proc.terminate()
        restore_config(existed)
        return 1
    hwnd = wins[0][0]
    foreground(hwnd)
    time.sleep(0.6)
    cr = client_rect(hwnd)
    print("client size:", cr.right, cr.bottom)

    try:
        if mode == "probe":
            shot(hwnd, "00-probe.png")
        else:
            run_steps(hwnd)
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        restore_config(existed)
    return 0


def run_steps(hwnd):
    steps = [
        ("shot", "01-serial.png"),
        ("click", "mode"),                      # 串口 → SSH
        ("shot", "02-ssh.png"),
        ("click", "host"),
        ("type", "10.255.255.1"),               # 黑洞地址，连接会挂住到超时
        ("click", "connect"),
        ("sleep", 2.0),
        ("shot", "03-ssh-connecting.png"),
        ("click", "mode"),                      # 切回串口：SSH 不能被关掉
        ("sleep", 0.8),
        ("shot", "04-serial-back.png"),
        ("click", "mode"),                      # 再切回 SSH
        ("sleep", 0.8),
        ("shot", "05-ssh-still-connecting.png"),
        ("sleep", 11.0),                        # 等 ConnectTimeout 到点
        ("shot", "06-ssh-timeout.png"),
    ]
    for step in steps:
        kind = step[0]
        if kind == "shot":
            shot(hwnd, step[1])
        elif kind == "click":
            x, y = CLIENT[step[1]]
            print("click", step[1], x, y)
            click(hwnd, x, y)
        elif kind == "type":
            type_text(step[1])
        elif kind == "sleep":
            time.sleep(step[1])
        time.sleep(0.3)


if __name__ == "__main__":
    sys.exit(main())
