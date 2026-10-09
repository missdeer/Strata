$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
if ($target -ne 'D:\Strata') { throw 'Run this script on host11 from D:\Strata\network-opt-stage2.' }
$old = Get-Content -LiteralPath (Join-Path $target 'cluster.json') -Raw | ConvertFrom-Json
$node = @($old.nodes | Where-Object id -eq 'host11')[0]
$probe = New-Object Net.Sockets.TcpClient
try {
    $pending = $probe.ConnectAsync($node.host, [int]$node.port)
    try { $null = $pending.Wait(1000) } catch { }
    if ($probe.Connected) { throw 'Close the current worker before switching the shared plan.' }
} finally { $probe.Dispose() }
$source = Join-Path $PSScriptRoot 'final-cluster.json'
$installed = Join-Path $target 'cluster.json'
$backup = $installed + '.before-final-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff')
Copy-Item -LiteralPath $installed -Destination $backup
Copy-Item -LiteralPath $source -Destination $installed -Force
if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $installed).Hash) {
    Copy-Item -LiteralPath $backup -Destination $installed -Force
    throw 'Final plan hash mismatch; restored previous plan.'
}
Write-Host 'Final plan installed: 1 GiB local RAM cap; weighted LAN responses.'
