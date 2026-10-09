param([string]$TargetRoot, [string]$PackageRoot)
$ErrorActionPreference = 'Stop'
$stage = $PSScriptRoot
if (-not $TargetRoot) { $TargetRoot = Join-Path $stage '..' }
if (-not $PackageRoot) { $PackageRoot = Join-Path $stage 'payload' }
$target = [IO.Path]::GetFullPath($TargetRoot).TrimEnd('\')
if ($target -eq 'E:\Strata') {
    $mapping = Get-SmbMapping -LocalPath 'E:'
    if ($mapping.RemotePath.TrimEnd('\') -ne '\\192.168.233.126\D') { throw 'E: is not the host11 D: share.' }
} elseif ($target -ne 'D:\Strata') { throw "Unexpected update target: $target" }
$meta = Get-Content -LiteralPath (Join-Path $PackageRoot 'CLUSTER_BUILD.json') -Raw | ConvertFrom-Json
if ($meta.node -ne 'host11' -or -not $meta.portable -or $meta.backend -ne 'cpu' -or $meta.cxx_release_flags -match '/QxHost') {
    throw 'This is not the portable host11 CPU package.'
}
foreach ($program in $meta.programs.PSObject.Properties) {
    if ((Get-FileHash -LiteralPath (Join-Path $PackageRoot $program.Name) -Algorithm SHA256).Hash -ne $program.Value) {
        throw "Update hash mismatch: $($program.Name)"
    }
}
$cluster = Get-Content -LiteralPath (Join-Path $target 'cluster.json') -Raw | ConvertFrom-Json
$node = @($cluster.nodes | Where-Object id -eq 'host11')[0]
$probe = New-Object Net.Sockets.TcpClient
try {
    $pending = $probe.ConnectAsync($node.host, [int]$node.port)
    try { $null = $pending.Wait(1000) } catch { }
    if ($probe.Connected) { throw 'The old worker is still listening. Close its window, then run this update again.' }
} finally { $probe.Dispose() }
$files = @('strata_network_backend.dll', 'strata-network-check.exe', 'tools\network_worker.py', 'CLUSTER_BUILD.json')
foreach ($relative in @('tools\cluster_config.py', 'cluster.json')) {
    if (Test-Path -LiteralPath (Join-Path $PackageRoot $relative) -PathType Leaf) { $files += $relative }
}
foreach ($relative in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $target $relative) -PathType Leaf)) { throw "Missing installed file: $relative" }
}
$backup = Join-Path $target ('network-opt-backup-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $backup
foreach ($relative in $files) {
    $saved = Join-Path $backup $relative
    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($saved)) -Force
    Copy-Item -LiteralPath (Join-Path $target $relative) -Destination $saved
}
try {
    foreach ($relative in $files) {
        $source = Join-Path $PackageRoot $relative
        $destination = Join-Path $target $relative
        Copy-Item -LiteralPath $source -Destination $destination -Force
        if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
            throw "Installed file hash mismatch: $relative"
        }
    }
} catch {
    $failure = $_
    foreach ($relative in $files) { Copy-Item -LiteralPath (Join-Path $backup $relative) -Destination (Join-Path $target $relative) -Force }
    throw $failure
}
Write-Host "Network optimization installed. Backup: $backup"
Write-Host 'The network key is unchanged.'
