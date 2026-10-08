$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Strata'
$logDirectory = Join-Path $root 'tmp\dual-host-test'
$env:STRATA_NETWORK_KEY = (Get-Content -LiteralPath (Join-Path $logDirectory 'network.key') -Raw).Trim()
$env:STRATA_API_KEY = $env:STRATA_NETWORK_KEY
$process = Start-Process -FilePath 'D:\Python313\python.exe' -ArgumentList @('-u', 'tools/cluster.py', '--config',
    'tmp/dual-host-test/cluster.json', '--node', 'host16') -WorkingDirectory $root -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $logDirectory 'coordinator.stdout.log') `
    -RedirectStandardError (Join-Path $logDirectory 'coordinator.stderr.log')
$process.Id | Set-Content -LiteralPath (Join-Path $logDirectory 'coordinator.pid')
Write-Output "Host16 coordinator started; PID=$($process.Id); API=http://127.0.0.1:18080"
