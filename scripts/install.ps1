param(
    [string]$SourceExe = (Join-Path $PSScriptRoot "..\build\Release\PrintScreenRegionSnip.exe")
)

$ErrorActionPreference = 'Stop'

$resolvedExe = Resolve-Path $SourceExe -ErrorAction Stop
$installDir = Join-Path $env:LOCALAPPDATA 'PrintScreenRegionSnip'
$targetExe = Join-Path $installDir 'PrintScreenRegionSnip.exe'

New-Item -Path $installDir -ItemType Directory -Force | Out-Null
Copy-Item -Path $resolvedExe -Destination $targetExe -Force

$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
New-Item -Path $runKey -Force | Out-Null
Set-ItemProperty -Path $runKey -Name 'PrintScreenRegionSnip' -Value ('"{0}"' -f $targetExe)

Write-Output "Installed to: $targetExe"
Write-Output 'Startup entry set: HKCU\Software\Microsoft\Windows\CurrentVersion\Run\PrintScreenRegionSnip'
Write-Output 'Next step: launch once manually or log off/log on.'
