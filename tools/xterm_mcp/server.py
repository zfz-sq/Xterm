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
    "串口（含 STM32 USB CDC）是 \\n，SSH 是 \\r。\n"
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


def call_xterm(method, params, timeout_hint_ms):
    cfg = load_agent()
    if not cfg:
        raise RuntimeError("Xterm 未运行，或还没有写好本机连接信息")
    path = pipe_path(cfg.get("pipe") or PIPE_NAME)
    # 软件侧会在超时后回包，这里把读超时放宽一点
    wait_s = max(5.0, (timeout_hint_ms + 2000) / 1000.0)
    try:
        pipe = open(path, "r+b", buffering=0)
    except OSError as exc:
        raise RuntimeError("连不上 Xterm（%s）" % exc) from exc
    try:
        if os.name == "nt":
            # 命名管道没有标准的 settimeout；靠对端必定回一行。
            pass
        req = {
            "id": 1,
            "token": cfg["token"],
            "method": method,
            "params": params or {},
        }
        pipe.write(json.dumps(req, ensure_ascii=False).encode("utf-8") + b"\n")
        buf = b""
        while b"\n" not in buf:
            chunk = pipe.read(4096)
            if not chunk:
                break
            buf += chunk
            if len(buf) > 8 * 1024 * 1024:
                break
        if b"\n" not in buf:
            raise RuntimeError("Xterm 没有返回结果（等待约 %ss）" % wait_s)
        line = buf.split(b"\n", 1)[0]
        msg = json.loads(line.decode("utf-8"))
    finally:
        pipe.close()
    if not msg.get("ok"):
        raise RuntimeError(msg.get("error") or "Xterm 拒绝了请求")
    return msg.get("result") or {}


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
            "向 Xterm 当前连接发送一行。不要自行追加换行："
            "串口（含 STM32 USB CDC）由软件补 \\n，SSH 补 \\r。"
            "可选 wait：发完后等到屏幕新出现这段文字。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "一行内容，不要带换行"},
                "wait": {"type": "string", "description": "发完后等待屏幕里新出现的文本"},
                "timeout_ms": {"type": "integer", "description": "等待毫秒，默认 3000，最大 30000"},
            },
            "required": ["text"],
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
