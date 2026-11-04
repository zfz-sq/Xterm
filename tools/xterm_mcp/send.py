#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""通过 Xterm MCP 向当前连接的设备发送一行命令并取回屏幕输出。

用法:
  python send.py "free -m"          # 发送命令，等到回显标记后打印屏幕
  python send.py "top -bn1 | head"  # 任意 shell 命令

原理: sendLine 附带 wait=唯一标记，命令后追加 echo 标记，
输出到屏即返回。需要 Xterm 已连接设备（串口或 SSH）。
"""

import sys
import time

from test_mcp import load_agent, pipe_call, pipe_path


def send_and_read(line, timeout_ms=10000):
    path, cfg = load_agent(wait_s=3)
    if not cfg:
        raise RuntimeError("Xterm 未运行或未写 agent.json")
    # Xterm 的 wait 自带回显免疫（needle 是命令子串时等真实输出再次出现），
    # 直接 echo 标记即可，无需引号拆分技巧。
    marker = "__XT_DONE_%d__" % int(time.time() * 1000)
    msg = pipe_call(
        pipe_path(cfg["pipe"]), cfg["token"], "xterm.transport.sendLine",
        {"text": "%s; echo %s" % (line, marker),
         "wait": marker, "timeout_ms": timeout_ms})
    if not msg.get("ok"):
        raise RuntimeError(msg.get("error") or "设备未连接")
    return (msg.get("result") or {}).get("text", "")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    try:
        text = send_and_read(sys.argv[1])
    except (OSError, RuntimeError) as exc:
        print("失败: %s" % exc)
        return 1
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
