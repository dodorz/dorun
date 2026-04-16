param(
    [Parameter(Mandatory = $true)]
    [string]$Msbuild,

    [Parameter(Mandatory = $true)]
    [string]$Solution,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$Platform
)

$pathValue = if ($env:Path) { $env:Path } else { $env:PATH }
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$env:Path = $pathValue

& $Msbuild $Solution /m "/p:Configuration=$Configuration" "/p:Platform=$Platform"
exit $LASTEXITCODE
