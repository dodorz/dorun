param(
    [string]$Sections = '0,1,2,3,4,5',
    [string]$Tag = 'run',
    [switch]$SkipBackup
)

# Runs docs\verification\verify.ps1 hidden and tees the output to
# %LOCALAPPDATA%\DoRun\verification\console-<Tag>.log so long runs survive the
# console window. -NoPrompts keeps operator questions as PARTIAL entries.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $env:LOCALAPPDATA ("DoRun\verification\console-$Tag.log")
New-Item -ItemType Directory -Force -Path (Split-Path $out -Parent) | Out-Null
if (Test-Path $out) { Remove-Item $out -Force }
if (Test-Path "$out.err") { Remove-Item "$out.err" -Force }

$args = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $here 'verify.ps1'),
    '-Sections', $Sections,
    '-NoPrompts'
)
if ($SkipBackup) { $args += '-SkipBackup' }

$p = Start-Process -FilePath 'powershell.exe' -ArgumentList $args `
    -RedirectStandardOutput $out -RedirectStandardError "$out.err" `
    -PassThru -WindowStyle Hidden

Write-Host ("started pid=" + $p.Id + " sections=" + $Sections)
Write-Host ("console log: " + $out)
