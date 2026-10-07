# RDRVR uninstaller: removes the files RDRVR_install.json lists and the mod's logs, then puts back what the installer
# backed up. Lives in the game folder as RDRVR_uninstall.ps1. The player's settings (%LOCALAPPDATA%\RDRVR) are kept.
#   RDRVR_uninstall.ps1 [-GameDir <folder>] [-Quiet]
param(
    [string] $GameDir,
    [switch] $Quiet,
    [switch] $Elevated
)
$ErrorActionPreference = 'Stop'
if (-not $GameDir) { $GameDir = $PSScriptRoot }
$Runtime = @('RDRVR.log', 'RDRVR_status.json', 'RDRVR_status.json.tmp', 'RDRVR_cmd.txt', 'RDRVR_cmd.txt.tmp',
             'RDRVR_text_dump.bin', 'RedHook.log', 'RDRVR_framegraph_0.txt', 'RDRVR_framegraph_1.txt',
             'RDRVR_framegraph_2.txt', 'RDRVR_framegraph_3.txt', 'RDRVR_xr_runtime.txt')

function Say([string] $s) { Write-Host $s }
function Done([int] $code) {
    if (-not $Quiet) { Write-Host ''; Read-Host 'Press Enter to close' | Out-Null }
    exit $code
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
    try { $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $a -Wait -PassThru; exit $p.ExitCode }
    catch { Say 'Administrator rights were declined; nothing was removed.'; Done 1 }
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
if ($restored.Count -eq @($m.backups | ForEach-Object { $_.files } | Select-Object -Unique).Count) {
    $bd = Join-Path $GameDir 'RDRVR_backup'
    if (Test-Path -LiteralPath $bd) { Remove-Item -LiteralPath $bd -Recurse -Force }
}
Remove-Item -LiteralPath $manifestPath -Force  # RDRVR_Uninstall.cmd then deletes itself
Say ''
Say 'RDRVR is removed.'
$ui = Join-Path $env:LOCALAPPDATA 'RDRVR'
if (Test-Path -LiteralPath $ui) { Say "Your settings are kept in $ui (delete that folder to remove them too)." }
# This script removes itself last
Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
Done 0
