#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐一验证 UUID 候选，找到当前 Xterm 实例真正接受的 AgentBridge 令牌，
并把 agent.json 修正回写（用于修复被残留实例污染的令牌）。

用法: python find_token.py cand1 cand2 ...   或
      python find_token.py --file candidates.txt
"""

import json
import sys

from test_mcp import agent_file_candidates, pipe_call, pipe_path


def try_token(pipe, token):
    # 新名优先，旧短名兜底（修复工具可能面对新旧两版实例）
    for method in ("xterm.session.status", "status"):
        try:
            msg = pipe_call(pipe, token, method)
            if msg.get("ok") is True:
                return True
        except (OSError, RuntimeError):
            return False
    return False


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    cands = [a for a in args if a != "--file"]
    if args[0] == "--file":
        with open(args[1], "r", encoding="utf-8") as fh:
            cands = [ln.strip() for ln in fh if ln.strip()]

    cfg_path = None
    for path in agent_file_candidates():
        try:
            with open(path, "r", encoding="utf-8") as fh:
                cfg = json.load(fh)
        except (OSError, ValueError):
            continue
        if cfg.get("pipe"):
            cfg_path = path
            break
    if not cfg_path:
        print("失败: 找不到 agent.json")
        return 1
    pipe = pipe_path(cfg["pipe"])
    print("agent.json: %s (pipe=%s), 候选 %d 个" % (cfg_path, cfg["pipe"], len(cands)))

    found = None
    for i, tok in enumerate(cands):
        if try_token(pipe, tok):
            found = tok
            print("[%d/%d] 命中: %s" % (i + 1, len(cands), tok))
            break
    if not found:
        print("未找到有效令牌（候选全部被拒绝）")
        return 1

    cfg["token"] = found
    with open(cfg_path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, ensure_ascii=False, indent=2)
    print("agent.json 已修正回真实令牌")
    return 0


if __name__ == "__main__":
    sys.exit(main())
