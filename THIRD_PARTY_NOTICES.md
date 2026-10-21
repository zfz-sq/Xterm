# 第三方组件说明 (Third-Party Notices)

Xterm 的 SSH 功能由以下第三方开源库提供，均以动态链接方式随本程序分发
（程序目录下的 `*.dll`）。各库完整许可文本见其源码包。

## libssh

- 版本：0.12.2
- 许可：LGPL-2.1-only
- 主页：<https://www.libssh.org/>
- 用途：SSH2 协议实现（连接、shell / exec / SFTP 通道）
- 源码获取：本项目的 `.deps/vcpkg` 清单模式安装，overlay 端口位于仓库
  `.deps/overlay-ports/libssh/`；上游源码见
  <https://git.libssh.org/projects/libssh.git/>
- 依 LGPL 条款，以动态链接使用；如需获取对应源码或重新链接，
  请按上游仓库说明操作。

## OpenSSL

- 版本：3.6.5（含 legacy provider 组件）
- 许可：Apache License 2.0
- 主页：<https://www.openssl.org/>
- 用途：libssh 的加密后端（`libcrypto-3-x64.dll`、`libssl-3-x64.dll`、
  `legacy.dll`）
- 源码获取：随 `.deps/vcpkg` 清单模式安装；上游源码见
  <https://github.com/openssl/openssl/>

## mingw-w64 运行时 (winpthreads)

- 组件：`libwinpthread-1.dll`
- 许可：mingw-w64 宽松条款；此处使用的发行版为 llvm-mingw
  （Apache-2.0 with LLVM-exception）
- 主页：<https://mingw-w64.org/>，<https://github.com/mstorsjo/llvm-mingw>
- 用途：编译器（llvm-mingw 工具链）自带的线程运行时支持库
