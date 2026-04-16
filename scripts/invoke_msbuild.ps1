param(
    [Parameter(Mandatory = $true)]
    [string]$Msbuild,

    [Parameter(Mandatory = $true)]
    [string]$Solution,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$Platform,

    [Parameter(Mandatory = $false)]
    [string[]]$Targets = @("Build")
)

$pathValue = if ($env:Path) { $env:Path } else { $env:PATH }
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$env:Path = $pathValue

if ($Targets -and $Targets.Count -gt 0) {
    $targetArg = "/t:" + ($Targets -join ";")
    & $Msbuild $Solution /m $targetArg "/p:Configuration=$Configuration" "/p:Platform=$Platform"
} else {
    & $Msbuild $Solution /m "/p:Configuration=$Configuration" "/p:Platform=$Platform"
}
exit $LASTEXITCODE
