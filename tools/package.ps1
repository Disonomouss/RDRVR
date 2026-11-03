<#
.SYNOPSIS
    Make the release zip build\dist\RDRVR-<version>.zip from build\ (run tools\build.ps1 with the RedHook SDK first).

.DESCRIPTION
    The zip holds the installer (Install.cmd, install.ps1, uninstall scripts, README.txt) and the payload: dinput8.dll,
    RDRVR.Gameplay.red, config\RDRVR.ini, the click sounds (RDRVR_click_*.wav), the shader converter for single pass
    (RDRVR_Converter\rdrvr_vi_convert.exe and the DXC DLL beside it, built with -DRDRVR_DXC_DLL=<Microsoft's
    dxcompiler.dll>), the launcher (RDRVR_Launcher\RDRVR_Launcher.exe, built here by tools\launcher\build_launcher.ps1
    if build\ has none), LICENSE.txt and THIRD-PARTY-NOTICES.txt. RedHook is not included: the player installs it
    (README).
    The payload is flat (Compress-Archive in Windows PowerShell 5.1 writes backslash entry names): RDRVR_files.txt names
    each file by its place in the game folder and install.ps1 finds a subfolder's file by its name.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$version = (Get-Content -Raw (Join-Path $root 'vcpkg.json') | ConvertFrom-Json).version
$stage = Join-Path $build "dist\RDRVR-$version"
$zip = Join-Path $build "dist\RDRVR-$version.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$pay = Join-Path $stage 'payload'
New-Item -ItemType Directory -Force $pay | Out-Null
$map = [ordered]@{
    'dinput8.dll'            = Join-Path $build 'dinput8.dll'
    'RDRVR.Gameplay.red'     = Join-Path $build 'RDRVR.Gameplay.red'
    'RDRVR.ini'              = Join-Path $root 'config\RDRVR.ini'
    'RDRVR_click_revolver.wav' = Join-Path $root 'assets\sounds\revolver.wav'
    'RDRVR_click_rifle.wav'  = Join-Path $root 'assets\sounds\rifle.wav'
    'RDRVR_click_shotgun.wav' = Join-Path $root 'assets\sounds\shotgun.wav'
    'RDRVR_Converter\rdrvr_vi_convert.exe' = Join-Path $build 'rdrvr_vi_convert.exe'
    'RDRVR_Converter\rdrvr_dxcompiler.dll' = Join-Path $build 'rdrvr_dxcompiler.dll'
    'RDRVR_Launcher\RDRVR_Launcher.exe' = Join-Path $build 'RDRVR_Launcher.exe'
}
if (-not (Test-Path $map['RDRVR_Launcher\RDRVR_Launcher.exe'])) { & (Join-Path $PSScriptRoot 'launcher\build_launcher.ps1') }
foreach ($k in $map.Keys) {
    if (-not (Test-Path $map[$k])) { throw "missing $($map[$k]) (build with the RedHook SDK first: README)" }
    Copy-Item -LiteralPath $map[$k] -Destination (Join-Path $pay (Split-Path $k -Leaf))
}
Set-Content -LiteralPath (Join-Path $pay 'RDRVR_files.txt') -Value ($map.Keys -join "`r`n") -Encoding ascii
Set-Content -LiteralPath (Join-Path $pay 'RDRVR_version.txt') -Value $version -Encoding ascii
foreach ($f in 'install.ps1', 'uninstall.ps1', 'Install.cmd', 'Uninstall.cmd') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "installer\$f") -Destination $stage }
(Get-Content -Raw (Join-Path $PSScriptRoot 'installer\README.txt')).Replace('@VERSION@', $version) | Set-Content -LiteralPath (Join-Path $stage 'README.txt') -NoNewline
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.md') -Destination (Join-Path $stage 'THIRD-PARTY-NOTICES.txt')
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "zip: $zip"
foreach ($k in $map.Keys) { Write-Host ("  {0,-38} {1}" -f $k, (Get-FileHash -Algorithm SHA256 (Join-Path $pay (Split-Path $k -Leaf))).Hash.ToLowerInvariant()) }
