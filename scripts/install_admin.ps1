param(
    [string]$SourceExe = (Join-Path $PSScriptRoot "..\build\Release\PrintScreenRegionSnip.exe"),
    [string]$TaskName = 'PrintScreenRegionSnipAdmin',
    [switch]$KeepUserRun
)

$ErrorActionPreference = 'Stop'

$resolvedExe = Resolve-Path $SourceExe -ErrorAction Stop
$installDir = Join-Path $env:LOCALAPPDATA 'PrintScreenRegionSnip'
$targetExe = Join-Path $installDir 'PrintScreenRegionSnip.exe'

New-Item -Path $installDir -ItemType Directory -Force | Out-Null
Copy-Item -Path $resolvedExe -Destination $targetExe -Force

if (-not $KeepUserRun) {
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    if (Get-ItemProperty -Path $runKey -Name 'PrintScreenRegionSnip' -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -Path $runKey -Name 'PrintScreenRegionSnip' -ErrorAction Stop
    }
}

$taskCommand = ('"{0}"' -f $targetExe)

cmd /c "schtasks /Delete /TN `"$TaskName`" /F >nul 2>&1"

& schtasks /Create /TN $TaskName /TR $taskCommand /SC ONLOGON /RL HIGHEST /F | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create scheduled task: $TaskName"
}

Get-Process -Name 'PrintScreenRegionSnip' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Process -FilePath $targetExe

Write-Output "Installed EXE: $targetExe"
Write-Output "Scheduled task created: $TaskName (ONLOGON, RL=HIGHEST)"
if (-not $KeepUserRun) {
    Write-Output 'Removed HKCU\\Run startup entry to avoid duplicate instances.'
}
Write-Output 'Process started for current session.'
