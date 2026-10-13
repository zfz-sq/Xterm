// 决定性实验: ConPty setup() 管道连接顺序 + ssh.exe 端到端
//
// Phase A: 复刻 conpty.cpp 旧顺序 —— 重叠管道 + 同线程「先 ConnectNamedPipe 等
//          客户端、后 CreateFileW 当客户端」。验证 ConnectNamedPipe(nullptr)
//          在重叠句柄上是否阻塞(= 自等待死锁)。
// Phase B: 新顺序 —— 去掉 ConnectNamedPipe, 客户端直接 CreateFileW, 完整跑通
//          ConPTY + cmd.exe 输出回读。
// Phase C: 真实 ssh.exe 连一个必然立刻拒绝的地址(127.0.0.1:1), 验证
//          「发起 → 输出直通 → ssh 退出 → EOF」全程不卡(对应界面:
//          不再永远「正在连接」, 失败快速回到可重试状态)。
// Phase D: 复刻取消路径 —— ssh 连一个不可达地址(挂到 ConnectTimeout), 800ms 后
//          按 ConPty::shutdownHandles 的顺序收尾(关输入写端 → 400ms → 强杀),
//          验证收尾时间(对应界面: 点「取消」不再永远「断开中」)。
// Phase E: STARTUPINFOEXW 的 cb 必须是 sizeof(STARTUPINFOEXW) —— 复刻旧写法
//          (cb 被 STARTUPINFOW 拷贝带小)证实 CreateProcessW 报 Win32 错误 87,
//          正确写法可正常启动。
//
// 构建运行: cmake -S tools/ptytest -B build/ptytest && cmake --build build/ptytest && 运行
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

using Hpc = void *;
using CreatePseudoConsoleFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, Hpc *);
using ClosePseudoConsoleFn = void(WINAPI *)(Hpc);

static int g_failures = 0;

static void expect(bool cond, const char *what)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        ++g_failures;
    fflush(stdout);
}

static std::wstring pipeNameFor(const wchar_t *tag)
{
    static int seq = 0;
    wchar_t buf[128];
    swprintf(buf, 128, L"\\\\.\\pipe\\ptytest-%u-%d-%s", GetCurrentProcessId(), seq++, tag);
    return buf;
}

// 与 conpty.cpp::setup() 完全一致的创建参数(重叠): drainUntilEof 也用与项目
// 相同的「OVERLAPPED + 事件」读法, 有超时保险丝且能可靠感知 EOF。
// (同步句柄的 ReadFile 无限期阻塞; 重叠句柄 + NULL overlapped 在管道关闭后
//  也会挂起——都不是正确的读法。)
static HANDLE makeServerPipe(const std::wstring &name, DWORD direction)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return CreateNamedPipeW(name.c_str(), direction | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                            1, 64 * 1024, 64 * 1024, 0, &sa);
}

// ---------------------------------------------------------------- Phase A

struct PhaseA
{
    HANDLE server = nullptr;              // 服务端句柄(重叠)
    std::atomic<DWORD> connectResult{};   // ConnectNamedPipe 返回后的 GetLastError
    std::atomic<bool> returned{false};
};

static void phaseA()
{
    printf("== Phase A: legacy order (ConnectNamedPipe before CreateFileW) ==\n");
    PhaseA a;
    const std::wstring name = pipeNameFor(L"in");
    a.server = makeServerPipe(name, PIPE_ACCESS_OUTBOUND);
    if (a.server == INVALID_HANDLE_VALUE) {
        expect(false, "A0 CreateNamedPipeW");
        return;
    }

    std::thread t([&a] {
        // conpty.cpp 旧行为 — ConnectNamedPipe(重叠句柄, nullptr)
        const BOOL ok = ConnectNamedPipe(a.server, nullptr);
        a.connectResult.store(ok ? ERROR_SUCCESS : GetLastError());
        a.returned.store(true); // 若执行到这里说明没有阻塞
    });

    Sleep(3000); // 给它三秒钟机会返回
    const bool blocked = !a.returned.load();
    expect(blocked, "A1 ConnectNamedPipe(nullptr) blocks forever (deadlock confirmed)");
    if (blocked) {
        printf("     => IO thread stuck in ConnectNamedPipe; the CreateFileW(client)\n"
               "        that would satisfy it never runs (it is AFTER this call in\n"
               "        the same thread): self-wait deadlock.\n");
    } else {
        printf("     ConnectNamedPipe returned immediately, GetLastError=%lu\n",
               static_cast<unsigned long>(a.connectResult.load()));
    }
    std::thread killer;
    if (blocked)
        killer = std::thread([&a] {   // 收尾: 关句柄让卡着的线程返回
            Sleep(100);
            CloseHandle(a.server);
        });
    if (t.joinable())
        t.join();
    if (killer.joinable())
        killer.join();
}

// ------------------------------------------------- 可复用的 ConPTY 会话

struct PtySession
{
    HANDLE srvIn = nullptr;    // 服务端: 我们写 -> pty 读 (重叠)
    HANDLE srvOut = nullptr;   // 服务端: pty 写 -> 我们读 (重叠)
    Hpc pty = nullptr;
    HANDLE child = nullptr;
};

static CreatePseudoConsoleFn g_createPty = nullptr;
static ClosePseudoConsoleFn g_closePty = nullptr;

// 新顺序: CreateNamedPipeW -> CreateFileW(client) -> CreatePseudoConsole -> spawn
static bool spawnInPty(PtySession &s, const wchar_t *command)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    const std::wstring inName = pipeNameFor(L"in");
    const std::wstring outName = pipeNameFor(L"out");
    s.srvIn = makeServerPipe(inName, PIPE_ACCESS_OUTBOUND);
    s.srvOut = makeServerPipe(outName, PIPE_ACCESS_INBOUND);
    if (s.srvIn == INVALID_HANDLE_VALUE || s.srvOut == INVALID_HANDLE_VALUE)
        return false;

    // ★ 修复核心: 不再 ConnectNamedPipe, 客户端直接打开
    HANDLE cliIn = CreateFileW(inName.c_str(), GENERIC_READ, 0, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE cliOut = CreateFileW(outName.c_str(), GENERIC_WRITE, 0, &sa,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (cliIn == INVALID_HANDLE_VALUE || cliOut == INVALID_HANDLE_VALUE)
        return false;

    COORD size = {80, 24};
    HRESULT hr = g_createPty(size, cliIn, cliOut, 0, &s.pty);
    CloseHandle(cliIn); // conhost 已复制, 我们必须放手(否则看不到 EOF)
    CloseHandle(cliOut);
    if (FAILED(hr))
        return false;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              s.pty, sizeof(s.pty), nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    std::vector<wchar_t> cmd(command, command + wcslen(command) + 1);
    PROCESS_INFORMATION pi = {};
    const BOOL spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!spawned)
        return false;
    CloseHandle(pi.hThread);
    s.child = pi.hProcess;
    return true;
}

static void teardownPty(PtySession &s, bool terminateChild)
{
    // 复刻 ConPty::shutdownHandles 的顺序
    if (s.srvIn) {
        CloseHandle(s.srvIn);   // 1) 输入写端 EOF, 交互程序通常自己退
        s.srvIn = nullptr;
    }
    if (s.child) {
        if (WaitForSingleObject(s.child, 400) == WAIT_TIMEOUT && terminateChild) {
            TerminateProcess(s.child, 0xE0FF);                       // 2) 400ms 后强杀
            WaitForSingleObject(s.child, 1500);
        }
        CloseHandle(s.child);
        s.child = nullptr;
    }
    if (s.pty) {
        g_closePty(s.pty);      // 3) 关 pty 与读端
        s.pty = nullptr;
    }
    if (s.srvOut) {
        CloseHandle(s.srvOut);
        s.srvOut = nullptr;
    }
}

static char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

static bool contains_ci(const std::string &hay, const char *needle)
{
    std::string h, n;
    for (char c : hay) h += asciiLower(c);
    for (const char *p = needle; *p; ++p) n += asciiLower(*p);
    return h.find(n) != std::string::npos;
}

// 带超时的重叠读: 与 conpty.cpp 主循环相同的「OVERLAPPED + 事件」用法。
// 管道关闭时 GetOverlappedResult 以 BROKEN_PIPE 失败 → EOF; 超时则取消挂起读。
// stopMarker 非空时, 收到该子串即提前返回(不等 EOF)。
static std::string drainUntilEof(HANDLE h, ULONGLONG maxMs, const char *stopMarker = nullptr)
{
    std::string got;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return got;
    char buf[4096];
    const ULONGLONG deadline = GetTickCount64() + maxMs;
    bool pending = false;
    for (;;) {
        if (stopMarker && contains_ci(got, stopMarker))
            break;
        if (!pending) {
            ResetEvent(ov.hEvent);
            DWORD n = 0;
            if (!ReadFile(h, buf, sizeof(buf), &n, &ov)) {
                const DWORD e = GetLastError();
                if (e == ERROR_IO_PENDING) {
                    pending = true; // 下面等它落地
                } else {
                    break; // BROKEN_PIPE / EOF / 断开: 会话结束
                }
            } else {
                if (n == 0)
                    break;
                got.append(buf, n);
                continue;
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        if (WaitForSingleObject(ov.hEvent, DWORD(deadline - now)) != WAIT_OBJECT_0)
            break; // 超时
        pending = false;
        DWORD done = 0;
        if (!GetOverlappedResult(h, &ov, &done, FALSE))
            break; // EOF / 断开
        if (done == 0)
            break;
        got.append(buf, done);
    }
    if (pending) {
        CancelIoEx(h, &ov);
        DWORD done = 0;
        GetOverlappedResult(h, &ov, &done, TRUE);
    }
    CloseHandle(ov.hEvent);
    return got;
}

// ---------------------------------------------------------------- Phase B

static bool phaseB()
{
    printf("== Phase B: new order (no ConnectNamedPipe) + cmd.exe roundtrip ==\n");
    PtySession s;
    if (!spawnInPty(s, L"cmd.exe /c echo ptytest-ok")) {
        expect(false, "B1 spawn in ConPTY (pipes+client+CreatePseudoConsole+process)");
        return false;
    }
    expect(true, "B1 spawn in ConPTY (pipes+client+CreatePseudoConsole+process)");

    const std::string out = drainUntilEof(s.srvOut, 8000);
    expect(out.find("ptytest-ok") != std::string::npos, "B2 cmd output roundtrip");
    teardownPty(s, true);
    return true;
}

// ---------------------------------------------------------------- Phase C

static void phaseC()
{
    printf("== Phase C: real ssh.exe to a refused port (fast failure path) ==\n");
    const wchar_t *ssh = L"C:\\Windows\\System32\\OpenSSH\\ssh.exe";
    if (GetFileAttributesW(ssh) == INVALID_FILE_ATTRIBUTES) {
        printf("     ssh.exe not found, skipped\n");
        return;
    }
    PtySession s;
    const wchar_t *cmd =
        L"\"C:\\Windows\\System32\\OpenSSH\\ssh.exe\" -tt -p 1 "
        L"-o ConnectTimeout=3 -o StrictHostKeyChecking=no -o BatchMode=yes "
        L"127.0.0.1 exit";
    const ULONGLONG t0 = GetTickCount64();
    if (!spawnInPty(s, cmd)) {
        expect(false, "C1 spawn ssh.exe in ConPTY");
        return;
    }
    const ULONGLONG launchedMs = GetTickCount64() - t0;
    expect(launchedMs < 1000, "C1 spawn ssh.exe in ConPTY (launched < 1s)");

    // 读到错误文本即返回(用户视角: 错误已上屏), 之后分段计时收尾链路
    const std::string out = drainUntilEof(s.srvOut, 10000, "refused");
    const ULONGLONG textMs = GetTickCount64() - t0;
    expect(contains_ci(out, "refused"), "C2 ssh error text reached the terminal");
    printf("     error text on screen after %.1fs, tail:\n", double(textMs) / 1000.0);
    const size_t tail = out.size() > 120 ? out.size() - 120 : 0;
    fwrite(out.data() + tail, 1, out.size() - tail, stdout);
    printf("\n");
    expect(textMs < 4000, "C3 error visible quickly (under 4s)");

    // ssh.exe 进程退出用时(不依赖管道 EOF)
    const DWORD wr = WaitForSingleObject(s.child, 12000);
    const ULONGLONG exitMs = GetTickCount64() - t0;
    expect(wr == WAIT_OBJECT_0, "C4 ssh.exe exited after failure");
    printf("     ssh.exe exited after %.1fs\n", double(exitMs) / 1000.0);

    // 模拟 Xterm 新逻辑: 进程退出即开 800ms 收割窗口读完剩余输出, 然后收尾
    // (conhost 关管道滞后 >6s, 不能作为会话结束依据)
    const std::string rest = drainUntilEof(s.srvOut, 800);
    const ULONGLONG cycleMs = GetTickCount64() - t0;
    printf("     drain window got %zu extra bytes; cycle (spawn..drained) %.1fs\n",
           rest.size(), double(cycleMs) / 1000.0);
    const ULONGLONG tTear = GetTickCount64();
    teardownPty(s, true);
    const ULONGLONG teardown2Ms = GetTickCount64() - tTear;
    printf("     final teardown %lums\n", static_cast<unsigned long>(teardown2Ms));
    expect(cycleMs + teardown2Ms < 5000,
           "C5 full cycle under Xterm semantics (exit+drain+teardown) under 5s");
}

// ---------------------------------------------------------------- Phase D

static void phaseD()
{
    printf("== Phase D: cancel path (unreachable host, stop while connecting) ==\n");
    const wchar_t *cmd =
        L"\"C:\\Windows\\System32\\OpenSSH\\ssh.exe\" -tt -p 22 "
        L"-o ConnectTimeout=15 -o StrictHostKeyChecking=no -o BatchMode=yes "
        L"10.255.255.1 exit";
    PtySession s;
    if (!spawnInPty(s, cmd)) {
        expect(false, "D1 spawn ssh.exe (to unreachable host)");
        return;
    }
    expect(true, "D1 spawn ssh.exe (to unreachable host)");

    Sleep(800); // 模拟用户在「正在连接」期间点了取消
    const ULONGLONG t0 = GetTickCount64();
    teardownPty(s, true);
    const ULONGLONG teardownMs = GetTickCount64() - t0;
    expect(teardownMs < 3000, "D2 cancel teardown under 3s (no endless 'disconnecting')");
    printf("     teardown took %lums\n", static_cast<unsigned long>(teardownMs));
}

// ---------------------------------------------------------------- Phase E

// 复刻 Xterm 旧写法: si.StartupInfo.cb = sizeof(STARTUPINFOW) 而非 EXW。
// EXTENDED_STARTUPINFO_PRESENT 下 CreateProcessW 拒绝该尺寸 → Win32 错误 87。
static void phaseE()
{
    printf("== Phase E: STARTUPINFOEXW.cb size (Win32 error 87 repro) ==\n");
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const std::wstring inName = pipeNameFor(L"in");
    const std::wstring outName = pipeNameFor(L"out");
    HANDLE srvIn = makeServerPipe(inName, PIPE_ACCESS_OUTBOUND);
    HANDLE srvOut = makeServerPipe(outName, PIPE_ACCESS_INBOUND);
    HANDLE cliIn = CreateFileW(inName.c_str(), GENERIC_READ, 0, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE cliOut = CreateFileW(outName.c_str(), GENERIC_WRITE, 0, &sa,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    COORD size = {80, 24};
    Hpc pty = nullptr;
    if (srvIn == INVALID_HANDLE_VALUE || srvOut == INVALID_HANDLE_VALUE
        || cliIn == INVALID_HANDLE_VALUE || cliOut == INVALID_HANDLE_VALUE
        || FAILED(g_createPty(size, cliIn, cliOut, 0, &pty))) {
        expect(false, "E0 prepare pipes + pty");
        return;
    }
    CloseHandle(cliIn);
    CloseHandle(cliOut);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              pty, sizeof(pty), nullptr, nullptr);

    // 旧写法: 整个 STARTUPINFOW 拷进去, cb 变成 sizeof(STARTUPINFOW)
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    STARTUPINFOEXW siBad = {};
    siBad.StartupInfo = startup;
    siBad.lpAttributeList = attrs;

    wchar_t cmd[] = L"cmd.exe /c exit";
    PROCESS_INFORMATION pi = {};
    const BOOL badOk = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
                                      EXTENDED_STARTUPINFO_PRESENT
                                          | CREATE_UNICODE_ENVIRONMENT,
                                      nullptr, nullptr, &siBad.StartupInfo, &pi);
    const DWORD badErr = badOk ? ERROR_SUCCESS : GetLastError();
    expect(!badOk && badErr == ERROR_INVALID_PARAMETER,
           "E1 legacy cb (sizeof STARTUPINFOW) rejected with error 87");
    printf("     legacy spawn -> ok=%d GetLastError=%lu\n", badOk,
           static_cast<unsigned long>(badErr));
    if (badOk) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    // 正确写法已在 spawnInPty(Phase B/C/D) 验证; 这里单点复验 cb=EXW 可用
    STARTUPINFOEXW siGood = {};
    siGood.StartupInfo.cb = sizeof(siGood);
    siGood.lpAttributeList = attrs;
    const BOOL goodOk = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
                                       EXTENDED_STARTUPINFO_PRESENT
                                           | CREATE_UNICODE_ENVIRONMENT,
                                       nullptr, nullptr, &siGood.StartupInfo, &pi);
    expect(goodOk, "E2 cb = sizeof(STARTUPINFOEXW) spawns fine");
    if (goodOk) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    DeleteProcThreadAttributeList(attrs);
    g_closePty(pty);
    CloseHandle(srvIn);
    CloseHandle(srvOut);
}

int main()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    g_createPty = reinterpret_cast<CreatePseudoConsoleFn>(
        GetProcAddress(k32, "CreatePseudoConsole"));
    g_closePty = reinterpret_cast<ClosePseudoConsoleFn>(
        GetProcAddress(k32, "ClosePseudoConsole"));
    if (!g_createPty || !g_closePty) {
        printf("[FAIL] ConPTY API not available (Win10 1809+ required)\n");
        return 1;
    }

    phaseA();
    bool ok = true;
    ok = phaseB() && ok;
    phaseC();
    phaseD();
    phaseE();
    printf("\n%s\n", (g_failures == 0) ? "ALL PASS" : "HAS FAILURES");
    return (g_failures == 0 && ok) ? 0 : 1;
}
