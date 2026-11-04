#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Xterm MCP 链路自测脚本。

两层验证:
  1. 直连 Xterm 的命名管道（AgentBridge 的行协议: 一行请求 JSON，一行响应）
  2. 通过 tools/xterm_mcp/server.py 走标准 MCP stdio（JSON-RPC 2.0）

用法:
  python test_mcp.py                     # Xterm 已在运行
  python test_mcp.py --exe <Xterm.exe>   # 自动拉起 Xterm，测完关闭
  python test_mcp.py --send "help"       # 附加真实 sendLine 测试（会发到设备！）

退出码: 0 = 全部通过, 1 = 有失败。
"""

import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "server.py")

_n = {"pass": 0, "fail": 0, "skip": 0}


def report(name, ok, detail=""):
    _n["pass" if ok else "fail"] += 1
    tag = "PASS" if ok else "FAIL"
    print("[%s] %s%s" % (tag, name, (" — " + detail) if detail else ""))
    return ok


def skip(name, detail=""):
    _n["skip"] += 1
    print("[SKIP] %s%s" % (name, (" — " + detail) if detail else ""))


def agent_file_candidates():
    out = []
    local = os.environ.get("LOCALAPPDATA", "")
    roaming = os.environ.get("APPDATA", "")
    if local:
        out.append(os.path.join(local, "Xterm", "Xterm", "agent.json"))
    if roaming:
        out.append(os.path.join(roaming, "Xterm", "Xterm", "agent.json"))
    return out


def load_agent(wait_s=0.0):
    """轮询等待 agent.json 出现并可用。返回 (路径, 配置) 或 (None, None)。"""
    deadline = time.time() + wait_s
    while True:
        for path in agent_file_candidates():
            if not os.path.isfile(path):
                continue
            try:
                with open(path, "r", encoding="utf-8") as fh:
                    data = json.load(fh)
            except (OSError, ValueError):
                continue
            if data.get("pipe") and data.get("token"):
                return path, data
        if time.time() >= deadline:
            return None, None
        time.sleep(0.2)


def pipe_path(name):
    return "\\\\.\\pipe\\" + name if os.name == "nt" else name


def pipe_call(pipe, token, method, params=None, token_override=None, id_=1):
    """直连管道调用一次。对端保证每个请求必回一行（wait 类在超时后回）。"""
    req = {"id": id_, "token": token_override or token,
           "method": method, "params": params or {}}
    with open(pipe, "r+b", buffering=0) as fh:
        fh.write(json.dumps(req, ensure_ascii=False).encode("utf-8") + b"\n")
        buf = b""
        while b"\n" not in buf:
            chunk = fh.read(4096)
            if not chunk:
                break
            buf += chunk
    if b"\n" not in buf:
        raise RuntimeError("Xterm 未回包")
    return json.loads(buf.split(b"\n", 1)[0].decode("utf-8"))


class McpClient:
    """按行模式驱动 server.py 的 stdio JSON-RPC。"""

    def __init__(self):
        self.proc = subprocess.Popen(
            [sys.executable, SERVER], cwd=HERE,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL)

    def request(self, obj):
        self.proc.stdin.write(
            json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("MCP server 无响应（进程退出?）")
        return json.loads(line.decode("utf-8"))

    def notify(self, obj):
        self.proc.stdin.write(
            json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\n")
        self.proc.stdin.flush()

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            pass


def test_pipe(cfg):
    print("—— 直连 AgentBridge 命名管道 ——")
    pipe = pipe_path(cfg["pipe"])
    token = cfg["token"]

    try:
        msg = pipe_call(pipe, token, "xterm.session.status", token_override="wrong-token")
        report("错误令牌被拒绝", msg.get("ok") is False
               and "令牌" in str(msg.get("error", "")), str(msg.get("error")))
    except (OSError, RuntimeError) as exc:
        report("错误令牌被拒绝", False, str(exc))

    try:
        msg = pipe_call(pipe, token, "xterm.session.status")
        report("xterm.session.status", msg.get("ok") is True, json.dumps(
            msg.get("result", {}), ensure_ascii=False)[:120])
    except (OSError, RuntimeError) as exc:
        report("xterm.session.status", False, str(exc))

    try:
        msg = pipe_call(pipe, token, "tools.list")
        names = [t.get("name", "") for t in (msg.get("result") or {}).get("tools", [])]
        report("tools.list 自发现", msg.get("ok") is True
               and "xterm.transport.sendLine" in names, "，".join(names))
    except (OSError, RuntimeError) as exc:
        report("tools.list 自发现", False, str(exc))

    try:
        msg = pipe_call(pipe, token, "xterm.screen.read", {"max_lines": 3})
        report("xterm.screen.read(max_lines=3)", msg.get("ok") is True)
    except (OSError, RuntimeError) as exc:
        report("xterm.screen.read(max_lines=3)", False, str(exc))

    try:
        msg = pipe_call(pipe, token, "no_such_method")
        report("未知方法报错", msg.get("ok") is False
               and "未知工具" in str(msg.get("error", "")))
    except (OSError, RuntimeError) as exc:
        report("未知方法报错", False, str(exc))

    # wait 回显免疫: needle 是命令子串时应等真实输出再命中（需要已连接设备）
    try:
        st = pipe_call(pipe, token, "xterm.session.status")
        if (st.get("result") or {}).get("connected"):
            tag = "ECHOIMMUNE%d" % os.getpid()
            msg = pipe_call(pipe, token, "xterm.transport.sendLine",
                            {"text": "echo " + tag, "wait": tag,
                             "timeout_ms": 8000})
            result = msg.get("result") or {}
            text = result.get("text", "")
            hits = text.count(tag)
            report("sendLine+wait 回显免疫", msg.get("ok") and result.get("matched"),
                   "tag 出现 %d 次（2=回显+输出，免疫生效）" % hits)
        else:
            skip("sendLine+wait 回显免疫", "设备未连接")
    except (OSError, RuntimeError) as exc:
        report("sendLine+wait 回显免疫", False, str(exc))

    # sendControl: 控制字符直达（ctrl+c 无害——只是中断当前行出新提示符）
    try:
        msg = pipe_call(pipe, token, "xterm.transport.sendControl", {"key": "ctrl+c"})
        report("sendControl ctrl+c", msg.get("ok") is True)
    except (OSError, RuntimeError) as exc:
        report("sendControl ctrl+c", False, str(exc))
    try:
        msg = pipe_call(pipe, token, "xterm.transport.sendControl", {"key": "bogus"})
        report("sendControl 非法键报错", msg.get("ok") is False)
    except (OSError, RuntimeError) as exc:
        report("sendControl 非法键报错", False, str(exc))


def test_mcp():
    print("—— MCP stdio（server.py）——")
    client = McpClient()
    try:
        resp = client.request({"jsonrpc": "2.0", "id": 1,
                               "method": "initialize", "params": {}})
        info = ((resp.get("result") or {}).get("serverInfo") or {})
        report("initialize", info.get("name") == "xterm", str(info))
        client.notify({"jsonrpc": "2.0", "method": "notifications/initialized"})

        resp = client.request({"jsonrpc": "2.0", "id": 2,
                               "method": "tools/list"})
        tools = [t.get("name") for t in
                 ((resp.get("result") or {}).get("tools") or [])]
        report("tools/list", set(tools) >= {
            "xterm.session.status",
            "xterm.screen.read",
            "xterm.transport.sendLine",
            "xterm.screen.wait",
            # M2.f 起六件会话工具 + B 组正名两件也在清单里
            "xterm.session.list",
            "xterm.session.switch",
            "xterm.transport.sendTypewriter",
            "xterm.transport.interrupt",
        }, str(tools))

        resp = client.request({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                               "params": {"name": "xterm.session.status",
                                          "arguments": {}}})
        result = resp.get("result") or {}
        report("tools/call xterm.session.status", not result.get("isError"),
               (result.get("content") or [{}])[0].get("text", "")[:120])
    finally:
        client.close()


def main():
    ap = argparse.ArgumentParser(description="Xterm MCP 链路自测")
    ap.add_argument("--exe", help="Xterm.exe 路径；提供则自动拉起，测完关闭")
    ap.add_argument("--send", help="额外真实测试 sendLine（文本会发到设备！）")
    args = ap.parse_args()

    launched = None
    if args.exe:
        print("启动 Xterm: %s" % args.exe)
        launched = subprocess.Popen([args.exe])
        path, cfg = load_agent(wait_s=15)
    else:
        path, cfg = load_agent(wait_s=3)

    try:
        if not cfg:
            print("失败: 找不到可用的 agent.json。请先运行 Xterm，"
                  "或用 --exe 指定 Xterm.exe 路径。候选路径:")
            for p in agent_file_candidates():
                print("  " + p)
            return 1
        print("agent.json: %s（pipe=%s）" % (path, cfg["pipe"]))

        test_pipe(cfg)
        test_mcp()

        if args.send:
            print("—— sendLine 实发（--send）——")
            try:
                msg = pipe_call(pipe_path(cfg["pipe"]), cfg["token"],
                                "xterm.transport.sendLine", {"text": args.send})
                if msg.get("ok"):
                    report("sendLine", True)
                elif "未连接" in str(msg.get("error", "")):
                    skip("sendLine", "设备未连接: " + str(msg.get("error")))
                else:
                    report("sendLine", False, str(msg.get("error")))
            except (OSError, RuntimeError) as exc:
                report("sendLine", False, str(exc))

        print("\n结果: %d 通过, %d 失败, %d 跳过"
              % (_n["pass"], _n["fail"], _n["skip"]))
        return 0 if _n["fail"] == 0 else 1
    finally:
        if launched:
            # 强杀不会触发 Xterm 的收尾（agent.json 可能残留），
            # 下次 Xterm 启动会整体重写该文件，无碍。
            launched.terminate()


if __name__ == "__main__":
    sys.exit(main())
