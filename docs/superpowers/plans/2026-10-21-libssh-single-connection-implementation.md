# Xterm Single-Connection libssh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `ssh.exe`/ConPTY and the two extra libssh2 sessions with one in-process libssh connection that carries the interactive shell, SFTP, and host statistics.

**Architecture:** `SshSession` remains the GUI-facing `Transport`, while one `SshWorker` thread exclusively owns a non-blocking `ssh_session`. Shell, SFTP AIO, and stats exec channels are cooperatively scheduled on that thread; GUI adapters preserve the current `SftpClient` and `HostStats` signal contracts.

**Tech Stack:** C++17, Qt 6.8.3 Widgets/Core/Test, libssh 0.12.2, OpenSSL 3.6.5, CMake 3.19+, vcpkg pinned at `7bb3ef7c1aebcb18a9ad7f5d7c6e3ea3be006196`, Windows 10/11, LLVM-MinGW 17.0.6.

## Global Constraints

- Target only Windows 10/11 with Qt 6.8.3 and the `llvm-mingw_64` kit.
- Do not add hardware-accelerated terminal rendering in this migration.
- Every libssh handle is created, used, and destroyed on the `SshWorker` thread.
- Shell bytes pass through unchanged; the SSH layer must not parse VT sequences or password text.
- Use one SSH connection only. Never open a hidden fallback connection for SFTP or stats.
- Use application-private OpenSSH-format known_hosts with TOFU; changed keys require confirmation.
- Keep the GUI thread free of blocking network calls.
- Preserve unrelated existing working-tree changes; stage and commit only files named by each task.
- Each task must compile and pass its focused tests before its commit.

---

## File Structure

New core files:

- `ssh/sshtypes.h`: shared errors, prompts, lifecycle phases, request IDs, and Qt metatypes.
- `ssh/knownhostsstore.h/.cpp`: private known_hosts path, legacy fingerprint migration, and atomic file handling.
- `ssh/libsshconnection.h/.cpp`: libssh session options, handshake, host verification, authentication, keepalive, and disconnect.
- `ssh/shellchannel.h/.cpp`: PTY/shell lifecycle and raw input/output.
- `ssh/sftpchannel.h/.cpp`: metadata requests and AIO transfer state machines.
- `ssh/statschannel.h/.cpp`: long-lived stats exec channel and retry state.
- `ssh/sshworker.h/.cpp`: sole libssh owner and fair event-loop scheduler.

Adapters and integration:

- `sshsession.h/.cpp`: GUI-thread `Transport` facade over `SshWorker`.
- `sftpclient.h/.cpp`: GUI adapter over the shared session.
- `hoststats.h/.cpp`: GUI adapter and `/proc` parser over the shared session.
- `mainwindow.h/.cpp`: structured host-key and authentication prompts; shared adapters.
- `filebrowserdialog.h/.cpp`: consume the connected session instead of a target that creates another session.

Build and tests:

- `vcpkg.json`: pinned dependency manifest.
- `scripts/bootstrap-ssh-deps.ps1`: reproducible dynamic dependency bootstrap.
- `cmake/DeploySshRuntime.cmake`: copy dependency DLLs beside each executable.
- `tests/CMakeLists.txt` and focused Qt Test executables.
- `tools/sshtest`: real-host shell integration test.
- `tools/sftptest`: real-host shared-session SFTP integration test.

---

### Task 1: Reproducible libssh 0.12.2 Dynamic Dependency

**Files:**
- Create: `vcpkg.json`
- Create: `scripts/bootstrap-ssh-deps.ps1`
- Create: `cmake/DeploySshRuntime.cmake`
- Create: `tools/libsshprobe/CMakeLists.txt`
- Create: `tools/libsshprobe/main.cpp`
- Modify: `.gitignore`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces CMake imported target `ssh`.
- Produces runtime directory `${XTERM_SSH_DEPS_ROOT}/bin`.
- Produces `xterm_deploy_ssh_runtime(target)` for application and test targets.

- [ ] **Step 1: Add the failing configure probe**

Create `tools/libsshprobe/main.cpp`:

```cpp
#include <libssh/libssh.h>
#include <cstdio>

int main()
{
    const char *version = ssh_version(0);
    if (!version) {
        std::fputs("ssh_version returned null\n", stderr);
        return 1;
    }
    std::printf("%s\n", version);
    return 0;
}
```

Create its CMake file with `find_package(libssh CONFIG REQUIRED)`, link `ssh`, and call the deployment helper.

- [ ] **Step 2: Verify dependency discovery currently fails**

Run:

```powershell
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' -S tools/libsshprobe -B build/libsshprobe `
  -G 'MinGW Makefiles' `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER='D:/Qt6/Tools/llvm-mingw1706_64/bin/clang.exe' `
  -DCMAKE_CXX_COMPILER='D:/Qt6/Tools/llvm-mingw1706_64/bin/clang++.exe'
```

Expected: configure fails because `libssh-config.cmake` is absent.

- [ ] **Step 3: Add a pinned vcpkg manifest and bootstrap script**

Use this manifest:

```json
{
  "builtin-baseline": "7bb3ef7c1aebcb18a9ad7f5d7c6e3ea3be006196",
  "dependencies": [
    {
      "name": "libssh",
      "default-features": false
    }
  ],
  "overrides": [
    {
      "name": "libssh",
      "version": "0.12.2"
    },
    {
      "name": "openssl",
      "version": "3.6.5",
      "port-version": 1
    }
  ]
}
```

The PowerShell script must:

1. Clone vcpkg into `.deps/vcpkg` at the pinned commit.
2. Copy that commit's `ports/libssh` to `.deps/overlay-ports/libssh`.
3. Change the overlay version to `0.12.2`.
4. Change the tarball SHA512 to `92b95d64772906fc0fe497fed4dc34a160f2397f71ef3871dc1ea0fe1e8e3c00df699ed0efdc0b754feb23e2140bda17be41d16aee48ff715487384023fdd467`.
5. Bootstrap vcpkg.
6. Install with `x64-mingw-dynamic`, `--overlay-ports`, and install root `.deps/vcpkg_installed`.
7. Exit nonzero if neither `libssh.dll` nor `ssh.dll` exists, or if
   `libcrypto-3-x64.dll` or the libssh CMake package is missing.

Ignore `.deps/` in `.gitignore`.

- [ ] **Step 4: Add CMake discovery and DLL deployment**

Define:

```cmake
set(XTERM_SSH_DEPS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vcpkg_installed/x64-mingw-dynamic"
    CACHE PATH "Root of dynamic libssh/OpenSSL dependencies")
list(PREPEND CMAKE_PREFIX_PATH "${XTERM_SSH_DEPS_ROOT}")
find_package(libssh CONFIG REQUIRED)

function(xterm_deploy_ssh_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            -DSOURCE_DIR="${XTERM_SSH_DEPS_ROOT}/bin"
            -DDEST_DIR="$<TARGET_FILE_DIR:${target}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeploySshRuntime.cmake"
        VERBATIM)
endfunction()
```

`DeploySshRuntime.cmake` must copy every `*.dll` from the dependency `bin` directory with
`copy_if_different`; it must accept either MinGW's `libssh.dll` or `ssh.dll`, and fail if
no libssh runtime or no `libcrypto-3-x64.dll` is present.

- [ ] **Step 5: Bootstrap, build, and run the probe**

Run:

```powershell
& scripts/bootstrap-ssh-deps.ps1
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' -S tools/libsshprobe -B build/libsshprobe `
  -G 'MinGW Makefiles' `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER='D:/Qt6/Tools/llvm-mingw1706_64/bin/clang.exe' `
  -DCMAKE_CXX_COMPILER='D:/Qt6/Tools/llvm-mingw1706_64/bin/clang++.exe' `
  -DXTERM_SSH_DEPS_ROOT="$PWD/.deps/vcpkg_installed/x64-mingw-dynamic"
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' --build build/libsshprobe -j 8
& build/libsshprobe/libsshprobe.exe
```

Expected: output contains `0.12.2`; the executable directory contains a libssh runtime DLL
and the OpenSSL DLLs.

- [ ] **Step 6: Commit**

```powershell
git add .gitignore vcpkg.json scripts/bootstrap-ssh-deps.ps1 `
  cmake/DeploySshRuntime.cmake tools/libsshprobe CMakeLists.txt
git commit -m "build: add pinned dynamic libssh dependencies"
```

---

### Task 2: Shared SSH Types and Private known_hosts Migration

**Files:**
- Create: `ssh/sshtypes.h`
- Create: `ssh/knownhostsstore.h`
- Create: `ssh/knownhostsstore.cpp`
- Create: `tests/CMakeLists.txt`
- Create: `tests/test_knownhosts.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces `SshError`, `SshPhase`, `SshAuthPrompt`, `SshHostKeyChange`, and `SshRequestId`.
- Produces `KnownHostsStore::path()`, `legacyPath()`, `prepareMigration()`, `legacyFingerprint()`, and `rememberLegacyMatch()`.

- [ ] **Step 1: Write failing known_hosts tests**

Tests must cover:

```cpp
void migratesLegacyFileWithoutLosingIt();
void readsLegacySha256ForHostAndPort();
void ignoresMalformedLegacyRows();
void standardKnownHostsPathStaysInsideAppConfig();
void atomicRewritePreservesUnrelatedHosts();
```

Use `QTemporaryDir` and an injectable root directory. Verify the old `host:port sha256hex` file is renamed to `known_hosts.legacy`, while the new `known_hosts` starts as an OpenSSH-format file.

- [ ] **Step 2: Run the test and verify failure**

Run:

```powershell
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' -S . -B build/ssh-debug `
  -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH='D:/Qt6/6.8.3/llvm-mingw_64' `
  -DXTERM_SSH_DEPS_ROOT="$PWD/.deps/vcpkg_installed/x64-mingw-dynamic" `
  -DBUILD_TESTING=ON
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' --build build/ssh-debug --target test_knownhosts -j 8
```

Expected: compilation fails because `KnownHostsStore` does not exist.

- [ ] **Step 3: Implement types and migration**

Use the following public shapes:

```cpp
using SshRequestId = quint64;

struct SshError {
    enum class Domain { None, Configuration, Network, HostKey, Authentication,
                        Shell, Sftp, Stats, Cancelled, Internal };
    Domain domain = Domain::None;
    int code = 0;
    QString message;
    bool retryable = false;
    bool isError() const { return domain != Domain::None; }
};

enum class SshPhase {
    Idle, ResolvingConfig, Connecting, VerifyingHost, Authenticating,
    OpeningShell, Ready, Closing
};

struct SshAuthPrompt {
    QString text;
    bool echo = false;
};
```

Register every cross-thread type with `Q_DECLARE_METATYPE`.

`KnownHostsStore` must never synthesize an OpenSSH host-key line from a fingerprint. It only preserves and queries legacy fingerprints; `LibsshConnection` writes full keys through libssh after a verified match.

- [ ] **Step 4: Run focused tests**

Run:

```powershell
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' --build build/ssh-debug --target test_knownhosts -j 8
& build/ssh-debug/tests/test_knownhosts.exe
```

Expected: all test slots pass.

- [ ] **Step 5: Commit**

```powershell
git add ssh/sshtypes.h ssh/knownhostsstore.h ssh/knownhostsstore.cpp `
  tests/CMakeLists.txt tests/test_knownhosts.cpp CMakeLists.txt
git commit -m "feat: add SSH types and known-host migration"
```

---

### Task 3: Non-Blocking libssh Connection and Authentication State Machine

**Files:**
- Create: `ssh/libsshconnection.h`
- Create: `ssh/libsshconnection.cpp`
- Create: `ssh/libsshapi.h`
- Create: `ssh/libsshapi.cpp`
- Create: `tests/fakes/fakelibsshapi.h`
- Create: `tests/test_libsshconnection.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes `SshTarget`, `KnownHostsStore`, and types from Task 2.
- Produces `LibsshConnection::begin()`, `advance()`, `submitHostKeyDecision()`, `submitAuthAnswers()`, `sendKeepalive()`, and `close()`.
- Exposes the connected `ssh_session` only to `SshWorker` on the owner thread.

- [ ] **Step 1: Write failing state-machine tests**

Cover these exact transitions:

```cpp
void explicitUiOptionsOverrideParsedConfig();
void unknownHostUsesTofuAndWritesFullKey();
void matchingLegacyFingerprintMigratesToFullKey();
void changedHostKeyWaitsForDecision();
void rejectedChangedKeyDisconnects();
void triesAgentThenExplicitKeyThenAutoKeyThenKeyboardInteractiveThenPassword();
void keyboardInteractivePublishesAllPromptsAndResumes();
void cancellationCompletesOnce();
void keepaliveFailureBecomesRetryableNetworkError();
```

The fake API must script return values including `SSH_AGAIN`, `SSH_AUTH_AGAIN`, prompt arrays, and host-key states without opening a socket.

- [ ] **Step 2: Run tests and verify failure**

Expected: compilation fails because `LibsshConnection` and `LibsshApi` are absent.

- [ ] **Step 3: Implement the injectable API boundary**

Wrap only libssh calls used by the project. Do not expose raw calls throughout the codebase. The interface must include session allocation/free, option setting/config parsing, connect, known-host checks/update, key export/hash, agent/public-key/keyboard-interactive/password auth, keepalive, disconnect, and error access.

Production `LibsshApi` delegates directly to libssh 0.12.2. Tests inject `FakeLibsshApi`.

- [ ] **Step 4: Implement connection phases**

Rules:

- Set host alias first, then parse default config.
- Apply non-empty UI user/private-key and valid UI port/timeout as explicit overrides.
- Set `SSH_OPTIONS_PROCESS_CONFIG` and `SSH_OPTIONS_KNOWNHOSTS` to the application-private path.
- Set non-blocking mode before connect.
- Return control on `SSH_AGAIN`; never spin.
- TOFU writes unknown keys automatically.
- Changed keys emit old/new SHA-256 and wait for a request-ID-matched answer.
- Authentication order is agent/public-key, explicit identity, automatic identity, keyboard-interactive, then password.
- Clear copied password/auth-answer buffers after success or terminal failure.

- [ ] **Step 5: Run focused tests**

Run `test_libsshconnection.exe`; expected all tests pass and no test exceeds one second.

- [ ] **Step 6: Commit**

```powershell
git add ssh/libsshapi.* ssh/libsshconnection.* tests/fakes/fakelibsshapi.h `
  tests/test_libsshconnection.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add non-blocking libssh connection state machine"
```

---

### Task 4: Raw PTY/Shell Channel and Fair Worker Scheduler

**Files:**
- Create: `ssh/shellchannel.h`
- Create: `ssh/shellchannel.cpp`
- Create: `ssh/sshworker.h`
- Create: `ssh/sshworker.cpp`
- Create: `tests/test_sshworker.cpp`
- Modify: `ssh/libsshapi.h`
- Modify: `ssh/libsshapi.cpp`
- Modify: `tests/fakes/fakelibsshapi.h`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces worker slots `connectToHost(generation,target,cols,rows)`, `writeShell(generation,data)`, `resizeShell(generation,cols,rows)`, `closeConnection(generation)`.
- Produces signals `connected`, `shellData`, `phaseChanged`, `hostKeyDecisionRequired`, `authAnswersRequired`, `connectionFailed`, and `connectionClosed`.

- [ ] **Step 1: Write failing worker tests**

Cover:

```cpp
void requestsXtermPtyWithInitialSize();
void forwardsShellBytesWithoutModification();
void handlesPartialWritesWithoutDroppingInput();
void coalescesResizeAndSendsNewestDimensions();
void shellEofClosesConnectionExactlyOnce();
void staleGenerationCommandsAreIgnored();
void closeDuringConnectDoesNotEmitConnected();
void schedulerReturnsToQtEventLoopOnSshAgain();
```

Include a byte-for-byte assertion containing `ESC[2J`, OSC title, UTF-8, NUL, and split CSI sequences.

- [ ] **Step 2: Verify tests fail**

Expected: missing `ShellChannel` and `SshWorker`.

- [ ] **Step 3: Implement `ShellChannel`**

Request `xterm` PTY with current cols/rows, set `TERM=xterm` and `COLORTERM=truecolor`, then request shell. Maintain bounded pending input with partial-write offsets. Read stdout and stderr non-blockingly into separate buffers but forward both unchanged to the terminal in arrival order available from libssh.

- [ ] **Step 4: Implement `SshWorker` scheduling**

Use a zero/short single-shot `QTimer` to call `advance()`; one tick has bounded operation counts. Assert owner thread in every method touching libssh. Emit GUI-facing events only with generation IDs.

- [ ] **Step 5: Run worker tests and full build**

Run:

```powershell
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' --build build/ssh-debug --target test_sshworker Xterm -j 8
& build/ssh-debug/tests/test_sshworker.exe
```

Expected: tests pass; application links with libssh DLL imports.

- [ ] **Step 6: Commit**

```powershell
git add ssh/shellchannel.* ssh/sshworker.* ssh/libsshapi.* `
  tests/fakes/fakelibsshapi.h tests/test_sshworker.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add raw shell channel and SSH scheduler"
```

---

### Task 5: Replace `SshSession` and Wire Structured GUI Prompts

**Files:**
- Modify: `sshsession.h`
- Modify: `sshsession.cpp`
- Modify: `mainwindow.h`
- Modify: `mainwindow.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tools/sshtest/CMakeLists.txt`
- Modify: `tools/sshtest/main.cpp`

**Interfaces:**
- Preserves `Transport`, `open`, `close`, `send`, `setPtySize`, `target`, `summary`, and `sessionEnded`.
- Replaces scraped prompt signals with `hostKeyDecisionRequired(...)`, `authAnswersRequired(...)`, `answerHostKey(...)`, and `answerAuthentication(...)`.

- [ ] **Step 1: Add/adjust facade tests**

Add tests proving:

- `open()` returns immediately and enters `Connecting`.
- worker `connected` maps to exactly one `opened`.
- raw worker bytes map unchanged to `received`.
- close is idempotent.
- reopening waits for the old generation to close.
- stale worker events cannot alter the new session.

- [ ] **Step 2: Replace ConPTY internals**

`SshSession` creates `SshWorker`, moves it to a private `QThread`, and connects every command/event with queued connections. `availabilityError()` checks that libssh initialized successfully rather than searching for `ssh.exe` or ConPTY.

Delete `onOutputReceived`, prompt-tail fields, argument construction, executable discovery, and every password/yes-no scraper.

- [ ] **Step 3: Replace MainWindow prompt handling**

For changed host keys, show host, old SHA-256, and new SHA-256; default button is Reject. For structured authentication, show each prompt using password echo mode when `echo == false`, and return answers with the request ID. Closing a prompt rejects/cancels that request.

Do not show a dialog for first-use TOFU; append one informational terminal note that the new key was recorded.

- [ ] **Step 4: Rewrite real-host `sshtest`**

Read target fields from environment variables, connect through `SshSession`, send:

```sh
printf '\033[2JXTERM_RAW_MARKER\n'; stty size; exit
```

Assert raw output contains the clear sequence and marker, the reported rows/cols match the requested size, and the session closes normally.

- [ ] **Step 5: Build and run terminal validation**

Run unit tests, then:

```powershell
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' --build build/ssh-debug --target Xterm sshtest -j 8
& build/ssh-debug/tools/sshtest/sshtest.exe
```

Expected with configured environment: login succeeds, marker and dimensions pass. Manually verify vim, top, clear, UTF-8, resize, disconnect, and cancellation.

- [ ] **Step 6: Remove ConPTY from production target and commit**

Keep source files temporarily for old diagnostic tools, but remove `conpty.cpp/.h` from `Xterm`.

```powershell
git add sshsession.* mainwindow.* CMakeLists.txt tools/sshtest
git commit -m "feat: replace SSH terminal with in-process libssh"
```

---

### Task 6: Shared SFTP Channel with AIO Transfers

**Files:**
- Create: `ssh/sftprequest.h`
- Create: `ssh/sftpchannel.h`
- Create: `ssh/sftpchannel.cpp`
- Create: `tests/test_sftpchannel.cpp`
- Modify: `ssh/sshworker.h`
- Modify: `ssh/sshworker.cpp`
- Modify: `sshsession.h`
- Modify: `sshsession.cpp`
- Modify: `ssh/libsshapi.h`
- Modify: `ssh/libsshapi.cpp`
- Modify: `tests/fakes/fakelibsshapi.h`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces request methods matching all current `SftpClient` operations.
- Produces generation/request-ID-tagged results, progress, and one terminal completion per request.

- [ ] **Step 1: Write failing SFTP state-machine tests**

Cover list/realpath, upload, download, mkdir, create, delete, rename, recursive copy, chmod, exec, partial AIO, `SSH_AGAIN`, cancellation, channel rebuild, connection loss, and exactly-once completion.

- [ ] **Step 2: Verify tests fail**

Expected: missing `SftpChannel`.

- [ ] **Step 3: Implement metadata operations and lazy channel creation**

Create SFTP only on first request. Queue operations; do not create a second SSH session. On channel failure, fail the active operation, free SFTP state, and allow exactly one lazy rebuild for the next operation.

- [ ] **Step 4: Implement bounded AIO transfers**

Use `sftp_aio_begin_read/write`, `sftp_aio_wait_read/write`, and `sftp_aio_free`. Limit in-flight requests and per-worker-tick bytes. Preserve download atomicity with a temporary local file followed by rename; cancellation removes the temporary file.

- [ ] **Step 5: Add worker/facade forwarding and run tests**

Assert a sustained fake transfer cannot prevent shell reads from being serviced every scheduler tick.

- [ ] **Step 6: Commit**

```powershell
git add ssh/sftprequest.h ssh/sftpchannel.* ssh/sshworker.* ssh/libsshapi.* `
  sshsession.* tests/test_sftpchannel.cpp tests/fakes/fakelibsshapi.h `
  tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add shared asynchronous SFTP channel"
```

---

### Task 7: Convert File Browser to the Shared Session

**Files:**
- Modify: `sftpclient.h`
- Modify: `sftpclient.cpp`
- Modify: `filebrowserdialog.h`
- Modify: `filebrowserdialog.cpp`
- Modify: `mainwindow.h`
- Modify: `mainwindow.cpp`
- Modify: `tools/sftptest/CMakeLists.txt`
- Modify: `tools/sftptest/main.cpp`

**Interfaces:**
- `SftpClient(SshSession *session, QObject *parent = nullptr)` becomes a GUI adapter.
- `FileBrowserDialog(SshSession *session, QWidget *parent)` requires an already connected session.

- [ ] **Step 1: Add adapter tests**

Verify each existing public slot forwards one request and each session result maps to the existing signal. Verify deleting the dialog cancels only its requests and never closes the shell.

- [ ] **Step 2: Replace libssh2 implementation**

Remove the `QThread`, socket, libssh2 handles, host-key prompter, and independent `connectToHost` handshake. Keep UI-facing signals to minimize dialog churn.

- [ ] **Step 3: Update dialog and MainWindow construction**

The Files button is enabled only while `SshSession` is `Connected`. Opening the dialog creates an adapter attached to `m_ssh`; closing it cancels outstanding file requests.

- [ ] **Step 4: Rewrite and run `sftptest`**

Connect one `SshSession`, open SFTP through it, list home, upload a generated file, download it, compare SHA-256, rename, chmod, and delete. During transfer, send shell marker commands and assert responses continue.

- [ ] **Step 5: Commit**

```powershell
git add sftpclient.* filebrowserdialog.* mainwindow.* tools/sftptest
git commit -m "feat: move file browser onto shared SSH session"
```

---

### Task 8: Move Host Statistics onto a Shared Exec Channel

**Files:**
- Create: `ssh/statschannel.h`
- Create: `ssh/statschannel.cpp`
- Create: `tests/test_statschannel.cpp`
- Modify: `hoststats.h`
- Modify: `hoststats.cpp`
- Modify: `ssh/sshworker.h`
- Modify: `ssh/sshworker.cpp`
- Modify: `sshsession.h`
- Modify: `sshsession.cpp`
- Modify: `mainwindow.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `HostStats(SshSession *session, QObject *parent = nullptr)` becomes a GUI adapter/parser.
- `StatsChannel` produces framed samples and degraded/recovered state.

- [ ] **Step 1: Extract and test `/proc` parsing**

Test normal Linux output, missing `MemAvailable`, malformed counters, counter reset, split frame boundaries, and multiple frames in one read.

- [ ] **Step 2: Write channel retry tests**

Verify shell remains ready when stats open/read fails, retries use bounded exponential delays up to 30 seconds, and stopping stats cancels retries.

- [ ] **Step 3: Implement a long-lived framed exec channel**

Use a remote loop that prints an unambiguous begin/end token around `/proc/stat` and `/proc/meminfo`, then sleeps three seconds. Parse only the stats channel. Never inject commands into shell.

- [ ] **Step 4: Replace independent HostStats session**

Remove socket, QThread, libssh2, duplicate authentication, and duplicate known-host code. MainWindow constructs `HostStats(m_ssh, this)`.

- [ ] **Step 5: Run tests and real-host soak**

Keep shell, stats, and a transfer active for at least five minutes. Expected: shell remains interactive, status updates, and no stats bytes appear in terminal output.

- [ ] **Step 6: Commit**

```powershell
git add ssh/statschannel.* hoststats.* ssh/sshworker.* sshsession.* `
  mainwindow.cpp tests/test_statschannel.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: share SSH connection with host statistics"
```

---

### Task 9: Remove Legacy Backends and Complete Release Verification

**Files:**
- Delete: `conpty.cpp`
- Delete: `conpty.h`
- Delete: `third_party/libssh2/**`
- Delete or rewrite: `tools/ptytest/**`
- Modify: `CMakeLists.txt`
- Modify: diagnostic tool CMake files that reference ConPTY/libssh2
- Create: `THIRD_PARTY_NOTICES.md`
- Modify: `.gitignore`
- Modify: comments in `sshtarget.h`, `mainwindow.h`, and related files

**Interfaces:**
- Final production and tools contain no `ssh.exe`, ConPTY, or libssh2 dependency.

- [ ] **Step 1: Prove no production references remain**

Run:

```powershell
rg -n "ConPty|CreatePseudoConsole|ssh\\.exe|LIBSSH2_|libssh2" `
  --glob '!docs/**' --glob '!build/**' .
```

Expected before cleanup: only legacy files/tools and stale comments match.

- [ ] **Step 2: Delete/rewrite legacy code**

Remove old backends and update diagnostics to call the new session. Remove ConPTY-specific device-query workarounds only after terminal replay and real-host tests prove they are no longer required for serial behavior.

- [ ] **Step 3: Add dependency notices and install deployment**

Document libssh LGPL-2.1 and OpenSSL Apache-2.0 licenses, versions, project URLs, and source acquisition instructions. Ensure `cmake --install` includes all required DLLs and notices.

- [ ] **Step 4: Run complete verification**

Run:

```powershell
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' --build build/ssh-debug -j 8
& 'D:/Qt6/Tools/CMake_64/bin/ctest.exe' --test-dir build/ssh-debug `
  --output-on-failure
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' -S . -B build/ssh-release `
  -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH='D:/Qt6/6.8.3/llvm-mingw_64' `
  -DXTERM_SSH_DEPS_ROOT="$PWD/.deps/vcpkg_installed/x64-mingw-dynamic"
& 'D:/Qt6/Tools/CMake_64/bin/cmake.exe' --build build/ssh-release -j 8
```

Expected: all tests pass; Debug and Release executables start with DLLs deployed.

Repeat the real-host shell/SFTP/stats matrix and active-transfer disconnect test.

- [ ] **Step 5: Check source tree and commit**

Expected final search: no production match for ConPTY, `ssh.exe`, or libssh2.

```powershell
git add -A -- conpty.cpp conpty.h third_party/libssh2 tools CMakeLists.txt `
  THIRD_PARTY_NOTICES.md .gitignore sshtarget.h mainwindow.h
git commit -m "chore: remove legacy SSH backends"
```

---

## Final Acceptance Checklist

- [ ] One network connection appears for shell, SFTP, and stats.
- [ ] Password, keyboard-interactive, key, agent, config alias, and ProxyJump paths are exercised.
- [ ] First host use records a private OpenSSH-format key; a changed key blocks for confirmation.
- [ ] Shell output preserves VT bytes and supports vim/top/clear/UTF-8/resize.
- [ ] Large transfer remains cancellable and does not starve terminal interaction.
- [ ] SFTP/stats channel failure does not disconnect shell.
- [ ] Closing during connect or transfer does not freeze the GUI.
- [ ] Debug and Release builds pass all tests and deploy required DLLs/licenses.
- [ ] Hardware rendering remains unchanged.
