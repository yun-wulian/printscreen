param(
    [string]$TaskName = 'PrintScreenRegionSnipAdmin',
    [switch]$RestoreUserRun,
    [switch]$RemoveInstalledExe
)

$ErrorActionPreference = 'Stop'

Get-Process -Name 'PrintScreenRegionSnip' -ErrorAction SilentlyContinue | Stop-Process -Force

& schtasks /Delete /TN $TaskName /F *> $null

if ($RestoreUserRun) {
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $targetExe = Join-Path (Join-Path $env:LOCALAPPDATA 'PrintScreenRegionSnip') 'PrintScreenRegionSnip.exe'
    if (Test-Path $targetExe) {
        New-Item -Path $runKey -Force | Out-Null
        Set-ItemProperty -Path $runKey -Name 'PrintScreenRegionSnip' -Value ('"{0}"' -f $targetExe)
    }
}

if ($RemoveInstalledExe) {
    $installDir = Join-Path $env:LOCALAPPDATA 'PrintScreenRegionSnip'
    if (Test-Path $installDir) {
        Remove-Item -Path $installDir -Recurse -Force
    }
}

Write-Output "Scheduled task removed (if existed): $TaskName"
if ($RestoreUserRun) {
    Write-Output 'HKCU\\Run startup entry restored (if EXE exists).'
}
if ($RemoveInstalledExe) {
    Write-Output 'Local install directory removed.'
}
