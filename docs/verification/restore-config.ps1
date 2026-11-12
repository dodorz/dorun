$cfg = 'C:\Users\dodo\Scoop\persist\DoRun\DoRun.yaml'
$lines = @([System.IO.File]::ReadAllLines($cfg, [System.Text.Encoding]::UTF8))
$idx = -1
for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^CLIPBOARD_HISTORY_IGNORE_PROCESSES:') { $idx = $i; break } }
if ($idx -ge 0) {
    $end = $idx + 1
    while ($end -lt $lines.Count -and $lines[$end] -match '^\s+- ') { $end++ }
    $new = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $idx; $i++) { $new.Add($lines[$i]) }
    $new.Add('CLIPBOARD_HISTORY_IGNORE_PROCESSES:')
    $new.Add('  - "keepass*.exe"')
    $new.Add('  - "1password.exe"')
    $new.Add('  - "bitwarden.exe"')
    for ($i = $end; $i -lt $lines.Count; $i++) { $new.Add($lines[$i]) }
    [System.IO.File]::WriteAllText($cfg, (($new.ToArray() -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host 'restored the ignore list to the documented password-manager defaults'
}
Start-Sleep -Milliseconds 2400
Get-Content $cfg -Encoding UTF8 | Select-String '^CLIPBOARD_' | ForEach-Object { Write-Host ('  ' + $_.Line) }
