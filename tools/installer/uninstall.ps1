# RDRVR uninstaller: removes the files RDRVR_install.json lists and the mod's logs, then puts back what the installer
# backed up. Lives in the game folder as RDRVR_uninstall.ps1. The player's settings (%LOCALAPPDATA%\RDRVR) are kept.
#   RDRVR_uninstall.ps1 [-GameDir <folder>] [-Quiet]
# Everything it prints also goes to %TEMP%\RDRVR_install.log; RDRVR_Uninstall.cmd passes -FromCmd and pauses itself.
param(
    [string] $GameDir,
    [switch] $Quiet,
    [switch] $Elevated,
    [switch] $FromCmd
)
$ErrorActionPreference = 'Stop'
$LogPath = Join-Path $env:TEMP 'RDRVR_install.log'
try { Start-Transcript -LiteralPath $LogPath -Append | Out-Null } catch {}
if (-not $GameDir) { $GameDir = $PSScriptRoot }
$Runtime = @('RDRVR.log', 'RDRVR_status.json', 'RDRVR_status.json.tmp', 'RDRVR_cmd.txt', 'RDRVR_cmd.txt.tmp',
             'RDRVR_text_dump.bin', 'RedHook.log', 'RDRVR_framegraph_0.txt', 'RDRVR_framegraph_1.txt',
             'RDRVR_framegraph_2.txt', 'RDRVR_framegraph_3.txt', 'RDRVR_xr_runtime.txt', 'RDRVR_build_report.txt')

function Say([string] $s) { Write-Host $s }
function Done([int] $code) {
    try { Stop-Transcript | Out-Null } catch {}
    if (-not $Quiet -and -not $FromCmd) { Write-Host ''; Read-Host 'Press Enter to close' | Out-Null }
    exit $code
}
trap {
    Write-Host ''
    Write-Host 'The uninstaller stopped on an error:' -ForegroundColor Red
    Write-Host "  $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo -and $_.InvocationInfo.Line) { Write-Host "  (line $($_.InvocationInfo.ScriptLineNumber): $($_.InvocationInfo.Line.Trim()))" }
    Write-Host 'If a file is in use, close the game and its crash reporter (or wait a minute) and run it again.'
    Write-Host "Otherwise send this text, or the log $LogPath, to RDRVR's author."
    Done 1
}

$manifestPath = Join-Path $GameDir 'RDRVR_install.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { Say "No RDRVR install found in '$GameDir' (no RDRVR_install.json)."; Done 1 }
# Only this folder's game counts (a process whose path cannot be read is taken as this one)
$running = @(Get-Process -Name RDR -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or $_.Path.StartsWith($GameDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) })
if ($running.Count) { Say 'Red Dead Redemption is running. Quit the game and run the uninstaller again.'; Done 1 }

$probe = Join-Path $GameDir ('RDRVR_write_test_{0}.tmp' -f $PID)
try { [IO.File]::WriteAllText($probe, 'x'); Remove-Item -LiteralPath $probe -Force }
catch {
    if ($Elevated) { Say "Cannot write to the game folder even as administrator: $($_.Exception.Message)"; Done 1 }
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-GameDir', "`"$GameDir`"", '-Elevated')
    if ($Quiet) { $a += '-Quiet' }
    try { Stop-Transcript | Out-Null } catch {}  # the elevated run appends to the same log
    try { $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $a -Wait -PassThru; exit $p.ExitCode }
    catch {
        try { Start-Transcript -LiteralPath $LogPath -Append | Out-Null } catch {}
        Say 'Administrator rights were declined; nothing was removed.'; Done 1
    }
}

$m = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
Say "Removing RDRVR $($m.version) from $GameDir"
foreach ($f in @($m.files)) {
    $p = Join-Path $GameDir $f.file
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Say "  removed $($f.file)" }
}
foreach ($f in $Runtime) {
    $p = Join-Path $GameDir $f
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Say "  removed $f" }
}
# Restore the oldest backup of each file: what was there before RDRVR was first installed
$restored = @{}
foreach ($b in @($m.backups)) {
    foreach ($f in @($b.files)) {
        if ($restored.ContainsKey($f)) { continue }
        $src = Join-Path (Join-Path $GameDir $b.dir) $f
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $GameDir $f) -Force
            $restored[$f] = $true
            Say "  restored $f (from $($b.dir))"
        }
    }
}
# The recorded backups go once restored, RDRVR_backup itself only when nothing else is in it (2026-10-10: a backup made
# by an install that stopped part-way is in no manifest, and removing the whole folder lost it)
if ($restored.Count -eq @($m.backups | ForEach-Object { $_.files } | Select-Object -Unique).Count) {
    foreach ($b in @($m.backups)) {
        $d = Join-Path $GameDir $b.dir
        if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    }
    $bd = Join-Path $GameDir 'RDRVR_backup'
    if ((Test-Path -LiteralPath $bd) -and -not @(Get-ChildItem -LiteralPath $bd -Force).Count) { Remove-Item -LiteralPath $bd -Force }
    elseif (Test-Path -LiteralPath $bd) { Say "  kept RDRVR_backup (it holds backups from an install that did not finish)" }
}
Remove-Item -LiteralPath $manifestPath -Force  # RDRVR_Uninstall.cmd then deletes itself
Say ''
Say 'RDRVR is removed.'
$ui = Join-Path $env:LOCALAPPDATA 'RDRVR'
if (Test-Path -LiteralPath $ui) { Say "Your settings are kept in $ui (delete that folder to remove them too)." }
# This script removes itself last
Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
Done 0
