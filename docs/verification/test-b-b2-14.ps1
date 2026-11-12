# Operator-free replay of 1.2 case B / B2 and 1.4, using the deterministic
# WM_COPY copy path (no focus dependency) so the timing is exact.
Add-Type -Namespace W1B -Name K -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int m);
[DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendText(IntPtr h, uint msg, IntPtr w, string l);
[DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendN(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
public delegate bool EnumProc(IntPtr h, IntPtr l);
'@

$cfg  = 'C:\Users\dodo\Scoop\persist\DoRun\DoRun.yaml'
$log  = Join-Path $env:LOCALAPPDATA 'DoRun\hotkey-debug.log'
$hist = Join-Path $env:LOCALAPPDATA 'DoRun\clipboard-history.txt'

function Put-List([string[]]$v) {
    $lines = @([System.IO.File]::ReadAllLines($cfg, [System.Text.Encoding]::UTF8))
    $idx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^CLIPBOARD_HISTORY_IGNORE_PROCESSES:') { $idx = $i; break } }
    $blk = @('CLIPBOARD_HISTORY_IGNORE_PROCESSES:')
    foreach ($x in $v) { $blk += '  - "' + $x + '"' }
    if ($idx -lt 0) { $lines = @($lines) + $blk }
    else {
        $e = $idx + 1
        while ($e -lt $lines.Count -and $lines[$e] -match '^\s+- ') { $e++ }
        $n = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt $idx; $i++) { $n.Add($lines[$i]) }
        foreach ($b in $blk) { $n.Add($b) }
        for ($i = $e; $i -lt $lines.Count; $i++) { $n.Add($lines[$i]) }
        $lines = $n.ToArray()
    }
    [System.IO.File]::WriteAllText($cfg, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Milliseconds 2200
}
function LC { return @(Get-Content $log -Encoding UTF8).Count }
function SkipSince([int]$n) {
    $a = @(Get-Content $log -Encoding UTF8)
    if ($n -ge $a.Count) { return @() }
    return @($a[$n..($a.Count - 1)] | Where-Object { $_ -match 'Clipboard capture skipped' })
}
function TopText { $a = @(Get-Content $hist -Encoding UTF8) ; if ($a.Count -lt 2) { return '' } ; return ($a[1] -split "`t")[1] }

function Start-NotepadWithText([string]$Text) {
    Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 1000
    [void](Start-Process notepad.exe)
    Start-Sleep -Milliseconds 3500
    $real = Get-Process notepad -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    $wh = [IntPtr]$real.MainWindowHandle
    $script:E = [IntPtr]::Zero
    $cb = { param($x, $l) $sb = New-Object System.Text.StringBuilder 256 ; [void][W1B.K]::GetClassNameW($x, $sb, 256) ; if ($sb.ToString() -match '^(RichEdit|Edit)') { $script:E = $x } ; return $true }
    [void][W1B.K]::EnumChildWindows($wh, $cb, [IntPtr]::Zero)
    [void][W1B.K]::SendText($script:E, 0x000C, [IntPtr]::Zero, $Text)
    Start-Sleep -Milliseconds 300
    [void][W1B.K]::SendN($script:E, 0x00B1, [IntPtr]0, [IntPtr](-1))
    Start-Sleep -Milliseconds 300
    [void][W1B.K]::SendN($script:E, 0x0301, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_COPY
    return $real
}

Write-Host '=== 1.2 case B: copy, then close the source before the poll ==='
Put-List @('notepad.exe')
$m = 'DORUN-B-' + (Get-Date -Format 'HHmmss')
$mark = LC
$np = Start-NotepadWithText $m
Stop-Process -Id $np.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 2500
$sk = @(SkipSince $mark)
Write-Host ('B: skip=' + $sk.Count + ' top="' + (TopText) + '"')
$sk | ForEach-Object { Write-Host ('   ' + $_) }

Write-Host ''
Write-Host '=== 1.2 case B2: copy, then Alt+Tab away immediately ==='
$m2 = 'DORUN-B2-' + (Get-Date -Format 'HHmmss')
$mark = LC
$np = Start-NotepadWithText $m2
[void][W1B.K]::SetForegroundWindow([IntPtr]$np.MainWindowHandle)
[W1B.K]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)   # Alt down
[W1B.K]::keybd_event(0x09, 0, 0, [UIntPtr]::Zero)   # Tab
[W1B.K]::keybd_event(0x09, 0, 2, [UIntPtr]::Zero)
[W1B.K]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 2500
$sk = @(SkipSince $mark)
Write-Host ('B2: skip=' + $sk.Count + ' top="' + (TopText) + '"')
$sk | ForEach-Object { Write-Host ('   ' + $_) }
Stop-Process -Id $np.Id -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '=== 1.4 own copy with ignore list "*" ==='
Put-List @('*')
$mark = LC
$helper = 'C:\~\Sync\Doc\dorun\verify-run\tool\ClipHelper.exe'
# Popup copy path: publish an entry, then let DoRun re-record it via the popup.
& $helper text ('DORUN-OWNCOPY-' + (Get-Date -Format 'HHmmss'))
Start-Sleep -Milliseconds 1800
$sk = @(SkipSince $mark)
Write-Host ('1.4 helper copy skipped? skip lines=' + $sk.Count)
$sk | ForEach-Object { Write-Host ('   ' + $_) }
$a = @(Get-Content $hist -Encoding UTF8)
Write-Host ('1.4 history top: ' + $a[1])

Put-List @()
Write-Host ''
Write-Host 'restored ignore list:'
Get-Content $cfg -Encoding UTF8 | Select-String '^CLIPBOARD_HISTORY_IGNORE_PROCESSES:' -Context 0,3 | ForEach-Object { $_.Line }
