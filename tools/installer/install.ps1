# RDRVR installer: copies the mod's files into the Red Dead Redemption folder.
# Files it would replace are backed up first (RDRVR_backup\<time>), and RDRVR_Uninstall.cmd puts them back.
# Nothing of the game's own is changed, and the player's settings (%LOCALAPPDATA%\RDRVR\RDRVR.user.ini) are never touched.
#   install.ps1 [-GameDir <folder>] [-Quiet]
# Everything it prints also goes to %TEMP%\RDRVR_install.log. Install.cmd passes -FromCmd and pauses itself, so its
# window stays open whatever happens (2026-10-10: a player's window closed at once on red text, unreadable).
param(
    [string] $GameDir,
    [switch] $Quiet,
    [switch] $Elevated,
    [switch] $FromCmd
)
$ErrorActionPreference = 'Stop'
$LogPath = Join-Path $env:TEMP 'RDRVR_install.log'
try { Start-Transcript -LiteralPath $LogPath -Append | Out-Null } catch {}

function Say([string] $s) { Write-Host $s }
function Done([int] $code) {
    try { Stop-Transcript | Out-Null } catch {}
    if (-not $Quiet -and -not $FromCmd) { Write-Host ''; Read-Host 'Press Enter to close' | Out-Null }
    exit $code
}
# Any error not handled below: shown with its line, the window kept open, nothing more changed
trap {
    Write-Host ''
    Write-Host 'The installer stopped on an error:' -ForegroundColor Red
    Write-Host "  $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo -and $_.InvocationInfo.Line) { Write-Host "  (line $($_.InvocationInfo.ScriptLineNumber): $($_.InvocationInfo.Line.Trim()))" }
    Write-Host 'If a file is in use, close the game and its crash reporter (or wait a minute) and run the installer again.'
    Write-Host "Otherwise send this text, or the log $LogPath, to RDRVR's author."
    Done 1
}
$Payload = Join-Path $PSScriptRoot 'payload'
if (-not (Test-Path -LiteralPath $Payload)) { $Payload = $PSScriptRoot }  # the .exe extracts everything flat
$Version = (Get-Content -LiteralPath (Join-Path $Payload 'RDRVR_version.txt') -TotalCount 1).Trim()
# plain strings: Windows PowerShell's Get-Content lines carry note properties that ConvertTo-Json would write out
$Files = @(Get-Content -LiteralPath (Join-Path $Payload 'RDRVR_files.txt') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
# A test build's simulator pointer: it would send the eyes to the simulator instead of the headset (removed, not kept)
$Leftovers = @('RDRVR_xr_runtime.txt')

function Sha([string] $p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLowerInvariant() }

function Find-Game {
    $cands = @()
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $sp = (Get-ItemProperty -LiteralPath $k -ErrorAction Stop)
            $root = if ($sp.SteamPath) { $sp.SteamPath } else { $sp.InstallPath }
            if (-not $root) { continue }
            $root = $root -replace '/', '\'
            $libs = @($root)
            $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf) {
                foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $libs += ($m.Groups[1].Value -replace '\\\\', '\')
                }
            }
            foreach ($l in $libs) { $cands += Join-Path $l 'steamapps\common\Red Dead Redemption' }
        } catch {}
    }
    foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Rockstar Games\Red Dead Redemption', 'HKLM:\SOFTWARE\Rockstar Games\Red Dead Redemption') {
        try { $p = (Get-ItemProperty -LiteralPath $k -ErrorAction Stop).InstallFolder; if ($p) { $cands += $p } } catch {}
    }
    foreach ($c in $cands | Select-Object -Unique) {
        if (Test-Path -LiteralPath (Join-Path $c 'RDR.exe')) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

function Pick-Folder {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $d = New-Object System.Windows.Forms.FolderBrowserDialog
        $d.Description = 'Choose the Red Dead Redemption folder (the one with RDR.exe)'
        if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    } catch {}
    return $null
}

Say "RDRVR installer ($Version)"
Say ''
if (-not $GameDir) { $GameDir = Find-Game }
if (-not $GameDir) {
    Say 'Red Dead Redemption was not found in Steam''s or Rockstar''s install locations.'
    if (-not $Quiet) { $GameDir = Pick-Folder }
}
if (-not $GameDir -or -not (Test-Path -LiteralPath (Join-Path $GameDir 'RDR.exe'))) {
    Say "No RDR.exe in '$GameDir'. Run again with: Install.cmd -GameDir ""<the game folder>"""
    Done 1
}
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
Say "Game folder: $GameDir"
# RedHook (K3rhos, https://www.nexusmods.com/reddeadredemption/mods/192) runs the gameplay plugin; it is not bundled
if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'RedHook.dll')) -or -not (Test-Path -LiteralPath (Join-Path $GameDir 'winmm.dll'))) {
    Say 'RedHook is not installed in the game folder (no RedHook.dll / winmm.dll). RDRVR needs it: install RedHook v0.8'
    Say 'first (https://www.nexusmods.com/reddeadredemption/mods/192), set [DirectXHook] Disabled=true in its RedHook.ini,'
    Say 'then run this installer again.'
    Done 1
}

# Only this folder's game counts (a process whose path cannot be read is taken as this one)
$running = @(Get-Process -Name RDR -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or $_.Path.StartsWith($GameDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) })
if ($running.Count) { Say 'Red Dead Redemption is running. Quit the game and run the installer again.'; Done 1 }
# The game's crash reporter (in the game folder) can outlive the game for a moment and hold the mod's files
$reporter = @(Get-Process -Name crashpad_handler -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($GameDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) })
if ($reporter.Count) { Say 'The game''s crash reporter (crashpad_handler.exe) is still running. Wait a minute and run the installer again.'; Done 1 }

# Steam's folder is usually writable; if not, ask for administrator rights once
$probe = Join-Path $GameDir ('RDRVR_write_test_{0}.tmp' -f $PID)
try { [IO.File]::WriteAllText($probe, 'x'); Remove-Item -LiteralPath $probe -Force }
catch {
    if ($Elevated) { Say "Cannot write to the game folder even as administrator: $($_.Exception.Message)"; Done 1 }
    Say 'The game folder needs administrator rights to write; asking for them (a second window)...'
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-GameDir', "`"$GameDir`"", '-Elevated')
    if ($Quiet) { $a += '-Quiet' }
    # the elevated run appends to the same log: this run's transcript closed first (a second one on the same file
    # writes nothing while the first holds it)
    try { Stop-Transcript | Out-Null } catch {}
    try { $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $a -Wait -PassThru; exit $p.ExitCode }
    catch {
        try { Start-Transcript -LiteralPath $LogPath -Append | Out-Null } catch {}
        Say 'Administrator rights were declined; nothing was installed.'; Done 1
    }
}

# Every file it would replace must be free (nothing changed yet if one is held: the game, its crash reporter, a scan)
function Test-Free([string] $p) {
    for ($i = 0; $i -lt 6; $i++) {
        try { $s = [IO.File]::Open($p, 'Open', 'ReadWrite', 'None'); $s.Close(); return $true }
        catch [System.IO.IOException] { Start-Sleep -Seconds 1 }
    }
    return $false
}
foreach ($f in $Files) {
    $dst = Join-Path $GameDir $f
    if ((Test-Path -LiteralPath $dst) -and -not (Test-Free $dst)) {
        Say "  $f in the game folder is in use by another program (the game, its crash reporter, or a virus scan)."
        Say '  Nothing was changed. Close it (or restart the PC) and run the installer again.'
        Done 1
    }
}

# What this installer would replace: back it up (an earlier RDRVR install's files are recorded too, so an uninstall
# after an upgrade still restores what was there before RDRVR)
$manifestPath = Join-Path $GameDir 'RDRVR_install.json'
$prev = $null
if (Test-Path -LiteralPath $manifestPath) { try { $prev = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json } catch {} }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupRel = "RDRVR_backup\$stamp"
$backupDir = Join-Path $GameDir $backupRel
$backedUp = @()
foreach ($f in $Files) {
    $dst = Join-Path $GameDir $f
    if (-not (Test-Path -LiteralPath $dst)) { continue }
    $src = Join-Path $Payload $f
    if ((Test-Path -LiteralPath $src) -and (Sha $src) -eq (Sha $dst)) { continue }  # already this version's file
    $ours = $prev -and @($prev.files | Where-Object { $_.file -eq $f -and $_.sha256 -eq (Sha $dst) }).Count
    if ($ours) { continue }  # an earlier RDRVR install's file: replaced, not kept
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    Copy-Item -LiteralPath $dst -Destination (Join-Path $backupDir $f) -Force
    $backedUp += $f
    Say "  backed up $f -> $backupRel"
}
$backups = @()
if ($prev -and $prev.backups) { $backups += @($prev.backups) }
if ($backedUp.Count) { $backups += [pscustomobject]@{ dir = $backupRel; files = $backedUp } }

# Copy (a file held by another program is tried again for 5 s, then named)
function Copy-Retry([string] $src, [string] $dst) {
    for ($i = 0; $i -lt 6; $i++) {
        try { Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop; return $true }
        catch [System.IO.IOException] { Start-Sleep -Seconds 1 }
    }
    return $false
}
$record = @()
foreach ($f in $Files) {
    $src = Join-Path $Payload $f
    $dst = Join-Path $GameDir $f
    if (-not (Copy-Retry $src $dst)) {
        Say "  $f is in use by another program (the game, its crash reporter, or a virus scan)."
        Say '  Close it (or restart the PC) and run the installer again; the files copied so far are replaced then.'
        Done 1
    }
    $h = Sha $dst
    if ($h -ne (Sha $src)) { Say "  copy of $f does not match; the install is incomplete"; Done 1 }
    $record += [pscustomobject]@{ file = $f; sha256 = $h }
    Say "  installed $f"
}
foreach ($f in $Leftovers) {
    $p = Join-Path $GameDir $f
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Say "  removed $f (a test setting)" }
}
# The uninstaller, next to the game
foreach ($u in @(@{ s = 'uninstall.ps1'; d = 'RDRVR_uninstall.ps1' }, @{ s = 'Uninstall.cmd'; d = 'RDRVR_Uninstall.cmd' })) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $u.s) -Destination (Join-Path $GameDir $u.d) -Force
}
[pscustomobject]@{
    version     = $Version
    installedAt = (Get-Date).ToString('s')
    files       = $record
    backups     = $backups
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8

Say ''
Say "RDRVR $Version is installed."
try {
    $rt = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction Stop).ActiveRuntime
    Say "OpenXR runtime: $rt"
} catch {
    Say 'No OpenXR runtime is set on this PC. Install your headset''s software (SteamVR, Meta Quest Link, Virtual Desktop'
    Say 'or WMR) and make it the active OpenXR runtime before starting the game.'
}
$ui = Join-Path $env:LOCALAPPDATA 'RDRVR\RDRVR.user.ini'
if (Test-Path -LiteralPath $ui) { Say "Your in-game settings are kept: $ui" }
Say "To remove it: RDRVR_Uninstall.cmd in the game folder."
Done 0
