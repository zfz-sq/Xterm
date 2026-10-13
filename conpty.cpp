#include "conpty.h"

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ============================================================================
// Win32 侧实现。非 Windows 平台整块编译为空，ConPty::isSupported() 返回 false，
// 上层据此提示"SSH 功能需要 Windows 10 1809 及以上"，不会崩。
// ============================================================================
#ifdef Q_OS_WIN

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

// 光标继承：少一次 conhost 把整屏光标状态重刷进管道。老系统不认这个标志时下面会退回 0。
#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
#define PSEUDOCONSOLE_INHERIT_CURSOR 0x1
#endif

namespace {

// ConPTY 的三个 API 在 Win10 1809 才进 kernel32。老系统上 GetProcAddress
// 取不到，我们据此给出明确提示，而不是让加载期就失败。
using Hpc = void *; // HPCON 本质是句柄，直接当 void* 用最省事且 ABI 兼容
using CreatePseudoConsoleFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, Hpc *);
using ResizePseudoConsoleFn = void(WINAPI *)(Hpc, COORD);
using ClosePseudoConsoleFn = void(WINAPI *)(Hpc);

struct ConPtyApi
{
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
    bool ok = false;
};

const ConPtyApi &conPtyApi()
{
    static const ConPtyApi api = [] {
        ConPtyApi a;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32) {
            a.create = reinterpret_cast<CreatePseudoConsoleFn>(
                GetProcAddress(k32, "CreatePseudoConsole"));
            a.resize = reinterpret_cast<ResizePseudoConsoleFn>(
                GetProcAddress(k32, "ResizePseudoConsole"));
            a.close = reinterpret_cast<ClosePseudoConsoleFn>(
                GetProcAddress(k32, "ClosePseudoConsole"));
            a.ok = a.create && a.resize && a.close;
        }
        return a;
    }();
    return api;
}

// 管道缓冲给足：ConPTY 输出突发（cat 大文件 / top 全量重绘）时不至于频繁阻塞写端。
constexpr int kPipeBuffer = 64 * 1024;
constexpr int kReadChunk = 32 * 1024;
constexpr int kWriteChunk = 4 * 1024;   // 分批写，单次不会长时间占住 IO 线程
constexpr DWORD kWriteTimeoutMs = 250;  // 写超时后余量重新入队，保证读不被饿死
constexpr DWORD kGracefulExitMs = 400;  // 关闭输入后给子进程自我退出的时间
constexpr DWORD kTerminateWaitMs = 1500;
constexpr DWORD kChildExitDrainMs = 800; // 子进程退出后收割剩余输出的窗口
                                          // （conhost 断管可能滞后，不能等它）
constexpr UINT kTerminateExitCode = 0xE0FF; // 我们强杀时的退出码标记

QString pipeNameFor(const QString &tag)
{
    static std::atomic<quintptr> counter{0};
    const quintptr seq = counter.fetch_add(1);
    return QStringLiteral(R"(\\.\pipe\xterm-conpty-%1-%2-%3)")
        .arg(::GetCurrentProcessId())
        .arg(seq)
        .arg(tag);
}

// CreateProcessW 需要的命令行，按 MSVCRT 的引用规则转义（空格/引号/尾部反斜杠）。
QString buildCommandLine(const QString &program, const QStringList &args)
{
    auto needsQuoting = [](const QString &s) {
        if (s.isEmpty())
            return true;
        for (const QChar c : s) {
            if (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('"'))
                return true;
        }
        return false;
    };
    auto appendQuoted = [&needsQuoting](const QString &value) {
        if (!needsQuoting(value))
            return value;
        QString out(QLatin1Char('"'));
        int backslashes = 0;
        for (const QChar c : value) {
            if (c == QLatin1Char('\\')) {
                ++backslashes;
                out += c;
                continue;
            }
            if (c == QLatin1Char('"')) {
                // 引号前的反斜杠要加倍，再补一个被转义的引号
                out += QString(backslashes + 1, QLatin1Char('\\'));
                out += QLatin1Char('"');
            } else {
                out += c;
            }
            backslashes = 0;
        }
        if (backslashes > 0)
            out += QString(backslashes, QLatin1Char('\\')); // 结尾反斜杠加倍，避免吃掉引号
        out += QLatin1Char('"');
        return out;
    };

    QString line = appendQuoted(program);
    for (const QString &a : args) {
        line += QLatin1Char(' ');
        line += appendQuoted(a);
    }
    return line;
}

// conhost 在伪终端建立后会发设备查询，等不到回应则整个渲染管线停摆：
//   ESC[6n   光标位置查询（DSR-CPR）。用了 PSEUDOCONSOLE_INHERIT_CURSOR 后必发，
//            实测不回则 conhost 一个字都不再输出——连 ssh 的报错都被扣住，
//            外观就是「终端零响应」。回复 ESC[<row>;<col>R。
//   ESC[?c / ESC[c   Primary DA，回 VT102 身份 ESC[?1;2c。
//   ESC[>c   Secondary DA，回 xterm 身份 ESC[>0;95;0c。
//   ESC[?u   kitty 键盘协议查询，回「不支持」。
// 查询序列可能被 IO 拆在两个 chunk 交界，所以扫描要带上上一块的未完前缀（m_daCarry），
// 只保留尚未终结的转义前缀做拼接，完整的序列不会进 carry，也就不会被应答两次。
// 应答的同时把查询从转发流里删掉：下游 TermScreen 看不到 6n 就不会二次回 CPR，
// 避免第二份 ESC[1;1R 被 conhost 当键盘输入转进子进程、落到远端 shell 里成垃圾。
QByteArray deviceAttributeReply(const QByteArray &scan, int *queryStart, int *queryLen)
{
    for (int i = 0; i + 3 < scan.size(); ++i) {
        if (scan.at(i) != '\x1b' || scan.at(i + 1) != '[')
            continue;
        const char p0 = scan.at(i + 2);
        if (p0 == '?') {
            if (i + 4 > scan.size())
                continue; // 还没到齐，留在 carry 里等下一块
            const char p1 = scan.at(i + 3);
            if (p1 == 'c' || p1 == 'u') {
                *queryStart = i;
                *queryLen = 4;
                return p1 == 'c' ? QByteArrayLiteral("\x1b[?1;2c")
                                 : QByteArrayLiteral("\x1b[?0u");
            }
        } else if (p0 == '>') {
            if (i + 4 > scan.size())
                continue;
            if (scan.at(i + 3) == 'c') {
                *queryStart = i;
                *queryLen = 4;
                return QByteArrayLiteral("\x1b[>0;95;0c");
            }
        } else if (p0 == 'c') {
            *queryStart = i;
            *queryLen = 3;
            return QByteArrayLiteral("\x1b[?1;2c");
        } else if (p0 == '6') {
            if (i + 4 > scan.size())
                continue;
            if (scan.at(i + 3) == 'n') {
                *queryStart = i;
                *queryLen = 4;
                // 建立时光标必在原点；这也是 conhost INHERIT_CURSOR 期望的答案
                return QByteArrayLiteral("\x1b[1;1R");
            }
        }
    }
    return QByteArray();
}

// 块尾的未完转义前缀（ESC 起头但还没终结符）。查询序列最长 4 字节，
// 前缀最多 3 字节，这里留 4 字节余量。
QByteArray danglingEscapePrefix(const QByteArray &scan)
{
    const int esc = scan.lastIndexOf('\x1b');
    if (esc < 0 || scan.size() - esc >= 4)
        return QByteArray(); // 尾部没有 ESC，或前缀已超过任何查询的长度
    return scan.mid(esc);
}

} // namespace

// ---------------------------------------------------------------------------
// PtyTask: 一条线程独占所有句柄，读写都在里面完成，避免跨线程句柄竞争。
// ---------------------------------------------------------------------------
class PtyTask : public QThread
{
    Q_OBJECT

public:
    PtyTask(const QString &program, const QStringList &args, int cols, int rows)
        : m_program(program),
          m_args(args),
          m_cols(cols),
          m_rows(rows)
    {
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_writeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // 自动重置
        m_resizeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~PtyTask() override
    {
        for (HANDLE h : {m_stopEvent, m_writeEvent, m_resizeEvent}) {
            if (h)
                CloseHandle(h);
        }
    }

    // 下面三个方法从 GUI 线程调用，只做「置标志 + 唤醒」，绝不触碰句柄。
    void enqueueWrite(const QByteArray &data)
    {
        if (data.isEmpty())
            return;
        {
            QMutexLocker lk(&m_writeMutex);
            m_writeQueue += data;
        }
        if (m_writeEvent)
            SetEvent(m_writeEvent);
    }

    void requestResize(int cols, int rows)
    {
        if (cols < 1 || rows < 1)
            return;
        if (m_cols == cols && m_rows == rows)
            return;
        m_cols = cols;
        m_rows = rows;
        if (m_resizeEvent)
            SetEvent(m_resizeEvent);
    }

    void requestStop()
    {
        if (m_stopEvent)
            SetEvent(m_stopEvent);
    }

    bool workerRunning() const { return m_running.load(); }

signals:
    void launched(const QString &errorText);
    void outputReceived(const QByteArray &chunk);
    void taskFinished(int exitCode, bool terminatedByUs);

protected:
    void run() override;

private:
    void emitOutput(const QByteArray &chunk);
    QString setup();
    QString spawn();
    void drainWrites();
    void applyResize();
    int shutdownHandles();
    bool stopRequested() const
    {
        return WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0;
    }

    QString m_program;
    QStringList m_args;
    std::atomic<int> m_cols{80};
    std::atomic<int> m_rows{24};
    std::atomic<bool> m_running{false};

    QMutex m_writeMutex;
    QByteArray m_writeQueue;

    HANDLE m_stopEvent = nullptr;
    HANDLE m_writeEvent = nullptr;
    HANDLE m_resizeEvent = nullptr;

    // 仅在 run() 所在线程访问
    HANDLE m_hInputWrite = nullptr;  // 我们写 → pty 读
    HANDLE m_hOutputRead = nullptr;  // pty 写 → 我们读
    Hpc m_pty = nullptr;
    HANDLE m_childProcess = nullptr;
    bool m_terminatedByUs = false;
    // 已经告知伪终端的尺寸，用于抑制重复 resize
    SHORT m_appliedCols = 0;
    SHORT m_appliedRows = 0;
    // 上一块尾部未完的转义前缀，拼接进下一块一起扫设备查询
    QByteArray m_daCarry;
};

void PtyTask::run()
{
    // 键入和回显都走这条线程。抬高优先级，避免被界面绘制挤到后面。
    setPriority(QThread::HighPriority);

    const QString err = setup();
    if (!err.isEmpty()) {
        emit launched(err);
        shutdownHandles();
        return;
    }
    emit launched(QString());
    m_running.store(true);

    std::vector<CHAR> buffer(kReadChunk);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        emit launched(QStringLiteral("创建事件对象失败 (%1)").arg(::GetLastError()));
        m_running.store(false);
        shutdownHandles();
        return;
    }

    bool readPending = false;
    QString readError;
    // 子进程退出后 conhost 关闭管道可能滞后很久（实测 ssh.exe 失败退出后
    // >6s 才断管）。会话结束不能只等管道 EOF：把子进程句柄也挂进等待列表，
    // 进程一退就开启一个短暂的「收割窗口」把剩余输出读完，然后主动收尾。
    ULONGLONG drainDeadlineMs = 0;

    for (;;) {
        if (stopRequested())
            break;
        if (drainDeadlineMs && GetTickCount64() >= drainDeadlineMs)
            break; // 收割窗口结束（正常 EOF 通常远早于这个时刻）

        drainWrites();
        applyResize();

        if (!readPending) {
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            const BOOL ok = ReadFile(m_hOutputRead, buffer.data(),
                                     DWORD(buffer.size()), &got, &ov);
            const DWORD le = ::GetLastError();
            if (ok) {
                if (got > 0)
                    emitOutput(QByteArray(buffer.data(), int(got)));
                continue;
            }
            if (le == ERROR_IO_PENDING) {
                readPending = true;
            } else if (le == ERROR_MORE_DATA) {
                // 缓冲区放不下全部可读字节：已写入量仍然有效，取回后继续
                DWORD done = 0;
                GetOverlappedResult(m_hOutputRead, &ov, &done, FALSE);
                if (done > 0)
                    emitOutput(QByteArray(buffer.data(), int(done)));
                continue;
            } else if (le == ERROR_BROKEN_PIPE || le == ERROR_PIPE_NOT_CONNECTED
                       || le == ERROR_HANDLE_EOF) {
                break; // 伪终端关闭 → 会话结束
            } else {
                readError = QStringLiteral("读取伪终端失败 (%1)").arg(le);
                break;
            }
        }

        HANDLE wakes[5] = { ov.hEvent, m_stopEvent, m_writeEvent, m_resizeEvent,
                            m_childProcess };
        const DWORD nWakes = m_childProcess ? 5 : 4;
        const DWORD waitMs = drainDeadlineMs
            ? DWORD(qBound<ULONGLONG>(1, drainDeadlineMs - GetTickCount64(),
                                      kChildExitDrainMs))
            : INFINITE;
        const DWORD r = WaitForMultipleObjects(nWakes, wakes, FALSE, waitMs);
        if (r == WAIT_OBJECT_0) {
            // 读完成
            readPending = false;
            DWORD done = 0;
            if (!GetOverlappedResult(m_hOutputRead, &ov, &done, FALSE)) {
                const DWORD le = ::GetLastError();
                if (le != ERROR_BROKEN_PIPE && le != ERROR_PIPE_NOT_CONNECTED
                    && le != ERROR_HANDLE_EOF)
                    readError = QStringLiteral("伪终端读取完成失败 (%1)").arg(le);
                break;
            }
            if (done > 0)
                emitOutput(QByteArray(buffer.data(), int(done)));
            continue;
        }
        if (r == WAIT_OBJECT_0 + 1) {
            m_terminatedByUs = true;
            break; // 上层要求停止
        }
        if (r == WAIT_OBJECT_0 + 2 || r == WAIT_OBJECT_0 + 3)
            continue; // 有新输入 / 尺寸变化，回循环头处理
        if (r == WAIT_OBJECT_0 + 4) {
            // 子进程退出：开启收割窗口。进程句柄保持 signaled，不会重复触发
            if (!drainDeadlineMs)
                drainDeadlineMs = GetTickCount64() + kChildExitDrainMs;
            continue;
        }
        if (r == WAIT_TIMEOUT)
            continue; // 收割窗口计时到点，循环头会 break
        readError = QStringLiteral("等待伪终端失败 (%1)").arg(r);
        break;
    }

    if (readPending) {
        // 让挂起的读落地，避免关闭句柄时仍有 IO 在跑
        CancelIoEx(m_hOutputRead, &ov);
        DWORD done = 0;
        GetOverlappedResult(m_hOutputRead, &ov, &done, TRUE);
    }
    CloseHandle(ov.hEvent);

    if (!readError.isEmpty())
        qWarning() << "ConPty:" << readError;

    m_running.store(false);
    const int exitCode = shutdownHandles();
    emit taskFinished(exitCode, m_terminatedByUs);
}

void PtyTask::emitOutput(const QByteArray &chunk)
{
    // 先回答 conhost 的设备查询，再把查询序列从转发流里删掉，顺序与真实终端一致。
    // 扫描窗口 = 上一块的未完前缀 + 当前块，跨块撕裂的查询也能对上。
    QByteArray scan = m_daCarry + chunk;
    int start = -1;
    int len = 0;
    const QByteArray reply = deviceAttributeReply(scan, &start, &len);
    if (start >= 0 && len > 0) {
        scan.remove(start, len);
        if (!reply.isEmpty())
            enqueueWrite(reply);
    }
    // 尾部还没终结的转义前缀留给下一块拼接；刚被消费掉的查询不会在这里出现
    m_daCarry = danglingEscapePrefix(scan);
    emit outputReceived(scan);
}

QString PtyTask::setup()
{
    const ConPtyApi &api = conPtyApi();
    if (!api.ok)
        return QStringLiteral("当前系统不支持 ConPTY（需要 Windows 10 1809 及以上版本）");

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    const QString inPath = pipeNameFor(QStringLiteral("in"));
    const QString outPath = pipeNameFor(QStringLiteral("out"));

    // 我们的服务端句柄走重叠 IO：可取消、可超时，停止时不需要任何 hack。
    m_hInputWrite = CreateNamedPipeW(
        reinterpret_cast<LPCWSTR>(inPath.utf16()),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
        1, kPipeBuffer, kPipeBuffer, 0, &sa);
    if (m_hInputWrite == INVALID_HANDLE_VALUE)
        return QStringLiteral("创建输入管道失败 (%1)").arg(::GetLastError());

    m_hOutputRead = CreateNamedPipeW(
        reinterpret_cast<LPCWSTR>(outPath.utf16()),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
        1, kPipeBuffer, kPipeBuffer, 0, &sa);
    if (m_hOutputRead == INVALID_HANDLE_VALUE)
        return QStringLiteral("创建输出管道失败 (%1)").arg(::GetLastError());

    // 不调 ConnectNamedPipe：它是「服务端等客户端来连」的原语，而客户端句柄在
    // 下面才由本线程创建，先等后开会自等待死锁（IO 线程永远卡在这里，
    // launched/finished 都发不出去，界面表现为永远「正在连接」、取消后永远
    // 「断开中」）。单实例、自连自用的管道直接 CreateFileW 打开即完成连接，
    // 与 ConPTY 官方示例（EchoCon）一致。
    // 交给伪终端的两个客户端句柄用同步方式（conhost 内部按同步读写使用它们）。
    const HANDLE hInClient = CreateFileW(
        reinterpret_cast<LPCWSTR>(inPath.utf16()), GENERIC_READ, 0, &sa,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hInClient == INVALID_HANDLE_VALUE)
        return QStringLiteral("打开输入管道失败 (%1)").arg(::GetLastError());

    const HANDLE hOutClient = CreateFileW(
        reinterpret_cast<LPCWSTR>(outPath.utf16()), GENERIC_WRITE, 0, &sa,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOutClient == INVALID_HANDLE_VALUE) {
        CloseHandle(hInClient);
        return QStringLiteral("打开输出管道失败 (%1)").arg(::GetLastError());
    }

    COORD size;
    size.X = SHORT(qBound(20, m_cols.load(), 500));
    size.Y = SHORT(qBound(4, m_rows.load(), 200));
    m_appliedCols = size.X; // 建终端时用的就是这个尺寸，别在循环里再"变一次"
    m_appliedRows = size.Y;
    HRESULT hr = api.create(size, hInClient, hOutClient, PSEUDOCONSOLE_INHERIT_CURSOR, &m_pty);
    if (FAILED(hr))
        hr = api.create(size, hInClient, hOutClient, 0, &m_pty);

    // 必须关闭这两个客户端句柄：conhost 已把它们复制到自己的进程；而我们若留着
    // 写端（hOutClient），本进程的读端永远看不到 EOF，子进程退出也就检测不到。
    CloseHandle(hInClient);
    CloseHandle(hOutClient);

    if (FAILED(hr))
        return QStringLiteral("创建伪终端失败 (0x%1)")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));

    return spawn();
}

QString PtyTask::spawn()
{
    const QString line = buildCommandLine(m_program, m_args);
    std::vector<WCHAR> cmdLine;
    cmdLine.reserve(size_t(line.size()) + 1);
    for (const QChar c : line)
        cmdLine.push_back(WCHAR(c.unicode()));
    cmdLine.push_back(L'\0');

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    if (attrSize == 0)
        return QStringLiteral("初始化进程属性列表失败 (%1)").arg(::GetLastError());

    std::vector<BYTE> attrBuf(attrSize);
    auto *attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize))
        return QStringLiteral("初始化进程属性列表失败 (%1)").arg(::GetLastError());

    const BOOL updated = UpdateProcThreadAttribute(
        attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, m_pty, sizeof(m_pty),
        nullptr, nullptr);
    if (!updated) {
        const DWORD e = ::GetLastError();
        DeleteProcThreadAttributeList(attrList);
        return QStringLiteral("绑定伪终端失败 (%1)").arg(e);
    }

    // EXTENDED_STARTUPINFO_PRESENT 要求 cb 为 sizeof(STARTUPINFOEXW)：拷贝整个
    // STARTUPINFOW 会把 cb 带成普通结构尺寸，CreateProcessW 拒绝（Win32 错误 87）。
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrList;

    // 必须显式声明 STARTF_USESTDHANDLES 并把三个标准句柄留 NULL：
    // 不声明时子进程会沿用「本进程自己的」标准句柄。Xterm 被 PowerShell
    // 带重定向拉起时 stderr 是文件句柄，ssh.exe 的 Permission denied / 
    // 认证提示就顺着这个句柄漏进文件，终端里一个字都看不到 —— 用户敲错密码
    // 只见到又一行 password 提示，没有任何解释，看起来就像"输入没反应"。
    // 三个字段给 NULL 等于告诉 CreateProcess「别继承，用子进程控制台的那套」，
    // 而子进程的控制台正是我们的伪终端。
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.hStdOutput = nullptr;
    si.StartupInfo.hStdError = nullptr;

    // 向子进程声明终端类型：ssh.exe 把 TERM 原样填进 pty-req 转告远端，远端据此
    // 选 terminfo。GUI 程序的继承环境里根本没有 TERM，远端只能按 unknown/dumb 办，
    // 于是 clear / tput 这类靠 terminfo 取能力的命令查不到能力、一个字都不输出，
    // 看着就像命令没执行。这里覆盖而不是仅在缺失时补：从别的终端启动 Xterm 时
    // 继承来的 TERM 描述的是那个终端，不是我们。
    // 选 xterm 而不是 xterm-256color：Buildroot/BusyBox 这类最小系统未必带后者，
    // 宁可颜色少几档，也不能让 clear 又变成没反应。
    // 用「改本进程环境 + 保持整体继承」而不是自造环境块：环境块得手拼，容易丢掉
    // =C: 这类记录各盘符当前目录的隐藏变量，cmd.exe 会认不出换盘后该落在哪个目录。
    // 仍用 xterm 而不是 xterm-256color：很多嵌入式 rootfs 没有后者的 terminfo，
    // clear / tput 会直接没输出。本终端自己能画 256 色和真彩色，远端发了就能显示。
    ::SetEnvironmentVariableW(L"TERM", L"xterm");
    ::SetEnvironmentVariableW(L"COLORTERM", L"truecolor");

    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                   nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrList);

    if (!ok) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return QStringLiteral("找不到 %1，请确认系统已安装 OpenSSH 客户端").arg(m_program);
        if (e == ERROR_INVALID_PARAMETER)
            return QStringLiteral("启动 %1 失败：命令行参数非法 (Win32 错误 87)，"
                                  "请检查主机、端口、私钥路径里是否混入了换行或控制字符")
                .arg(m_program);
        return QStringLiteral("启动 %1 失败 (Win32 错误 %2)").arg(m_program).arg(e);
    }
    m_childProcess = pi.hProcess;
    CloseHandle(pi.hThread); // 线程句柄用不上
    return QString();
}

void PtyTask::drainWrites()
{
    QByteArray data;
    {
        QMutexLocker lk(&m_writeMutex);
        data.swap(m_writeQueue);
    }
    if (data.isEmpty())
        return;
    if (!m_hInputWrite) {
        qWarning() << "ConPty: 输入管道未就绪，丢弃" << data.size() << "字节";
        return;
    }

    int offset = 0;
    while (offset < data.size() && !stopRequested()) {
        const int chunk = qMin(data.size() - offset, kWriteChunk);
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
            break;

        DWORD written = 0;
        BOOL done = WriteFile(m_hInputWrite, data.constData() + offset, DWORD(chunk),
                              &written, &ov);
        DWORD le = done ? ERROR_SUCCESS : ::GetLastError();
        if (!done && le == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, kWriteTimeoutMs) == WAIT_OBJECT_0) {
                done = GetOverlappedResult(m_hInputWrite, &ov, &written, FALSE);
                le = done ? ERROR_SUCCESS : ::GetLastError();
            } else {
                // 对端不读（远端阻塞）：取消这次写，余量重新入队，先回去处理输出
                CancelIoEx(m_hInputWrite, &ov);
                DWORD actual = 0;
                GetOverlappedResult(m_hInputWrite, &ov, &actual, TRUE);
                written = actual;
                done = FALSE;
                le = ERROR_TIMEOUT;
            }
        } else if (!done && le == ERROR_MORE_DATA) {
            GetOverlappedResult(m_hInputWrite, &ov, &written, FALSE);
            done = TRUE;
            le = ERROR_SUCCESS;
        }

        CloseHandle(ov.hEvent);
        offset += int(written);
        if (done)
            continue;
        if (le == ERROR_BROKEN_PIPE || le == ERROR_NO_DATA || le == ERROR_PIPE_NOT_CONNECTED)
            break; // 子进程已退出，输入无处可去
        if (le != ERROR_TIMEOUT)
            qWarning() << "ConPty: 写入失败" << le;
        break;
    }

    if (offset < data.size() && !stopRequested()) {
        QMutexLocker lk(&m_writeMutex);
        m_writeQueue.prepend(data.mid(offset));
    }
}

void PtyTask::applyResize()
{
    if (!m_pty)
        return;
    const ConPtyApi &api = conPtyApi();
    if (!api.resize)
        return;
    const SHORT cols = SHORT(qBound(20, m_cols.load(), 500));
    const SHORT rows = SHORT(qBound(4, m_rows.load(), 200));
    // 尺寸没变绝不能调：conhost 每次收到 resize 都重绘整屏（清行 + 重排光标），
    // 而本函数在每轮循环头都会被叫一次 —— 无条件调用就成了「重绘产生输出 →
    // 输出唤醒循环 → 再重绘」的自激环路，表现为整屏空白闪动、敲键没反应。
    // 真实终端模拟器都只在尺寸真的变了才通知伪终端。
    if (cols == m_appliedCols && rows == m_appliedRows)
        return;
    m_appliedCols = cols;
    m_appliedRows = rows;
    COORD size;
    size.X = cols;
    size.Y = rows;
    // 只在 IO 线程调：与读写串行，避免 conhost 在输出途中改尺寸
    api.resize(m_pty, size);
}

int PtyTask::shutdownHandles()
{
    // 1) 关掉输入写端 → pty 侧 EOF，交互式 shell 通常自己退出（不打扰用户）
    if (m_hInputWrite) {
        CloseHandle(m_hInputWrite);
        m_hInputWrite = nullptr;
    }

    // 2) 等一小会儿，还不退就强杀。这些都发生在 IO 线程里，GUI 零感知
    int exitCode = 0;
    if (m_childProcess) {
        if (WaitForSingleObject(m_childProcess, kGracefulExitMs) == WAIT_TIMEOUT) {
            m_terminatedByUs = true;
            TerminateProcess(m_childProcess, kTerminateExitCode);
            WaitForSingleObject(m_childProcess, kTerminateWaitMs);
        }
        DWORD code = 0;
        if (GetExitCodeProcess(m_childProcess, &code) && code != STILL_ACTIVE)
            exitCode = int(code);
        CloseHandle(m_childProcess);
        m_childProcess = nullptr;
    }

    // 3) 伪终端与剩下的读端句柄
    if (m_pty) {
        conPtyApi().close(m_pty);
        m_pty = nullptr;
    }
    if (m_hOutputRead) {
        CloseHandle(m_hOutputRead);
        m_hOutputRead = nullptr;
    }
    return exitCode;
}

#endif // Q_OS_WIN

// ============================================================================
// ConPty 门面
// ============================================================================
ConPty::ConPty(QObject *parent)
    : QObject(parent)
{
}

ConPty::~ConPty()
{
    if (!m_task)
        return;
#ifdef Q_OS_WIN
    m_task->requestStop();
    m_task->wait(3000);
    if (m_task->isRunning()) {
        // 走到这里说明 IO 线程没能收尾（理论不该发生，比如句柄被外部占住）。
        // 此时进程正在退出，terminate 后由 OS 回收句柄，不再等待。
        qWarning() << "ConPty: IO 线程未在 3 秒内退出，强制结束";
        m_task->terminate();
        m_task->wait(500);
    }
#endif
    delete m_task;
    m_task = nullptr;
}

bool ConPty::isSupported()
{
#ifdef Q_OS_WIN
    return conPtyApi().ok;
#else
    return false;
#endif
}

bool ConPty::start(const QString &program, const QStringList &args, int cols, int rows)
{
#ifdef Q_OS_WIN
    if (m_task && m_task->workerRunning())
        return false;
    reset();

    auto *task = new PtyTask(program, args, cols, rows);
    // task 在 GUI 线程创建，run() 里 emit → AutoConnection 自动跨队投递到 GUI 线程
    connect(task, &PtyTask::launched, this, &ConPty::launched);
    connect(task, &PtyTask::outputReceived, this, &ConPty::outputReceived);
    connect(task, &PtyTask::taskFinished, this, &ConPty::finished);
    m_task = task;
    task->start();
    return true;
#else
    Q_UNUSED(program)
    Q_UNUSED(args)
    Q_UNUSED(cols)
    Q_UNUSED(rows)
    return false;
#endif
}

void ConPty::write(const QByteArray &data)
{
#ifdef Q_OS_WIN
    if (m_task)
        m_task->enqueueWrite(data);
#else
    Q_UNUSED(data)
#endif
}

void ConPty::resize(int cols, int rows)
{
#ifdef Q_OS_WIN
    if (m_task)
        m_task->requestResize(cols, rows);
#else
    Q_UNUSED(cols)
    Q_UNUSED(rows)
#endif
}

void ConPty::stop()
{
#ifdef Q_OS_WIN
    if (m_task)
        m_task->requestStop();
#endif
}

void ConPty::waitForFinished(int msecs)
{
#ifdef Q_OS_WIN
    if (m_task)
        m_task->wait(msecs);
#else
    Q_UNUSED(msecs)
#endif
}

bool ConPty::isRunning() const
{
#ifdef Q_OS_WIN
    return m_task && m_task->workerRunning();
#else
    return false;
#endif
}

void ConPty::reset()
{
#ifdef Q_OS_WIN
    if (!m_task)
        return;
    if (m_task->isRunning())
        return; // 线程还在跑，此时删除是 UB；等 finished 之后再 reset
    delete m_task;
    m_task = nullptr;
#endif
}

#ifdef Q_OS_WIN
#include "conpty.moc"
#endif
