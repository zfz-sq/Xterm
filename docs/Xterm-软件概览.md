# Xterm 软件概览

> 版本 0.1.0 · Windows 桌面应用 · Qt 6.8 / C++17 · MIT 许可
>
> 这份文档介绍 Xterm 是什么、已经做了什么、还缺什么。欢迎在文末「征求建议」处提意见。
> 构建、测试与打包的具体步骤见仓库 [README.md](../README.md)。

## 一句话定位

面向嵌入式开发的**串口 + SSH 双模终端**：一条串口链路 + 一条 SSH 链路同时存活、一键切换，自带 SFTP 文件管理、命令栏、关键词高亮，并能把终端暴露给本机 AI（MCP）当操作对象。Windows 11 原生体验（无边框、暗色/亮色主题、圆角窗口）。

## 解决什么问题

调试一块开发板时，典型姿势是：串口看启动日志 + SSH 干活 + SFTP 传文件。传统做法要开三个工具（PuTTY/Xshell + WinSCP），各有各的皮肤、快捷键、连接配置。Xterm 把这三件事装进一个窗口：

- 串口和 SSH **各自独立连接**，切换模式（F9）不打断任何一边，两边可以同时在线；
- SFTP 复用 SSH 终端的**同一条连接**，不用再输一遍密码；
- 命令栏把常用命令做成按钮，点一下直接发到当前链路。

## 功能清单

### 终端核心（串口 / SSH 共用一套）

- 自绘格子模型终端（TermScreen + ScreenView）：VT 转义解析（光标定位、滚动区、备用屏、16/256/真彩色 SGR、设备状态应答），`vim` / `htop` / `top` 等全屏程序可正常渲染与退出还原；光标闪烁半周期跟随系统设置，输出后立即点亮
- 滚动历史：滚轮翻看（上限可配，默认 1 万行）；备用屏程序里滚轮自动变成方向键
- 选区即复制默认关（右键菜单可开），Ctrl+C 显式复制，右键粘贴；双击选词、三击选行；Ctrl+滚轮改字号
- Ctrl+F / Ctrl+Shift+F 在滚动历史里查找并高亮命中，支持「.*」正则切换（大小写不敏感）；Enter / F3 环形跳转（全屏程序里 Ctrl+F 直通给程序）
- 超链接与剪贴板回传：OSC 8 链接下划线渲染，Ctrl+点击打开（http(s) 交系统，file:// 本地优先、SSH 在线时远端路径自动经 SFTP 取回后用关联程序打开）；OSC 52 剪贴板写入弹窗授权，可勾选「以后自动允许」
- 鼠标上报：SGR 模式（vim / htop 里滚轮翻页、点击定位坐标上报），Shift 强制本地选区/滚历史不受程序接管
- Ctrl+S（XOFF）可能挂起设备输出（画面假死、按键仍在发送）：状态栏黄色常驻提示，Ctrl+Q（XON）或断开连接时熄灭
- 串口场景：本地行编辑与全屏程序自动共存——远端一画屏（备用屏或 BusyBox top 这类仅光标定位的程序），行编辑自动让路变直通；输出回落到提示符样纯文本时自动恢复
- 客户端 Tab 补全：内置常用 Linux/shell 命令 + 命令栏里的命令自动成为候选（嵌入式 shell 多数不吃 Tab，所以补全在本地做）
- 关键词高亮：可配置「关键词 + 颜色 + 启用」规则列表（设置菜单编辑，色块直接预览关键词在终端底色上的效果）；命中自动扩展到完整单词（Successfully 不会只染前半截 Success）；内置日志惯例默认规则（错误红、警告黄、成功绿、信息青，true/false 绿红）开箱即用、可一键补齐也可改删；终端里选中一个词右键「高亮此词」一键添加；选区/查找命中的优先级高于关键词
- 大块输出分片解析，界面不卡

### SSH（libssh 实现）

- 密码 / 交互式认证 / 私钥登录；主机密钥指纹校验，变更时弹窗确认（本地 known_hosts 存储）
- 端口常用值下拉 + 自定义输入
- 状态栏实时显示**远端主机 CPU / 内存**（走共享连接的独立通道，不开第二条连接）
- SFTP 双栏文件管理器（WinSCP 风格）：左栏本地、右栏远程，各自工具栏与右键菜单；本地栏与远程同列展示（名称/大小/修改时间），目录置顶 + 字母序，大小右对齐；上传支持单个文件或整个文件夹递归（按序自动建目录，同名目录合并前确认）；传输进度实时显示在底部状态栏（文件名、百分比、速率、排队数），传输队列窗口不自动弹出、可手动打开查看详情与取消；记住上次浏览的本地/远程目录；断开会话时文件窗口自动关闭，shell 不受影响

### 串口

- 端口 / 波特率等参数栏，独立连接状态与 SSH 互不影响

### 命令栏

- 预设命令列表：添加、删除、拖动手柄排序、复制、上移/下移；点击直接发送到当前链路（串口/SSH 各自记忆）
- 整栏可收起成侧边小按钮组（状态持久化，下次启动直接恢复）

### AI / MCP 集成

- AgentBridge：本机命名管道 + 本次启动令牌，把已连上的串口/SSH 暴露给 MCP 客户端（Claude 等 AI 工具），五组 `xterm.*` 工具：
  - **Screen**：`read` 纯文本 / `snapshot` 结构化快照（每行文本 + 颜色段 + 超链接段，绝对行号）/ `find` 滚动历史查找（子串/正则）/ `wait`·`waitForPattern` 等新输出（回显免疫，正则可设最少命中次数）
  - **Transport**：`sendLine`（打字机式逐字发送，`sendTypewriter` 为正式命名入口，可附带 wait）/ `sendControl`（裸控制字符）/ `interrupt`（停打字机 + 发 Ctrl+C 中断远端命令）
  - **FS**（仅 SSH）：`fsStat` / `fsList` / `fsRead` / `fsDownload`，写类（`fsWrite` / `fsUpload`）需 agent.json 显式 `allow_write: true`，路径限当前会话根目录沙箱——形成「读远端日志 → 分析 → 改文件 → 推回」闭环
  - **Host**（仅 SSH）：`cpu` / `mem`（状态栏同源采样）/ `disk` / `uptime` / `top`
  - **Session**：`list` / `current` / `switch`（切串口/SSH 不断另一条）/ `openSsh` / `openSerial`（参数写回参数栏再连接，凭据只认界面配置）/ `close`
- 可观测与防护：所有调用落 `logs/agent-audit.jsonl`（按大小滚动保留）；状态栏角标三态（就绪 / 打字机中 / 「MCP · 工具名（N 秒前）」）；令牌桶限流防 AI 循环刷屏卡界面
- AI 打字期间用户输入锁定，用户 Ctrl+C 可打断（AI 会收到明确的 interrupted 错误）；管道名与令牌在配置目录 agent.json，工具菜单里有现成的 MCP 提示词可复制

### 外观与平台

- 无边框主窗口 + 自绘标题栏（菜单、主题切换、置顶、系统按钮），所有弹窗统一为同风格的无边框对话框（Win11 圆角）
- 暗 / 亮主题一键切换（QSS 全局换肤，主题跟随每个对话框与取色器）
- 配置持久化：profile（连接参数）、主题、字号、命令栏状态、关键词规则、SFTP 上次目录，落盘在用户配置目录

### 快捷键

| 键 | 作用 |
|---|---|
| F5 | 连接 / 断开（菜单文案随状态变化） |
| F9 | 切换串口 / SSH（两条链路都不断开） |
| F10 | 打开 SFTP 文件管理 |
| Ctrl+F / Ctrl+Shift+F | 滚动历史查找 |
| Ctrl+滚轮 | 调整字号 |
| F1–F8、F11、F12 | 不占用，原样透传给远端程序 |

## 技术架构（简）

```
MainWindow（胶水层：构造编排、信号接线、关闭流程）
 ├─ 五协作类
 │    ├─ SessionController  会话生命周期（串口/SSH 建立、断开、切模式、认证、配置档）
 │    ├─ UiBuilder          UI 编排（标题栏、连接组合框、状态栏、模式/连接态刷新）
 │    ├─ HotkeyManager      窗口级快捷键（F5/F9/F10）
 │    ├─ ThemeManager       主题切换 + QSS 应用
 │    └─ McpMainHook        MCP 角标与挂钩
 ├─ Transport 契约（接口，由 SessionController 持有）
 │    ├─ SerialSession   （QSerialPort，独占串口）
 │    └─ SshSession      （libssh facade，GUI 线程）
 │          └─ SshWorker（私有 QThread，命令/事件全队列投递）
 │                ├─ ShellChannel   终端
 │                ├─ SftpChannel    文件（与终端共享同一条连接）
 │                └─ StatsChannel   主机 CPU/内存（HostStats 汇聚展示）
 ├─ TermScreen / ScreenView   终端仿真与自绘视图（UiBuilder 创建）
 ├─ CommandBar / CompletionEngine  命令栏与补全
 ├─ FileBrowserDialog   SFTP 双栏（model / transferqueue / widgets 分层，SessionController 持有）
 ├─ TransferDialog      传输队列窗口
 ├─ AgentBridge   MCP 命名管道服务（main 组装 Hooks 后启动）
 └─ FramelessDialog / TitleBar  统一无边框 UI
```

- 依赖：Qt 6.8（Widgets / SerialPort / Network）+ libssh + OpenSSL（vcpkg 管理）
- 构建：CMake ≥ 3.19，Windows llvm-mingw 工具链；自动部署 SSH 相关 DLL（步骤与构建选项详见 [README.md](../README.md)，含 `XTERM_BUILD_TOOLS`、`XTERM_VCPKG_TRIPLET`）
- 测试：19 个单测目标（终端解析、视图渲染、SSH 连接/会话/SFTP 通道/客户端、known_hosts、主机统计解析、AgentBridge 与四组扩展工具桥、审计日志、主题、快捷键、MCP 挂钩、会话控制器组装），当前 19/19 通过；另有 sshtest / sftptest 调试工具（默认不构建，`-DXTERM_BUILD_TOOLS=ON` 开启）

## 当前状态与已知边界

- 主线工作流（串口调试、SSH 终端、SFTP、命令栏、关键词高亮、MCP）可用且被测试覆盖
- 工程以 MIT 开源（[LICENSE](../LICENSE)），README 提供完整构建/测试/打包指南
- 仅支持 Windows（Mac 打包占位存在，未验证）
- 尚未实现（候选方向，欢迎投票或补充）：
  - 会话日志记录（输出按时间戳落盘）
  - rz/sz（ZMODEM）自动收发；串口 XMODEM
  - 串口慢速粘贴（粘贴多行时按行加延时防丢字节）
  - 串口热插拔自动重连（USB 转串口拔插后 COM 号变化）
  - 静默后再输出时的任务栏提醒（等板子重启时不用盯屏）

## 征求建议

最想听到这三类意见：

1. **缺什么**：你日常用的终端工具有、而这里没有的功能？按「每周会用几次」的标准提，不追求数量。
2. **哪里别扭**：现有交互（快捷键、右键菜单、命令栏、SFTP 双栏）用起来不顺手的地方。
3. **值不值得**：上面候选方向里哪些你会真的用？哪些应该直接砍掉？
