# Xterm 软件概览

> 版本 0.1.0 · Windows 桌面应用 · Qt 6.8 / C++17 · MIT 许可
>
> 这份文档介绍 Xterm 是什么、已经做了什么、还缺什么。欢迎在文末「征求建议」处提意见。
> 构建、测试与打包的具体步骤见仓库 [README.md](../README.md)。

## 一句话定位

面向嵌入式开发的**串口 + SSH 双模终端**：一条串口链路 + 一条 SSH 链路同时存活、一键切换，自带 SFTP 文件管理、命令栏、关键词高亮，并能把终端暴露给本机 AI（MCP）当操作对象。Windows 11 原生体验（无边框、多套内置主题、圆角窗口）。

## 解决什么问题

调试一块开发板时，典型姿势是：串口看启动日志 + SSH 干活 + SFTP 传文件。传统做法要开三个工具（PuTTY/Xshell + WinSCP），各有各的皮肤、快捷键、连接配置。Xterm 把这三件事装进一个窗口：

- 串口和 SSH **各自独立连接**，切换模式（F9）不打断任何一边，两边可以同时在线；
- SFTP 复用 SSH 终端的**同一条连接**，不用再输一遍密码；
- 命令栏把常用命令做成按钮，点一下直接发到当前链路。

## 功能清单

### 终端核心（串口 / SSH 共用一套）

- 自绘格子模型终端（TermScreen + ScreenView）：VT 转义解析（光标定位、滚动区、备用屏、16/256/真彩色 SGR、设备状态应答），`vim` / `htop` / `top` 等全屏程序可正常渲染与退出还原；光标闪烁半周期跟随系统设置，输出后立即点亮
- 滚动历史：滚轮翻看（上限可配，默认 1 万行）；备用屏程序里滚轮自动变成方向键
- 滚动条按需浮现（用户定稿）：不足一屏（无回滚历史）时轨道滑块彻底不显示（syncBar 物理隐藏 + QSS 待机透明双保险）；有回滚历史时平时隐藏，仅两种用户行为触发浮现——滚动鼠标滚轮、鼠标从窗口外移入应用窗口（ScrollFlash「用户滚动门」：noteUserActivity 开门，隐去即关门）；日志信息流等程序化滚动（syncBar 的 setValue）不会点亮滑轨；鼠标移出应用窗口立即隐去（noteMouseLeft，不等 2s；拖拽滑块中不打断，移到悬浮命令栏等窗口内区域不算离开）；停止操作 2s 后自动隐去；5px 细轨道（6px 基础上再减 1/10），浮现时轨道实色（Termius 主题 #2c2f3f）、滑块与命令栏卡片保持 2px 间距；ScrollFlash 在 ScreenView 内显式 attach（不依赖宿主 attachTree，规避创建时序问题），不占布局无回流
- 选区即复制默认关（右键菜单可开），Ctrl+C 显式复制，右键粘贴；双击选词、三击选行；Ctrl+滚轮改字号
- Ctrl+F / Ctrl+Shift+F 在滚动历史里查找并高亮命中，支持「.*」正则切换（大小写不敏感）；Enter / F3 环形跳转（全屏程序里 Ctrl+F 直通给程序）
- 超链接与剪贴板回传：OSC 8 链接下划线渲染，Ctrl+点击打开（http(s) 交系统，file:// 本地优先、SSH 在线时远端路径自动经 SFTP 取回后用关联程序打开）；OSC 52 剪贴板写入弹窗授权，可勾选「以后自动允许」
- 鼠标上报：SGR 模式（vim / htop 里滚轮翻页、点击定位坐标上报），Shift 强制本地选区/滚历史不受程序接管
- Ctrl+S（XOFF）可能挂起设备输出（画面假死、按键仍在发送）：状态栏黄色常驻提示，Ctrl+Q（XON）或断开连接时熄灭
- 串口场景：本地行编辑与全屏程序自动共存——远端一画屏（备用屏或 BusyBox top 这类仅光标定位的程序），行编辑自动让路变直通；输出回落到提示符样纯文本时自动恢复
- 客户端 Tab 补全：内置常用 Linux/shell 命令 + 命令栏里的命令自动成为候选（嵌入式 shell 多数不吃 Tab，所以补全在本地做）
- 关键词高亮：可配置「关键词 + 颜色 + 启用」规则列表（设置菜单编辑，色块直接预览关键词在终端底色上的效果）；命中自动扩展到完整单词（Successfully 不会只染前半截 Success）；内置日志惯例默认规则（错误红、警告黄、成功绿、信息青，true/false 绿红）开箱即用、可一键补齐也可改删，默认规则随主题调色板换色（深色主题下深底亮色规则换到浅色主题不再发淟，改过色的保持固定色）；终端里选中一个词右键「高亮此词」一键添加；选区/查找命中的优先级高于关键词
- 大块输出分片解析，界面不卡

### SSH（libssh 实现）

- 密码 / 交互式认证 / 私钥登录；主机密钥指纹校验，变更时弹窗确认（本地 known_hosts 存储）
- 端口常用值下拉 + 自定义输入
- 状态栏实时显示**远端主机 CPU / 内存**（走共享连接的独立通道，不开第二条连接）
- SFTP 双栏文件管理器（WinSCP 风格）：左栏本地、右栏远程，各自工具栏与右键菜单；本地栏与远程同列展示（名称/大小/修改时间），目录置顶 + 字母序，大小右对齐；上传支持单个文件或整个文件夹递归（按序自动建目录，同名目录合并前确认）；传输进度实时显示在底部状态栏（文件名、百分比、速率、排队数），传输队列窗口不自动弹出、可手动打开查看详情与取消；记住上次浏览的本地/远程目录；断开会话时文件窗口自动关闭，shell 不受影响

### 串口

- 端口 / 波特率等参数栏，独立连接状态与 SSH 互不影响
- 多行慢速粘贴：按行加延时发送，防嵌入式端 RX 缓冲溢出丢字节；状态栏显示进度，Esc / Ctrl+C 随时取消；可整体关闭、延时可在设置菜单调（默认 50ms）
- 热插拔自动重连：USB 转串口拔插（COM 号变化）后自动重连原端口，等待期间状态栏提示「等待串口重新插入」，可关闭
- 长时间静默后再出日志时任务栏闪烁提醒（等板子重启时不用盯屏，串口/SSH 均生效，静默阈值可配，默认 5 秒）

### 命令栏

- 预设命令列表：添加、删除、拖动手柄排序、复制、上移/下移；点击行内发送钮直接发送到当前链路（串口/SSH 各自记忆）；命令页直接就是列表形态（用户定稿 2026-11-14：按钮列形态整体删除——齿轮/收起按钮/空提示一并移除，无形态切换），行 = 序号 + 命令文本框 + 发送钮 + 拖动手柄，编辑即持久化，底部操作栏（复制/+/-/上移/下移）作用于选中行，Ctrl+Z 撤销删除随时生效
- 多功能侧边栏（用户定稿 2026-11-14）：命令栏卡片顶部一行图标标签切换三页（间距 10px，避免过挤不协调；主题标签用颜料盘图标 palette.png，由 tools/make_icons.py 管线生成——圆盘+拇指孔+颜料点，与主流终端一致；注意 applyTheme() 按索引重设 tab 图标，图标名必须与 mkTab 初始设置同名，否则启动后主题刷新会把新图标覆盖回旧图）——命令页（预设命令列表）/ 主题页（Termius 式主题列表：缩略图+名称，点击即切，当前主题高亮）/ MCP 提示页（原「MCP 提示词」弹窗内容：说明文本 QTextEdit 富文本布局——QPlainTextEdit 布局引擎忽略块行距，行距 160%+段间距 2px、分隔空行压成 8px 固定高避免大空白，提示词区透明融入卡片，顶部提示语已删；配置 JSON 等宽字体+输入框风格代码块随主题，复制按钮）；后续可再扩新页；标题栏主题按钮右侧新增侧边栏切换按钮（Termius 同位，标题栏保持 30px；三类按钮统一 **28×28 正方形**居中不贴边——图标/侧边栏钮与最小化/最大化/关闭钮同尺寸、菜单钮同高，hover/按下显示 8px 圆润圆角块、无常亮选中态，展开态只存内部状态）控制卡片展开/收起，收起后终端铺满窗口，显隐状态持久化；原「收起成侧边小按钮组」形态已废弃
- Termius 式悬浮卡片：终端铺满窗口为唯一主体（无边框，底色即终端画布色），命令栏是浮在其右缘的半透明圆角卡片（8px 圆角、左右留 8px 间隙、顶部收窄至 4px + 卡片内顶边距 8px——用户定稿 2026-11-14：与参数栏之间空白带太大，收窄约 8px，底部内边距仍 12px、向下延伸贴到底部、内容距卡片边缘 8px 不压边），三页统一页宽（用户定稿 2026-11-14：以主题页宽度为准 kPageWidth=240，切标签卡片宽度恒定不变，高度仍随页内容伸缩；卡片宽 = 统一页宽 + 左右内边距，由 CommandBar::sizeHint 自管，堆栈用 PageStack 按当前页取高度 hint，规避 QStackedWidget 全页取 max 把卡片铏死），文本网格与滚动条自动避让卡片宽度
- 卡片毛玻璃质感：底色为主题面板色抬档后的半透明 rgba（暗色更亮/亮色更暗，保证与终端底可辨），无边框线，半透明底透出终端画布色淡影
- 参数栏同款毛玻璃（用户定稿 2026-11-15）：顶部连接栏与 SSH 高级参数条与命令栏卡片统一——半透明玻璃底+8px 圆角+无 1px 描边，透出窗口背景淡影成为悬浮面；静态 qss 兜底值按同公式预计算（lighter(112)/darker(104)+alpha 200），运行时被逐主题 override 覆盖。注意模板改造时的 arg 错位陷阱：多参 arg 等价链式替换最小编号 marker，删 %n 必须守卫（已在模板尾部加注释占位，test_thememanager 有错位断言）
- 主题背景一体化（用户定稿 2026-11-15）：全部主题 windowBg == terminalBg——终端铺满窗口为唯一主体，背景即画布色，不允许两色割裂；早期 6 套双色主题（dracula/nord/monokai/oneDark/solarized dark/light）已统一，themeLibraryHasUniqueIds 有逐主题守卫断言，statusBarBg 是独立设计的状态栏色不受此约束
- 菜单主题入口整体移除（用户定稿 2026-11-15）：设置菜单不再有「主题」子菜单与「主题设置...」对话框（连带删除 openThemeSettings 死代码）——主题切换唯一入口是侧边栏主题页，深浅快切走标题栏主题按钮，菜单不再维护第二套主题列表；「工具」菜单随后也整体移除（见下条）
- 标题栏「工具」菜单整体移除（用户定稿 2026-11-15）：菜单栏只剩「设置 / 帮助」——F5/F9/F10 快捷键由 HotkeyManager 独立注册不受影响（action 访问器保留作快捷键载体，test_hotkeymanager 依赖），对应功能按钮（连接/文件/模式切换）仍在界面直达；连带修正 hotkeymanager.h/titlebar.cpp 注释中过时的菜单挂载描述
- 设置/关于集成到左上角图标（用户定稿 2026-11-15）：标题栏不再有任何文字菜单按钮（addMenuButton 整体删除）——点击左上角应用图标（TitleIcon，手型光标+提示「应用菜单」）在图标正下方弹出唯一主菜单：设置组（关键词高亮/字体/串口三项+两个档位子菜单/退出）+ 分隔线 + 帮助组（文档/关于 Qt/关于 Xterm）；TitleBar 只发 logoClicked 信号（eventFilter 拦 iconLabel 的 MouseButtonPress，不影响拖窗口），菜单内容与弹出位置由 UiBuilder 构建接线，仍复用已有对话框/快捷键行为
- 主题页字体区（用户定稿 2026-11-15）：主题页底部一行「字体」+ 右侧展开箭头（'›' ↔ '˅'，自绘细线 QIcon——实心大三角被用户否定后重做，颜色取 palette 文字色，applyTheme 重刷随主题），展开为字体族（等宽过滤）+ 字号 stepper（− 11 + 自绘 glyph，靠右对齐——用户定稿 2026-11-15，参考 Termius：Text Size 左、stepper 贴右；U+2212 减号字符在主题字体缺字形会渲染成省略号，不能用字符）+ 恢复默认，可再点收起；改动走与设置对话框/Ctrl+滚轮同一信号链（两页互同步+落盘，值相同不发信号不回环），终端本身就是实时预览不再另设预览块；浮层高度固定贴底，展开只压缩上方列表（stretch 吸收）；「恢复默认字体」发信号回抛 UiBuilder（默认族定义在 ScreenView，CommandBar 不反向依赖）；启动恢复/Ctrl+滚轮经 setCurrentTerminalFont 回推控件（守卫不发请求信号）
- 命令列表去卡片化：无边框+透明底（CommandBar QListWidget 作用域规则，融入毛玻璃卡片整体；行级 hover/选中反馈保留）；命令页功能按钮行下沉贴底（用户定稿 2026-11-15：按钮行负底 margin 6px，视觉距底 12→6px，仅命令页不影响其他页）
- 选区与标题栏按钮色全主题适配（用户指出硬编码问题后修复 2026-11-14）：选区色（文本选中底/字）唯一由 ThemeManager override 按主题注入（%9=selectionBg、%26=listSelectedFg，9 套主题各自定义），静态 qss 里的硬编码 selection 已删；标题栏按钮 hover/按下/文字色用 #TitleBar 同构特异性规则按主题注入（静态 qss 的 #TitleBar 前缀特异性更高必须同构覆盖，close 键红色 hover 是 Win 惯例保留）；终端画布内选区色走主题 selectionBg（第三方主题经自定义调色板注入）

### AI / MCP 集成

- AgentBridge：本机命名管道 + 本次启动令牌，把已连上的串口/SSH 暴露给 MCP 客户端（Claude 等 AI 工具），五组 `xterm.*` 工具：
  - **Screen**：`read` 纯文本 / `snapshot` 结构化快照（每行文本 + 颜色段 + 超链接段，绝对行号）/ `find` 滚动历史查找（子串/正则）/ `wait`·`waitForPattern` 等新输出（回显免疫，正则可设最少命中次数）
  - **Transport**：`sendLine`（打字机式逐字发送，`sendTypewriter` 为正式命名入口，可附带 wait）/ `sendControl`（裸控制字符）/ `interrupt`（停打字机 + 发 Ctrl+C 中断远端命令）
  - **FS**（仅 SSH）：`fsStat` / `fsList` / `fsRead` / `fsDownload`，写类（`fsWrite` / `fsUpload`）需 agent.json 显式 `allow_write: true`，路径限当前会话根目录沙箱——形成「读远端日志 → 分析 → 改文件 → 推回」闭环
  - **Host**（仅 SSH）：`cpu` / `mem`（状态栏同源采样）/ `disk` / `uptime` / `top`
  - **Session**：`list` / `current` / `switch`（切串口/SSH 不断另一条）/ `openSsh` / `openSerial`（参数写回参数栏再连接，凭据只认界面配置）/ `close`
- 可观测与防护：所有调用落 `logs/agent-audit.jsonl`（按大小滚动保留）；状态栏角标三态（就绪 / 打字机中 / 「MCP · 工具名（N 秒前）」）；令牌桶限流防 AI 循环刷屏卡界面
- AI 打字期间用户输入锁定，用户 Ctrl+C 可打断（AI 会收到明确的 interrupted 错误）；管道名与令牌在配置目录 agent.json，侧边栏第三页（MCP 页）与状态栏 MCP 按钮弹窗都有现成的 MCP 提示词可复制

### 外观与平台

- 无边框主窗口 + 自绘标题栏（菜单、主题切换、置顶、系统按钮），所有弹窗统一为同风格的无边框对话框（Win11 圆角）；最大化/还原切换自维护伪最大化状态（2026-11-14 修复：FramelessWindowHint 下 Qt 的 setWindowState(Maximized) 只 setGeometry 不置系统 zoomed 位，isMaximized() 恒 false 导致按钮点了没反应——UiBuilder::toggleMaximized 自管状态与还原几何，标题栏按钮/双击共用，按钮字形 ▢/❐ 联动；最大化期间禁用 Win11 圆角——伪最大化非系统 zoomed 态 DWM 仍按普通窗口画圆角，四角透出桌面（用户看到的“透明间隙”，实测四角像素为壁纸色，2026-11-14 修复：toggleMaximized 联动 setWin11RoundedCorners(DONOTROUND/ROUND)，还原恢复圆角））；外围留白恒定 6px 不随窗口状态变（用户定稿 2026-11-14：最大化清零留白后连接栏/卡片贴边有剪切感，两边间距要保持，边距联动逻辑已删）
- 39 套内置主题（原 9 套：VS Code Dark / GitHub Light / Dracula / Nord / Monokai / One Dark / Solarized Dark·Light / Termius 夜航绿；2026-11-14 按 Termius 主题列表复刻新增 30 套：Flexoki Dark·Light、Kanagawa Wave·Dragon·Lotus、Hacker Blue·Green·Red、Everforest Dark·Light、Night Owl / Light Owl、Aura、Rosé Pine·Moon·Dawn、Cobalt2、Octocat Dark·Light、Ayu Dark·Light、Cyberpunk / Cyberpunk Scarlet、Romania Night·Day、Aubergine、Peach Fresh、1984 Dark·Light、Winter Night——全部定义在 thememanager.cpp createThemes()，侧边栏主题页列表与缩略图自动跟随新增；知名开源配色用官方调色板，Termius 独有主题按截图观感近似）：主题设置对话框色块预览 + 终端实时预览、选中即生效、取消可回退；菜单内快捷单选，标题栏一键深/浅翻转；QSS 全局换肤，终端调色板逐主题定制，第三方主题终端正文用主题招牌色（Dracula 淡紫 / Monokai 米黄 / Nord 霜蓝 / One Dark 冷蓝 / Solarized 青灰 / Termius 亮绿），主题跟随每个对话框与取色器；按钮与输入控件全状态适配（普通/悬停/按下/选中/焦点/禁用，含 #id 高特异性的模式切换/齿轮/眼睛按钮与下拉框、数字输入，端口选择框等首个焦点控件不再残留旧灰）；SFTP 文件浏览器/传输队列的树/表格/表头/斑马纹/悬停选中行/pathBar 同步跟随主题（列表类字段逐主题定制）；滚动条 Termius 式：6px 细轨道+同宽直角滑块、平时隐藏，滚动时浮现（轨道色 Termius 主题实色 #2C2F3F，滑块 #285049→悬停招牌亮绿 #21B568），滑块长度钳制 48–100px，内容不足一屏时不浮现、滚动中内容收缩立即隐去，待机鼠标穿透不拦截点击，终端滚动条为 overlay 不再挤压终端列数
- 终端字体设置：等宽字体族 + 字号对话框，实时预览、两页同步；也可 Ctrl+滚轮直接调字号
- 配置持久化：profile（连接参数）、主题、终端字体与字号、串口行为开关（慢速粘贴/自动重连/静默提醒）、命令栏状态、关键词规则、SFTP 上次目录，落盘在用户配置目录

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
- 测试：20 个单测目标（终端解析、视图渲染、SSH 连接/会话/SFTP 通道/客户端、known_hosts、主机统计解析、AgentBridge 与四组扩展工具桥、审计日志、主题、快捷键、MCP 挂钩、会话控制器组装），当前 20/20 通过；另有 sshtest / sftptest 调试工具（默认不构建，`-DXTERM_BUILD_TOOLS=ON` 开启）

## 当前状态与已知边界

- 主线工作流（串口调试、SSH 终端、SFTP、命令栏、关键词高亮、MCP）可用且被测试覆盖
- 工程以 MIT 开源（[LICENSE](../LICENSE)），README 提供完整构建/测试/打包指南
- 仅支持 Windows（Mac 打包占位存在，未验证）
- 尚未实现（候选方向，欢迎投票或补充）：
  - 会话日志记录（输出按时间戳落盘）
  - rz/sz（ZMODEM）自动收发；串口 XMODEM

## 征求建议

最想听到这三类意见：

1. **缺什么**：你日常用的终端工具有、而这里没有的功能？按「每周会用几次」的标准提，不追求数量。
2. **哪里别扭**：现有交互（快捷键、右键菜单、命令栏、SFTP 双栏）用起来不顺手的地方。
3. **值不值得**：上面候选方向里哪些你会真的用？哪些应该直接砍掉？
