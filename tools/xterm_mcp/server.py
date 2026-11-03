#!/usr/bin/env python3
"""Xterm 的 MCP 服务。stdio 进，本机命名管道出。

只转发到已经打开的 Xterm，不自己占用串口或 SSH。
stdout 只写 MCP 消息，诊断写到 stderr。
"""

import json
import os
import sys

PIPE_NAME = "XtermAgent"
INSTRUCTIONS = (
    "你通过 MCP 工具操作用户电脑上已经打开的 Xterm，从而使用它当前连着的设备。"
    "不要自己打开串口，不要另起 ssh，不要去点这个软件的窗口。\n"
    "\n"
    "先调用 status。若未连接，告诉用户在 Xterm 里点「连接」，不要猜测端口、波特率或主机。\n"
    "\n"
    "用 send_line 发送一行。不要自行追加 \\n 或 \\r，软件会按当前链路补上："
    "串口（含 STM32 USB CDC）是 \\n，SSH 是 \\r。"
    "send_line 是打字机式逐字发送，用户在界面上能看到输入过程。"
    "打字期间用户终端的普通输入被软件锁定，用户按 Ctrl+C 会主动打断 AI 输入，"
    "此时调用会收到 interrupted by user 错误——这不是设备或网络故障，"
    "也不代表命令失败，是用户要亲自操作设备，应停止当前动作等待用户指示。\n"
    "\n"
    "发送后用 wait，或给 send_line 带上 wait，等到预期输出或超时。"
    "不要用固定睡眠判断成功。需要看屏幕已有内容时用 read。\n"
    "\n"
    "不要用这些工具操作 top、vim 等全屏程序。\n"
    "\n"
    "这些调用会直接作用到连接的设备上，与在终端里敲命令相同。"
)


def agent_file_candidates():
    names = []
    local = os.environ.get("LOCALAPPDATA", "")
    roaming = os.environ.get("APPDATA", "")
    if local:
        names.append(os.path.join(local, "Xterm", "Xterm", "agent.json"))
    if roaming:
        names.append(os.path.join(roaming, "Xterm", "Xterm", "agent.json"))
    return names


def load_agent():
    for path in agent_file_candidates():
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        if data.get("pipe") and data.get("token"):
            return data
    return None


def pipe_path(name):
    if os.name == "nt":
        return "\\\\.\\pipe\\" + name
    return name


# ---------- 持久连接: 管道复用，断了自动重连一次 ----------
# QLocalServer 支持同一连接上多请求（一行一答），无需每个工具调用都重连。
_pipe_fh = None
_pipe_key = None
_next_id = [0]


def close_pipe():
    global _pipe_fh, _pipe_key
    if _pipe_fh is not None:
        try:
            _pipe_fh.close()
        except OSError:
            pass
        _pipe_fh = None
        _pipe_key = None


def get_pipe(cfg):
    """取当前可用的管道句柄；配置变化（Xterm 重启换令牌）时自动重连。"""
    global _pipe_fh, _pipe_key
    key = (cfg.get("pipe"), cfg.get("token"))
    if _pipe_fh is not None and _pipe_key != key:
        close_pipe()  # Xterm 重启了: 旧令牌已失效
    if _pipe_fh is not None:
        return _pipe_fh
    try:
        _pipe_fh = open(pipe_path(cfg.get("pipe") or PIPE_NAME), "r+b", buffering=0)
        _pipe_key = key
    except OSError as exc:
        raise RuntimeError("连不上 Xterm（%s）" % exc) from exc
    return _pipe_fh


def one_roundtrip(cfg, method, params):
    """在持久管道上完成一次请求-响应。"""
    pipe = get_pipe(cfg)
    _next_id[0] += 1
    req = {
        "id": _next_id[0],
        "token": cfg["token"],
        "method": method,
        "params": params or {},
    }
    pipe.write(json.dumps(req, ensure_ascii=False).encode("utf-8") + b"\n")
    buf = b""
    while b"\n" not in buf:
        chunk = pipe.read(4096)
        if not chunk:
            raise ConnectionError("管道被 Xterm 关闭")
        buf += chunk
        if len(buf) > 8 * 1024 * 1024:
            break
    if b"\n" not in buf:
        raise ConnectionError("Xterm 没有返回结果")
    return json.loads(buf.split(b"\n", 1)[0].decode("utf-8"))


def call_xterm(method, params, timeout_hint_ms):
    cfg = load_agent()
    if not cfg:
        raise RuntimeError("Xterm 未运行，或还没有写好本机连接信息")
    # 首次失败（对端重启/残留令牌）时断开重连再试一次
    for attempt in (1, 2):
        try:
            msg = one_roundtrip(cfg, method, params)
        except (OSError, ConnectionError) as exc:
            close_pipe()
            if attempt == 2:
                raise RuntimeError("与 Xterm 通信失败（%s）" % exc) from exc
            continue
        if not msg.get("ok"):
            err = msg.get("error") or "Xterm 拒绝了请求"
            if "令牌" in err:
                # agent.json 是异常退出残留: 提示重启 Xterm 即可自愈（新实例会重写）
                close_pipe()
                raise RuntimeError(
                    err + "；agent.json 可能是残留文件，重启 Xterm 即可自动修复")
            raise RuntimeError(err)
        return msg.get("result") or {}
    return {}


def tool_text(result):
    if isinstance(result, str):
        return result
    return json.dumps(result, ensure_ascii=False, indent=2)


TOOLS = [
    {
        "name": "status",
        "description": "查看 Xterm 当前是串口还是 SSH、连的是谁、是否已连接。操作设备前先调用。",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "send_line",
        "description": (
            "向 Xterm 当前连接发送一行（打字机式逐字发送，用户能看到输入过程）。"
            "不要自行追加换行：串口（含 STM32 USB CDC）由软件补 \\n，SSH 补 \\r。"
            "打字期间用户普通输入被锁定；用户按 Ctrl+C 打断时会收到 "
            "interrupted by user 错误——这不是故障，是用户要亲自操作，"
            "应停止当前动作等待用户指示。"
            "可选 wait：发完后等到屏幕新出现这段文字。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "一行内容，不要带换行"},
                "wait": {"type": "string", "description": "发完后等待屏幕里新出现的文本"},
                "timeout_ms": {"type": "integer", "description": "等待毫秒，默认 3000，最大 30000"},
                "interval_ms": {"type": "integer", "description": "打字间隔毫秒，默认 20，最大 1000（慢速演示）"},
            },
            "required": ["text"],
        },
    },
    {
        "name": "send_control",
        "description": (
            "发送控制字符到 Xterm 当前连接。"
            "长输出命令失控想中断时用 ctrl+c；结束输入用 ctrl+d；esc 退出。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {
                    "type": "string",
                    "enum": ["ctrl+c", "ctrl+d", "ctrl+z", "ctrl+u", "ctrl+l", "esc", "tab"],
                    "description": "控制键名",
                },
            },
            "required": ["key"],
        },
    },
    {
        "name": "read",
        "description": "读取 Xterm 当前屏幕以及滚动历史的末尾。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "max_lines": {"type": "integer", "description": "最多返回多少行，默认 120"},
            },
        },
    },
    {
        "name": "wait",
        "description": "等到屏幕里新出现 contains 这段文字，或超时。不要用睡眠代替。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "contains": {"type": "string", "description": "要等的文本"},
                "timeout_ms": {"type": "integer", "description": "等待毫秒，默认 3000，最大 30000"},
            },
            "required": ["contains"],
        },
    },
]


def dispatch_tool(name, arguments):
    arguments = arguments or {}
    if name == "status":
        return call_xterm("status", {}, 3000)
    if name == "read":
        return call_xterm("read", {"max_lines": arguments.get("max_lines", 120)}, 3000)
    if name == "send_line":
        params = {"text": arguments.get("text", "")}
        if arguments.get("wait"):
            params["wait"] = arguments["wait"]
        if arguments.get("timeout_ms"):
            params["timeout_ms"] = arguments["timeout_ms"]
        timeout = int(arguments.get("timeout_ms") or 3000)
        return call_xterm("send_line", params, timeout)
    if name == "send_control":
        return call_xterm("send_control", {"key": arguments.get("key", "")}, 3000)
    if name == "wait":
        timeout = int(arguments.get("timeout_ms") or 3000)
        return call_xterm(
            "wait",
            {"contains": arguments.get("contains", ""), "timeout_ms": timeout},
            timeout,
        )
    raise RuntimeError("未知工具: " + name)


class Stdio:
    def __init__(self):
        self._buf = b""
        self._headers = False

    def read(self):
        stdin = sys.stdin.buffer
        while True:
            if self._headers or self._buf.startswith(b"Content-Length"):
                self._headers = True
                while b"\r\n\r\n" not in self._buf:
                    chunk = stdin.read(1)
                    if not chunk:
                        return None
                    self._buf += chunk
                header, rest = self._buf.split(b"\r\n\r\n", 1)
                length = 0
                for line in header.split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        length = int(line.split(b":", 1)[1].strip())
                while len(rest) < length:
                    chunk = stdin.read(length - len(rest))
                    if not chunk:
                        return None
                    rest += chunk
                body, self._buf = rest[:length], rest[length:]
                return json.loads(body.decode("utf-8"))
            if b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                return json.loads(line.decode("utf-8"))
            chunk = stdin.read(1)
            if not chunk:
                return None
            self._buf += chunk

    def write(self, obj):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        if self._headers:
            sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(data))
            sys.stdout.buffer.write(data)
        else:
            sys.stdout.buffer.write(data + b"\n")
        sys.stdout.buffer.flush()


def result_message(req_id, text, is_error=False):
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "result": {
            "content": [{"type": "text", "text": text}],
            "isError": is_error,
        },
    }


def instructions_text():
    cfg = load_agent()
    if cfg and cfg.get("instructions"):
        return cfg["instructions"]
    return INSTRUCTIONS


def main():
    io = Stdio()
    while True:
        msg = io.read()
        if msg is None:
            return 0
        method = msg.get("method", "")
        req_id = msg.get("id")
        if req_id is None and method.startswith("notifications/"):
            continue
        if method == "initialize":
            io.write({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "xterm", "version": "0.1.0"},
                    "instructions": instructions_text(),
                },
            })
            continue
        if method == "ping":
            io.write({"jsonrpc": "2.0", "id": req_id, "result": {}})
            continue
        if method == "tools/list":
            io.write({"jsonrpc": "2.0", "id": req_id, "result": {"tools": TOOLS}})
            continue
        if method == "tools/call":
            params = msg.get("params") or {}
            try:
                result = dispatch_tool(params.get("name", ""), params.get("arguments") or {})
                io.write(result_message(req_id, tool_text(result), False))
            except Exception as exc:  # noqa: BLE001 — 工具错误要回到模型，不能把进程打崩
                io.write(result_message(req_id, str(exc), True))
            continue
        if req_id is not None:
            io.write({
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32601, "message": "Method not found: " + method},
            })


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(0)
