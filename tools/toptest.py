# 诊断: 用 ConPTY(与 conpty.cpp 同样的中间层 conhost)跑 ssh + top,
# 抓 conhost 发回的原始字节流,定位「top 无任何显示」的环节。
# 用法: python tools/toptest.py [out.bin] [duration_sec]
import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import threading
import time

import ctypes.util

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
EXTENDED_STARTUPINFO_PRESENT = 0x00080000
STARTF_USESTDHANDLES = 0x00000100

# conpty.cpp: 老系统不认 INHERIT_CURSOR 时退回 0;两档都试
PSEUDOCONSOLE_INHERIT_CURSOR = 0x1


class STARTUPINFOEXW(ctypes.Structure):
    class _SI(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR),
                    ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                    ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                    ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                    ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD),
                    ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
                    ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                    ("lpReserved2", ctypes.c_void_p),
                    ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                    ("hStdError", wt.HANDLE)]

    _fields_ = [("StartupInfo", _SI),
                ("lpAttributeList", ctypes.c_void_p)]


class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("nLength", wt.DWORD), ("lpSecurityDescriptor", ctypes.c_void_p),
                ("bInheritHandle", wt.BOOL)]


kernel32.CreatePipe.argtypes = [ctypes.POINTER(wt.HANDLE), ctypes.POINTER(wt.HANDLE),
                                ctypes.POINTER(SECURITY_ATTRIBUTES), wt.DWORD]


class COORD(ctypes.Structure):
    _fields_ = [("X", wt.SHORT), ("Y", wt.SHORT)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


kernel32.CreatePseudoConsole.argtypes = [COORD, wt.HANDLE, wt.HANDLE, wt.DWORD,
                                         ctypes.POINTER(ctypes.c_void_p)]
kernel32.ResizePseudoConsole.argtypes = [ctypes.c_void_p, COORD]
kernel32.ClosePseudoConsole.argtypes = [ctypes.c_void_p]
kernel32.InitializeProcThreadAttributeList.restype = wt.BOOL
kernel32.UpdateProcThreadAttribute.restype = wt.BOOL


def die(msg):
    print("ERROR:", msg, "gle=", ctypes.GetLastError())
    sys.exit(1)


def open_pty(cols, rows):
    sa = SECURITY_ATTRIBUTES(ctypes.sizeof(SECURITY_ATTRIBUTES), None, True)
    in_rd = wt.HANDLE(); in_wr = wt.HANDLE()
    out_rd = wt.HANDLE(); out_wr = wt.HANDLE()
    if not kernel32.CreatePipe(ctypes.byref(in_rd), ctypes.byref(in_wr), ctypes.byref(sa), 0):
        die("CreatePipe in")
    if not kernel32.CreatePipe(ctypes.byref(out_rd), ctypes.byref(out_wr), ctypes.byref(sa), 0):
        die("CreatePipe out")
    size = COORD(cols, rows)
    pty = ctypes.c_void_p()
    flag = 0 if os.environ.get("TOPT_NOCURSOR") else PSEUDOCONSOLE_INHERIT_CURSOR
    hr = kernel32.CreatePseudoConsole(size, in_rd, out_wr, flag, ctypes.byref(pty))
    if hr != 0 and flag != 0:
        print("INHERIT_CURSOR failed (0x%x), retry with 0" % (hr & 0xFFFFFFFF))
        hr = kernel32.CreatePseudoConsole(size, in_rd, out_wr, 0, ctypes.byref(pty))
    if hr != 0:
        die("CreatePseudoConsole 0x%x" % (hr & 0xFFFFFFFF))
    # conhost 已复制句柄,多余的都关掉(否则读端看不到 EOF)
    kernel32.CloseHandle(in_rd)
    kernel32.CloseHandle(out_wr)
    return pty, in_wr, out_rd


def spawn(cmdline, pty):
    size = wt.DWORD(0)
    kernel32.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
    buf = ctypes.create_string_buffer(size.value)
    attr = ctypes.cast(buf, ctypes.c_void_p)
    if not kernel32.InitializeProcThreadAttributeList(attr, 1, 0, ctypes.byref(size)):
        die("InitializeProcThreadAttributeList")
    if not kernel32.UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                              pty, ctypes.sizeof(ctypes.c_void_p), None, None):
        die("UpdateProcThreadAttribute")
    si = STARTUPINFOEXW()
    si.StartupInfo.cb = ctypes.sizeof(si)
    si.lpAttributeList = attr
    # conpty.cpp 同款: 显式声明 STDHANDLES 且三句柄为 NULL, 不继承本进程句柄
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES
    pi = PROCESS_INFORMATION()
    if not kernel32.CreateProcessW(None, cmdline, None, None, True,
                                   EXTENDED_STARTUPINFO_PRESENT, None, None,
                                   ctypes.byref(si.StartupInfo), ctypes.byref(pi)):
        die("CreateProcessW")
    kernel32.DeleteProcThreadAttributeList(attr)
    kernel32.CloseHandle(pi.hThread)
    return pi.hProcess


def reader(h, sink):
    buf = ctypes.create_string_buffer(65536)
    got = wt.DWORD(0)
    while True:
        ok = kernel32.ReadFile(h, buf, len(buf), ctypes.byref(got), None)
        if not ok or got.value == 0:
            sink(None)
            return
        sink(buf.raw[:got.value])


def writer(h, data):
    put = wt.DWORD(0)
    ok = kernel32.WriteFile(h, data, len(data), ctypes.byref(put), None)
    return ok, put.value


def esc_repr(b):
    out = []
    for ch in b:
        if ch == 0x1B:
            out.append("\\e")
        elif ch == 0x0D:
            out.append("\\r")
        elif ch == 0x0A:
            out.append("\\n")
        elif ch == 0x07:
            out.append("\\a")
        elif 32 <= ch < 127:
            out.append(chr(ch))
        else:
            out.append("\\x%02x" % ch)
    return "".join(out)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "build/topdump.bin"
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 14.0

    prog = os.environ.get("TOPT_PROG")
    if prog:
        # 本地全屏程序模式: 跑任意 exe(如 Git 的 vim), 按键由 TOPT_KEYS 驱动
        os.environ.setdefault("TERM", "xterm")
        pty, tx, rx = open_pty(int(os.environ.get("TOPT_COLS", "100")),
                               int(os.environ.get("TOPT_ROWS", "30")))
        cmdline = prog
        extra = os.environ.get("TOPT_ARGS", "")
        if extra:
            cmdline += " " + extra
        t0 = time.time()
        proc = spawn(cmdline, pty)
        print("[%.2fs] launched %s" % (time.time() - t0, prog))
        chunks = []
        lock = threading.Lock()

        def sink(data):
            stamp = time.time() - t0
            with lock:
                if data is None:
                    chunks.append((stamp, None))
                    print("[%.2fs] RX EOF" % stamp)
                    return
                chunks.append((stamp, data))
            printable = "".join(chr(c) if 32 <= c < 127 else "" for c in data)
            if printable.strip():
                print("[%.2fs] RX %dB | %s" % (stamp, len(data), printable[:100]))
            else:
                print("[%.2fs] RX %dB (纯控制序列) | %s"
                      % (stamp, len(data), esc_repr(data)[:150]))
            # 只应答查询, 其他一律转发给产品侧对应的应答逻辑
            if data.find(b"\x1b[") >= 0:
                for probe, r in ((b"\x1b[6n", b"\x1b[1;1R"), (b"\x1b[?c", b"\x1b[?1;2c"),
                                  (b"\x1b[c", b"\x1b[?1;2c"), (b"\x1b[>c", b"\x1b[>0;95;0c")):
                    if probe in data:
                        writer(tx, r)
                        print("[%.2fs] TX auto-reply for %s" % (time.time() - t0, esc_repr(probe)))

        threading.Thread(target=reader, args=(rx, sink), daemon=True).start()
        keys = os.environ.get("TOPT_KEYS", "")
        for spec in (keys.split("|") if keys else []):
            delay, hexs = spec.split(":", 1)
            data = bytes.fromhex(hexs)
            def later(d=data):
                writer(tx, d)
                print("[%.2fs] TX %r" % (time.time() - t0, d))
            threading.Timer(float(delay), later).start()
        time.sleep(duration)
        print("[%.2fs] stop" % (time.time() - t0))
        kernel32.TerminateProcess(proc, 0)
        kernel32.WaitForSingleObject(proc, 2000)
        kernel32.CloseHandle(proc)
        kernel32.ClosePseudoConsole(pty)
        time.sleep(0.3)
        with open(out_path, "wb") as f:
            for _, data in chunks:
                if data:
                    f.write(data)
        total = sum(len(d) for _, d in chunks if d)
        print("落盘 %s: %d 字节, %d 个块" % (out_path, total, len(chunks)))
        return

    cfg = os.path.join(os.environ["LOCALAPPDATA"], "Xterm", "Xterm", "config.json")
    with open(cfg, "r", encoding="utf-8") as f:
        root = json.load(f)
    prof = None
    for p in root.get("profiles", []):
        if p.get("name") == root.get("lastProfile"):
            prof = p
    if prof is None and root.get("profiles"):
        prof = root["profiles"][0]
    s = prof.get("ssh", {})
    host, user, pw = s.get("host", ""), s.get("user", ""), s.get("password", "")
    port = int(s.get("port", 22) or 22)
    if not host:
        die("config.json has no ssh host")

    # conpty.cpp 同款: 向子进程声明终端类型
    os.environ["TERM"] = "xterm"
    os.environ["COLORTERM"] = "truecolor"

    pty, tx, rx = open_pty(100, 30)
    cmdline = (r"C:\Windows\System32\OpenSSH\ssh.exe -tt -o ConnectTimeout=10 "
               r"-o StrictHostKeyChecking=accept-new -p %d %s@%s"
               % (port, user, host))
    t0 = time.time()
    proc = spawn(cmdline, pty)
    print("[%.2fs] launched ssh %s@%s:%d" % (time.time() - t0, user, host, port))

    chunks = []
    lock = threading.Lock()
    tail = bytearray()

    def sink(data):
        stamp = time.time() - t0
        with lock:
            if data is None:
                chunks.append((stamp, None))
                return
            chunks.append((stamp, data))
            tail.extend(data)
            del tail[:-400]
        printable = "".join(chr(c) if 32 <= c < 127 else "" for c in data)
        if printable.strip():
            print("[%.2fs] RX %dB | %s" % (stamp, len(data), printable[:110]))
        else:
            print("[%.2fs] RX %dB (纯控制序列) | %s" % (stamp, len(data), esc_repr(data)[:110]))
        # conhost 建立时的设备查询要立即应答, 否则管道会卡死:
        #   ESC[6n  → ESC[<row>;<col>R  (光标位置, INHERIT_CURSOR 必需)
        #   ESC[?c / ESC[c → ESC[?1;2c  (Primary DA)
        #   ESC[>c  → ESC[>0;95;0c     (Secondary DA)
        if os.environ.get("TOPT_REPLY6N") and b"\x1b[6n" in data:
            writer(tx, b"\x1b[1;1R")
            print("[%.2fs] TX auto-reply CPR" % (time.time() - t0))
        if data.find(b"\x1b[") >= 0:
            for probe in (b"\x1b[?c", b"\x1b[c", b"\x1b[>c"):
                if probe in data:
                    r = b"\x1b[?1;2c" if probe != b"\x1b[>c" else b"\x1b[>0;95;0c"
                    writer(tx, r)
                    print("[%.2fs] TX auto-reply %s" % (time.time() - t0, esc_repr(r)))

    th = threading.Thread(target=reader, args=(rx, sink), daemon=True)
    th.start()

    stage = {"pw": 0, "cmd": 0, "quit": False}

    def send(data, tag):
        ok, put = writer(tx, data)
        print("[%.2fs] TX %s -> %r (%d bytes)" % (time.time() - t0, tag, data, put))

    deadline = t0 + duration
    while time.time() < deadline:
        time.sleep(0.15)
        with lock:
            low = tail.decode("utf-8", "ignore").lower()
        if "password:" in low and stage["pw"] < 3:
            stage["pw"] += 1
            tail.clear()
            send(pw.encode() + b"\r", "password #%d" % stage["pw"])
        elif ("yes/no" in low):
            tail.clear()
            send(b"yes\r", "hostkey")
        elif stage["pw"] >= 1 and stage["cmd"] == 0 and (
                low.rstrip().endswith("#") or low.rstrip().endswith("$")
                or "\x1b[?2004h" in low or "root@" in low and low.rstrip().endswith("~")):
            stage["cmd"] = 1
            tail.clear()
            send(os.environ.get("TOPT_CMD", "TERM=$TERM top").encode() + b"\r", "cmd")
        elif stage["cmd"] == 1 and time.time() - t0 > 8 and not stage["quit"]:
            stage["quit"] = True
            send(b"q", "quit top")

    print("[%.2fs] stop" % (time.time() - t0))
    kernel32.TerminateProcess(proc, 0)
    kernel32.WaitForSingleObject(proc, 2000)
    kernel32.CloseHandle(proc)
    kernel32.ClosePseudoConsole(pty)
    time.sleep(0.3)

    with open(out_path, "wb") as f:
        for _, data in chunks:
            if data:
                f.write(data)
    total = sum(len(d) for _, d in chunks if d)
    print("落盘 %s: %d 字节, %d 个块" % (out_path, total, len(chunks)))


if __name__ == "__main__":
    main()
