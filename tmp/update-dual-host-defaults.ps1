$ErrorActionPreference = 'Stop'
$target = (Resolve-Path -LiteralPath 'E:\Strata').Path
$root = 'D:\Projects\Strata'
$files = @(
    @{Source = 'tmp\dual-host-test\cluster.json'; Target = 'cluster.json'},
    @{Source = 'tools\cluster_config.py'; Target = 'tools\cluster_config.py'},
    @{Source = 'tools\network_worker.py'; Target = 'tools\network_worker.py'}
)
foreach ($entry in $files) {
    $sourceFile = Join-Path $root $entry.Source
    $targetFile = [IO.Path]::GetFullPath((Join-Path $target $entry.Target))
    if (-not $targetFile.StartsWith($target.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Invalid destination path.'
    }
    if (Test-Path -LiteralPath $targetFile) {
        $backup = $targetFile + '.before-preload'
        if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $targetFile -Destination $backup }
    }
    Copy-Item -LiteralPath $sourceFile -Destination $targetFile -Force
    if ((Get-FileHash -LiteralPath $sourceFile).Hash -ne (Get-FileHash -LiteralPath $targetFile).Hash) {
        throw "Copied file differs: $($entry.Target)"
    }
}
Write-Output 'Host11 shared configuration, validator and preload worker updated and verified.'
