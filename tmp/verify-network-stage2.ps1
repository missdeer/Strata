param([switch]$FixedOnly, [switch]$FinalOnly)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath 'D:\Projects\Strata'
if ($FixedOnly) {
    if (Get-Process -Name strata -ErrorAction SilentlyContinue) { throw 'Another engine is running.' }
    foreach ($mode in @('raw', 'reduce', 'local')) {
        & 'D:\Python313\python.exe' -u 'tmp/bench-dual-host-decode-opt.py' --mode $mode --expert-cache 2400 --prefix ('network-stage2-fixed-' + $mode)
        if ($LASTEXITCODE -ne 0) { throw "Fixed-cache $mode measurement failed." }
    }
    Copy-Item -LiteralPath 'E:\Strata\network-opt-worker.log' -Destination 'tmp/network-stage2-fixed-worker.log' -Force
    Write-Output 'Fixed-cache comparison complete.'
    exit 0
}
$planLines = & 'D:\Python313\python.exe' 'tools/cluster.py' --config 'tmp/dual-host-test/cluster.json' --validate
if ($LASTEXITCODE -ne 0) { throw 'Invalid coordinator plan.' }
$expectedDigest = [regex]::Match(($planLines -join ' '), 'config_sha256=([0-9a-f]{64})').Groups[1].Value
$deadline = [DateTime]::UtcNow.AddMinutes(6)
$ready = $false
do {
    $header = Get-Content -LiteralPath 'E:\Strata\network-opt-worker.log' -TotalCount 1
    $last = Get-Content -LiteralPath 'E:\Strata\network-opt-worker.log' -Tail 1
    Write-Output $last
    if ($last -match 'worker ready:' -and $header -match $expectedDigest) {
        $ready = $true; break
    }
    if ($last -match 'error:|Traceback') { throw 'Worker startup failed; inspect its log.' }
    Start-Sleep -Seconds 15
} while ([DateTime]::UtcNow -lt $deadline)
if (-not $ready) { throw 'Stage 2 worker is not ready; inspect E:\Strata\network-opt-worker.log.' }
$env:STRATA_NETWORK_REDUCE = '1'
& 'D:\Python313\python.exe' 'tmp/check-dual-host-network.py'
if ($LASTEXITCODE -ne 0) { throw 'Reduced LAN expert parity failed.' }
Remove-Item Env:\STRATA_NETWORK_REDUCE
if ($FinalOnly) {
    & 'D:\Python313\python.exe' -u 'tmp/bench-dual-host-decode-opt.py' --mode configured --prefix 'network-stage2-final'
    if ($LASTEXITCODE -ne 0) { throw 'Final configured generation failed.' }
    Copy-Item -LiteralPath 'E:\Strata\network-opt-worker.log' -Destination 'tmp/network-stage2-final-worker.log' -Force
    & 'D:\Python313\python.exe' 'tmp/verify-start-host16.py'
    if ($LASTEXITCODE -ne 0) { throw 'Final coordinator launcher check failed.' }
    Write-Output 'Final configuration verified.'
    exit 0
}
& 'D:\Python313\python.exe' -u 'tmp/bench-dual-host-lan.py' --output 'tmp/network-stage2-lan-raw.json'
if ($LASTEXITCODE -ne 0) { throw 'Raw LAN benchmark failed.' }
& 'D:\Python313\python.exe' -u 'tmp/bench-dual-host-lan.py' --reduce --output 'tmp/network-stage2-lan-reduce.json'
if ($LASTEXITCODE -ne 0) { throw 'Reduced LAN benchmark failed.' }
Copy-Item -LiteralPath 'E:\Strata\network-opt-worker.log' -Destination 'tmp/network-stage2-worker-lan.log' -Force
if (Get-Process -Name strata -ErrorAction SilentlyContinue) { throw 'Another engine is running; finish it before matched decode measurements.' }
foreach ($mode in @('raw', 'reduce', 'local')) {
    & 'D:\Python313\python.exe' -u 'tmp/bench-dual-host-decode-opt.py' --mode $mode --prefix ('network-stage2-' + $mode)
    if ($LASTEXITCODE -ne 0) { throw "Full-engine $mode measurement failed." }
}
Copy-Item -LiteralPath 'E:\Strata\network-opt-worker.log' -Destination 'tmp/network-stage2-worker-full.log' -Force
Write-Output 'Stage 2 matched LAN and decode measurements complete.'
