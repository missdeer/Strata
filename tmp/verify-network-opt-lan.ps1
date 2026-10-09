$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath 'D:\Projects\Strata'
$deadline = [DateTime]::UtcNow.AddMinutes(4)
$ready = $false
do {
    $last = Get-Content -LiteralPath 'E:\Strata\network-opt-worker.log' -Tail 1
    Write-Output $last
    if ($last -match 'worker ready:') { $ready = $true; break }
    if ($last -match 'error:|Traceback') { throw 'Worker startup failed; inspect E:\Strata\network-opt-worker.log.' }
    Start-Sleep -Seconds 15
} while ([DateTime]::UtcNow -lt $deadline)
if (-not $ready) { throw 'Worker is still not ready; preload progress is in E:\Strata\network-opt-worker.log.' }
$header = Get-Content -LiteralPath 'E:\Strata\network-opt-worker.log' -TotalCount 3
if (-not ($header -match '^worker finite checks: native$')) { throw 'The new native validator is not active.' }
& 'D:\Python313\python.exe' 'tmp/check-dual-host-network.py'
if ($LASTEXITCODE -ne 0) { throw 'LAN expert parity failed.' }
& 'D:\Python313\python.exe' -u 'tmp/bench-dual-host-lan.py' --output 'tmp/dual-host-lan-after.json'
if ($LASTEXITCODE -ne 0) { throw 'LAN benchmark failed.' }
Copy-Item -LiteralPath 'E:\Strata\network-opt-worker.log' -Destination 'tmp/network-opt-worker-lan.log' -Force
if (Get-Process -Name strata -ErrorAction SilentlyContinue) {
    Write-Output 'An engine is already running; saved LAN benchmark without starting another GPU process.'
} else {
    & 'D:\Python313\python.exe' -u 'tmp/bench-dual-host-decode-opt.py'
    if ($LASTEXITCODE -ne 0) { throw 'Full-engine generation failed; inspect tmp/network-opt-full-decode.log.' }
    Copy-Item -LiteralPath 'E:\Strata\network-opt-worker.log' -Destination 'tmp/network-opt-worker-full.log' -Force
}
