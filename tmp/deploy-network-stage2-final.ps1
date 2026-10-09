$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Strata'
$target = 'E:\Strata\network-opt-stage2'
$mapping = Get-SmbMapping -LocalPath 'E:'
if ($mapping.RemotePath.TrimEnd('\') -ne '\\192.168.233.126\D') { throw 'Wrong E: mapping.' }
$items = @{
    'network-stage2-final-cluster.json' = 'final-cluster.json'
    'network-stage2-apply-final.ps1' = 'apply-final-config.ps1'
    'network-stage2-switch-final.bat' = 'switch-final-and-start.bat'
}
foreach ($name in $items.Keys) {
    $source = Join-Path $root ('tmp\' + $name)
    $destination = Join-Path $target $items[$name]
    Copy-Item -LiteralPath $source -Destination $destination -Force
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) { throw "Copy mismatch: $name" }
}
$installed = Join-Path $root 'tmp\dual-host-test\cluster.json'
Copy-Item -LiteralPath $installed -Destination ($installed + '.before-final') -Force
Copy-Item -LiteralPath (Join-Path $root 'tmp\network-stage2-final-cluster.json') -Destination $installed -Force
Write-Output 'Final plan staged on E:\Strata and coordinator plan synchronized.'
