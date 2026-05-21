$ErrorActionPreference = 'Stop'

$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if (Get-ItemProperty -Path $runKey -Name 'PrintScreenRegionSnip' -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $runKey -Name 'PrintScreenRegionSnip' -ErrorAction Stop
}

Get-Process -Name 'PrintScreenRegionSnip' -ErrorAction SilentlyContinue | Stop-Process -Force

$installDir = Join-Path $env:LOCALAPPDATA 'PrintScreenRegionSnip'
if (Test-Path $installDir) {
    Remove-Item -Path $installDir -Recurse -Force
}

Write-Output 'Uninstalled startup entry and removed local install directory.'
