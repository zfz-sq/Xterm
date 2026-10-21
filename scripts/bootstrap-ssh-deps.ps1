$ErrorActionPreference = 'Stop'

$VcpkgCommit = '7bb3ef7c1aebcb18a9ad7f5d7c6e3ea3be006196'
$LibsshVersion = '0.12.2'
$LibsshSha512 = '92b95d64772906fc0fe497fed4dc34a160f2397f71ef3871dc1ea0fe1e8e3c00df699ed0efdc0b754feb23e2140bda17be41d16aee48ff715487384023fdd467'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$DepsRoot = Join-Path $ProjectRoot '.deps'
$VcpkgRoot = Join-Path $DepsRoot 'vcpkg'
$OverlayRoot = Join-Path $DepsRoot 'overlay-ports'
$OverlayPort = Join-Path $OverlayRoot 'libssh'
$InstallRoot = Join-Path $DepsRoot 'vcpkg_installed'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [string[]]$ArgumentList = @()
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath $($ArgumentList -join ' ')' failed with exit code $LASTEXITCODE."
    }
}

function Replace-ExactlyOnce {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Pattern,

        [Parameter(Mandatory = $true)]
        [string]$Replacement
    )

    $content = [System.IO.File]::ReadAllText($Path)
    $matches = [regex]::Matches($content, $Pattern)
    if ($matches.Count -ne 1) {
        throw "Expected one match for '$Pattern' in '$Path', found $($matches.Count)."
    }
    $updated = [regex]::Replace($content, $Pattern, $Replacement, 1)
    [System.IO.File]::WriteAllText($Path, $updated, [System.Text.UTF8Encoding]::new($false))
}

New-Item -ItemType Directory -Force -Path $DepsRoot, $OverlayRoot | Out-Null

if (-not (Test-Path (Join-Path $VcpkgRoot '.git'))) {
    if (Test-Path $VcpkgRoot) {
        throw "'$VcpkgRoot' exists but is not a Git checkout."
    }
    Invoke-Checked -FilePath git -ArgumentList @(
        'clone',
        'https://github.com/microsoft/vcpkg.git',
        $VcpkgRoot
    )
}

Invoke-Checked -FilePath git -ArgumentList @(
    '-C', $VcpkgRoot, 'fetch', 'origin', $VcpkgCommit
)
Invoke-Checked -FilePath git -ArgumentList @(
    '-C', $VcpkgRoot, 'checkout', '--detach', $VcpkgCommit
)

if (Test-Path $OverlayPort) {
    Remove-Item -Recurse -Force $OverlayPort
}
Copy-Item -Recurse -Force (Join-Path $VcpkgRoot 'ports/libssh') $OverlayPort

$OverlayManifest = Join-Path $OverlayPort 'vcpkg.json'
$OverlayPortfile = Join-Path $OverlayPort 'portfile.cmake'
Replace-ExactlyOnce $OverlayManifest `
    '("version(?:-semver|-string|-date)?"\s*:\s*")[^"]+(")' `
    ('${1}' + $LibsshVersion + '${2}')
Replace-ExactlyOnce $OverlayPortfile `
    '(?m)^(\s*SHA512\s+)[0-9a-fA-F]+(\s*)$' `
    ('${1}' + $LibsshSha512 + '${2}')

$Bootstrap = Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'
Invoke-Checked -FilePath $Bootstrap -ArgumentList @('-disableMetrics')

$Vcpkg = Join-Path $VcpkgRoot 'vcpkg.exe'
Invoke-Checked -FilePath $Vcpkg -ArgumentList @(
    'install',
    '--triplet', 'x64-mingw-dynamic',
    '--host-triplet', 'x64-mingw-dynamic',
    "--overlay-ports=$OverlayRoot",
    "--x-install-root=$InstallRoot"
)

$RuntimeDir = Join-Path $InstallRoot 'x64-mingw-dynamic/bin'
$LibsshRuntime = @(
    (Join-Path $RuntimeDir 'libssh.dll'),
    (Join-Path $RuntimeDir 'ssh.dll')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $LibsshRuntime) {
    throw "Neither libssh.dll nor ssh.dll exists in '$RuntimeDir'."
}

$LibcryptoRuntime = Join-Path $RuntimeDir 'libcrypto-3-x64.dll'
if (-not (Test-Path $LibcryptoRuntime)) {
    throw "'$LibcryptoRuntime' is missing."
}

$LibsshConfig = Get-ChildItem `
    -Path (Join-Path $InstallRoot 'x64-mingw-dynamic') `
    -Recurse `
    -File `
    -Include 'libssh-config.cmake', 'libsshConfig.cmake' |
    Select-Object -First 1
if (-not $LibsshConfig) {
    throw 'The libssh CMake package is missing.'
}

Write-Host "Installed libssh $LibsshVersion runtime: $LibsshRuntime"
Write-Host "Installed OpenSSL runtime: $LibcryptoRuntime"
Write-Host "Installed libssh CMake package: $($LibsshConfig.FullName)"
