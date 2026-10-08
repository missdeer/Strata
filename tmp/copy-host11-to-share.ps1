$ErrorActionPreference = 'Stop'
$sourceDirectory = (Resolve-Path -LiteralPath 'D:\Projects\Strata\dist\cluster\host11').Path
$destinationDirectory = (Resolve-Path -LiteralPath 'E:\Strata').Path.TrimEnd('\')
$metadata = Get-Content -LiteralPath (Join-Path $sourceDirectory 'CLUSTER_BUILD.json') -Raw | ConvertFrom-Json
if ($metadata.node -ne 'host11' -or $metadata.backend -ne 'cpu' -or -not $metadata.portable -or
    $metadata.cxx_release_flags -match '/QxHost') {
    throw 'The source is not the portable host11 CPU package.'
}
foreach ($program in $metadata.programs.PSObject.Properties) {
    $actualHash = (Get-FileHash -LiteralPath (Join-Path $sourceDirectory $program.Name) -Algorithm SHA256).Hash
    if ($actualHash -ne $program.Value) { throw "Source binary hash mismatch: $($program.Name)" }
}
$sourceFiles = @(Get-ChildItem -LiteralPath $sourceDirectory -File -Recurse)
$copiedFiles = foreach ($sourceFile in $sourceFiles) {
    $relativeName = $sourceFile.FullName.Substring($sourceDirectory.Length + 1)
    $targetFile = [IO.Path]::GetFullPath((Join-Path $destinationDirectory $relativeName))
    if (-not $targetFile.StartsWith($destinationDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Destination is outside the requested share: $targetFile"
    }
    $sourceHash = (Get-FileHash -LiteralPath $sourceFile.FullName -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $targetFile) {
        if ((Get-FileHash -LiteralPath $targetFile -Algorithm SHA256).Hash -ne $sourceHash) {
            throw "A different file already exists on the share: $targetFile"
        }
    } else {
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($targetFile)) -Force
        Copy-Item -LiteralPath $sourceFile.FullName -Destination $targetFile
    }
    if ((Get-FileHash -LiteralPath $targetFile -Algorithm SHA256).Hash -ne $sourceHash) {
        throw "Copied file hash mismatch: $targetFile"
    }
    [PSCustomObject]@{File = $relativeName; Bytes = $sourceFile.Length; SHA256 = $sourceHash}
}
$copiedFiles | Format-Table File,Bytes -AutoSize
Write-Output "Verified $($copiedFiles.Count) files in $destinationDirectory"
