param(
    [string]$Wix = "$env:USERPROFILE\.dotnet\tools\wix.exe"
)

$ErrorActionPreference = "Stop"
$msi = Join-Path $PSScriptRoot "..\dist\Boxify-0.1.1-x64.msi"

Push-Location $PSScriptRoot
try {
    & $Wix build "Boxify.wxs" -arch x64 -o $msi
    if ($LASTEXITCODE -ne 0) {
        throw "WiX build failed with exit code $LASTEXITCODE"
    }
    $wixpdb = [System.IO.Path]::ChangeExtension($msi, ".wixpdb")
    if (Test-Path -LiteralPath $wixpdb) {
        Remove-Item -LiteralPath $wixpdb -Force
    }
    Write-Host "MSI OK: $msi"
}
finally {
    Pop-Location
}
