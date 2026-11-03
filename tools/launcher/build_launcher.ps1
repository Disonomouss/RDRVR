# Builds build\RDRVR_Launcher.exe with the C# compiler that ships with Windows (.NET Framework 4.8): no SDK, no
# packages. menu_schema.json (made from src/core/menu.cpp: README.md) is embedded as a resource.
#   powershell -ExecutionPolicy Bypass -File tools\launcher\build_launcher.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Split-Path (Split-Path $here -Parent) -Parent
$out = Join-Path $root 'build\RDRVR_Launcher.exe'
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "the .NET Framework 4 compiler is missing: $csc" }
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$src = @('Core.cs', 'Controls.cs', 'Graphics.cs', 'Launcher.cs') | ForEach-Object { Join-Path $here $_ }
$cscArgs = @('/nologo', '/target:winexe', '/optimize+', '/platform:anycpu', "/out:$out",
          '/r:System.dll', '/r:System.Core.dll', '/r:System.Drawing.dll', '/r:System.Windows.Forms.dll', '/r:System.Web.Extensions.dll')
$schema = Join-Path $here 'menu_schema.json'
if (Test-Path $schema) { $cscArgs += "/resource:$schema,RdrvrLauncher.menu_schema.json" } else { Write-Host 'no menu_schema.json: the Mod settings tab will be empty' }
& $csc @cscArgs @src
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
$h = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
Write-Host ("BUILD OK: RDRVR_Launcher.exe  {0}  {1:N0} bytes  sha256 {2}" -f (Get-Item $out).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), (Get-Item $out).Length, $h)
