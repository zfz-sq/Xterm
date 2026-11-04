# Xterm 升级路线图

> 制定日期：2026-11-03 · 覆盖版本：0.1.0 之后
>
> 这份文档固化三大升级方向的范围、顺序与验收标准。执行时按里程碑拆 PR，
> 每个 PR 对应文中一个子步骤，便于对齐进度与回滚。

## 总原则

1. **交付顺序：M1 → M3 → M2**
   - M1（MainWindow 拆分）是纯重构、无功能变更，为 M2/M3 提供干净挂载点；
   - M3（终端现代化）只动 `termscreen` / `screenview`，改动半径小、有现成测试兜底；
   - M2（MCP 扩展）依赖 M1 拆出的 `SessionController` 与 M3 定稿的屏幕语义。
2. **拆分粒度：同目录协作类**。新模块直接放工程根目录（与 `commandbar.*` 同级），
   `CMakeLists.txt` 源文件列表加行即可，不引入子目录、不改 include 结构。
3. **单 PR 单模块**：每次 PR 只搬一个协作类，跑通 `ctest` 再合。
4. **不动公共 API**：`MainWindow` 对外的信号槽与 `sendRaw / sendLine / toggleKmsgMute`
   等跨模块入口保留，只做内部转发，`mainwindow.ui` / `main.cpp` / `tests` 不跟着改。
5. **MCP 协议直接切换到 `xterm.*` 命名空间**，不留兼容层；
   `AgentBridge::instructions()` 与 MCP 提示词同步更新。

三大方向依赖关系：

```
      ┌── M1 MainWindow 拆分 ──┐              ┌── M2 MCP 工具集扩展 ──┐
      │  先做：M2/M3 都要挂到  │ ── 依赖 ──►  │  挂在新 McpMainHook    │
      │  新拆出的模块上        │              │  + SessionController   │
      └────────────────────────┘              └────────────────────────┘
                    ▲
                    │ 屏幕 API 稳定后再叠
      ┌── M3 终端现代化 ──┐
      │  只改 termscreen   │
      │  + screenview      │
      └────────────────────┘
```

---

## M1 — MainWindow 拆分

现状：`mainwindow.cpp` 约 91KB，同时承担会话生命周期、UI 编排、快捷键、
MCP 挂钩、主题切换五件事；`mainwindow.h` 里 `m_hostCpu` / `m_mcpActive` /
`m_kmsgMuted` 等状态散落在 MainWindow 成员中。

目标：拆成 5 个协作类，`mainwindow.cpp` 压到 ≤ 22KB，只保留
`Ui::MainWindow*`、5 个 controller 指针、`closeEvent` 编排与胶水代码。

### 协作类职责划分

| 新模块 | 承担职责 | 从 MainWindow 迁出的关键成员/函数 |
|---|---|---|
| `sessioncontroller.{h,cpp}` | 会话生命周期：串口/SSH 建立、断开、参数变更、模式切换、认证交互 | `onConnectClicked` `onConnectionParamChanged` `onSerialModeToggled` `setMode` `activeTransport` `applyCurrentProfile` `syncCurrentProfile` `scheduleProfileSave` `confirmHostKeyChange` `promptHostKeyDecision` `promptAuthAnswers` `refreshPorts` `reloadProfiles` |
| `uibuilder.{h,cpp}` | UI 编排：标题栏、连接组合框、状态栏 label、命令栏挂载、模式/连接状态刷新 | `setupIcons` `setupTitleBar` `setupConnectionCombos` `setupSshInputs` `fitSshPortEdit` `setupTerminal` `updateModeUi` `updateConnectionUi` `updatePersistentStatus` `connectionSummary` |
| `hotkeymanager.{h,cpp}` | 快捷键与 QAction 注册（F5/F9/F10/Ctrl+F/Ctrl+滚轮），F1–F8/F11/F12 透传白名单 | `m_connectAction` `m_switchModeAction` 的构造与文案联动、`eventFilter` 中的按键分发 |
| `mcpmainhook.{h,cpp}` | MCP 挂钩与在线角标：组装 `AgentBridge::Hooks`、图标/状态刷新 | `agentStatus` `agentRecentText` `agentSendLine` `agentSendBytes` `setMcpActive` `refreshMcpIcon` `refreshMcpStatus` `m_mcpBtn` `m_mcpActiveTimer` `m_agentLockLabel` |
| `thememanager.{h,cpp}` | 主题切换 + QSS 加载应用 + 图标重刷 | `applyTheme`、`changeEvent` 中主题相关分支、`resources/*.qss` 加载路径集中 |

### 协作约束

- controller 之间通过窄接口互访（如 `SessionController` 写状态栏 → 调
  `UiBuilder::setStatusText()`），**不得互相持有 `MainWindow*`**，避免假解耦。
- 顺带下沉两处状态：
  - `m_hostCpu / m_hostMemPercent / m_hostUsedKiB / m_hostTotalKiB` 及 `*Ok` 标志
    挪进 `hoststats.{h,cpp}`，MainWindow 只调 `hostStats->statusText()`；
  - `m_kmsgMuted / m_kmsgMuteBtn / updateKmsgMuteUi / toggleKmsgMute` 抽成
    `kmsgmutetoggle.{h,cpp}`（QWidget 子类），仅串口页挂载。

### 子步骤与验收

| 步骤 | 内容 | 风险 | 验收判定 |
|---|---|---|---|
| M1.a | 抽 `ThemeManager` | 最低，先跑通协作类模板 | 主题切换无回归，`ctest` 全绿 |
| M1.b | 抽 `HotkeyManager`，新增 `test_hotkeymanager.cpp` | 低 | F1–F12 透传矩阵用例全绿 |
| M1.c | 抽 `McpMainHook`，扩展 `test_agentbridge.cpp` | 中 | MCP 状态灯/打字机无回归 |
| M1.d | 抽 `SessionController` + `HostStats` 缓存下沉 + `KmsgMuteToggle`，新增 `test_sessioncontroller.cpp` | 最高（易引 regression） | `mainwindow.cpp` ≤ 25KB；连接/断开/切模式/认证弹窗用例全绿 |

---

## M3 — 终端能力对齐现代客户端

范围：只动 `termscreen.*` 与 `screenview.*`（`transport` 收字节路径基本不变），
测试依托现有 `test_screenview.cpp` / `test_termscreen.cpp`。

### 子步骤

| 步骤 | 内容 | 要点 | 验收判定 |
|---|---|---|---|
| M3.1 | SGR 鼠标上报（DECSET 1000/1002/1006） | `termscreen` 解析开关；`screenview` 按下/移动/释放转 `\e[<btn;x;y;M\|m/mM` 发当前链路；**Shift+拖动强制本地选区**；开关关闭时保持现有选区复制行为，两种模式互不干扰要有测试锁 | vim/htop 内可点光标；选区复制不回归 |
| M3.2 | OSC 8 超链接 + OSC 52 剪贴板 | cell 增加可选 `uri` 字段，渲染加下划线；Ctrl+点击：`file://` 打开本地/触发 SFTP 下载，`http(s)://` 走 `QDesktopServices`；OSC 52 解析 `ESC ] 52 ; p ; <base64> BEL` 写系统剪贴板，默认「询问」开关 | URL 可点击；远端 `printf '\e]52;...'` 能进剪贴板 |
| M3.3 | 正则查找 + 全部高亮 + n/N；双击选词 / 三击选行；选区即复制（默认关） | `Ctrl+F` 输入框加 `.*` 切换；词边界考虑 `/ . _ -` 与 URL；匹配缓存 + 当前匹配异色高亮 | 正则命中跳转；双击选中完整路径 |
| M3.4 | 滚动历史环形缓冲（默认 10000 行可配） | 超限丢最老行；**同步修改 `AgentBridge::Hooks.recentLineCount` 语义为「绝对序号」**，否则 `wait` 锚点漂移——这是 M3 唯一影响 M2 语义的点 | 长跑（`dmesg -w` 数小时）内存不涨；wait 锚点用例更新 |
| M3.5 | `paintEvent` 帧合并（16ms 节流）+ 脏矩形 | 可选，先加 screenview 基准测试再动手 | 高频刷屏掉帧显著下降 |

每个子步骤独立可交付，用户可立即感知。

---

## M2 — MCP 桥升级为杀手级能力

现状：`AgentBridge` 以 `Hooks` 结构注入能力（status / recentText / sendLine /
sendBytes / wait），抽象干净。扩展原则：**加 hook、加路由分支，不把业务逻辑塞进
bridge**。

### M2.1 协议层

- 新增 `tools.list` 请求：返回当前实例支持的工具数组（含 schema），客户端自动发现；
- 方法名带命名空间：`xterm.screen.*` / `xterm.transport.*` / `xterm.fs.*` /
  `xterm.host.*` / `xterm.session.*`；
- `Hooks` 中每类工具收敛为 `QHash<QString, HandlerFn>`，`AgentBridge` 只做
  路由、令牌鉴权、异常兜底；
- 直接切换，不保留旧方法名（见「总原则」第 5 条）。

### M2.2 五组工具

| 组 | 工具 | 依托 |
|---|---|---|
| A. Screen | `read`（已有）、`snapshot`（二维结构含字符/FG/BG/URI，依赖 M3.2）、`find(regex)`（复用 M3.3）、`waitForPattern(regex)`（wait 升级为正则） | `termscreen` / `screenview` |
| B. Transport | `sendLine` / `sendBytes`（已有）、`sendTypewriter`（现打字机正式命名入口）、`interrupt`（API 级 Ctrl+C，区别于现有只停打字机的 `interruptTyping`） | 当前链路 |
| C. FS（最大增量） | `fsStat / fsList / fsRead / fsWrite / fsDownload / fsUpload`，转发给现成 `SftpClient`；形成「读远端日志→分析→改文件→推回」闭环 | `sftpclient.*`，需 M1 后挂载 |
| D. Host | `cpu / mem / disk / uptime / top`，直连已跑通的 `HostStats` 独立通道 | `hoststats.*` |
| E. Session | `list / current / switch / openSsh / openSerial / close`，**必须等 M1.d 的 `SessionController` 就位** | `sessioncontroller.*` |

FS 安全边界：

- 默认只开 read/list/download；写类工具需 `agent.json` 显式 `"allow_write": true`；
- 路径沙箱：只允许操作当前 SFTP 会话根目录及子路径。

### M2.3 可观测

- **审计日志**：所有 MCP 调用（hook 名/参数摘要/结果状态/耗时）落
  `logs/agent-audit.jsonl`，滚动保留；
- **状态栏三态**：`idle / 打字机中 / 最近调用 <tool.name> (Ns 前)`，
  替代现在粗粒度的 `setMcpActive`；
- **速率限制**：令牌桶防 AI 循环刷 `screen.read` 卡死 GUI 线程。

### M2.4 传输层（可选，放最后）

命名管道跨机不通。加可选开关：本地 HTTP + SSE（`QHttpServer` 或轻量 socket），
只绑 `127.0.0.1`，同一套令牌校验。远程 AI / WSL 内的 MCP 客户端可连。

### 子步骤与验收

| 步骤 | 内容 | 验收判定 |
|---|---|---|
| M2.a | 命名空间路由 + `tools.list`，更新 `test_agentbridge.cpp` | 客户端可自发现工具清单 |
| M2.b | FS 工具 + 沙箱 + allow_write，fake SftpClient 新测试 `test_agentbridge_fs.cpp` | AI 能列目录/读文件；写类默认拒绝 |
| M2.c | Host 工具 | AI 能取 CPU/内存 |
| M2.d | Screen snapshot/find/waitForPattern（依赖 M3.2/M3.3/M3.4 定稿） | 正则等待 + URI 可见 |
| M2.e | 审计日志 + 三态徽标 + 限流 | jsonl 落盘、限流生效 |
| M2.f | Session 工具（依赖 M1.d） | AI 可开关/切换会话 |
| M2.g | （可选）HTTP/SSE 端点 | 127.0.0.1 外拒绝连接 |

---

## 明确不在本路线图内

以下项评估后暂缓，避免范围膨胀（后续单独立项）：

- 多标签/分屏会话树与快速连接面板（P0 客户端能力，改动 `mainwindow` 结构太大，
  等 M1 拆完后以「多 Session」为独立里程碑做，M2.f 的 Session 工具先按现有
  双链路实现）；
- SSH ProxyJump / 端口转发 / Windows Credential Locker 凭据存储；
- 串口 HEX 视图、AT 指令面板、协议插件；
- macOS / Linux / ARM64 跨平台；
- CI/CD、崩溃收集、i18n。

## 执行约定

- 每个子步骤一个 PR，PR 描述引用本文对应编号（如 `M1.a`）；
- M1 期间禁止夹带功能改动（纯搬迁）；
- 涉及语义变更的步骤（M3.4 行号锚点、M2.1 命名空间）必须在同一 PR 内
  更新 `AgentBridge::instructions()` 与本文件；
- 里程碑完成时在下方进度表打勾并记录版本号。

## 进度跟踪

- [x] M1.a ThemeManager（2026-11-03，test_thememanager 6 用例，全套 11/11 绿）
- [x] M1.b HotkeyManager（2026-11-03，test_hotkeymanager 6 用例，全套 12/12 绿；范围收敛为窗口级 F5/F9/F10，Ctrl+F/滚轮留在 ScreenView、AI 锁键盘留在主窗口）
- [x] M1.c McpMainHook（2026-11-03，test_mcpmainhook 3 用例，全套 13/13 绿；桥接/角标按钮/状态栏标签/锁定提示/AI 打字守卫迁出协作类，Hooks 仍由主窗口注入，agent* 数据适配器留待 M1.d 归 SessionController）
- [x] M1.d SessionController + UiBuilder + HostStats 下沉 + KmsgMuteToggle（2026-11-04，
      test_sessioncontroller 11 用例，全套 14/14 绿；为达 ≤25KB 验收线同 PR 拆出第 5 类
      UiBuilder。M1 收官：mainwindow.cpp 85KB→6.8KB/127 行，只余构造编排/信号接线/
      AI 锁 eventFilter/closeEvent 转发；sendRaw/sendLine 实现直迁 SessionController
      （无外部调用者，不留转发死代码），Mode 枚举迁至 SessionController）
- [ ] M3.1 SGR 鼠标上报
- [ ] M3.2 OSC 8 / OSC 52
- [ ] M3.3 正则查找 + 选区增强
- [ ] M3.4 环形缓冲 + 行号语义
- [ ] M3.5 渲染帧合并（可选）
- [ ] M2.a 命名空间 + tools.list
- [ ] M2.b FS 工具
- [ ] M2.c Host 工具
- [ ] M2.d Screen 扩展工具
- [ ] M2.e 审计 + 徽标 + 限流
- [ ] M2.f Session 工具
- [ ] M2.g HTTP/SSE 端点（可选）
