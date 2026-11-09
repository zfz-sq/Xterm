# Xterm Windows 打包脚本: 生成可独立运行的 install 目录。
# 用法: powershell -File scripts/package.ps1 [-BuildDir build/ssh-release]
# 步骤: cmake --install 刷新 exe/第三方 → 拷 Qt DLL + libc++ + 插件 → 拷 MCP 工具。

param(
    [string]$BuildDir = "build/ssh-release",
    [string]$QtBin = "D:/Qt6/6.8.3/llvm-mingw_64/bin",
    [string]$QtPlugins = "D:/Qt6/6.8.3/llvm-mingw_64/plugins"
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path $PSScriptRoot -Parent
$Install = Join-Path $Repo "$BuildDir/install"
$Bin = Join-Path $Install "bin"

# 1. 刷新 exe 与第三方运行库（直接从构建目录拷贝，避免 cmake install 前缀权限问题）
Write-Host "== exe & third-party =="
Copy-Item (Join-Path $Repo "$BuildDir/Xterm.exe") -Destination $Bin -Force
foreach ($dll in "legacy.dll", "libssh.dll", "libcrypto-3-x64.dll",
                 "libssl-3-x64.dll", "libwinpthread-1.dll") {
    $src = Join-Path $Repo "$BuildDir/$dll"
    if (Test-Path $src) {
        Copy-Item $src -Destination $Bin -Force
    }
}

# 2. Qt DLL 与 llvm-mingw 运行库（libunwind 是 libc++ 的 unwind 依赖，缺了弹“找不到 libunwind.dll”）
Write-Host "== Qt DLLs =="
foreach ($dll in "Qt6Core.dll", "Qt6Gui.dll", "Qt6Network.dll", "Qt6SerialPort.dll",
                 "Qt6Widgets.dll", "libc++.dll", "libunwind.dll") {
    Copy-Item (Join-Path $QtBin $dll) -Destination $Bin -Force
}

# 3. 插件（只拷必需的子集）
Write-Host "== Qt plugins =="
foreach ($pair in @(
    @{ src = "platforms";            dst = "platforms" },
    @{ src = "styles";               dst = "styles" },
    @{ src = "imageformats";         dst = "imageformats" },
    @{ src = "tls";                  dst = "tls" },
    @{ src = "networkinformation";   dst = "networkinformation" }
)) {
    $dstDir = Join-Path $Bin $pair.dst
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    Copy-Item (Join-Path $QtPlugins "$($pair.src)/*.dll") -Destination $dstDir -Force
}
# platforms 里只留 qwindows（direct2d/minimal/offscreen 用不上，减小体积）
Remove-Item (Join-Path $Bin "platforms/qdirect2d.dll"),
            (Join-Path $Bin "platforms/qminimal.dll"),
            (Join-Path $Bin "platforms/qoffscreen.dll") -Force -ErrorAction SilentlyContinue

# 4. MCP 工具（server.py / 测试脚本，随包分发）
Write-Host "== MCP tools =="
$toolsDst = Join-Path $Install "tools/xterm_mcp"
New-Item -ItemType Directory -Force -Path $toolsDst | Out-Null
Copy-Item (Join-Path $Repo "tools/xterm_mcp/*.py") -Destination $toolsDst -Force

Write-Host "== 打包完成: $Install =="
Get-ChildItem $Bin -Filter "*.dll" | ForEach-Object { Write-Host ("  bin/{0}" -f $_.Name) }
Write-Host ("  bin/platforms/{0}" -f "qwindows.dll")
Write-Host "  tools/xterm_mcp/*.py"
