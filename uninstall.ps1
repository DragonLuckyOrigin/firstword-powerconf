# FirstWord uninstaller (standard user).
#   powershell -ExecutionPolicy Bypass -File .\uninstall.ps1 [-RemoveLog]
param([switch]$RemoveLog)
$ErrorActionPreference = 'Stop'
$name = 'FirstWord'
$target = Join-Path $env:LOCALAPPDATA "Programs\$name"
$exe = Join-Path $target "$name.exe"
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

# 1) Stop through the stop event (no EXE is executed); one shared 10 s deadline, otherwise abort without deleting.
$deadline = (Get-Date).AddSeconds(10)
$stopEvent = $null
if ([System.Threading.EventWaitHandle]::TryOpenExisting('Local\FirstWord.Stop',
        [System.Security.AccessControl.EventWaitHandleRights]::Modify, [ref]$stopEvent)) {
    [void]$stopEvent.Set()
    $stopEvent.Dispose()
}
while (Get-Process -Name $name -ErrorAction SilentlyContinue) {
    if ((Get-Date) -gt $deadline) { throw "$name did not stop within 10 s. Nothing was deleted." }
    Start-Sleep -Milliseconds 200
}

# 2) Remove only our own Run value and our own files.
if (Get-ItemProperty -Path $runKey -Name $name -ErrorAction SilentlyContinue) { Remove-ItemProperty -Path $runKey -Name $name }
if (Test-Path $exe) { Remove-Item $exe -Force }
if ((Test-Path $target) -and -not (Get-ChildItem $target -Force)) { Remove-Item $target -Force }
if ($RemoveLog) {
    $logDir = Join-Path $env:LOCALAPPDATA $name
    foreach ($f in 'firstword.log', 'firstword.log.1') { $p = Join-Path $logDir $f; if (Test-Path $p) { Remove-Item $p -Force } }
    if ((Test-Path $logDir) -and -not (Get-ChildItem $logDir -Force)) { Remove-Item $logDir -Force }
}
Write-Host "Removed. No autostart, no process, no program file."
