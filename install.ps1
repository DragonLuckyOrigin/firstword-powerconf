# FirstWord installer (standard user, no administrator rights).
# Run in a normal PowerShell inside the unzipped folder:
#   powershell -ExecutionPolicy Bypass -File .\install.ps1
$ErrorActionPreference = 'Stop'
$name = 'FirstWord'
$source = Join-Path $PSScriptRoot "$name.exe"
$sums = Join-Path $PSScriptRoot 'SHA256SUMS.txt'
$target = Join-Path $env:LOCALAPPDATA "Programs\$name"
$exe = Join-Path $target "$name.exe"
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

# 1) Verify the checksum before anything runs (detects corruption; it does not prove where the file came from).
if (-not (Test-Path $source) -or -not (Test-Path $sums)) { throw "$name.exe or SHA256SUMS.txt is missing next to install.ps1." }
$line = Get-Content $sums | Where-Object { $_ -match "^\s*([0-9a-fA-F]{64})\s+\*?$name\.exe\s*$" } | Select-Object -First 1
if (-not $line) { throw "No entry for $name.exe in SHA256SUMS.txt." }
$expected = ([regex]::Match($line, '[0-9a-fA-F]{64}')).Value.ToUpperInvariant()
$actual = (Get-FileHash $source -Algorithm SHA256).Hash
if ($actual -ne $expected) { throw "SHA-256 mismatch: expected $expected, found $actual. Nothing was changed." }

# 2) Stop a running instance through its stop event (no installed EXE is executed); one shared 10 s deadline.
function Stop-FirstWord {
    $deadline = (Get-Date).AddSeconds(10)
    $stopEvent = $null
    if ([System.Threading.EventWaitHandle]::TryOpenExisting('Local\FirstWord.Stop',
            [System.Security.AccessControl.EventWaitHandleRights]::Modify, [ref]$stopEvent)) {
        [void]$stopEvent.Set()
        $stopEvent.Dispose()
    }
    while (Get-Process -Name $name -ErrorAction SilentlyContinue) {
        if ((Get-Date) -gt $deadline) { return $false }
        Start-Sleep -Milliseconds 200
    }
    return $true
}
if (-not (Stop-FirstWord)) { throw "The running $name did not stop within 10 s. Nothing was changed." }

# 3) Copy, register autostart, start, verify.
New-Item -ItemType Directory -Force $target | Out-Null
Copy-Item $source $exe -Force
if ((Get-FileHash $exe -Algorithm SHA256).Hash -ne $expected) { throw "Copy is corrupted: $exe" }
Set-ItemProperty -Path $runKey -Name $name -Value ('"{0}"' -f $exe)
Start-Process -FilePath $exe
Start-Sleep -Seconds 2
if (-not (Get-Process -Name $name -ErrorAction SilentlyContinue)) { throw "$name is not running after start. Log: $env:LOCALAPPDATA\$name\firstword.log" }
Write-Host "Installed: $exe"
Write-Host "Autostart: HKCU Run\$name"
Write-Host "Log:       $env:LOCALAPPDATA\$name\firstword.log"
