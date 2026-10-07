<#
.SYNOPSIS
    Build RDRVR: build\dinput8.dll (core) and build\RDRVR.Gameplay.red (RedHook plugin).

.DESCRIPTION
    Visual Studio 2022 toolchain + Ninja, vcpkg manifest mode with the x64-windows-static triplet (static
    CRT, one self-contained binary each). Prints each output's write time, size and sha256 so a deploy can
    be matched to a build.

    The gameplay plugin needs the RedHook SDK (not included): -RedHookSdk <folder> with include\RedHook.h and
    lib\RedHook.lib, default extern\redhook-sdk. Without it only dinput8.dll is built.

.EXAMPLE
    tools\build.ps1
    tools\build.ps1 -Clean -RedHookSdk C:\src\RedHookSDK-layout
#>
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')] [string] $Config = 'Release',
    [switch] $Clean,
    [string] $RedHookSdk
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

# Resolve vcpkg BEFORE entering the VS dev shell, which points VCPKG_ROOT at VS's bundled copy.
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\dev\vcpkg' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'No Visual Studio installation with the C++ toolchain was found.' }
$cmakeDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path $cmakeDir) { $env:PATH = "$cmakeDir;$ninjaDir;$env:PATH" }

Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$env:VCPKG_ROOT = $vcpkgRoot
$toolchain = (Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake') -replace '\\', '/'
if (-not (Test-Path $toolchain)) { throw "vcpkg toolchain not found at $toolchain (set VCPKG_ROOT)" }

cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
      '-DVCPKG_TARGET_TRIPLET=x64-windows-static' $(if ($RedHookSdk) { "-DREDHOOK_SDK_DIR=$(($RedHookSdk -replace '\\', '/'))" })
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

function Get-Machine($p) { $b = [IO.File]::ReadAllBytes($p); [BitConverter]::ToUInt16($b, [BitConverter]::ToInt32($b, 0x3c) + 4) }
foreach ($name in 'dinput8.dll', 'RDRVR.Gameplay.red') {
    $out = Join-Path $build $name
    if (-not (Test-Path $out)) {
        if ($name -eq 'RDRVR.Gameplay.red') { Write-Warning 'RDRVR.Gameplay.red not built (no RedHook SDK; see README)'; continue }
        throw "missing output $out"
    }
    if ((Get-Machine $out) -ne 0x8664) { throw "$name is not x64" }
    $fi = Get-Item $out
    $sha = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLowerInvariant()
    Write-Host ("BUILD OK: {0}  {1:yyyy-MM-dd HH:mm:ss}  {2:N0} bytes  sha256 {3}" -f $name, $fi.LastWriteTime, $fi.Length, $sha) -ForegroundColor Green
}
$exports = & dumpbin /nologo /exports (Join-Path $build 'dinput8.dll') | Select-String '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' |
           ForEach-Object { $_.Matches[0].Groups[1].Value }
foreach ($e in 'DirectInput8Create', 'RDRVR_GetApi') {
    if ($exports -notcontains $e) { throw "dinput8.dll does not export $e (exports: $($exports -join ', '))" }
}
$imports = & dumpbin /nologo /imports (Join-Path $build 'RDRVR.Gameplay.red') | Select-String 'RedHook.dll'
if (-not $imports) { throw 'RDRVR.Gameplay.red does not import RedHook.dll (RedHook would refuse to load it)' }
Write-Host "exports: $($exports -join ', ')"
