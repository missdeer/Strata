$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Strata'
Set-Location -LiteralPath $root
$probe = New-Object Net.Sockets.TcpClient
try {
    try { $null = $probe.ConnectAsync('127.0.0.1', 8080).Wait(1000) } catch { }
    if ($probe.Connected) { throw 'Port 8080 is already in use; no second coordinator was started.' }
} finally { $probe.Dispose() }
if (Get-Process -Name strata -ErrorAction SilentlyContinue) { throw 'Another Strata engine is running.' }
$env:STRATA_NETWORK_KEY = (Get-Content -LiteralPath 'tmp\dual-host-test\network.key' -Raw).Trim()
$env:STRATA_API_KEY = $env:STRATA_NETWORK_KEY
$process = Start-Process -FilePath 'D:\Python313\python.exe' -ArgumentList @('-u', 'tools/cluster.py', '--config',
    'tmp/dual-host-test/cluster.json', '--node', 'host16') -WorkingDirectory $root -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $root 'tmp\network-stage2-api.stdout.log') `
    -RedirectStandardError (Join-Path $root 'tmp\network-stage2-api.stderr.log')
$process.Id | Set-Content -LiteralPath 'tmp\network-stage2-api.pid'
Write-Output "Optimized coordinator started: PID=$($process.Id), API=http://127.0.0.1:8080/v1"
