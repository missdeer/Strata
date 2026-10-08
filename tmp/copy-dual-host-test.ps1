$ErrorActionPreference = 'Stop'
$sourceDirectory = (Resolve-Path -LiteralPath 'D:\Projects\Strata\tmp\dual-host-test').Path
$targetDirectory = (Resolve-Path -LiteralPath 'E:\Strata').Path
foreach ($name in @('cluster.json', 'network.key', 'start-host11.bat')) {
    $sourceFile = Join-Path $sourceDirectory $name
    $targetFile = Join-Path $targetDirectory $name
    if (Test-Path -LiteralPath $targetFile) {
        if ((Get-FileHash -LiteralPath $sourceFile).Hash -ne (Get-FileHash -LiteralPath $targetFile).Hash) {
            if ($name -ne 'start-host11.bat') { throw "Test file differs on the share: $targetFile" }
            Copy-Item -LiteralPath $sourceFile -Destination $targetFile -Force
        }
    } else {
        Copy-Item -LiteralPath $sourceFile -Destination $targetFile
    }
    if ((Get-FileHash -LiteralPath $sourceFile).Hash -ne (Get-FileHash -LiteralPath $targetFile).Hash) {
        throw "Copied test file failed verification: $name"
    }
}
Write-Output 'Shared host11 startup files copied and verified.'
