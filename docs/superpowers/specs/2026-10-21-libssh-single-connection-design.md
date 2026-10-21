# Xterm 单连接 libssh 架构设计

## 目标

将终端、SFTP 文件管理和主机状态采集迁移到同一条进程内 SSH 连接。远端 PTY
字节原样进入 `TermScreen`，不再经过 `ssh.exe`、ConPTY 或输出提示词扫描。

首版仅支持 Windows 10/11、Qt 6.8 和 LLVM-MinGW。实现采用 libssh 0.12.2
与动态 OpenSSL 后端，应用随包部署 libssh 和 OpenSSL DLL。

## 成功标准

- 一次 TCP/SSH 握手和认证承载 shell、SFTP、stats 三类通道。
- Shell 支持 UTF-8、ANSI、vim、top、clear、窗口尺寸变化和交互式认证。
- 支持密码、keyboard-interactive、显式私钥、默认私钥、SSH agent、
  OpenSSH 配置和 ProxyJump 等主流客户端能力。
- SFTP 大文件传输期间，终端输入输出保持可交互且可取消传输。
- SFTP 或 stats 通道失败不结束正常的终端通道。
- 不依赖系统安装 OpenSSH 客户端，不在运行时保留旧 SSH 后端。

不承诺与 OpenSSH CLI 的所有边缘行为完全一致。FIDO、安全密钥、PKCS#11、
GSSAPI 和所有复杂 `ssh_config` 语义不属于首版硬性验收条件。

## 技术选择

### 采用：libssh 单连接多通道

libssh 提供配置解析、ProxyJump、agent、结构化认证、PTY、shell、SFTP 和 exec
通道。0.12 系列提供 SFTP AIO API，可以在同一会话线程中协作调度终端和文件传输。

### 不采用：继续扩展 libssh2

libssh2 已在项目中用于 SFTP，但缺少完整的 OpenSSH 配置和跳板能力。补齐这些行为
会把协议库之外的客户端策略长期留给本项目维护。

### 不采用：OpenSSH 子进程

OpenSSH 子进程兼容性高，但认证事件仍混在终端字节流，SFTP 和状态采集不能自然复用
同一会话。ConPTY passthrough 只能减少字节改写，不能解决会话割裂。

## 总体架构

`SshSession` 保留现有 `Transport` 接口，作为 GUI 层唯一 SSH 门面。它持有一个
`SshWorker` 和专用 `QThread`。所有 libssh 句柄只能由 `SshWorker` 所在线程访问。

`SshWorker` 由以下职责单一的单元组成：

- `LibsshConnection`：配置解析、连接、握手、主机密钥、认证、保活和断开。
- `ShellChannel`：PTY、shell、输入输出、窗口尺寸和退出状态。
- `SftpChannel`：目录与元数据操作、AIO 上传下载及取消。
- `StatsChannel`：长期 exec 通道、采样协议和重试。

`SftpClient` 和 `HostStats` 保留为 UI 适配器，但不再拥有 socket、SSH session 或
工作线程。它们把现有界面请求转发给 `SshSession`，再把带请求 ID 的结果转换为
现有信号。

连接阶段依次为：

1. 加载目标和 OpenSSH 配置。
2. 建立 TCP 或 ProxyJump 连接。
3. SSH 握手。
4. 校验应用私有 known_hosts。
5. 执行结构化认证。
6. 请求 PTY 并打开 shell。
7. 按需打开 SFTP 和 stats 通道。

## 线程与调度

`ssh_session`、所有 `ssh_channel` 和 `sftp_session` 不跨线程。GUI 到 worker 的
调用全部使用 queued signal/slot，并携带递增的连接代次和请求 ID。旧连接或已关闭
对话框的迟到结果会被丢弃。

worker 使用非阻塞 libssh 会话和 `ssh_event` 推进状态机。每个调度周期按以下顺序
分配工作：

1. 读取 shell 输出。
2. 写入待发 shell 输入和窗口尺寸。
3. 推进有限数量的 SFTP AIO 请求。
4. 处理 SFTP 元数据操作。
5. 读取或重建 stats 通道。
6. 处理保活和超时。

SFTP 上传下载使用 `sftp_aio_begin_*` 与 `sftp_aio_wait_*`，收到 `SSH_AGAIN` 时把
控制权还给调度器。单轮传输有字节和请求数预算，避免大文件传输饿死终端。

## 配置与认证

目标中的 host 可以是 `~/.ssh/config` 别名。配置解析后，界面明确填写的端口、
用户和私钥作为显式选项。首版支持：

- SSH agent 和自动公钥认证。
- 显式私钥与默认身份文件。
- 密码认证。
- keyboard-interactive 的多个结构化提示及 echo 标志。
- ProxyJump。

认证提示以 request ID、提示文本和是否回显传给 GUI。GUI 回答后，worker 继续认证
状态机。禁止通过扫描 shell 输出识别密码或主机确认。敏感回答不写日志，认证结束后
清空 worker 中的临时字节缓冲。

## 主机密钥

应用继续使用 `QStandardPaths::AppConfigLocation` 下的私有 known_hosts，不读写
用户的 `~/.ssh/known_hosts`。新文件采用标准 OpenSSH known_hosts 行格式，由 libssh
原生读取和更新。

安全策略为 TOFU：

- 首次未知主机自动信任并记录。
- 已知主机密钥一致时继续连接。
- 密钥变化时暂停连接，显示旧、新 SHA-256 指纹并要求用户确认。
- 用户拒绝时立即终止连接。

旧文件使用 `host:port sha256hex` 自定义格式，无法直接转换成完整 OpenSSH 记录。
迁移时先把旧文件原样改名保留。目标首次连接时若存在旧 SHA-256，则与当前密钥比较；
一致时写入完整 OpenSSH 记录，不一致时按密钥变化流程处理。

TOFU 不能防止首次连接时的中间人攻击，界面帮助文案必须说明这一限制。

## 终端数据

Shell 读取路径为：

`ssh_channel_read_nonblocking()` → worker 信号 → `Transport::received()` →
`ScreenView` → `TermScreen`。

SSH 层不解释、不删除、不应答且不改写 VT 序列。窗口变化通过
`ssh_channel_change_pty_size()` 发送。远端 EOF、exit-status 和 exit-signal 转换为
结构化会话结束事件。

## SFTP

SFTP 与 shell 共用认证后的 `ssh_session`。目录、stat、权限、重命名和删除操作异步
排队；上传下载采用 AIO 状态机。每个操作具有请求 ID、连接代次、取消标志和一次性
完成保证。

SFTP 通道自身失败时：

1. 当前操作返回结构化错误。
2. worker 释放损坏的 SFTP 句柄。
3. 下一次请求尝试重建一次 SFTP 子系统。
4. 重建失败则文件功能进入降级状态，shell 保持连接。

服务器拒绝新增通道时给出明确错误，不建立第二条 SSH 连接。

## 主机状态

stats 使用同一会话的长期 exec 通道，周期性输出带边界标记的 `/proc/stat` 和
`/proc/meminfo`。解析器只消费该通道，不向交互 shell 注入命令。

stats 失败时状态栏显示 `--`，并指数退避重开通道，最大退避 30 秒。错误不会反复
弹窗，也不会结束 shell。

## 生命周期与错误

内部连接状态包括：

- `ResolvingConfig`
- `Connecting`
- `VerifyingHost`
- `Authenticating`
- `OpeningShell`
- `Ready`
- `Closing`

GUI 仍使用 `Transport` 的四态。错误结构包含领域、错误码、用户消息和是否可重试。

关闭流程：

1. 停止接收新请求。
2. 取消并释放 SFTP AIO。
3. 关闭 stats 和 SFTP。
4. 关闭 shell。
5. 发送 SSH disconnect。
6. 释放 session 和线程侧对象。

关闭和连接过程均可取消，GUI 线程不得等待阻塞式网络调用。Shell EOF 或底层连接断开
会取消所有附属操作并且只发一次 `closed` 和至多一次主错误。SFTP/stats 局部失败
不会结束 shell。

交互 shell 不自动重连，因为新连接无法恢复远端进程状态。网络静默超时按主连接失败
处理，并由用户明确重新连接。

## 实施阶段

### 阶段 1：依赖与构建基线

- 锁定 libssh 0.12.2、OpenSSL 版本和下载校验值。
- 建立 Windows LLVM-MinGW 的可复现依赖构建与 DLL 部署规则。
- Debug 和 Release 均通过最小连接探针。

### 阶段 2：新终端链路

- 实现连接、known_hosts 迁移、认证、PTY/shell、resize、保活和关闭。
- 将 `SshSession` 切换到新实现。
- 验证 shell、vim、top、clear、ANSI、UTF-8 和窗口变化。

### 阶段 3：共享 SFTP

- 将现有文件操作迁移到共享 SFTP 通道。
- 实现 AIO 传输、进度、取消和通道重建。
- 校验目录操作与上传下载内容。

### 阶段 4：共享状态采集

- 将 `HostStats` 改成共享 exec 通道。
- 实现采样解析、退避和降级。

### 阶段 5：清理与发布验收

- 删除 ConPTY、`ssh.exe` 后端和 libssh2。
- 删除密码提示扫描、设备查询应答及仅为 ConPTY 存在的终端启发式。
- 更新部署规则、第三方许可证和诊断工具。
- 完成 Debug/Release 构建与真机回归。

实现期间允许旧文件暂时留在源码树中以保持阶段可构建，但产品不提供双后端选择，
最终构建不包含旧实现。

## 测试

### 单元测试

- 配置别名和显式字段优先级。
- 私有 known_hosts 的未知、一致、变化和旧格式迁移。
- agent、私钥、password、keyboard-interactive 认证状态机。
- 请求 ID、连接代次、取消和一次性完成。
- libssh 错误到领域错误的映射。
- stats 分帧和 `/proc` 解析。

### 组件测试

通过可注入的 libssh API 边界模拟 `SSH_AGAIN`、部分读写、通道 EOF、连接中断和
SFTP 响应乱序，验证调度器不会阻塞、饿死终端或重复完成请求。

### 真机端到端测试

使用现有测试主机和环境变量提供的凭据验证：

- 登录、退出、取消连接和网络断开。
- `stty size` 与多次窗口调整。
- ANSI、UTF-8、vim、top 和 clear。
- SFTP 列表、上传、下载、重命名、权限、删除和 SHA-256 内容校验。
- 大文件传输期间持续执行 shell 往返探测并验证交互可用。
- 活跃传输中关闭会话，确认 GUI 不阻塞且所有请求只结束一次。
- stats 更新且不污染 shell 输出。

每个实施阶段必须可编译、可独立验证并单独提交后，才能进入下一阶段。

## 许可证与发布

libssh 动态链接并随应用分发 DLL。发布包包含 libssh、OpenSSL 的许可证文本、版本和
源码获取说明。依赖 DLL 版本锁定并记录校验值，安全更新可以不修改应用业务代码。
