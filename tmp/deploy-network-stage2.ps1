$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Strata'
$package = Join-Path $root 'dist\cluster-network-stage2\host11'
$target = 'E:\Strata\network-opt-stage2'
$mapping = Get-SmbMapping -LocalPath 'E:'
if ($mapping.RemotePath.TrimEnd('\') -ne '\\192.168.233.126\D') { throw 'E: is not the host11 D: share.' }
$files = @('strata_network_backend.dll', 'strata-network-check.exe', 'tools\network_worker.py', 'tools\cluster_config.py', 'CLUSTER_BUILD.json')
foreach ($relative in $files) {
    $source = Join-Path $package $relative
    $destination = Join-Path $target ('payload\' + $relative)
    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force
    Copy-Item -LiteralPath $source -Destination $destination -Force
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) { throw "Hash mismatch: $relative" }
}
$scripts = @{
    'network-opt-apply.ps1' = 'apply-update.ps1'
    'network-opt-apply-and-start.bat' = 'apply-and-start.bat'
    'network-opt-start-profiled-host11.bat' = 'start-profiled-host11.bat'
}
foreach ($name in $scripts.Keys) {
    $original = Join-Path $root ('tmp\' + $name)
    $destination = Join-Path $target $scripts[$name]
    Copy-Item -LiteralPath $original -Destination $destination -Force
    if ((Get-FileHash -LiteralPath $original).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) { throw "Hash mismatch: $name" }
}
$config = Join-Path $root 'tmp\network-stage2-cluster.json'
$destinationConfig = Join-Path $target 'payload\cluster.json'
Copy-Item -LiteralPath $config -Destination $destinationConfig -Force
if ((Get-FileHash -LiteralPath $config).Hash -ne (Get-FileHash -LiteralPath $destinationConfig).Hash) { throw 'Cluster copy hash mismatch.' }
$installed = Join-Path $root 'tmp\dual-host-test\cluster.json'
Copy-Item -LiteralPath $installed -Destination ($installed + '.before-stage2') -Force
Copy-Item -LiteralPath $config -Destination $installed -Force
Write-Output 'Stage 2 update verified at E:\Strata\network-opt-stage2. Host16 plan synchronized.'
Write-Output 'On host11: close the old worker, then run D:\Strata\network-opt-stage2\apply-and-start.bat.'
