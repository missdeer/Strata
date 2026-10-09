$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Strata'
$source = Join-Path $root 'dist\cluster-network-opt\host11'
$target = 'E:\Strata\network-opt-update'
if (-not (Test-Path -LiteralPath 'E:\Strata\cluster.json' -PathType Leaf)) { throw 'The host11 share is not available.' }
$payloadFiles = @('strata_network_backend.dll', 'strata-network-check.exe', 'tools\network_worker.py', 'CLUSTER_BUILD.json')
$meta = Get-Content -LiteralPath (Join-Path $source 'CLUSTER_BUILD.json') -Raw | ConvertFrom-Json
if ($meta.node -ne 'host11' -or -not $meta.portable -or $meta.cxx_release_flags -match '/QxHost') { throw 'Wrong CPU package.' }
foreach ($relative in $payloadFiles) {
    $destination = Join-Path $target ('payload\' + $relative)
    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force
    Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $destination -Force
    if ((Get-FileHash -LiteralPath (Join-Path $source $relative)).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
        throw "Share copy hash mismatch: $relative"
    }
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
    if ((Get-FileHash -LiteralPath $original).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) { throw "Script copy hash mismatch: $name" }
}
Write-Output 'Verified update copied to E:\Strata\network-opt-update.'
Write-Output 'On host11: close the old worker, then run D:\Strata\network-opt-update\apply-and-start.bat.'
